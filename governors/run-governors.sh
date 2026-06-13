#!/bin/sh
# run-governors.sh - start the PS5 CPU + GPU governors together (userspace PoC).
#
# No kernel module / no insmod: the governors reach the SMU mailbox directly
# over PCI config space and serialize via an flock (see ../smn.h). They are run
# with DELIBERATELY DIFFERENT intervals (400 vs 700 ms) anyway. There is no DF
# governor on purpose (DF writes can deadlock the mailbox). This script controls
# only clocking; fan curves are handled elsewhere (ps5fan / platform thermal
# control), so we keep cooling policy separate.
#
#   sudo ./run-governors.sh        # foreground, Ctrl+C stops + restores clocks
#
# On stop each governor restores full clock (CPU P0/3200, GPU 2000).
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
CPU_GOV="$DIR/ps5_cpu_gov"
GPU_GOV="$DIR/ps5_gpu_gov"
FAN_GOV="$DIR/ps5_fan_gov"
CPU_CTL="$ROOT/ps5_cpu"
GPU_CTL="$ROOT/ps5_gpu"
BOOST_CTL="$ROOT/ps5_boost"
SMN_CFG="/sys/bus/pci/devices/0000:00:00.0/config"
PS5GOV_CONFIG="${PS5GOV_CONFIG:-/etc/ps5-linux-power/ps5gov.conf}"
PERFORMANCE_STATE="${PS5GOV_PERFORMANCE_STATE:-/run/ps5-power.performance}"
PRINT_CONFIG=0

ENV_PROFILE_SET=0; [ -z "${PROFILE+x}" ] || { ENV_PROFILE_SET=1; ENV_PROFILE_VALUE=$PROFILE; }
ENV_FAN_ENABLED_SET=0; [ -z "${FAN_ENABLED+x}" ] || { ENV_FAN_ENABLED_SET=1; ENV_FAN_ENABLED_VALUE=$FAN_ENABLED; }
ENV_FAN_CURVE_SET=0; [ -z "${FAN_CURVE+x}" ] || { ENV_FAN_CURVE_SET=1; ENV_FAN_CURVE_VALUE=$FAN_CURVE; }
ENV_FAN_HYSTERESIS_SET=0; [ -z "${FAN_HYSTERESIS+x}" ] || { ENV_FAN_HYSTERESIS_SET=1; ENV_FAN_HYSTERESIS_VALUE=$FAN_HYSTERESIS; }
ENV_FAN_ARGS_SET=0; [ -z "${FAN_ARGS+x}" ] || { ENV_FAN_ARGS_SET=1; ENV_FAN_ARGS_VALUE=$FAN_ARGS; }
ENV_CPU_ARGS_SET=0; [ -z "${CPU_ARGS+x}" ] || { ENV_CPU_ARGS_SET=1; ENV_CPU_ARGS_VALUE=$CPU_ARGS; }
ENV_GPU_ARGS_SET=0; [ -z "${GPU_ARGS+x}" ] || { ENV_GPU_ARGS_SET=1; ENV_GPU_ARGS_VALUE=$GPU_ARGS; }

case "${1:-}" in
--print-config)
	PRINT_CONFIG=1
	shift
	;;
esac

die() { echo "run-governors: $*" >&2; exit 1; }
log() { echo "run-governors: $*"; }

reset_runtime_config() {
	unset PROFILE FAN_ENABLED FAN_CURVE FAN_HYSTERESIS FAN_ARGS CPU_ARGS GPU_ARGS
	[ "$ENV_PROFILE_SET" -eq 0 ] || PROFILE=$ENV_PROFILE_VALUE
	[ "$ENV_FAN_ENABLED_SET" -eq 0 ] || FAN_ENABLED=$ENV_FAN_ENABLED_VALUE
	[ "$ENV_FAN_CURVE_SET" -eq 0 ] || FAN_CURVE=$ENV_FAN_CURVE_VALUE
	[ "$ENV_FAN_HYSTERESIS_SET" -eq 0 ] || FAN_HYSTERESIS=$ENV_FAN_HYSTERESIS_VALUE
	[ "$ENV_FAN_ARGS_SET" -eq 0 ] || FAN_ARGS=$ENV_FAN_ARGS_VALUE
	[ "$ENV_CPU_ARGS_SET" -eq 0 ] || CPU_ARGS=$ENV_CPU_ARGS_VALUE
	[ "$ENV_GPU_ARGS_SET" -eq 0 ] || GPU_ARGS=$ENV_GPU_ARGS_VALUE
}

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
		FAN_ENABLED) [ -z "${FAN_ENABLED+x}" ] && FAN_ENABLED=$value ;;
		FAN_CURVE) [ -z "${FAN_CURVE+x}" ] && FAN_CURVE=$value ;;
		FAN_HYSTERESIS) [ -z "${FAN_HYSTERESIS+x}" ] && FAN_HYSTERESIS=$value ;;
		FAN_ARGS) [ -z "${FAN_ARGS+x}" ] && FAN_ARGS=$value ;;
		CPU_ARGS) [ -z "${CPU_ARGS+x}" ] && CPU_ARGS=$value ;;
		GPU_ARGS) [ -z "${GPU_ARGS+x}" ] && GPU_ARGS=$value ;;
		esac
	done < "$PS5GOV_CONFIG"
}

load_runtime_config() {
	reset_runtime_config
	load_config
	apply_performance_override
	apply_profile
	apply_fan_profile
}

performance_override_enabled() {
	[ -r "$PERFORMANCE_STATE" ] || return 1
	case "$(cat "$PERFORMANCE_STATE" 2>/dev/null)" in
	1|on|true|performance) return 0 ;;
	*) return 1 ;;
	esac
}

apply_performance_override() {
	PERFORMANCE_MODE=0
	if performance_override_enabled; then
		PERFORMANCE_MODE=1
		PROFILE=performance
		unset CPU_ARGS GPU_ARGS
	fi
}

apply_profile() {
	PROFILE="${PROFILE:-performance}"
	case "$PROFILE" in
	auto)
		: "${CPU_ARGS:=-i 400 -d 4 -H 40 -M 20 -L 8}"
		: "${GPU_ARGS:=-P auto}"
		;;
	powersave)
		: "${CPU_ARGS:=-i 800 -d 8 -H 65 -M 35 -L 12}"
		: "${GPU_ARGS:=-P quiet -n 400 -x 1500}"
		;;
	quiet)
		: "${CPU_ARGS:=-i 600 -d 6 -H 55 -M 30 -L 12}"
		: "${GPU_ARGS:=-P quiet}"
		;;
	balanced)
		: "${CPU_ARGS:=-i 400 -d 4 -H 40 -M 20 -L 8}"
		: "${GPU_ARGS:=-P balanced}"
		;;
	performance)
		: "${CPU_ARGS:=-i 250 -d 6 -H 25 -M 12 -L 4}"
		: "${GPU_ARGS:=-P performance}"
		;;
	*)
		die "invalid PROFILE=$PROFILE (use auto, powersave, quiet, balanced, or performance)"
		;;
	esac
}

apply_fan_profile() {
	FAN_ENABLED="${FAN_ENABLED:-1}"
	case "$FAN_ENABLED" in
	0|1) ;;
	*)
		die "invalid FAN_ENABLED=$FAN_ENABLED (use 0 or 1)"
		;;
	esac
	: "${FAN_ARGS:=-i 3000 -H 55 -L 45 -s auto}"
	if [ -n "${FAN_CURVE:-}" ]; then
		FAN_ARGS="$FAN_ARGS -c $FAN_CURVE"
	fi
	if [ -n "${FAN_HYSTERESIS:-}" ]; then
		FAN_ARGS="$FAN_ARGS -y $FAN_HYSTERESIS"
	fi
}

restore_defaults() {
	[ -x "$FAN_GOV" ] && "$FAN_GOV" -n -f 0 >/dev/null 2>&1 || true
	[ -x "$BOOST_CTL" ] && "$BOOST_CTL" off >/dev/null 2>&1
	[ -x "$CPU_CTL" ] && "$CPU_CTL" set 0xff 0 >/dev/null 2>&1
	[ -x "$GPU_CTL" ] && "$GPU_CTL" reset >/dev/null 2>&1
}

load_runtime_config

if [ "$PRINT_CONFIG" -eq 1 ]; then
	printf 'PROFILE=%s\nPERFORMANCE_MODE=%s\nCPU_ARGS=%s\nGPU_ARGS=%s\nFAN_ENABLED=%s\nFAN_CURVE=%s\nFAN_HYSTERESIS=%s\nFAN_ARGS=%s\n' \
		"$PROFILE" "$PERFORMANCE_MODE" "$CPU_ARGS" "$GPU_ARGS" "$FAN_ENABLED" "${FAN_CURVE:-}" "${FAN_HYSTERESIS:-}" "$FAN_ARGS"
	exit 0
fi

[ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)"
[ -x "$CPU_GOV" ] || die "missing $CPU_GOV (run 'make' first)"
[ -x "$GPU_GOV" ] || die "missing $GPU_GOV (run 'make' first)"
[ "$FAN_ENABLED" -ne 1 ] || [ -x "$FAN_GOV" ] || die "missing $FAN_GOV (run 'make' first)"
[ -x "$CPU_CTL" ] || die "missing $CPU_CTL (run 'make' first)"
[ -x "$GPU_CTL" ] || die "missing $GPU_CTL (run 'make' first)"
[ -x "$BOOST_CTL" ] || die "missing $BOOST_CTL (run 'make' first)"
[ -e /dev/mp1 ] || die "missing /dev/mp1 (required for boost control)"
[ -e "$SMN_CFG" ] || die "missing $SMN_CFG (required for mailbox access)"

check_runtime_config() {
	[ "$FAN_ENABLED" -ne 1 ] || [ -x "$FAN_GOV" ] || die "missing $FAN_GOV (run 'make' first)"
	if [ "$FAN_ENABLED" -eq 1 ]; then
		ps5fan_state=$(systemctl is-active ps5fan.service 2>/dev/null || true)
		if [ "$ps5fan_state" = "active" ]; then
			log "warning: ps5fan.service is active and fan policy is also enabled in ps5gov; dual fan control possible."
		fi
	fi
}

CPU_PID=""
GPU_PID=""
FAN_PID=""

stop_governors() {
	[ -n "$CPU_PID" ] && kill "$CPU_PID" 2>/dev/null
	[ -n "$GPU_PID" ] && kill "$GPU_PID" 2>/dev/null
	[ -n "$FAN_PID" ] && kill "$FAN_PID" 2>/dev/null
	wait 2>/dev/null
	CPU_PID=""
	GPU_PID=""
	FAN_PID=""
}

start_governors() {
	check_runtime_config
	if [ "$FAN_ENABLED" -eq 1 ]; then
		"$FAN_GOV" $FAN_ARGS &
		FAN_PID=$!
	fi
	"$CPU_GOV" $CPU_ARGS & CPU_PID=$!
	sleep 1
	"$GPU_GOV" $GPU_ARGS & GPU_PID=$!
	log "started profile=$PROFILE performance_mode=$PERFORMANCE_MODE fan_enabled=$FAN_ENABLED fan_pid=$FAN_PID fan_args=\"$FAN_ARGS\" cpu_pid=$CPU_PID cpu_args=\"$CPU_ARGS\" gpu_pid=$GPU_PID gpu_args=\"$GPU_ARGS\""
}

cleanup() {
	log "stopping..."
	stop_governors
	restore_defaults
	log "stopped profile=$PROFILE restored=1"
	exit 0
}

reload_requested=0
request_reload() {
	reload_requested=1
	log "reload requested"
	stop_governors
}

trap cleanup INT TERM
trap request_reload HUP

start_governors

while :; do
	wait -n 2>/dev/null || wait
	if [ "$reload_requested" -eq 1 ]; then
		reload_requested=0
		load_runtime_config
		log "reloading profile=$PROFILE"
		start_governors
		continue
	fi
	cleanup
done
