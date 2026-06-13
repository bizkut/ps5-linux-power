# ps5-linux-power release

## Install

Download the package pair for your distro family from this release. Install both
the userspace package and the DKMS package with the distro package manager so
dependencies are resolved automatically.

Debian/Ubuntu:

```sh
sudo apt install ./ps5-linux-power_*.deb ./ps5-linux-power-dkms_*.deb
```

Fedora:

```sh
sudo dnf install ./ps5-linux-power-*.rpm ./ps5-linux-power-dkms-*.rpm
```

Arch:

```sh
sudo pacman -U ps5-linux-power-*.pkg.tar.zst ps5-linux-power-dkms-*.pkg.tar.zst
```

Then enable the governor service:

```sh
sudo systemctl enable --now ps5gov
ps5govctl sensors
```

## Kernel Header Requirement

The DKMS package does not install distro kernel header packages. PS5 Linux users
normally run custom kernels, and the matching kernel build tree must already be
provided by that kernel at:

```text
/lib/modules/$(uname -r)/build
```

If that path is missing, install or expose the headers for the currently running
PS5 custom kernel, then reinstall the `ps5-linux-power-dkms` package.

## Packages

- `ps5-linux-power`: userspace tools, governors, systemd service, config, and
  documentation.
- `ps5-linux-power-dkms`: optional `/dev/ps5-smu` and `/dev/ps5-fan` kernel
  transports built through DKMS.
