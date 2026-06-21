#!/bin/sh
set -eu

VERSION="${VERSION:-0.1.0}"
ARCH="${ARCH:-$(dpkg --print-architecture 2>/dev/null || echo amd64)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-$ROOT/dist}"
WORK="$OUT/deb-work"

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"

pkg_power="$WORK/ps5-linux-power_${VERSION}_${ARCH}"
pkg_dkms="$WORK/ps5-linux-power-dkms_${VERSION}_all"

install_power() {
	make -C "$ROOT" clean
	make -C "$ROOT"
	make -C "$ROOT" install-systemd DESTDIR="$pkg_power" PREFIX=/usr/local SYSCONFDIR=/etc
	mkdir -p "$pkg_power/usr/share/doc/ps5-linux-power"
	cp "$ROOT/README.md" "$pkg_power/usr/share/doc/ps5-linux-power/README.md"
	cp -r "$ROOT/docs" "$pkg_power/usr/share/doc/ps5-linux-power/"
}

control_power() {
	mkdir -p "$pkg_power/DEBIAN"
	cat > "$pkg_power/DEBIAN/control" <<EOF
Package: ps5-linux-power
Version: $VERSION
Section: admin
Priority: optional
Architecture: $ARCH
Depends: systemd, libc6
Maintainer: PS5 Linux contributors
Description: PS5 Linux userspace power, fan, and governor tools
 CPU/GPU governors, fan policy, trace collection, and manual recovery tools for
 PS5 Linux power control.
EOF
	cat > "$pkg_power/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
systemctl daemon-reload >/dev/null 2>&1 || true
if [ -z "${2:-}" ]; then
	systemctl preset ps5gov.service >/dev/null 2>&1 || true
fi
if systemctl is-enabled --quiet ps5gov.service 2>/dev/null; then
	systemctl restart ps5gov.service >/dev/null 2>&1 || true
fi
exit 0
EOF
	chmod 0755 "$pkg_power/DEBIAN/postinst"
}

install_dkms() {
	mkdir -p "$pkg_dkms/usr/src/ps5-smu-$VERSION" "$pkg_dkms/usr/src/ps5-icc-fan-$VERSION"
	cp "$ROOT/dkms/ps5-smu/Makefile" "$ROOT/dkms/ps5-smu/dkms.conf" \
	   "$ROOT/dkms/ps5-smu/ps5_smu.c" "$ROOT/dkms/ps5-smu/ps5_smu.h" \
	   "$pkg_dkms/usr/src/ps5-smu-$VERSION/"
	cp "$ROOT/dkms/ps5-icc-fan/Makefile" "$ROOT/dkms/ps5-icc-fan/dkms.conf" \
	   "$ROOT/dkms/ps5-icc-fan/ps5_icc_fan.c" "$ROOT/dkms/ps5-icc-fan/ps5_icc_fan.h" \
	   "$pkg_dkms/usr/src/ps5-icc-fan-$VERSION/"
	sed -i "s/PACKAGE_VERSION=\"0.1.0\"/PACKAGE_VERSION=\"$VERSION\"/" \
		"$pkg_dkms/usr/src/ps5-smu-$VERSION/dkms.conf" \
		"$pkg_dkms/usr/src/ps5-icc-fan-$VERSION/dkms.conf"
	mkdir -p "$pkg_dkms/etc/modules-load.d"
	printf 'ps5_smu\n' > "$pkg_dkms/etc/modules-load.d/ps5-smu.conf"
	printf 'ps5_icc_fan\n' > "$pkg_dkms/etc/modules-load.d/ps5-icc-fan.conf"
}

control_dkms() {
	mkdir -p "$pkg_dkms/DEBIAN"
	cat > "$pkg_dkms/DEBIAN/control" <<EOF
Package: ps5-linux-power-dkms
Version: $VERSION
Section: kernel
Priority: optional
Architecture: all
Depends: dkms, make, gcc
Maintainer: PS5 Linux contributors
Description: DKMS kernel transports for ps5-linux-power
 Optional PS5 SMU and ICC fan modules. The running custom kernel must provide
 matching headers at /lib/modules/\$(uname -r)/build.
EOF
	cat > "$pkg_dkms/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e

cleanup_old_dkms() {
	module=\$1
	for tree in /var/lib/dkms/\$module/*; do
		[ -d "\$tree" ] || continue
		oldver=\${tree##*/}
		[ "\$oldver" = "$VERSION" ] && continue
		dkms remove -m "\$module" -v "\$oldver" --all || true
	done
}

cleanup_old_dkms ps5-smu
cleanup_old_dkms ps5-icc-fan

KBUILD="/lib/modules/\$(uname -r)/build"
if [ ! -e "\$KBUILD" ]; then
	echo "ps5-linux-power-dkms: missing \$KBUILD" >&2
	echo "Install matching headers for the running PS5 custom kernel." >&2
	exit 1
fi
dkms add -m ps5-smu -v "$VERSION" || true
dkms build -m ps5-smu -v "$VERSION"
dkms install -m ps5-smu -v "$VERSION"
dkms add -m ps5-icc-fan -v "$VERSION" || true
dkms build -m ps5-icc-fan -v "$VERSION"
dkms install -m ps5-icc-fan -v "$VERSION"
depmod -a || true
modprobe ps5_smu || true
modprobe ps5_icc_fan || true
exit 0
EOF
	cat > "$pkg_dkms/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
case "\${1:-}" in
remove|upgrade)
	dkms remove -m ps5-smu -v "$VERSION" --all || true
	dkms remove -m ps5-icc-fan -v "$VERSION" --all || true
	;;
esac
exit 0
EOF
	chmod 0755 "$pkg_dkms/DEBIAN/postinst" "$pkg_dkms/DEBIAN/prerm"
}

install_power
control_power
install_dkms
control_dkms

dpkg-deb --root-owner-group --build "$pkg_power" "$OUT/ps5-linux-power_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$pkg_dkms" "$OUT/ps5-linux-power-dkms_${VERSION}_all.deb"
