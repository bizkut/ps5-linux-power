Name: ps5-linux-power
Version: 0.1.0
Release: 1%{?dist}
Summary: PS5 Linux userspace power, fan, and governor tools
License: GPL-2.0-only
URL: https://github.com/bizkut/ps5-linux-power
Source0: %{name}-%{version}.tar.gz
Requires: systemd
BuildRequires: gcc
BuildRequires: make

%description
CPU/GPU governors, fan policy, trace collection, and manual recovery tools for
PS5 Linux power control.

%prep
%autosetup

%build
%make_build

%install
%make_install install-systemd PREFIX=/usr/local SYSCONFDIR=/etc DESTDIR=%{buildroot}
install -d %{buildroot}%{_docdir}/%{name}
install -m 0644 README.md %{buildroot}%{_docdir}/%{name}/README.md
cp -a docs %{buildroot}%{_docdir}/%{name}/

%post
systemctl daemon-reload >/dev/null 2>&1 || true

%files
/usr/local/bin/ps5govctl
/usr/local/lib/ps5-linux-power
/etc/ps5-linux-power
/etc/systemd/system/ps5gov.service
%{_docdir}/%{name}
