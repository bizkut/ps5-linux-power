# Packaging

`ps5-linux-power` ships two package families per distro:

- `ps5-linux-power`: userspace tools, governors, config, systemd service, and
  docs.
- `ps5-linux-power-dkms`: optional DKMS modules for `/dev/ps5-smu` and
  `/dev/ps5-fan`.
- `akmod-ps5-linux-power`: optional Fedora/Bazzite akmods package for
  `/dev/ps5-smu` and `/dev/ps5-fan`.

The DKMS package depends on `dkms`, `gcc`, and `make`. It intentionally does not
depend on distro kernel header packages because PS5 Linux systems usually run
custom kernels. The running kernel must provide matching headers at:

```text
/lib/modules/$(uname -r)/build
```

The akmod package has the same header requirement, but it follows Fedora's
akmods flow instead of DKMS. Prefer `akmod-ps5-linux-power` on Bazzite and
Fedora Atomic systems.

## Local Builds

Debian/Ubuntu:

```sh
VERSION=0.1.0 packaging/deb/build-deb.sh
```

Fedora/RPM:

```sh
VERSION=0.1.0 packaging/rpm/build-rpm.sh
```

Arch:

```sh
VERSION=0.1.0 packaging/arch/build-arch.sh
```

Artifacts are written to `dist/`.

## Bazzite Install

Bazzite uses `rpm-ostree` for host packages. Install the userspace RPM and the
akmod RPM from the release, then reboot into the new deployment:

```sh
sudo rpm-ostree install \
  ./ps5-linux-power-*.x86_64.rpm \
  ./akmod-ps5-linux-power-*.noarch.rpm

sudo systemctl reboot
```

After reboot:

```sh
sudo systemctl enable --now ps5gov
ps5govctl sensors
```

If the module build fails, check that the PS5 custom kernel exposes:

```text
/lib/modules/$(uname -r)/build
```

## Release Builds

GitHub Actions builds release packages when a tag matching `v*` is pushed. The
workflow uploads Debian, RPM, and Arch artifacts to a GitHub Release and uses
`packaging/release/RELEASE_NOTES.md` as the release body.

Tag example:

```sh
git tag v0.1.0
git push origin v0.1.0
```
