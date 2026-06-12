#!/bin/sh
# Validate the optional ps5-icc-fan DKMS module on target PS5 Linux.
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
WRITE_TESTS=0
FAIL=0

usage() {
	echo "usage: $0 [--write-tests]" >&2
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

run() {
	desc=$1
	shift
	if "$@" >/dev/null 2>&1; then
		ok "$desc"
	else
		fail "$desc"
	fi
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--write-tests) WRITE_TESTS=1 ;;
	-h|--help) usage ;;
	*) usage ;;
	esac
	shift
done

if grep -q ' icc_query$' /proc/kallsyms 2>/dev/null; then
	ok "kernel exports icc_query"
else
	fail "kernel does not export icc_query"
fi

if command -v dkms >/dev/null 2>&1; then
	dkms status ps5-icc-fan 2>/dev/null | grep -q installed && ok "DKMS installed" || warn "DKMS module not installed"
else
	warn "dkms command not found"
fi

if lsmod | awk '{print $1}' | grep -q '^ps5_icc_fan$'; then
	ok "ps5_icc_fan loaded"
else
	run "modprobe ps5_icc_fan" modprobe ps5_icc_fan
fi

[ -c /dev/ps5-fan ] && ok "/dev/ps5-fan exists" || fail "/dev/ps5-fan missing"

run "build ps5_fanctl" make -C "$DIR" userspace

if [ -x "$DIR/ps5_fanctl" ] && [ -c /dev/ps5-fan ]; then
	run "zone 0 temperature" "$DIR/ps5_fanctl" temp 0
	run "zone 0 mode-get" "$DIR/ps5_fanctl" mode-get 0
	run "zone 0 servo-get" "$DIR/ps5_fanctl" servo 0

	if [ "$WRITE_TESTS" -eq 1 ]; then
		run "set servo pattern 0" "$DIR/ps5_fanctl" pattern 0
		run "set MAINSOC target temp 58C" "$DIR/ps5_fanctl" target-temp 0 58
	else
		warn "write tests skipped; rerun with --write-tests after read-only checks pass"
	fi
fi

if [ "$FAIL" -ne 0 ]; then
	exit 1
fi
ok "ps5-icc-fan validation complete"
