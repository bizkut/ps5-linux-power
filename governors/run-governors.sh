#!/bin/sh
# run-governors.sh - start the PS5 CPU + GPU governors together (userspace PoC).
#
# No kernel module / no insmod: the governors reach the SMU mailbox directly
# over PCI config space and serialize via an flock (see ../smn.h). They are run
# with DELIBERATELY DIFFERENT intervals (400 vs 700 ms) anyway. There is no DF
# governor on purpose (DF writes can deadlock the mailbox).
#
#   sudo ./run-governors.sh        # foreground, Ctrl+C stops + restores clocks
#
# On stop each governor restores full clock (CPU P0/3200, GPU 2000).
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
CPU_GOV="$DIR/ps5_cpu_gov"
GPU_GOV="$DIR/ps5_gpu_gov"
CPU_ARGS="-i 400 -d 4"
GPU_ARGS="-i 700 -d 3"

die() { echo "run-governors: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)"
[ -x "$CPU_GOV" ] || die "missing $CPU_GOV (run 'make' first)"
[ -x "$GPU_GOV" ] || die "missing $GPU_GOV (run 'make' first)"

CPU_PID=""
GPU_PID=""
cleanup() {
	echo "run-governors: stopping..."
	[ -n "$CPU_PID" ] && kill "$CPU_PID" 2>/dev/null
	[ -n "$GPU_PID" ] && kill "$GPU_PID" 2>/dev/null
	wait 2>/dev/null
	echo "run-governors: stopped (clocks restored to full)."
	exit 0
}
trap cleanup INT TERM

"$CPU_GOV" $CPU_ARGS & CPU_PID=$!
sleep 1
"$GPU_GOV" $GPU_ARGS & GPU_PID=$!

echo "run-governors: cpu_gov pid=$CPU_PID [$CPU_ARGS], gpu_gov pid=$GPU_PID [$GPU_ARGS]"

wait -n 2>/dev/null || wait
cleanup
