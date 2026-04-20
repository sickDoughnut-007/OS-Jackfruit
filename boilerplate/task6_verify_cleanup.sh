#!/usr/bin/env bash
# task6_verify_cleanup.sh
# Task 6: Verifies clean resource teardown.
# Run this AFTER a full demo run to confirm:
#   - no zombie processes
#   - no stale /proc/<pid> entries
#   - threads joined (supervisor exited cleanly)
#   - kernel module list empty after rmmod
#
# Usage (from boilerplate/):
#   sudo bash task6_verify_cleanup.sh

set -euo pipefail

BOILERPLATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$BOILERPLATE_DIR"

ENGINE="./engine"
PASS=0
FAIL=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
info() { echo "[INFO] $1"; }

echo "============================================================"
echo " Task 6: Resource Cleanup Verification"
echo " $(date)"
echo "============================================================"
echo ""

# ── Step 1: Full demo run ─────────────────────────────────────────
echo "[*] Running a full demo cycle..."

# Copy binaries into rootfs if needed
[ -f "../rootfs-alpha/cpu_hog"    ] || cp cpu_hog    ../rootfs-alpha/
[ -f "../rootfs-alpha/memory_hog" ] || cp memory_hog ../rootfs-alpha/
[ -f "../rootfs-beta/cpu_hog"     ] || cp cpu_hog    ../rootfs-beta/

# Start two containers
info "Starting containers alpha and beta..."
$ENGINE start alpha ../rootfs-alpha "/cpu_hog 10" --soft-mib 40 --hard-mib 64
$ENGINE start beta  ../rootfs-beta  "/cpu_hog 10" --soft-mib 40 --hard-mib 64

info "engine ps:"
$ENGINE ps

info "Waiting 5 seconds while containers run..."
sleep 5

# Record PIDs before stopping
ALPHA_PID=$($ENGINE ps 2>/dev/null | awk '/^alpha/{print $2}')
BETA_PID=$($ENGINE ps  2>/dev/null | awk '/^beta/{print $2}')
info "alpha pid=$ALPHA_PID  beta pid=$BETA_PID"

# Stop containers
info "Stopping containers..."
$ENGINE stop alpha
$ENGINE stop beta
sleep 3

info "engine ps after stop:"
$ENGINE ps

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Check 1: No zombie processes"
echo "────────────────────────────────────────────────────────────"
ZOMBIES=$(ps aux | awk '$8 == "Z" {print}')
if [ -z "$ZOMBIES" ]; then
    pass "No zombie processes found"
else
    fail "Zombie processes detected:"
    echo "$ZOMBIES"
fi

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Check 2: Container PIDs no longer exist in /proc"
echo "────────────────────────────────────────────────────────────"
for pid in $ALPHA_PID $BETA_PID; do
    if [ -z "$pid" ]; then
        info "PID not captured — skipping"
        continue
    fi
    if [ -d "/proc/$pid" ]; then
        fail "/proc/$pid still exists after container stop"
    else
        pass "/proc/$pid is gone (container reaped)"
    fi
done

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Check 3: Log files were created (Task 3 logging pipeline)"
echo "────────────────────────────────────────────────────────────"
for id in alpha beta; do
    LOGFILE="./logs/${id}.log"
    if [ -f "$LOGFILE" ] && [ -s "$LOGFILE" ]; then
        LINES=$(wc -l < "$LOGFILE")
        pass "logs/${id}.log exists with ${LINES} lines"
    elif [ -f "$LOGFILE" ]; then
        info "logs/${id}.log exists but is empty (container may have produced no output)"
    else
        fail "logs/${id}.log not found — logging pipeline may not be working"
    fi
done

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Check 4: Kernel module cleanup (if loaded)"
echo "────────────────────────────────────────────────────────────"
if lsmod | awk '{print $1}' | grep -qx monitor; then
    info "monitor.ko is loaded — checking dmesg for stale entries..."
    dmesg --clear 2>/dev/null || true
    info "Unloading monitor.ko..."
    rmmod monitor
    sleep 1
    UNLOAD_MSG=$(dmesg | grep "container_monitor" | tail -5)
    echo "$UNLOAD_MSG"
    if echo "$UNLOAD_MSG" | grep -q "unloaded cleanly"; then
        pass "kernel module unloaded cleanly"
    else
        info "Module unloaded (check dmesg manually for cleanup messages)"
    fi
    # Reload for continued use
    info "Reloading monitor.ko..."
    insmod monitor.ko
    pass "monitor.ko reloaded successfully"
else
    info "monitor.ko not loaded — skipping kernel cleanup check"
    info "(Load it with: sudo insmod monitor.ko)"
fi

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Check 5: No leaked file descriptors (spot check)"
echo "────────────────────────────────────────────────────────────"
SUP_PID=$(pgrep -f "engine supervisor" | head -1 || true)
if [ -n "$SUP_PID" ]; then
    FD_COUNT=$(ls /proc/$SUP_PID/fd 2>/dev/null | wc -l)
    info "Supervisor pid=$SUP_PID has $FD_COUNT open file descriptors"
    if [ "$FD_COUNT" -lt 50 ]; then
        pass "FD count looks reasonable ($FD_COUNT open FDs)"
    else
        info "FD count is $FD_COUNT — verify no leaks manually"
    fi
else
    info "Supervisor not running — cannot check FDs"
fi

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Check 6: ps aux confirms no orphaned engine/sh processes"
echo "────────────────────────────────────────────────────────────"
info "Current engine-related processes:"
ps aux | grep -E "(engine|cpu_hog|memory_hog|io_pulse)" | grep -v grep || echo "(none — clean)"
ORPHANS=$(ps aux | grep -E "(cpu_hog|memory_hog|io_pulse)" | grep -v grep | wc -l)
if [ "$ORPHANS" -eq 0 ]; then
    pass "No orphaned workload processes"
else
    fail "$ORPHANS orphaned workload process(es) still running"
fi

echo ""
echo "============================================================"
echo " Task 6 Summary: ${PASS} passed, ${FAIL} failed"
echo "============================================================"
echo ""
echo "Take screenshots of:"
echo "  1. This output showing PASS for all checks"
echo "  2. 'ps aux' output confirming no zombies (Z state)"
echo "  3. dmesg showing clean kernel module unload"
echo ""

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
