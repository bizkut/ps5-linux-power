Name: ps5-linux-power-dkms
Version: 0.1.0
Release: 1%{?dist}
Summary: DKMS kernel transports for ps5-linux-power
License: GPL-2.0-only
URL: https://github.com/bizkut/ps5-linux-power
Source0: ps5-linux-power-%{version}.tar.gz
BuildArch: noarch
Requires: dkms
Requires: gcc
Requires: make

%description
Optional PS5 SMU and ICC fan DKMS modules. The running custom kernel must
provide matching headers at /lib/modules/$(uname -r)/build.

%prep
%autosetup -n ps5-linux-power-%{version}

%build

%install
install -d %{buildroot}/usr/src/ps5-smu-%{version}
install -m 0644 dkms/ps5-smu/Makefile dkms/ps5-smu/dkms.conf \
	dkms/ps5-smu/ps5_smu.c dkms/ps5-smu/ps5_smu.h \
	%{buildroot}/usr/src/ps5-smu-%{version}/
sed -i 's/PACKAGE_VERSION="0.1.0"/PACKAGE_VERSION="%{version}"/' \
	%{buildroot}/usr/src/ps5-smu-%{version}/dkms.conf

install -d %{buildroot}/usr/src/ps5-icc-fan-%{version}
install -m 0644 dkms/ps5-icc-fan/Makefile dkms/ps5-icc-fan/dkms.conf \
	dkms/ps5-icc-fan/ps5_icc_fan.c dkms/ps5-icc-fan/ps5_icc_fan.h \
	%{buildroot}/usr/src/ps5-icc-fan-%{version}/
sed -i 's/PACKAGE_VERSION="0.1.0"/PACKAGE_VERSION="%{version}"/' \
	%{buildroot}/usr/src/ps5-icc-fan-%{version}/dkms.conf

install -d %{buildroot}/etc/modules-load.d
printf 'ps5_smu\n' > %{buildroot}/etc/modules-load.d/ps5-smu.conf
printf 'ps5_icc_fan\n' > %{buildroot}/etc/modules-load.d/ps5-icc-fan.conf

%post
for module in ps5-smu ps5-icc-fan; do
	for tree in /var/lib/dkms/$module/*; do
		[ -d "$tree" ] || continue
		oldver="${tree##*/}"
		[ "$oldver" = "%{version}" ] && continue
		dkms remove -m "$module" -v "$oldver" --all || true
	done
done

KBUILD="/lib/modules/$(uname -r)/build"
if [ ! -e "$KBUILD" ]; then
	echo "ps5-linux-power-dkms: missing $KBUILD" >&2
	echo "Install matching headers for the running PS5 custom kernel." >&2
	exit 1
fi
dkms add -m ps5-smu -v "%{version}" || true
dkms build -m ps5-smu -v "%{version}"
dkms install -m ps5-smu -v "%{version}"
dkms add -m ps5-icc-fan -v "%{version}" || true
dkms build -m ps5-icc-fan -v "%{version}"
dkms install -m ps5-icc-fan -v "%{version}"
depmod -a || true
modprobe ps5_smu || true
modprobe ps5_icc_fan || true

%preun
if [ "$1" = 0 ] || [ "$1" = 1 ]; then
	dkms remove -m ps5-smu -v "%{version}" --all || true
	dkms remove -m ps5-icc-fan -v "%{version}" --all || true
fi

%files
/usr/src/ps5-smu-%{version}
/usr/src/ps5-icc-fan-%{version}
/etc/modules-load.d/ps5-smu.conf
/etc/modules-load.d/ps5-icc-fan.conf
