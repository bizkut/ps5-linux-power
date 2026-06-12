# ps5-icc-fan DKMS module

Optional out-of-tree fan control module for PS5 Linux.

This module does not patch kernel source. It depends on the running PS5 kernel
exporting `icc_query()`. If `icc_query` is not exported, DKMS may build the
module but `modprobe ps5_icc_fan` will fail with an unknown symbol.

## What it exposes

The module creates:

```text
/dev/ps5-fan
```

It exposes curated ioctls only:

- change fan servo pattern
- fan mode get/set
- thermal zone temperature get
- fan servo current settings get
- fan target temperature set

It intentionally does not expose raw ICC query passthrough.

## Install with DKMS

From this directory:

```sh
sudo mkdir -p /usr/src/ps5-icc-fan-0.1.0
sudo cp Makefile dkms.conf ps5_icc_fan.c ps5_icc_fan.h /usr/src/ps5-icc-fan-0.1.0/
sudo dkms add -m ps5-icc-fan -v 0.1.0
sudo dkms build -m ps5-icc-fan -v 0.1.0
sudo dkms install -m ps5-icc-fan -v 0.1.0
sudo modprobe ps5_icc_fan
```

## Verify

Check that the target kernel exports ICC query support:

```sh
grep ' icc_query$' /proc/kallsyms
```

Check DKMS status:

```sh
dkms status ps5-icc-fan
```

Check module load and device node:

```sh
lsmod | grep ps5_icc_fan
ls -l /dev/ps5-fan
dmesg | tail -50
```

Build the userspace probe:

```sh
make userspace
```

`make userspace` automatically adds the target kernel UAPI include directory when
needed for stripped userspace headers.

Run read-only checks first:

```sh
sudo ./ps5_fanctl temp 0
sudo ./ps5_fanctl mode-get 0
sudo ./ps5_fanctl servo 0
```

Only after read-only checks work, test writes:

```sh
sudo ./ps5_fanctl pattern 0
sudo ./ps5_fanctl target-temp 0 58
```

Or run the bundled validation script:

```sh
./validate.sh
sudo ./validate.sh --write-tests
```

The default validation mode checks `icc_query`, module/device state, probe build,
and read-only ioctls. `--write-tests` also changes servo pattern `0` and MAINSOC
target temperature `58C`.

On PS5 Linux `7.0.10` at `steam@10.0.1.41`, native userspace builds, module
build, read-only `/dev/ps5-fan` checks, conservative pattern writes, and
MAINSOC target-temp `58C` writes were validated. The target-temp write changed
servo `setting0` from `0x00005b00` to `0x00003a00`.

## Failure modes

- `Unknown symbol icc_query`: the installed kernel does not export `icc_query`;
  this cannot be fixed from DKMS without a kernel change.
- `/dev/ps5-fan` missing after `modprobe`: module did not load; check `dmesg`.
- ioctl returns `EINVAL`: bad zone, mode, or temperature argument.
- ioctl returns `EIO` or another ICC error: firmware rejected the command; stop
  testing writes and inspect `dmesg`.
