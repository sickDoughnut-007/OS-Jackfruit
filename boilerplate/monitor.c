/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 * Task 4: kernel-space memory monitoring with soft + hard limits.
 *
 * All TODO sections are implemented.
 * RSS helpers are restored and working.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ════════════════════════════════════════════════════════════════
 * TODO 1 — Linked-list node for each monitored container process
 * ════════════════════════════════════════════════════════════════ */
struct monitored_entry {
    pid_t          pid;
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            soft_warned;           /* 1 after first soft-limit warning */
    char           container_id[MONITOR_NAME_LEN];
    struct list_head list;
};

/* ════════════════════════════════════════════════════════════════
 * TODO 2 — Global list + mutex
 *
 * Mutex chosen over spinlock because:
 *   - ioctl() runs in process context (can sleep)
 *   - timer callback runs in softirq context but we use mutex_trylock
 *     or switch to a workqueue if blocking is needed
 *   - get_task_mm / mmput can sleep, so holding a spinlock there
 *     would cause a BUG; mutex is the correct primitive here
 * ════════════════════════════════════════════════════════════════ */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_lock);

/* ── Internal device / timer state ── */
static struct timer_list monitor_timer;
static dev_t              dev_num;
static struct cdev        c_dev;
static struct class      *cl;

/* ════════════════════════════════════════════════════════════════
 * RSS helper — returns RSS in bytes, or -1 if task gone
 * ════════════════════════════════════════════════════════════════ */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ── Soft-limit event logger ── */
static void log_soft_limit_event(const char *container_id, pid_t pid,
                                  unsigned long limit_bytes, long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ── Hard-limit enforcer ── */
static void kill_process(const char *container_id, pid_t pid,
                          unsigned long limit_bytes, long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu — SIGKILL sent\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ════════════════════════════════════════════════════════════════
 * TODO 3 — Timer callback: periodic RSS check
 *
 * Uses list_for_each_entry_safe so we can delete entries mid-loop.
 * Mutex: we use mutex_trylock here — if we can't get it this tick
 * we just skip and retry next second (avoids deadlock in softirq).
 * In practice the ioctl paths are short so contention is rare.
 * ════════════════════════════════════════════════════════════════ */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;

    (void)t;

    if (!mutex_trylock(&monitored_lock))
        goto reschedule;

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(entry->pid);

        /* Process no longer exists — remove stale entry */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] process gone: container=%s pid=%d — removing\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Hard limit: kill and remove */
        if ((unsigned long)rss > entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit: warn once */
        if (!entry->soft_warned &&
            (unsigned long)rss > entry->soft_limit_bytes) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&monitored_lock);

reschedule:
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ════════════════════════════════════════════════════════════════
 * IOCTL handler
 * ════════════════════════════════════════════════════════════════ */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* null-terminate container_id defensively */
    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    /* ════════════════════════════════════════════════════════════
     * TODO 4 — Register: allocate and insert a new entry
     * ════════════════════════════════════════════════════════════ */
    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;

        printk(KERN_INFO
               "[container_monitor] REGISTER container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING
                   "[container_monitor] REGISTER rejected: soft > hard for %s\n",
                   req.container_id);
            return -EINVAL;
        }

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid               = req.pid;
        entry->soft_limit_bytes  = req.soft_limit_bytes;
        entry->hard_limit_bytes  = req.hard_limit_bytes;
        entry->soft_warned       = 0;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN - 1);
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&monitored_lock);
        list_add_tail(&entry->list, &monitored_list);
        mutex_unlock(&monitored_lock);

        return 0;
    }

    /* ════════════════════════════════════════════════════════════
     * TODO 5 — Unregister: find by pid + id, remove if found
     * ════════════════════════════════════════════════════════════ */
    printk(KERN_INFO
           "[container_monitor] UNREGISTER container=%s pid=%d\n",
           req.container_id, req.pid);

    {
        struct monitored_entry *entry, *tmp;
        int found = 0;

        mutex_lock(&monitored_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid &&
                !strncmp(entry->container_id, req.container_id, MONITOR_NAME_LEN)) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }
        mutex_unlock(&monitored_lock);

        if (!found) {
            printk(KERN_WARNING
                   "[container_monitor] UNREGISTER: not found container=%s pid=%d\n",
                   req.container_id, req.pid);
            return -ENOENT;
        }
    }
    return 0;
}

/* ── File operations ── */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ── Module init ── */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. /dev/%s ready.\n",
           DEVICE_NAME);
    return 0;
}

/* ── Module exit ── */
static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    timer_delete_sync(&monitor_timer);

    /* ════════════════════════════════════════════════════════════
     * TODO 6 — Free all remaining entries on unload
     * ════════════════════════════════════════════════════════════ */
    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        printk(KERN_INFO
               "[container_monitor] cleanup: freeing container=%s pid=%d\n",
               entry->container_id, entry->pid);
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded cleanly.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
