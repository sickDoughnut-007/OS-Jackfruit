#!/usr/bin/env bash
# test_memory_limits.sh
# Task 4: Demonstrates soft-limit warning and hard-limit enforcement
# via the kernel monitor module.
#
# Prerequisites:
#   - supervisor already running: sudo ./engine supervisor ../rootfs-base
#   - monitor.ko loaded:          sudo insmod monitor.ko
#   - memory_hog copied into rootfs:
#       cp memory_hog ../rootfs-alpha/
#
# Usage (from boilerplate/ directory):
#   sudo bash test_memory_limits.sh

set -euo pipefail

BOILERPLATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$BOILERPLATE_DIR"

ENGINE="./engine"
ROOTFS_ALPHA="../rootfs-alpha"

echo "============================================================"
echo " Task 4: Memory Limit Test"
echo "============================================================"

# ── Sanity checks ────────────────────────────────────────────────
if ! lsmod | awk '{print $1}' | grep -qx monitor; then
    echo "[ERROR] monitor.ko is not loaded. Run: sudo insmod monitor.ko"
    exit 1
fi
echo "[OK] monitor.ko is loaded"

if [ ! -e /dev/container_monitor ]; then
    echo "[ERROR] /dev/container_monitor not found"
    exit 1
fi
echo "[OK] /dev/container_monitor exists"

if [ ! -f "${ROOTFS_ALPHA}/memory_hog" ]; then
    echo "[INFO] Copying memory_hog into rootfs-alpha..."
    cp memory_hog "${ROOTFS_ALPHA}/memory_hog"
fi

# ── Clear dmesg so output is easy to read ───────────────────────
echo "[*] Clearing dmesg..."
dmesg --clear 2>/dev/null || true

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Test 1: Soft-limit warning"
echo "  soft=20MiB hard=200MiB — memory_hog grows to ~30MiB"
echo "  Expected: dmesg shows SOFT LIMIT warning"
echo "────────────────────────────────────────────────────────────"

# Start container with low soft limit (20 MiB) but high hard limit (200 MiB)
# memory_hog allocates 8 MiB per second by default → will cross 20 MiB quickly
$ENGINE start memtest1 "$ROOTFS_ALPHA" /memory_hog \
    --soft-mib 20 --hard-mib 200

echo "[*] Container memtest1 started. Waiting 8 seconds for soft limit breach..."
sleep 8

echo ""
echo "[*] dmesg output (soft-limit events):"
echo "────────────────────────────────────────────────────────────"
dmesg | grep -E "container_monitor.*(memtest1|SOFT)" || echo "(no soft-limit events yet — wait longer or check limits)"
echo "────────────────────────────────────────────────────────────"

echo ""
echo "[*] engine ps:"
$ENGINE ps

# Stop cleanly
$ENGINE stop memtest1
echo "[*] memtest1 stopped."

sleep 2

echo ""
echo "────────────────────────────────────────────────────────────"
echo " Test 2: Hard-limit enforcement"
echo "  soft=10MiB hard=30MiB — memory_hog will be killed"
echo "  Expected: dmesg shows HARD LIMIT, engine ps shows hard_limit_killed"
echo "────────────────────────────────────────────────────────────"

dmesg --clear 2>/dev/null || true

$ENGINE start memtest2 "$ROOTFS_ALPHA" /memory_hog \
    --soft-mib 10 --hard-mib 30

echo "[*] Container memtest2 started. Waiting 10 seconds for hard limit breach..."
sleep 10

echo ""
echo "[*] dmesg output (hard-limit events):"
echo "────────────────────────────────────────────────────────────"
dmesg | grep -E "container_monitor.*(memtest2|SOFT|HARD)" || echo "(no events yet)"
echo "────────────────────────────────────────────────────────────"

echo ""
echo "[*] engine ps (should show hard_limit_killed or exited):"
$ENGINE ps

echo ""
echo "============================================================"
echo " Task 4 test complete."
echo " Take screenshots of:"
echo "   1. dmesg showing SOFT LIMIT warning for memtest1"
echo "   2. dmesg showing HARD LIMIT kill for memtest2"
echo "   3. engine ps showing 'hard_limit_killed' state"
echo "============================================================"
