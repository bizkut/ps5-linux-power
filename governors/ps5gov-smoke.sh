#!/bin/sh
# Target-side smoke checks for ps5gov build, config, sensors, and service flow.
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
CONFIG="${PS5GOV_CONFIG:-$DIR/ps5gov.conf}"
SERVICE="${PS5GOV_SERVICE:-ps5gov.service}"
RUNNER="${PS5GOV_RUNNER:-$DIR/run-governors.sh}"
CTL="${PS5GOV_CTL:-$DIR/ps5govctl}"
TOOLS_DIR="${PS5GOV_TOOLS_DIR:-$ROOT}"
SMN_CFG="/sys/bus/pci/devices/0000:00:00.0/config"
SERVICE_MODE=0
FAIL=0

usage() {
	echo "usage: $0 [--service]" >&2
	exit 2
}

ok() {
	printf 'ok: %s\n' "$*"
}

warn() {
	printf 'warn: %s\n' "$*" >&2
}

fail() {
	printf 'fail: %s\n' "$*" >&2
	FAIL=1
}

run_check() {
	desc="$1"
	shift
	if "$@" >/dev/null 2>&1; then
		ok "$desc"
	else
		fail "$desc"
	fi
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--service) SERVICE_MODE=1 ;;
	-h|--help) usage ;;
	*) usage ;;
	esac
	shift
done

run_check "ps5govctl syntax" sh -n "$CTL"
run_check "run-governors syntax" sh -n "$RUNNER"
run_check "ps5gov-trace syntax" sh -n "$DIR/ps5gov-trace.sh"
run_check "ps5gov-collect-trace syntax" sh -n "$DIR/ps5gov-collect-trace.sh"
run_check "ps5gov-fan-validate syntax" sh -n "$DIR/ps5gov-fan-validate.sh"

for exe in "$DIR/ps5_cpu_gov" "$DIR/ps5_gpu_gov" "$DIR/ps5_fan_gov" \
	   "$ROOT/ps5_cpu" "$ROOT/ps5_gpu" "$ROOT/ps5_boost"; do
	if [ -x "$exe" ]; then
		ok "executable $(basename "$exe")"
	else
		fail "missing executable $exe (run make first)"
	fi
done

if [ -e /dev/mp1 ]; then
	ok "/dev/mp1 present"
else
	warn "/dev/mp1 missing; governor startup will fail off target"
fi

if [ -e "$SMN_CFG" ]; then
	ok "$SMN_CFG present"
else
	warn "$SMN_CFG missing; mailbox access will fail off target"
fi

if PS5GOV_CONFIG="$CONFIG" "$RUNNER" --print-config >/tmp/ps5gov-smoke.config 2>/tmp/ps5gov-smoke.err; then
	ok "config renders"
	grep -q '^PROFILE=' /tmp/ps5gov-smoke.config || fail "rendered config missing PROFILE"
	grep -q '^CPU_ARGS=' /tmp/ps5gov-smoke.config || fail "rendered config missing CPU_ARGS"
	grep -q '^GPU_ARGS=' /tmp/ps5gov-smoke.config || fail "rendered config missing GPU_ARGS"
	grep -q '^FAN_ENABLED=' /tmp/ps5gov-smoke.config || fail "rendered config missing FAN_ENABLED"
else
	fail "config renders"
fi

PS5GOV_CONFIG="$CONFIG" PS5GOV_RUNNER="$RUNNER" PS5GOV_TOOLS_DIR="$TOOLS_DIR" \
	"$CTL" config >/dev/null 2>&1 && ok "ps5govctl config" || fail "ps5govctl config"

PS5GOV_CONFIG="$CONFIG" PS5GOV_RUNNER="$RUNNER" PS5GOV_TOOLS_DIR="$TOOLS_DIR" \
	"$CTL" sensors >/dev/null 2>&1 && ok "ps5govctl sensors" || warn "ps5govctl sensors returned non-zero"

if [ "$SERVICE_MODE" -eq 1 ]; then
	if [ "$(id -u)" -ne 0 ]; then
		fail "--service requires root"
	elif ! command -v systemctl >/dev/null 2>&1; then
		fail "systemctl not found"
	else
		systemctl daemon-reload >/dev/null 2>&1 || fail "systemctl daemon-reload"
		systemctl restart "$SERVICE" >/dev/null 2>&1 || fail "restart $SERVICE"
		sleep 2
		systemctl is-active --quiet "$SERVICE" && ok "$SERVICE active" || fail "$SERVICE active"
		PS5GOV_SERVICE="$SERVICE" "$CTL" reload >/dev/null 2>&1 && ok "reload $SERVICE" || fail "reload $SERVICE"
		sleep 2
		systemctl is-active --quiet "$SERVICE" && ok "$SERVICE active after reload" || fail "$SERVICE active after reload"
		PS5GOV_CONFIG="$CONFIG" PS5GOV_RUNNER="$RUNNER" PS5GOV_TOOLS_DIR="$TOOLS_DIR" \
			"$CTL" sensors >/dev/null 2>&1 && ok "service sensors" || fail "service sensors"
		PS5GOV_CONFIG="$CONFIG" PS5GOV_RUNNER="$RUNNER" PS5GOV_TOOLS_DIR="$TOOLS_DIR" \
			"$CTL" restore >/dev/null 2>&1 && ok "restore defaults" || fail "restore defaults"
		if systemctl is-active --quiet "$SERVICE"; then
			fail "$SERVICE stopped after restore"
		else
			ok "$SERVICE stopped after restore"
		fi
	fi
fi

rm -f /tmp/ps5gov-smoke.config /tmp/ps5gov-smoke.err

if [ "$FAIL" -ne 0 ]; then
	exit 1
fi
ok "smoke checks complete"
