#!/bin/sh
# Validate ps5gov fan behavior on target PS5 Linux.
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
TOOLS_DIR="${PS5GOV_TOOLS_DIR:-$ROOT}"
FANCTL="${PS5_FANCTL:-$TOOLS_DIR/dkms/ps5-icc-fan/ps5_fanctl}"
FANCTL_DIR="$(dirname "$FANCTL")"
FAN_GOV="${PS5_FAN_GOV:-$DIR/ps5_fan_gov}"
STATE="${PS5_FAN_STATE:-/run/ps5-power.fan}"
WRITE_TESTS=0
DEFAULT_PATTERN=0
COOL_PATTERN=1
TARGET_TEMP=58
SLEEP_SECS=5
FAIL=0

usage() {
	echo "usage: $0 [--write-tests] [--default-pattern n] [--cool-pattern n] [--target-temp c] [--sleep seconds]" >&2
	exit 2
}

help() {
	echo "usage: $0 [--write-tests] [--default-pattern n] [--cool-pattern n] [--target-temp c] [--sleep seconds]"
	exit 0
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

is_uint() {
	case "$1" in
	""|*[!0-9]*) return 1 ;;
	*) return 0 ;;
	esac
}

run_capture() {
	desc=$1
	shift
	printf '%s\n' "## $desc"
	if "$@"; then
		ok "$desc"
	else
		fail "$desc"
	fi
}

fanctl() {
	"$FANCTL" "$@"
}

require_uint() {
	name=$1
	value=$2
	limit=$3

	is_uint "$value" && [ "$value" -le "$limit" ] || {
		echo "invalid $name: $value" >&2
		exit 2
	}
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--write-tests) WRITE_TESTS=1 ;;
	--default-pattern)
		[ "$#" -ge 2 ] || usage
		DEFAULT_PATTERN=$2
		shift
		;;
	--cool-pattern)
		[ "$#" -ge 2 ] || usage
		COOL_PATTERN=$2
		shift
		;;
	--target-temp)
		[ "$#" -ge 2 ] || usage
		TARGET_TEMP=$2
		shift
		;;
	--sleep)
		[ "$#" -ge 2 ] || usage
		SLEEP_SECS=$2
		shift
		;;
	-h|--help) help ;;
	*) usage ;;
	esac
	shift
done

require_uint default-pattern "$DEFAULT_PATTERN" 255
require_uint cool-pattern "$COOL_PATTERN" 255
require_uint target-temp "$TARGET_TEMP" 110
require_uint sleep "$SLEEP_SECS" 3600

if [ ! -x "$FANCTL" ] && [ -f "$FANCTL_DIR/Makefile" ]; then
	make -C "$FANCTL_DIR" userspace >/dev/null 2>&1 || true
fi

[ -x "$FANCTL" ] || fail "missing ps5_fanctl at $FANCTL"
[ -x "$FAN_GOV" ] || fail "missing ps5_fan_gov at $FAN_GOV"
[ -c /dev/ps5-fan ] || fail "/dev/ps5-fan missing"

if [ "$FAIL" -ne 0 ]; then
	exit 1
fi

run_capture "zone 0 temperature before writes" fanctl temp 0
run_capture "zone 0 mode before writes" fanctl mode-get 0
run_capture "zone 0 servo before writes" fanctl servo 0

if [ "$WRITE_TESTS" -ne 1 ]; then
	warn "write tests skipped; rerun with --write-tests after read-only checks pass"
else
	run_capture "set default servo pattern $DEFAULT_PATTERN" fanctl pattern "$DEFAULT_PATTERN"
	sleep "$SLEEP_SECS"
	run_capture "temperature after default pattern" fanctl temp 0
	run_capture "servo after default pattern" fanctl servo 0

	run_capture "set cool servo pattern $COOL_PATTERN" fanctl pattern "$COOL_PATTERN"
	sleep "$SLEEP_SECS"
	run_capture "temperature after cool pattern" fanctl temp 0
	run_capture "servo after cool pattern" fanctl servo 0

	run_capture "set MAINSOC target temperature ${TARGET_TEMP}C" fanctl target-temp 0 "$TARGET_TEMP"
	sleep "$SLEEP_SECS"
	run_capture "servo after target temperature" fanctl servo 0

	if "$FAN_GOV" -n -f "$DEFAULT_PATTERN" >/dev/null 2>&1; then
		ok "restored default servo pattern $DEFAULT_PATTERN"
	else
		fail "restore default servo pattern $DEFAULT_PATTERN"
	fi
fi

if [ -r "$STATE" ]; then
	printf '%s\n' "## $STATE"
	sed 's/^/state_/' "$STATE"
else
	warn "$STATE not present; ps5_fan_gov may not be running"
fi

if [ "$FAIL" -ne 0 ]; then
	exit 1
fi

ok "fan validation complete"
