/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 * Implements Tasks 1, 2, and 3:
 *   Task 1: multi-container supervisor with namespace isolation + metadata
 *   Task 2: UNIX socket control-plane IPC (CLI <-> supervisor)
 *   Task 3: bounded-buffer logging pipeline (pipe -> buffer -> log file)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ── tunables ─────────────────────────────────────────────────── */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)
#define STOP_GRACE_MS        5000

/* ── enums (unchanged from boilerplate) ─────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

/* ── container metadata ──────────────────────────────────────────── */
typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                nice_value;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;
    char               log_path[PATH_MAX];
    char               command[CHILD_COMMAND_LEN];
    char               rootfs[PATH_MAX];
    int                pipe_read_fd;
    pthread_t          producer_tid;
    struct container_record *next;
} container_record_t;

/* ── bounded log buffer (Task 3) ────────────────────────────────── */
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/* ── control-plane messages (Task 2) ────────────────────────────── */
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

/* ── child clone() arg bundle ───────────────────────────────────── */
typedef struct {
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  pipe_write_fd;
} child_config_t;

/* ── supervisor context ─────────────────────────────────────────── */
typedef struct {
    int                server_fd;
    int                monitor_fd;
    volatile int       should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* global context needed by signal handlers */
static supervisor_ctx_t g_ctx;

/* forward declarations for monitor helpers */
int register_with_monitor(int fd, const char *id, pid_t pid,
                           unsigned long soft, unsigned long hard);
int unregister_from_monitor(int fd, const char *id, pid_t pid);

/* ════════════════════════════════════════════════════════════════
 * Parsing / utility helpers
 * ════════════════════════════════════════════════════════════════ */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s too large: %s\n", flag, value);
        return -1;
    }
    *out = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                 char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return -1;
        }
        if (!strcmp(argv[i], "--soft-mib")) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes))
                return -1;
            continue;
        }
        if (!strcmp(argv[i], "--hard-mib")) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes))
                return -1;
            continue;
        }
        if (!strcmp(argv[i], "--nice")) {
            char *end = NULL;
            errno = 0;
            long v = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || v < -20 || v > 19) {
                fprintf(stderr, "Invalid --nice (must be -20..19): %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)v;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}

static void ensure_log_dir(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) != 0)
        mkdir(LOG_DIR, 0755);
}

/* ════════════════════════════════════════════════════════════════
 * TASK 3 — Bounded buffer
 *
 * Synchronization choice: mutex + two condition variables.
 * - not_full:  producer waits here when buffer is at capacity.
 * - not_empty: consumer waits here when buffer is empty.
 * Race condition without sync: head/tail/count would be corrupted
 *   by simultaneous producer+consumer increments.
 * Deadlock prevention: shutdown broadcasts both CVs so no thread
 *   sleeps forever. Consumer drains remaining entries before exiting.
 * Lost-data prevention: producer BLOCKS (not drops) when full.
 * ════════════════════════════════════════════════════════════════ */
static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    if ((rc = pthread_mutex_init(&buf->mutex, NULL)))              return rc;
    if ((rc = pthread_cond_init(&buf->not_empty, NULL))) {
        pthread_mutex_destroy(&buf->mutex);                        return rc;
    }
    if ((rc = pthread_cond_init(&buf->not_full, NULL))) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);                        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/* Producer inserts an item. Blocks if full. Returns 0 on success,
 * -1 if the buffer is shutting down. */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* Consumer removes an item. Returns 1 on success, 0 when shut down
 * AND buffer is empty (caller should exit). */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0) {
        if (buf->shutting_down) {
            pthread_mutex_unlock(&buf->mutex);
            return 0;   /* fully drained — caller exits */
        }
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 1;
}

/* ════════════════════════════════════════════════════════════════
 * TASK 3 — Logging consumer thread
 * Drains buffer, writes each chunk to the correct per-container log.
 * ════════════════════════════════════════════════════════════════ */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item)) {
        char log_path[PATH_MAX] = {0};

        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *c = ctx->containers; c; c = c->next) {
            if (!strcmp(c->id, item.container_id)) {
                strncpy(log_path, c->log_path, PATH_MAX - 1);
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!log_path[0]) continue;

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        write(fd, item.data, item.length);
        close(fd);
    }

    fprintf(stderr, "[logger] consumer thread exiting, all log entries flushed\n");
    return NULL;
}

/* ════════════════════════════════════════════════════════════════
 * TASK 3 — Producer thread (one per container)
 * Reads from container's stdout pipe, pushes into bounded buffer.
 * ════════════════════════════════════════════════════════════════ */
typedef struct {
    int               pipe_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} producer_args_t;

static void *producer_thread(void *arg)
{
    producer_args_t *pa = (producer_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(item.container_id, 0, CONTAINER_ID_LEN);
    strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(pa->pipe_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        if (bounded_buffer_push(pa->buf, &item) != 0)
            break;
    }

    close(pa->pipe_fd);
    fprintf(stderr, "[logger] producer for '%s' exiting\n", pa->container_id);
    free(pa);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════
 * TASK 1 — Container child entry point (runs inside clone())
 * ════════════════════════════════════════════════════════════════ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout + stderr into the logging pipe */
    if (cfg->pipe_write_fd >= 0) {
        dup2(cfg->pipe_write_fd, STDOUT_FILENO);
        dup2(cfg->pipe_write_fd, STDERR_FILENO);
        close(cfg->pipe_write_fd);
    }

    /* Filesystem isolation */
    if (chroot(cfg->rootfs) != 0) { perror("chroot"); return 1; }
    if (chdir("/")           != 0) { perror("chdir");  return 1; }

    /* Mount /proc so ps, top, etc. work inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc");  /* non-fatal */

    /* Nice value */
    if (cfg->nice_value != 0 && nice(cfg->nice_value) == -1 && errno != 0)
        perror("nice");

    /* Split command string and exec */
    char *buf = strdup(cfg->command);
    char *exec_argv[64];
    int   na = 0;
    char *tok = strtok(buf, " \t");
    while (tok && na < 63) { exec_argv[na++] = tok; tok = strtok(NULL, " \t"); }
    exec_argv[na] = NULL;

    execvp(exec_argv[0], exec_argv);
    perror("execvp");
    free(buf);
    return 127;
}

/* ════════════════════════════════════════════════════════════════
 * TASK 1 — Launch container: clone() with new namespaces
 * ════════════════════════════════════════════════════════════════ */
static pid_t launch_container(supervisor_ctx_t *ctx, container_record_t *meta)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return -1; }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    child_config_t *cfg = malloc(sizeof(child_config_t));
    if (!cfg) {
        free(stack);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->rootfs,  meta->rootfs,  PATH_MAX - 1);
    strncpy(cfg->command, meta->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value    = meta->nice_value;
    cfg->pipe_write_fd = pipefd[1];

    /* New PID, UTS, and mount namespaces */
    int flags = SIGCHLD | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, cfg);

    close(pipefd[1]);   /* supervisor never writes to child's pipe */
    free(stack);        /* kernel copied the stack contents */
    free(cfg);          /* child already exec'd before we return */

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        return -1;
    }

    meta->pipe_read_fd = pipefd[0];

    /* Spawn producer thread (Task 3) */
    producer_args_t *pa = malloc(sizeof(producer_args_t));
    if (pa) {
        pa->pipe_fd = pipefd[0];
        strncpy(pa->container_id, meta->id, CONTAINER_ID_LEN - 1);
        pa->buf = &ctx->log_buffer;
        if (pthread_create(&meta->producer_tid, NULL, producer_thread, pa) != 0) {
            free(pa);
            close(pipefd[0]);
            meta->pipe_read_fd = -1;
        }
    }

    return pid;
}

/* ════════════════════════════════════════════════════════════════
 * Metadata helpers (call with metadata_lock held)
 * ════════════════════════════════════════════════════════════════ */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    for (container_record_t *c = ctx->containers; c; c = c->next)
        if (!strcmp(c->id, id)) return c;
    return NULL;
}

static container_record_t *find_by_pid(supervisor_ctx_t *ctx, pid_t pid)
{
    for (container_record_t *c = ctx->containers; c; c = c->next)
        if (c->host_pid == pid) return c;
    return NULL;
}

static container_record_t *alloc_record(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *r = calloc(1, sizeof(container_record_t));
    if (!r) return NULL;
    strncpy(r->id, id, CONTAINER_ID_LEN - 1);
    r->pipe_read_fd = -1;
    r->next = ctx->containers;
    ctx->containers = r;
    return r;
}

/* ════════════════════════════════════════════════════════════════
 * TASK 2 — Signal handlers
 * ════════════════════════════════════════════════════════════════ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx.metadata_lock);
        container_record_t *c = find_by_pid(&g_ctx, pid);
        if (c) {
            if (WIFEXITED(status)) {
                c->exit_code   = WEXITSTATUS(status);
                c->exit_signal = 0;
                c->state       = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                c->exit_code   = 128 + c->exit_signal;
                if (c->stop_requested)
                    c->state = CONTAINER_STOPPED;
                else if (c->exit_signal == SIGKILL)
                    c->state = CONTAINER_HARD_LIMIT_KILLED;
                else
                    c->state = CONTAINER_KILLED;
            }
            fprintf(stderr, "[supervisor] '%s' pid=%d exited state=%s\n",
                    c->id, pid, state_to_string(c->state));

            if (g_ctx.monitor_fd >= 0)
                unregister_from_monitor(g_ctx.monitor_fd, c->id, pid);
        }
        pthread_mutex_unlock(&g_ctx.metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[supervisor] shutdown signal — stopping\n");
    g_ctx.should_stop = 1;
}

/* ════════════════════════════════════════════════════════════════
 * TASK 2 — Supervisor command handlers
 * ════════════════════════════════════════════════════════════════ */
static void handle_start(supervisor_ctx_t *ctx, const control_request_t *req,
                          control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "container '%s' already exists", req->container_id);
        resp->status = -1;
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }

    container_record_t *rec = alloc_record(ctx, req->container_id);
    if (!rec) {
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "out of memory");
        resp->status = -1;
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }

    strncpy(rec->rootfs,  req->rootfs,  PATH_MAX - 1);
    strncpy(rec->command, req->command, CHILD_COMMAND_LEN - 1);
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice_value       = req->nice_value;
    rec->state            = CONTAINER_STARTING;
    rec->started_at       = time(NULL);
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);
    pthread_mutex_unlock(&ctx->metadata_lock);

    pid_t pid = launch_container(ctx, rec);
    if (pid < 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t **pp = &ctx->containers;
        while (*pp && *pp != rec) pp = &(*pp)->next;
        if (*pp) { *pp = rec->next; free(rec); }
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "failed to launch '%s'", req->container_id);
        resp->status = -1;
        return;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->host_pid = pid;
    rec->state    = CONTAINER_RUNNING;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    snprintf(resp->message, CONTROL_MESSAGE_LEN,
             "container '%s' started pid=%d", req->container_id, pid);
    resp->status = 0;
}

static void handle_stop(supervisor_ctx_t *ctx, const char *id,
                         control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, id);
    if (!c || c->state != CONTAINER_RUNNING) {
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 c ? "container '%s' not running" : "no container '%s'", id);
        resp->status = -1;
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    fprintf(stderr, "[supervisor] SIGTERM -> '%s' (pid %d)\n", id, pid);
    kill(pid, SIGTERM);

    for (int i = 0; i < STOP_GRACE_MS / 50; i++) {
        usleep(50000);
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *cc = find_container(ctx, id);
        int up = cc && cc->state == CONTAINER_RUNNING;
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (!up) {
            snprintf(resp->message, CONTROL_MESSAGE_LEN,
                     "container '%s' stopped gracefully", id);
            resp->status = 0;
            return;
        }
    }
    fprintf(stderr, "[supervisor] SIGKILL -> '%s' (pid %d)\n", id, pid);
    kill(pid, SIGKILL);
    snprintf(resp->message, CONTROL_MESSAGE_LEN,
             "container '%s' killed (did not stop in time)", id);
    resp->status = 0;
}

/* Write the ps table directly to a file descriptor */
static void write_ps_table(supervisor_ctx_t *ctx, int fd)
{
    char line[512];
    int  n;

    n = snprintf(line, sizeof(line),
        "%-12s %-7s %-20s %-10s %-10s %-5s %-22s %-s\n",
        "ID","PID","STARTED","SOFT_MiB","HARD_MiB","NICE","STATE","COMMAND");
    write(fd, line, n);

    pthread_mutex_lock(&ctx->metadata_lock);
    int found = 0;
    for (container_record_t *c = ctx->containers; c; c = c->next) {
        found = 1;
        char tbuf[20];
        struct tm *tm = localtime(&c->started_at);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
        n = snprintf(line, sizeof(line),
            "%-12s %-7d %-20s %-10lu %-10lu %-5d %-22s %-s\n",
            c->id, c->host_pid, tbuf,
            c->soft_limit_bytes >> 20,
            c->hard_limit_bytes >> 20,
            c->nice_value,
            state_to_string(c->state),
            c->command);
        write(fd, line, n);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!found) write(fd, "(no containers)\n", 16);
    write(fd, "__END__\n", 8);
}

/* Stream log file content to the client fd */
static void handle_logs(supervisor_ctx_t *ctx, const char *id,
                         int client_fd, control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, id);
    char log_path[PATH_MAX] = {0};
    if (c) strncpy(log_path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) {
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "no container '%s'", id);
        resp->status = -1;
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) {
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "no log yet for '%s'", id);
        resp->status = 0;
        return;
    }

    /* Send marker so client knows to read log data next */
    write(client_fd, "LOG_FOLLOWS\n", 12);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        write(client_fd, buf, n);
    fclose(f);

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "logs done");
}

/* ════════════════════════════════════════════════════════════════
 * TASK 2 — Supervisor event loop (UNIX socket accept loop)
 * ════════════════════════════════════════════════════════════════ */
static void supervisor_event_loop(supervisor_ctx_t *ctx)
{
    while (!ctx->should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->server_fd, &rfds);
        struct timeval tv = {0, 200000};  /* 200ms timeout so we check should_stop */
        int rc = select(ctx->server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) continue;

        int cfd = accept(ctx->server_fd, NULL, NULL);
        if (cfd < 0) continue;

        control_request_t  req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        if (read(cfd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
            close(cfd);
            continue;
        }

        switch (req.kind) {
        case CMD_START:
            handle_start(ctx, &req, &resp);
            break;
        case CMD_RUN:
            handle_start(ctx, &req, &resp);
            /* embed pid in message for client-side waitpid */
            if (resp.status == 0) {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *c = find_container(ctx, req.container_id);
                if (c) {
                    char extra[32];
                    snprintf(extra, sizeof(extra), " [pid=%d]", c->host_pid);
                    strncat(resp.message, extra,
                            CONTROL_MESSAGE_LEN - strlen(resp.message) - 1);
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
            break;
        case CMD_PS:
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "PS_FOLLOWS");
            break;
        case CMD_STOP:
            handle_stop(ctx, req.container_id, &resp);
            break;
        case CMD_LOGS:
            handle_logs(ctx, req.container_id, cfd, &resp);
            break;
        default:
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "unknown command");
        }

        write(cfd, &resp, sizeof(resp));

        /* Extra data after response struct */
        if (req.kind == CMD_PS && resp.status == 0)
            write_ps_table(ctx, cfd);

        close(cfd);
    }
}

/* ════════════════════════════════════════════════════════════════
 * run_supervisor — full supervisor setup + teardown
 * ════════════════════════════════════════════════════════════════ */
static int run_supervisor(const char *rootfs)
{
    int rc;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.server_fd  = -1;
    g_ctx.monitor_fd = -1;

    if ((rc = pthread_mutex_init(&g_ctx.metadata_lock, NULL))) {
        errno = rc; perror("pthread_mutex_init"); return 1;
    }
    if ((rc = bounded_buffer_init(&g_ctx.log_buffer))) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&g_ctx.metadata_lock); return 1;
    }

    ensure_log_dir();

    /* Try to open kernel monitor (Task 4 — optional until module loaded) */
    g_ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (g_ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] /dev/container_monitor not found "
                "(load monitor.ko for Task 4)\n");

    /* Create UNIX socket */
    unlink(CONTROL_PATH);
    g_ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(g_ctx.server_fd); return 1;
    }
    if (listen(g_ctx.server_fd, 16) < 0) {
        perror("listen"); close(g_ctx.server_fd); return 1;
    }
    chmod(CONTROL_PATH, 0777);

    /* Signal handlers */
    struct sigaction sa;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_flags   = 0;
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Start logging consumer thread */
    if (pthread_create(&g_ctx.logger_thread, NULL, logging_thread, &g_ctx)) {
        perror("pthread_create logger"); close(g_ctx.server_fd); return 1;
    }

    fprintf(stderr,
            "[supervisor] ready — pid=%d socket=%s rootfs=%s\n",
            getpid(), CONTROL_PATH, rootfs);
    fprintf(stderr,
            "[supervisor] use: engine start/run/ps/stop/logs from another terminal\n");

    /* Main loop */
    supervisor_event_loop(&g_ctx);

    /* ── Shutdown ── */
    fprintf(stderr, "[supervisor] shutting down...\n");

    /* SIGTERM all running containers */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *c = g_ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
            fprintf(stderr, "[supervisor] SIGTERM -> '%s'\n", c->id);
        }
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    sleep(2);

    /* Reap remaining */
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0) {}

    /* Join producer threads */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *c = g_ctx.containers; c; c = c->next)
        if (c->producer_tid) pthread_join(c->producer_tid, NULL);
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    /* Flush and join logger */
    bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
    pthread_join(g_ctx.logger_thread, NULL);
    bounded_buffer_destroy(&g_ctx.log_buffer);

    /* Free metadata */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    container_record_t *c = g_ctx.containers;
    while (c) { container_record_t *nx = c->next; free(c); c = nx; }
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    pthread_mutex_destroy(&g_ctx.metadata_lock);

    if (g_ctx.server_fd >= 0) close(g_ctx.server_fd);
    if (g_ctx.monitor_fd >= 0) close(g_ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] exited cleanly — no zombies, threads joined\n");
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * CLI client: send_control_request
 * ════════════════════════════════════════════════════════════════ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s\n"
                "Is 'engine supervisor' running?\n", CONTROL_PATH);
        close(fd); return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write"); close(fd); return 1;
    }

    control_response_t resp;
    if (read(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        perror("read"); close(fd); return 1;
    }

    if (resp.status != 0)
        fprintf(stderr, "Error: %s\n", resp.message);
    else if (req->kind != CMD_PS && req->kind != CMD_LOGS)
        printf("%s\n", resp.message);

    /* PS and LOGS: read remaining text */
    if ((req->kind == CMD_PS || req->kind == CMD_LOGS) && resp.status == 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(STDOUT_FILENO, buf, n);
    }

    /* RUN: block until container exits */
    if (req->kind == CMD_RUN && resp.status == 0) {
        pid_t pid = -1;
        char *p = strstr(resp.message, "[pid=");
        if (p) pid = (pid_t)atoi(p + 5);
        if (pid > 0) {
            int wst;
            waitpid(pid, &wst, 0);
            int code = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst);
            printf("container exited with code %d\n", code);
            close(fd);
            return code;
        }
    }

    close(fd);
    return resp.status == 0 ? 0 : 1;
}

/* ════════════════════════════════════════════════════════════════
 * CLI sub-commands
 * ════════════════════════════════════════════════════════════════ */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <command> [...]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <command> [...]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ── monitor helpers ────────────────────────────────────────────── */
int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid, unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0 ? -1 : 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0 ? -1 : 0;
}

/* ════════════════════════════════════════════════════════════════
 * main()
 * ════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "supervisor")) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (!strcmp(argv[1], "start")) return cmd_start(argc, argv);
    if (!strcmp(argv[1], "run"))   return cmd_run(argc, argv);
    if (!strcmp(argv[1], "ps"))    return cmd_ps();
    if (!strcmp(argv[1], "logs"))  return cmd_logs(argc, argv);
    if (!strcmp(argv[1], "stop"))  return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
