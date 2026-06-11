# PS5 governor hardening plan

This repo is now a userspace power-control PoC with systemd integration. The
remaining plan is mostly about validation and deciding whether the mailbox path
should stay in userspace or move behind a kernel-owned interface.

## Current state

- [x] One-shot tools for CPU P-state, DF P-state, GPU GFXCLK, and MP1 boost.
- [x] CPU/GPU load-adaptive governors.
- [x] systemd service and install target.
- [x] `/etc/ps5-linux-cpuclock/ps5gov.conf` config file.
- [x] `auto`, `quiet`, `balanced`, and `performance` profiles.
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
- [x] GPU moving target frequency policy with configurable significant-change,
      normal ramp, and burst ramp thresholds.
- [x] GPU governor presets (`auto`, `quiet`, `balanced`, `performance`) so the
      service can use concise profile selection while preserving manual tuning.
- [x] GPU frequency range limits, matching Cyan's configurable min/max policy
      concept while keeping PS5-safe defaults.

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
- [x] Implement optional in-repo fan governor (`ps5_fan_gov`) with fan state
      exposure (`/run/ps5-power.fan`) and configurable temperature hysteresis.
- [x] Fan governor auto sensor mode tracks the hottest hwmon temperature, restores
      the default servo pattern safely, and fails safe to the cool servo pattern
      after repeated sensor read errors.
- [x] Reviewed `ps5-emc-re` fan/EMC reverse-engineering notes; they point to a
      better long-term fan path than simple userspace pattern toggling.
- [x] Align `ps5_fan_gov` with the current kernel `/dev/icc` UAPI:
      `ICC_FAN_CHANGE_SERVO_PATTERN` (`_IOW('I', 1, u8)`).

## ps5-emc-re fan findings

`/Users/bizkut/Downloads/PS5/Linux/ps5-emc-re` shows that fan control should
eventually be EMC/ICC auto-servo control, not just `/dev/icc` servo-pattern
toggling:

- Fan service is `ICC_SERVICE_ID_FAN = 0x0a`.
- Thermal service is `ICC_SERVICE_ID_THERMAL = 0x0b`.
- Fan mode messages:
  - `0x02`: set fan mode.
  - `0x03`: get fan mode.
- Fan servo messages:
  - `0x06`: set one current servo setting.
  - `0x07`: get current servo settings.
- Thermal zone message:
  - thermal service `0x01`: read a zone temperature.
- Known fan zones:
  - `MAINSOC`, `LOCAL1`, `LOCAL2`, `LOCAL3`.
- Known fan modes:
  - `AUTO=1`, `MAXIMUM=2`, `MINIMUM=3`, `MANUAL=4`, `SP1=5`.
- First writable servo settings:
  - target temperature, P gain, I gain, I limit, U limit, D limit.

Preferred direction:

- Current userspace `/dev/icc` only exposes `ICC_FAN_CHANGE_SERVO_PATTERN`
  (`_IOW('I', 1, u8)`), so `ps5_fan_gov` now treats `-f`, default, and cool
  choices as servo pattern numbers rather than a true fan on/off switch.
- Keep `ps5_fan_gov` temperature hysteresis as a simple failsafe/manual pattern
  policy.
- Add a kernel-owned safe fan UAPI for EMC fan mode, zone temperature, servo
  status, and target-temperature updates.
- Prefer EMC `AUTO` mode plus target-temperature tuning over userspace fan
  pattern loops.
- Treat P/I/limit servo writes as experimental until measured on real hardware;
  target temperature is the first setting worth testing.
- Do not expose a raw generic ICC query interface by default; expose curated fan
  operations so userspace cannot accidentally send power/shutdown/unknown ICC
  messages.

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
- [x] Add structured GPU status output under `/run/ps5-power.gpu`.
- [x] Add structured CPU status output under `/run/ps5-power.cpu`.
- [ ] Add optional logging rate limits for verbose mode.
- [x] Add a target-side smoke test script for install, config, sensors, restore,
      and service lifecycle checks.
- [ ] Add a target-side fan validation script that compares EMC auto/target-temp
      behavior against forced fan servo patterns once the safe fan UAPI exists.
- [ ] Prototype kernel-side fan operations from `ps5-emc-re`: mode get/set, zone
      temperature get, servo current settings get, and MAINSOC target-temp set.
- [ ] Validate GPU moving-target defaults against idle desktop, game load, and
      sustained benchmark traces on real PS5 hardware.
- [ ] Decide whether GPU busy sampling should stay fdinfo-based or move to a
      safer kernel/libdrm path for Cyan-style busy sampling.
- [ ] If this becomes more than a PoC, move all SMN mailbox transactions behind a
      kernel `/dev/mp1` ioctl or another kernel-locked interface.

## Deferred by design

- [ ] Automatic DF governor. DF writes save power, but the failure mode is worse
      than CPU/GPU clock changes.
- [ ] Userspace MMIO mapping for GPU busy flags. It is not worth adding a second
      unsafe hardware access path while SMN is already the PoC risk.
- [ ] Supporting unknown AMD GPU ids automatically. The known PS5 id is `0x13fb`;
      fallback to a single AMD DRM card is enough for this repo.
