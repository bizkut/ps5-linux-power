# ps5-linux-power 0.1.0

## What is included

This release provides PS5 Linux userspace power tools, the `ps5gov` systemd
governor service, fan and thermal policy, trace helpers, and optional kernel
transports for `/dev/ps5-smu` and `/dev/ps5-fan`.

## Installation

Download the packages for your distro family from this release. Install the
userspace package plus the matching kernel-transport package with the distro
package manager so dependencies are resolved automatically.

### Debian/Ubuntu

```sh
sudo apt install ./ps5-linux-power_*.deb ./ps5-linux-power-dkms_*.deb
```

### Fedora

```sh
sudo dnf install ./ps5-linux-power-*.rpm ./ps5-linux-power-dkms-*.rpm
```

### Bazzite/Fedora Atomic

```sh
sudo rpm-ostree install \
  ./ps5-linux-power-*.x86_64.rpm \
  ./akmod-ps5-linux-power-*.noarch.rpm

sudo systemctl reboot
```

After the reboot:

```sh
sudo systemctl enable --now ps5gov
ps5govctl sensors
```

### Arch

```sh
sudo pacman -U ps5-linux-power-*.pkg.tar.zst ps5-linux-power-dkms-*.pkg.tar.zst
```

Then enable the governor service:

```sh
sudo systemctl enable --now ps5gov
ps5govctl sensors
```

## Kernel Header Requirement

The DKMS and akmod packages do not install distro kernel header packages. PS5
Linux users normally run custom kernels, and the matching kernel build tree must
already be provided by that kernel at:

```text
/lib/modules/$(uname -r)/build
```

If that path is missing, install or expose the headers for the currently running
PS5 custom kernel, then reinstall `ps5-linux-power-dkms` or
`akmod-ps5-linux-power`.

## Packages

- `ps5-linux-power`: userspace tools, governors, systemd service, config, and
  documentation.
- `ps5-linux-power-dkms`: optional `/dev/ps5-smu` and `/dev/ps5-fan` kernel
  transports built through DKMS. Use this on Debian/Ubuntu, Fedora, and Arch.
- `akmod-ps5-linux-power`: Fedora/Bazzite-native alternative to the DKMS
  package. Prefer this on Bazzite and Fedora Atomic systems.
