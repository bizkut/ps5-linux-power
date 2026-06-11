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
ROOT="$(cd "$DIR/.." && pwd)"
CPU_GOV="$DIR/ps5_cpu_gov"
GPU_GOV="$DIR/ps5_gpu_gov"
CPU_CTL="$ROOT/ps5_cpu"
GPU_CTL="$ROOT/ps5_gpu"
BOOST_CTL="$ROOT/ps5_boost"
SMN_CFG="/sys/bus/pci/devices/0000:00:00.0/config"
PS5GOV_CONFIG="${PS5GOV_CONFIG:-/etc/ps5-linux-cpuclock/ps5gov.conf}"
PRINT_CONFIG=0

case "${1:-}" in
--print-config)
	PRINT_CONFIG=1
	shift
	;;
esac

die() { echo "run-governors: $*" >&2; exit 1; }
log() { echo "run-governors: $*"; }

load_config() {
	line=""
	key=""
	value=""

	[ -r "$PS5GOV_CONFIG" ] || return 0
	while IFS= read -r line; do
		case "$line" in
		""|\#*) continue ;;
		*=*) ;;
		*) continue ;;
		esac

		key=${line%%=*}
		value=${line#*=}
		case "$value" in
		\"*\") value=${value#\"}; value=${value%\"} ;;
		\'*\') value=${value#\'}; value=${value%\'} ;;
		esac

		case "$key" in
		PROFILE) [ -z "${PROFILE+x}" ] && PROFILE=$value ;;
		CPU_ARGS) [ -z "${CPU_ARGS+x}" ] && CPU_ARGS=$value ;;
		GPU_ARGS) [ -z "${GPU_ARGS+x}" ] && GPU_ARGS=$value ;;
		esac
	done < "$PS5GOV_CONFIG"
}

apply_profile() {
	PROFILE="${PROFILE:-balanced}"
	case "$PROFILE" in
	quiet)
		: "${CPU_ARGS:=-i 600 -d 6 -H 55 -M 30 -L 12}"
		: "${GPU_ARGS:=-i 900 -d 5 -H 65 -M 30 -L 8 -T 80 -R 70 -S auto -m fdinfo}"
		;;
	balanced)
		: "${CPU_ARGS:=-i 400 -d 4 -H 40 -M 20 -L 8}"
		: "${GPU_ARGS:=-i 700 -d 3 -H 50 -M 20 -L 5 -T 85 -R 75 -S auto -m fdinfo}"
		;;
	performance)
		: "${CPU_ARGS:=-i 250 -d 6 -H 25 -M 12 -L 4}"
		: "${GPU_ARGS:=-i 350 -d 5 -H 30 -M 12 -L 3 -T 90 -R 80 -S auto -m fdinfo}"
		;;
	*)
		die "invalid PROFILE=$PROFILE (use quiet, balanced, or performance)"
		;;
	esac
}

restore_defaults() {
	[ -x "$BOOST_CTL" ] && "$BOOST_CTL" off >/dev/null 2>&1
	[ -x "$CPU_CTL" ] && "$CPU_CTL" set 0xff 0 >/dev/null 2>&1
	[ -x "$GPU_CTL" ] && "$GPU_CTL" reset >/dev/null 2>&1
}

load_config
apply_profile

if [ "$PRINT_CONFIG" -eq 1 ]; then
	printf 'PROFILE=%s\nCPU_ARGS=%s\nGPU_ARGS=%s\n' "$PROFILE" "$CPU_ARGS" "$GPU_ARGS"
	exit 0
fi

[ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)"
[ -x "$CPU_GOV" ] || die "missing $CPU_GOV (run 'make' first)"
[ -x "$GPU_GOV" ] || die "missing $GPU_GOV (run 'make' first)"
[ -x "$CPU_CTL" ] || die "missing $CPU_CTL (run 'make' first)"
[ -x "$GPU_CTL" ] || die "missing $GPU_CTL (run 'make' first)"
[ -x "$BOOST_CTL" ] || die "missing $BOOST_CTL (run 'make' first)"
[ -e /dev/mp1 ] || die "missing /dev/mp1 (required for boost control)"
[ -e "$SMN_CFG" ] || die "missing $SMN_CFG (required for mailbox access)"

CPU_PID=""
GPU_PID=""
cleanup() {
	log "stopping..."
	[ -n "$CPU_PID" ] && kill "$CPU_PID" 2>/dev/null
	[ -n "$GPU_PID" ] && kill "$GPU_PID" 2>/dev/null
	wait 2>/dev/null
	restore_defaults
	log "stopped profile=$PROFILE restored=1"
	exit 0
}
trap cleanup INT TERM

"$CPU_GOV" $CPU_ARGS & CPU_PID=$!
sleep 1
"$GPU_GOV" $GPU_ARGS & GPU_PID=$!

log "started profile=$PROFILE cpu_pid=$CPU_PID cpu_args=\"$CPU_ARGS\" gpu_pid=$GPU_PID gpu_args=\"$GPU_ARGS\""

wait -n 2>/dev/null || wait
cleanup
