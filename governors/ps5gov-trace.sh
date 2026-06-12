#!/bin/sh
# Capture ps5gov CPU/GPU/fan state and temperatures for target-side tuning.
set -u

DURATION=300
INTERVAL=1
OUT_DIR=/tmp/ps5gov-traces
NOTE="${PS5GOV_TRACE_NOTE:-}"
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
TOOLS_DIR="${PS5GOV_TOOLS_DIR:-/usr/local/lib/ps5-linux-cpuclock}"
if [ -n "${PS5_FANCTL:-}" ]; then
	FANCTL=$PS5_FANCTL
elif [ -x "$ROOT/dkms/ps5-icc-fan/ps5_fanctl" ]; then
	FANCTL="$ROOT/dkms/ps5-icc-fan/ps5_fanctl"
else
	FANCTL="$TOOLS_DIR/dkms/ps5-icc-fan/ps5_fanctl"
fi
SERVICE="${PS5GOV_SERVICE:-ps5gov.service}"

usage() {
	cat <<'EOF'
usage: ps5gov-trace.sh [-d seconds] [-i seconds] [-o output_dir] [-n note]

Records CSV samples for fan/GPU governor tuning:
  time, hottest hwmon temperature/source, EMC zone temps when /dev/ps5-fan works,
  CPU/GPU/fan /run state, boost state, and ps5gov service state.
EOF
}

is_uint() {
	case "$1" in
	""|*[!0-9]*) return 1 ;;
	*) return 0 ;;
	esac
}

csv_escape() {
	printf '%s' "$1" | tr '\n,' ' _'
}

while getopts ":d:i:o:n:h" opt; do
	case "$opt" in
	d) DURATION=$OPTARG ;;
	i) INTERVAL=$OPTARG ;;
	o) OUT_DIR=$OPTARG ;;
	n) NOTE=$OPTARG ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done

is_uint "$DURATION" && [ "$DURATION" -gt 0 ] || {
	echo "invalid duration" >&2
	exit 2
}
is_uint "$INTERVAL" && [ "$INTERVAL" -gt 0 ] || {
	echo "invalid interval" >&2
	exit 2
}

mkdir -p "$OUT_DIR" || exit 1
OUT="$OUT_DIR/ps5gov-trace-$(date +%s).csv"

read_state_value() {
	file=$1
	key=$2

	[ -r "$file" ] || {
		printf '?'
		return
	}
	awk -F= -v k="$key" '$1 == k { print $2; found=1; exit } END { if (!found) print "?" }' "$file"
}

hottest_hwmon() {
	best=-1
	best_src=unknown

	for temp_input in /sys/class/hwmon/hwmon*/temp*_input; do
		[ -r "$temp_input" ] || continue
		raw=$(cat "$temp_input" 2>/dev/null || echo "")
		case "$raw" in
		""|*[!0-9]*) continue ;;
		esac
		temp_c=$((raw / 1000))
		if [ "$temp_c" -gt "$best" ]; then
			best=$temp_c
			hwmon_dir=${temp_input%/*}
			name=unknown
			[ -r "$hwmon_dir/name" ] && name=$(cat "$hwmon_dir/name")
			name=$(printf '%s' "$name" | tr ',' '_')
			best_src="$name:$temp_input"
		fi
	done

	if [ "$best" -lt 0 ]; then
		printf '?,?'
	else
		printf '%s,%s' "$best" "$best_src"
	fi
}

emc_temp() {
	zone=$1

	if [ -x "$FANCTL" ] && [ -c /dev/ps5-fan ]; then
		"$FANCTL" temp "$zone" 2>/dev/null | sed -n 's/.*temp=\([0-9.]*\)C.*/\1/p'
	else
		printf '?'
	fi
}

service_state() {
	if command -v systemctl >/dev/null 2>&1; then
		systemctl is-active "$SERVICE" 2>/dev/null || printf 'unknown'
	else
		printf 'unknown'
	fi
}

boost_state() {
	if [ -r /run/ps5-power.boost ]; then
		cat /run/ps5-power.boost
	else
		printf '?'
	fi
}

printf '%s\n' \
	'ts,note,service,hwmon_temp_c,hwmon_source,emc_zone0_c,emc_zone1_c,emc_zone2_c,emc_zone3_c,cpu_pstate,cpu_mhz,gpu_load,gpu_load_method,gpu_active_load_method,gpu_fdinfo_gfx_ns,gpu_current_mhz,gpu_target_mhz,gpu_desired_mhz,gpu_boost,gpu_thermal_cap,fan_pattern,fan_curve_stage,fan_target_temp_c,fan_reason,boost_state' > "$OUT"

end=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -le "$end" ]; do
	now=$(date +%s)
	hwmon=$(hottest_hwmon)
	emc0=$(emc_temp 0)
	emc1=$(emc_temp 1)
	emc2=$(emc_temp 2)
	emc3=$(emc_temp 3)
	cpu_pstate=$(read_state_value /run/ps5-power.cpu current_pstate)
	cpu_mhz=$(read_state_value /run/ps5-power.cpu current_mhz)
	gpu_load=$(read_state_value /run/ps5-power.gpu load)
	gpu_method=$(read_state_value /run/ps5-power.gpu load_method)
	gpu_active_method=$(read_state_value /run/ps5-power.gpu active_load_method)
	gpu_fdinfo_gfx_ns=$(read_state_value /run/ps5-power.gpu fdinfo_gfx_ns)
	gpu_cur=$(read_state_value /run/ps5-power.gpu current_mhz)
	gpu_target=$(read_state_value /run/ps5-power.gpu target_mhz)
	gpu_desired=$(read_state_value /run/ps5-power.gpu desired_mhz)
	gpu_boost=$(read_state_value /run/ps5-power.gpu boost)
	gpu_cap=$(read_state_value /run/ps5-power.gpu thermal_cap)
	fan_pattern=$(read_state_value /run/ps5-power.fan pattern)
	fan_stage=$(read_state_value /run/ps5-power.fan curve_stage)
	fan_target=$(read_state_value /run/ps5-power.fan target_temp_c)
	fan_reason=$(read_state_value /run/ps5-power.fan reason | tr ',' '_')

	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$now" "$(csv_escape "$NOTE")" "$(service_state)" "$hwmon" "$emc0" "$emc1" "$emc2" "$emc3" \
		"$cpu_pstate" "$cpu_mhz" "$gpu_load" "$gpu_method" "$gpu_active_method" \
		"$gpu_fdinfo_gfx_ns" "$gpu_cur" "$gpu_target" "$gpu_desired" \
		"$gpu_boost" "$gpu_cap" "$fan_pattern" "$fan_stage" "$fan_target" \
		"$fan_reason" "$(boost_state)" >> "$OUT"
	sleep "$INTERVAL"
done

echo "$OUT"
