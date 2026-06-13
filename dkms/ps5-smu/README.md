# ps5-smu DKMS module

Optional out-of-tree SMU mailbox transport for PS5 Linux.

The module creates:

```text
/dev/ps5-smu
```

It exposes curated ioctls only:

- CPU P-state get/set
- GPU GFXCLK get/set
- CPU/GPU rail voltage reads

It intentionally does not expose raw arbitrary SMN reads/writes or arbitrary
mailbox messages. The userspace tools still contain the older PCI config-space
fallback, but prefer `/dev/ps5-smu` when this module is loaded.

## Install with DKMS

```sh
sudo mkdir -p /usr/src/ps5-smu-0.1.0
sudo cp Makefile dkms.conf ps5_smu.c ps5_smu.h /usr/src/ps5-smu-0.1.0/
sudo dkms add -m ps5-smu -v 0.1.0
sudo dkms build -m ps5-smu -v 0.1.0
sudo dkms install -m ps5-smu -v 0.1.0
sudo modprobe ps5_smu
printf 'ps5_smu\n' | sudo tee /etc/modules-load.d/ps5-smu.conf
```

## Verify

```sh
grep ' amd_smn_read$' /proc/kallsyms
grep ' amd_smn_write$' /proc/kallsyms
make userspace
sudo ./ps5_smuctl cpu-get 0
sudo ./ps5_smuctl gpu-get
sudo ./ps5_smuctl voltage
./validate.sh
sudo ./validate.sh --write-tests
```

The write tests restore safe normal values: CPU all-core P0 and GPU 2000 MHz.

## Failure modes

- `Unknown symbol amd_smn_read` or `amd_smn_write`: the running kernel does not
  export the SMN helper; this module cannot provide the safer transport there.
- `/dev/ps5-smu` missing after `modprobe`: module did not load; check `dmesg`.
- ioctl returns `EINVAL`: invalid core, mask, P-state, frequency, or rail.
- ioctl returns `EIO` or `ETIMEDOUT`: the SMU mailbox rejected or did not
  complete the command; stop policy writers and inspect `dmesg`.
