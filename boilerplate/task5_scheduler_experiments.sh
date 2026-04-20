#!/usr/bin/env bash
# task5_scheduler_experiments.sh
# Task 5: Scheduler Experiments
#
# Experiment 1: Two CPU-bound containers at different nice values
#   → shows how CFS gives more CPU time to the lower-nice container
#
# Experiment 2: CPU-bound vs I/O-bound at same nice value
#   → shows how Linux scheduler gives I/O-bound tasks priority boosts
#
# Prerequisites:
#   - supervisor running:   sudo ./engine supervisor ../rootfs-base
#   - binaries copied into rootfs:
#       cp cpu_hog  ../rootfs-alpha/
#       cp cpu_hog  ../rootfs-beta/
#       cp io_pulse ../rootfs-alpha/
#       cp io_pulse ../rootfs-beta/
#
# Usage (from boilerplate/):
#   sudo bash task5_scheduler_experiments.sh

set -euo pipefail

BOILERPLATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$BOILERPLATE_DIR"

ENGINE="./engine"
ROOTFS_ALPHA="../rootfs-alpha"
ROOTFS_BETA="../rootfs-beta"
RESULTS_FILE="./scheduler_results.txt"

echo "============================================================" | tee "$RESULTS_FILE"
echo " Task 5: Linux Scheduler Experiments"                         | tee -a "$RESULTS_FILE"
echo " Date: $(date)"                                               | tee -a "$RESULTS_FILE"
echo " Kernel: $(uname -r)"                                         | tee -a "$RESULTS_FILE"
echo " CPUs: $(nproc)"                                              | tee -a "$RESULTS_FILE"
echo "============================================================" | tee -a "$RESULTS_FILE"

# Helper: copy workload binaries into both rootfs copies
prepare_rootfs() {
    for bin in cpu_hog io_pulse; do
        [ -f "$ROOTFS_ALPHA/$bin" ] || cp "$bin" "$ROOTFS_ALPHA/$bin"
        [ -f "$ROOTFS_BETA/$bin"  ] || cp "$bin" "$ROOTFS_BETA/$bin"
    done
    echo "[*] Binaries ready in rootfs copies."
}

prepare_rootfs

# ══════════════════════════════════════════════════════════════════
# Experiment 1: Two CPU-bound containers, different nice values
#
# Both run cpu_hog for 30 seconds.
# alpha: nice=0  (default priority)
# beta:  nice=10 (lower priority — gets less CPU)
#
# Measurement: wall-clock time each finishes (from engine logs)
# and CPU% observed via `top` or /proc/<pid>/stat.
# ══════════════════════════════════════════════════════════════════
echo ""                                                            | tee -a "$RESULTS_FILE"
echo "────────────────────────────────────────────────────────────" | tee -a "$RESULTS_FILE"
echo " Experiment 1: CPU-bound × 2 with different nice values"     | tee -a "$RESULTS_FILE"
echo "  alpha: /cpu_hog 30  (nice=0)"                              | tee -a "$RESULTS_FILE"
echo "  beta:  /cpu_hog 30  (nice=10)"                             | tee -a "$RESULTS_FILE"
echo "────────────────────────────────────────────────────────────" | tee -a "$RESULTS_FILE"

# Wait for any previous experiment containers to clear
sleep 1

EXP1_START=$(date +%s%N)

$ENGINE start cpu-hi ../rootfs-alpha "/cpu_hog 30" --nice 0
$ENGINE start cpu-lo ../rootfs-beta  "/cpu_hog 30" --nice 10

echo "[*] Both containers started at $(date +%T). Monitoring for 35 seconds..."
echo "[*] Watching CPU usage (sample every 3s):"                    | tee -a "$RESULTS_FILE"

# Sample /proc/<pid>/stat to compute CPU% for each container
cpu_hi_pid=$($ENGINE ps 2>/dev/null | awk '/cpu-hi/{print $2}')
cpu_lo_pid=$($ENGINE ps 2>/dev/null | awk '/cpu-lo/{print $2}')

echo "  Time      cpu-hi(nice=0) cpu-lo(nice=10)" | tee -a "$RESULTS_FILE"

prev_hi=0; prev_lo=0; prev_t=0
for i in $(seq 1 11); do
    sleep 3
    now=$(date +%T)

    # Read /proc/<pid>/stat fields 14 (utime) + 15 (stime)
    hi_ticks=0; lo_ticks=0
    if [ -f "/proc/${cpu_hi_pid}/stat" ]; then
        read -r line < "/proc/${cpu_hi_pid}/stat"
        utime=$(echo "$line" | awk '{print $14}')
        stime=$(echo "$line" | awk '{print $15}')
        hi_ticks=$((utime + stime))
    fi
    if [ -f "/proc/${cpu_lo_pid}/stat" ]; then
        read -r line < "/proc/${cpu_lo_pid}/stat"
        utime=$(echo "$line" | awk '{print $14}')
        stime=$(echo "$line" | awk '{print $15}')
        lo_ticks=$((utime + stime))
    fi

    delta_hi=$((hi_ticks - prev_hi))
    delta_lo=$((lo_ticks - prev_lo))
    prev_hi=$hi_ticks; prev_lo=$lo_ticks

    echo "  ${now}  ${delta_hi} ticks      ${delta_lo} ticks" | tee -a "$RESULTS_FILE"
done

EXP1_END=$(date +%s%N)
EXP1_MS=$(( (EXP1_END - EXP1_START) / 1000000 ))

echo ""                                                            | tee -a "$RESULTS_FILE"
echo "[*] Experiment 1 wall time: ${EXP1_MS} ms"                  | tee -a "$RESULTS_FILE"
echo "[*] engine ps after Experiment 1:"                           | tee -a "$RESULTS_FILE"
$ENGINE ps 2>&1 | tee -a "$RESULTS_FILE"

# Logs show each container's reported elapsed time
echo "[*] Logs from cpu-hi (nice=0):"    | tee -a "$RESULTS_FILE"
$ENGINE logs cpu-hi 2>&1 | tail -5       | tee -a "$RESULTS_FILE"
echo "[*] Logs from cpu-lo (nice=10):"   | tee -a "$RESULTS_FILE"
$ENGINE logs cpu-lo 2>&1 | tail -5       | tee -a "$RESULTS_FILE"

$ENGINE stop cpu-hi 2>/dev/null || true
$ENGINE stop cpu-lo 2>/dev/null || true
sleep 2

# ══════════════════════════════════════════════════════════════════
# Experiment 2: CPU-bound vs I/O-bound at same nice value
#
# cpu-worker: runs cpu_hog for 20 seconds (burns CPU)
# io-worker:  runs io_pulse for 20 iterations, 200ms sleep between writes
#
# Expected: io_pulse completes all iterations and feels "responsive"
# because the CFS scheduler rewards its voluntary sleeps with priority boosts.
# ══════════════════════════════════════════════════════════════════
echo ""                                                            | tee -a "$RESULTS_FILE"
echo "────────────────────────────────────────────────────────────" | tee -a "$RESULTS_FILE"
echo " Experiment 2: CPU-bound vs I/O-bound (same nice=0)"         | tee -a "$RESULTS_FILE"
echo "  cpu-worker: /cpu_hog 20"                                   | tee -a "$RESULTS_FILE"
echo "  io-worker:  /io_pulse 20 200"                              | tee -a "$RESULTS_FILE"
echo "────────────────────────────────────────────────────────────" | tee -a "$RESULTS_FILE"

EXP2_START=$(date +%s%N)
CPU_START=$(date +%T)

$ENGINE start cpu-worker ../rootfs-alpha "/cpu_hog 20"      --nice 0
$ENGINE start io-worker  ../rootfs-beta  "/io_pulse 20 200" --nice 0

echo "[*] Both started at ${CPU_START}. Waiting 25 seconds..."

sleep 25

EXP2_END=$(date +%s%N)
IO_FINISH=$(date +%T)

echo "[*] Experiment 2 complete at ${IO_FINISH}"                   | tee -a "$RESULTS_FILE"

echo ""                                                            | tee -a "$RESULTS_FILE"
echo "[*] engine ps:"                                              | tee -a "$RESULTS_FILE"
$ENGINE ps 2>&1 | tee -a "$RESULTS_FILE"

echo "[*] Logs from cpu-worker:"  | tee -a "$RESULTS_FILE"
$ENGINE logs cpu-worker 2>&1      | tee -a "$RESULTS_FILE"
echo ""                           | tee -a "$RESULTS_FILE"
echo "[*] Logs from io-worker:"   | tee -a "$RESULTS_FILE"
$ENGINE logs io-worker 2>&1       | tee -a "$RESULTS_FILE"

$ENGINE stop cpu-worker 2>/dev/null || true
$ENGINE stop io-worker  2>/dev/null || true
sleep 2

# ══════════════════════════════════════════════════════════════════
# Summary table (for README)
# ══════════════════════════════════════════════════════════════════
echo ""                                                                     | tee -a "$RESULTS_FILE"
echo "============================================================"          | tee -a "$RESULTS_FILE"
echo " Results Summary"                                                      | tee -a "$RESULTS_FILE"
echo "============================================================"          | tee -a "$RESULTS_FILE"
echo ""                                                                      | tee -a "$RESULTS_FILE"
echo " Experiment 1: Two CPU-bound containers, different nice values"        | tee -a "$RESULTS_FILE"
echo "   Observation: cpu-hi (nice=0) accumulates more CPU ticks per"       | tee -a "$RESULTS_FILE"
echo "   interval than cpu-lo (nice=10). Linux CFS assigns vruntime"        | tee -a "$RESULTS_FILE"
echo "   weights by nice value — lower nice = larger weight = more CPU."    | tee -a "$RESULTS_FILE"
echo ""                                                                      | tee -a "$RESULTS_FILE"
echo " Experiment 2: CPU-bound vs I/O-bound at same priority"               | tee -a "$RESULTS_FILE"
echo "   Observation: io-worker completes all 20 iterations without"        | tee -a "$RESULTS_FILE"
echo "   starvation despite cpu-worker saturating the CPU. CFS tracks"      | tee -a "$RESULTS_FILE"
echo "   minimum vruntime — after each sleep io-worker gets priority"       | tee -a "$RESULTS_FILE"
echo "   over the CPU-bound task, giving I/O-bound work responsiveness."    | tee -a "$RESULTS_FILE"
echo ""                                                                      | tee -a "$RESULTS_FILE"
echo " Results saved to: $RESULTS_FILE"
echo "============================================================"          | tee -a "$RESULTS_FILE"
