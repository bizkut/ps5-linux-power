#!/bin/sh
# Validate the optional ps5-smu DKMS module on target PS5 Linux.
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
WRITE_TESTS=0
FAIL=0

usage() {
	echo "usage: $0 [--write-tests]" >&2
	exit 2
}

ok() { printf 'ok: %s\n' "$*"; }
warn() { printf 'warn: %s\n' "$*" >&2; }
fail() { printf 'fail: %s\n' "$*" >&2; FAIL=1; }

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

grep -q ' amd_smn_read$' /proc/kallsyms 2>/dev/null && ok "kernel exports amd_smn_read" || fail "kernel does not export amd_smn_read"
grep -q ' amd_smn_write$' /proc/kallsyms 2>/dev/null && ok "kernel exports amd_smn_write" || fail "kernel does not export amd_smn_write"

if command -v dkms >/dev/null 2>&1; then
	dkms status ps5-smu 2>/dev/null | grep -q installed && ok "DKMS installed" || warn "DKMS module not installed"
else
	warn "dkms command not found"
fi

if lsmod | awk '{print $1}' | grep -q '^ps5_smu$'; then
	ok "ps5_smu loaded"
else
	run "modprobe ps5_smu" modprobe ps5_smu
fi

[ -c /dev/ps5-smu ] && ok "/dev/ps5-smu exists" || fail "/dev/ps5-smu missing"

run "build ps5_smuctl" make -C "$DIR" userspace

if [ -x "$DIR/ps5_smuctl" ] && [ -c /dev/ps5-smu ]; then
	run "cpu-get core 0" "$DIR/ps5_smuctl" cpu-get 0
	run "gpu-get" "$DIR/ps5_smuctl" gpu-get
	run "voltage" "$DIR/ps5_smuctl" voltage

	if [ "$WRITE_TESTS" -eq 1 ]; then
		run "cpu-set all P0" "$DIR/ps5_smuctl" cpu-set 0xff 0
		run "gpu-set 2000" "$DIR/ps5_smuctl" gpu-set 2000
	else
		warn "write tests skipped; rerun with --write-tests after read-only checks pass"
	fi
fi

if [ "$FAIL" -ne 0 ]; then
	exit 1
fi
ok "ps5-smu validation complete"
