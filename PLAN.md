# PS5 governor hardening plan

This repo is now a userspace power-control PoC with systemd integration. The
remaining plan is mostly about validation and deciding whether the mailbox path
should stay in userspace or move behind a kernel-owned interface.

## Current state

- [x] One-shot tools for CPU P-state, DF P-state, GPU GFXCLK, and MP1 boost.
- [x] CPU/GPU load-adaptive governors.
- [x] systemd service and install target.
- [x] `/etc/ps5-linux-cpuclock/ps5gov.conf` config file.
- [x] `quiet`, `balanced`, and `performance` profiles.
- [x] `ps5govctl` for profile switching, config inspection, sensors, restore,
      restart, and boost recovery.
- [x] Shared boost-vote state so CPU and GPU governors do not fight each other.
- [x] Parent-level restore on governor shutdown: boost off, CPU P0, GPU 2000.
- [x] Local UAPI fallback headers for stripped/custom PS5 Linux images.

## Cyan-derived parts already used

- [x] Same userspace SMN mailbox transport through PCI config space
      `0000:00:00.0` index/data registers.
- [x] Same practical focus on GPU GFXCLK as the biggest power lever.
- [x] Same `/dev/mp1` boost envelope concept for peak-load behavior.
- [x] Same model of keeping DF control manual because DF writes are riskier.

## Safety policy

- [x] Do not run alongside `ps5boost.service`; `ps5gov.service` conflicts with it.
- [x] Keep DF out of the automatic governor.
- [x] Require `force` for direct DF/GPU writes.
- [x] Validate governor thresholds, intervals, down-counts, load method, and
      temperature source.
- [x] Refuse governor startup when `/dev/mp1` or the SMN PCI config path is
      missing.
- [x] Use staged GPU thermal caps:
      warm disables boost, hot caps to 1500 MHz, critical caps to 1200 MHz.

## Real PS5 validation checklist

- [ ] `make clean && make` on the target custom kernel image.
- [ ] `sudo make install-systemd && sudo systemctl daemon-reload`.
- [ ] `sudo systemctl disable --now ps5boost`.
- [ ] `sudo systemctl enable --now ps5gov`.
- [ ] `ps5govctl sensors` shows `k10temp` and any GPU hwmon sensors available.
- [ ] `cat /sys/class/drm/card*/device/device` confirms the GPU id is `0x13fb`.
- [ ] Under idle load, CPU/GPU clocks step down and boost state remains `0x0`.
- [ ] Under game/benchmark load, CPU/GPU ramp up and boost state becomes non-zero.
- [ ] At high temperature, boost is cleared and GPU cap falls in stages.
- [ ] `sudo ps5govctl restore` stops the service and restores CPU P0/GPU 2000.
- [ ] Reboot recovery is clean: service starts only once and no stale boost state
      remains after stop/start cycles.

## Next engineering steps

- [ ] Add runtime profile/range changes without restarting the service.
- [ ] Add structured status output from the governors, preferably under `/run`.
- [ ] Add optional logging rate limits for verbose mode.
- [ ] Add a target-side smoke test script for install, config, sensors, restore,
      and service lifecycle checks.
- [ ] Decide whether GPU busy sampling should stay fdinfo-based or move to a
      safer kernel/libdrm path for Cyan-style MMIO busy-flag sampling.
- [ ] If this becomes more than a PoC, move all SMN mailbox transactions behind a
      kernel `/dev/mp1` ioctl or another kernel-locked interface.

## Deferred by design

- [ ] Automatic DF governor. DF writes save power, but the failure mode is worse
      than CPU/GPU clock changes.
- [ ] Userspace MMIO mapping for GPU busy flags. It is not worth adding a second
      unsafe hardware access path while SMN is already the PoC risk.
- [ ] Supporting unknown AMD GPU ids automatically. The known PS5 id is `0x13fb`;
      fallback to a single AMD DRM card is enough for this repo.
