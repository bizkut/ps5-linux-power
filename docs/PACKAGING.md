# Packaging

`ps5-linux-power` ships two package families per distro:

- `ps5-linux-power`: userspace tools, governors, config, systemd service, and
  docs.
- `ps5-linux-power-dkms`: optional DKMS modules for `/dev/ps5-smu` and
  `/dev/ps5-fan`.

The DKMS package depends on `dkms`, `gcc`, and `make`. It intentionally does not
depend on distro kernel header packages because PS5 Linux systems usually run
custom kernels. The running kernel must provide matching headers at:

```text
/lib/modules/$(uname -r)/build
```

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

## Release Builds

GitHub Actions builds release packages when a tag matching `v*` is pushed. The
workflow uploads Debian, RPM, and Arch artifacts to a GitHub Release and uses
`packaging/release/RELEASE_NOTES.md` as the release body.

Tag example:

```sh
git tag v0.1.0
git push origin v0.1.0
```
