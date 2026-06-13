Name: ps5-linux-power-kmod
Version: 0.1.0
Release: 1%{?dist}
Summary: Kernel modules for ps5-linux-power
License: GPL-2.0-only
URL: https://github.com/bizkut/ps5-linux-power
Source0: ps5-linux-power-%{version}.tar.gz
BuildRequires: gcc
BuildRequires: make
%global debug_package %{nil}

%description
PS5 SMU and ICC fan kernel modules built by akmods for the running PS5 kernel.
The running custom kernel must provide headers at /lib/modules/<kernel>/build.

%prep
%autosetup -n ps5-linux-power-%{version}

%build
KERNELS="%{?kernels}"
if [ -z "$KERNELS" ]; then
	KERNELS="$(uname -r)"
fi
for kver in $KERNELS; do
	kbuild="/lib/modules/$kver/build"
	if [ ! -e "$kbuild" ]; then
		echo "missing $kbuild" >&2
		exit 1
	fi
	cp -a dkms/ps5-smu "_build-ps5-smu-$kver"
	cp -a dkms/ps5-icc-fan "_build-ps5-icc-fan-$kver"
	make -C "_build-ps5-smu-$kver" KDIR="$kbuild"
	make -C "_build-ps5-icc-fan-$kver" KDIR="$kbuild"
done

%install
KERNELS="%{?kernels}"
if [ -z "$KERNELS" ]; then
	KERNELS="$(uname -r)"
fi
: > files.list
for kver in $KERNELS; do
	install -d "%{buildroot}/usr/lib/modules/$kver/extra"
	install -m 0644 "_build-ps5-smu-$kver/ps5_smu.ko" \
		"%{buildroot}/usr/lib/modules/$kver/extra/ps5_smu.ko"
	install -m 0644 "_build-ps5-icc-fan-$kver/ps5_icc_fan.ko" \
		"%{buildroot}/usr/lib/modules/$kver/extra/ps5_icc_fan.ko"
	printf '/usr/lib/modules/%s/extra/ps5_smu.ko\n' "$kver" >> files.list
	printf '/usr/lib/modules/%s/extra/ps5_icc_fan.ko\n' "$kver" >> files.list
done

%post
for moddir in /usr/lib/modules/*; do
	[ -e "$moddir/extra/ps5_smu.ko" ] || [ -e "$moddir/extra/ps5_icc_fan.ko" ] || continue
	depmod -a "$(basename "$moddir")" >/dev/null 2>&1 || true
done

%postun
for moddir in /usr/lib/modules/*; do
	depmod -a "$(basename "$moddir")" >/dev/null 2>&1 || true
done

%files -f files.list
