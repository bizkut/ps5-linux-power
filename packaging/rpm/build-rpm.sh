#!/bin/sh
set -eu

VERSION="${VERSION:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-$ROOT/dist}"
RPMTOP="$OUT/rpmbuild"

rm -rf "$RPMTOP"
mkdir -p "$RPMTOP/BUILD" "$RPMTOP/RPMS" "$RPMTOP/SOURCES" "$RPMTOP/SPECS" "$RPMTOP/SRPMS" "$OUT"

tar --exclude-vcs --exclude='*/dist' --exclude='*.o' --exclude='*.ko' \
	--exclude='*.mod' --exclude='*.mod.c' --exclude='*.cmd' \
	--exclude='*/ps5_boost' --exclude='*/ps5_cpu' \
	--exclude='*/ps5_df' --exclude='*/ps5_gpu' \
	--exclude='*/governors/ps5_cpu_gov' \
	--exclude='*/governors/ps5_fan_gov' \
	--exclude='*/governors/ps5_gpu_gov' \
	--exclude='*/dkms/ps5-icc-fan/ps5_fanctl' \
	--exclude='*/dkms/ps5-smu/ps5_smuctl' \
	-C "$ROOT/.." -czf "$RPMTOP/SOURCES/ps5-linux-power-$VERSION.tar.gz" \
	--transform "s#$(basename "$ROOT")#ps5-linux-power-$VERSION#" "$(basename "$ROOT")"

sed "s/^Version:.*/Version: $VERSION/" "$ROOT/packaging/rpm/ps5-linux-power.spec" > "$RPMTOP/SPECS/ps5-linux-power.spec"
sed "s/^Version:.*/Version: $VERSION/" "$ROOT/packaging/rpm/ps5-linux-power-dkms.spec" > "$RPMTOP/SPECS/ps5-linux-power-dkms.spec"

rpmbuild --define "_topdir $RPMTOP" -ba "$RPMTOP/SPECS/ps5-linux-power.spec"
rpmbuild --define "_topdir $RPMTOP" -ba "$RPMTOP/SPECS/ps5-linux-power-dkms.spec"

find "$RPMTOP/RPMS" -type f -name '*.rpm' -exec cp {} "$OUT/" \;
