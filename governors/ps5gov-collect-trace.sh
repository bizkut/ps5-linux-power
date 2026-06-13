#!/bin/sh
# Collect a persistent ps5gov trace bundle that users can send for tuning.
set -u

DURATION=1800
INTERVAL=1
NOTE="${PS5GOV_TRACE_NOTE:-user-trace}"
OUT_DIR="${PS5GOV_TRACE_DIR:-$HOME/ps5gov-traces}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TRACE="$DIR/ps5gov-trace.sh"

usage() {
	cat <<'EOF'
usage: ps5gov-collect-trace.sh [-d seconds] [-i seconds] [-o output_dir] [-n note]

Run this while the game or workload is active. It writes a CSV trace plus a
small system/config snapshot, then packs them into a .tar.gz bundle to send for
tuning.
EOF
}

is_uint() {
	case "$1" in
	""|*[!0-9]*) return 1 ;;
	*) return 0 ;;
	esac
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
[ -x "$TRACE" ] || {
	echo "missing $TRACE" >&2
	exit 1
}

STAMP=$(date +%Y%m%d-%H%M%S)
SAFE_NOTE=$(printf '%s' "$NOTE" | tr -c 'A-Za-z0-9._-' '_' | sed 's/^_*//; s/_*$//')
[ -n "$SAFE_NOTE" ] || SAFE_NOTE=user-trace
RUN_DIR="$OUT_DIR/$STAMP-$SAFE_NOTE"
mkdir -p "$RUN_DIR" || exit 1

echo "collecting trace for ${DURATION}s into $RUN_DIR" >&2
CSV=$("$TRACE" -d "$DURATION" -i "$INTERVAL" -o "$RUN_DIR" -n "$NOTE")

{
	printf 'note=%s\n' "$NOTE"
	printf 'duration=%s\n' "$DURATION"
	printf 'interval=%s\n' "$INTERVAL"
	printf 'trace_csv=%s\n' "$CSV"
	printf 'date=%s\n' "$(date -Is)"
	printf 'kernel=%s\n' "$(uname -a)"
	printf 'user=%s\n' "$(id)"
} > "$RUN_DIR/metadata.txt"

ps5govctl config > "$RUN_DIR/ps5gov-config.txt" 2>&1 || true
ps5govctl sensors > "$RUN_DIR/ps5gov-sensors-after.txt" 2>&1 || true
systemctl status ps5gov.service > "$RUN_DIR/ps5gov-service-status.txt" 2>&1 || true

for file in \
	/etc/ps5-linux-power/ps5gov.conf \
	/run/ps5-power.cpu \
	/run/ps5-power.gpu \
	/run/ps5-power.fan \
	/run/ps5-power.boost
do
	if [ -r "$file" ]; then
		cp "$file" "$RUN_DIR/$(printf '%s' "$file" | tr '/' '_')"
	fi
done

BUNDLE="$RUN_DIR.tar.gz"
tar -C "$OUT_DIR" -czf "$BUNDLE" "$(basename "$RUN_DIR")"
echo "$BUNDLE"
