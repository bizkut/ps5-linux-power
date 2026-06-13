#!/bin/sh
set -eu

VERSION="${VERSION:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-$ROOT/dist}"
WORK="$OUT/arch-work"

rm -rf "$WORK"
mkdir -p "$WORK/src" "$OUT"

tar --exclude-vcs --exclude='*/dist' --exclude='*.o' --exclude='*.ko' \
	--exclude='*.mod' --exclude='*.mod.c' --exclude='*.cmd' \
	--exclude='*/ps5_boost' --exclude='*/ps5_cpu' \
	--exclude='*/ps5_df' --exclude='*/ps5_gpu' \
	--exclude='*/governors/ps5_cpu_gov' \
	--exclude='*/governors/ps5_fan_gov' \
	--exclude='*/governors/ps5_gpu_gov' \
	--exclude='*/dkms/ps5-icc-fan/ps5_fanctl' \
	--exclude='*/dkms/ps5-smu/ps5_smuctl' \
	-C "$ROOT/.." -czf "$WORK/ps5-linux-power-$VERSION.tar.gz" \
	--transform "s#$(basename "$ROOT")#ps5-linux-power-$VERSION#" "$(basename "$ROOT")"

for pkg in ps5-linux-power ps5-linux-power-dkms; do
	mkdir -p "$WORK/$pkg"
	cp "$ROOT/packaging/arch/$pkg/PKGBUILD" "$WORK/$pkg/"
	sed -i "s/^pkgver=.*/pkgver=$VERSION/" "$WORK/$pkg/PKGBUILD"
	if [ -f "$ROOT/packaging/arch/$pkg/$pkg.install" ]; then
		cp "$ROOT/packaging/arch/$pkg/$pkg.install" "$WORK/$pkg/"
		printf '\ninstall=%s.install\n' "$pkg" >> "$WORK/$pkg/PKGBUILD"
	fi
	cp "$WORK/ps5-linux-power-$VERSION.tar.gz" "$WORK/$pkg/"
	(cd "$WORK/$pkg" && makepkg -f --noconfirm)
	cp "$WORK/$pkg"/*.pkg.tar.* "$OUT/"
done
