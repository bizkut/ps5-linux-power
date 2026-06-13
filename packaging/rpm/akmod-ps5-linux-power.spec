Name: akmod-ps5-linux-power
Version: 0.1.0
Release: 1%{?dist}
Summary: Akmods source package for ps5-linux-power kernel modules
License: GPL-2.0-only
URL: https://github.com/bizkut/ps5-linux-power
Source0: ps5-linux-power-%{version}.tar.gz
Source1: ps5-linux-power-kmod.spec
BuildArch: noarch
BuildRequires: rpm-build
Requires: akmods
Requires: gcc
Requires: make
Requires: rpm-build

%description
Akmods source package for the optional PS5 SMU and ICC fan kernel transports.
This is the Fedora/Bazzite-native alternative to ps5-linux-power-dkms.

%prep
%setup -q -c -T

%build

%install
install -d %{buildroot}/usr/src/akmods
rpmbuild --define "_sourcedir %{_sourcedir}" \
	--define "_srcrpmdir %{buildroot}/usr/src/akmods" \
	%{?dist:--define "dist %{dist}"} \
	-bs --nodeps %{SOURCE1}
latest="$(ls %{buildroot}/usr/src/akmods/ps5-linux-power-kmod-*.src.rpm | head -1)"
ln -s "$(basename "$latest")" \
	%{buildroot}/usr/src/akmods/ps5-linux-power-kmod.latest

install -d %{buildroot}/etc/modules-load.d
printf 'ps5_smu\n' > %{buildroot}/etc/modules-load.d/ps5-smu.conf
printf 'ps5_icc_fan\n' > %{buildroot}/etc/modules-load.d/ps5-icc-fan.conf

%post
if [ -x /usr/sbin/akmods ] && [ -e "/lib/modules/$(uname -r)/build" ]; then
	/usr/sbin/akmods --force --kernels "$(uname -r)" || true
fi
modprobe ps5_smu >/dev/null 2>&1 || true
modprobe ps5_icc_fan >/dev/null 2>&1 || true

%files
/usr/src/akmods/ps5-linux-power-kmod-*.src.rpm
/usr/src/akmods/ps5-linux-power-kmod.latest
/etc/modules-load.d/ps5-smu.conf
/etc/modules-load.d/ps5-icc-fan.conf
