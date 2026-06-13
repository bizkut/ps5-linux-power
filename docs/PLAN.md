# PS5 governor hardening plan

This repo is now `ps5-linux-power`: a userspace PS5 power-control stack with
systemd integration, fan/thermal policy, trace collection, and optional DKMS
kernel transports. The remaining plan is validation-first.

## New agent handoff

Start here if taking over this thread.

Current local repo state:

- Branch: `main`.
- GitHub: `https://github.com/bizkut/ps5-linux-power`.
- Latest pushed commits:
  - `e00bbbf docs: rename project to ps5-linux-power`
  - `be4ac23 dkms: add curated PS5 SMU transport`
  - `0676947 governors: add shared thermal policy and trace bundles`
- Working tree should only have generated binaries after a build. Do not stage
  generated binaries unless deliberately adopting them.
- Packaging is being added for Debian/Ubuntu, Fedora/RPM, and Arch releases.
  The package split is `ps5-linux-power` for userspace and
  `ps5-linux-power-dkms` for optional `/dev/ps5-smu` and `/dev/ps5-fan`
  transports. DKMS packages depend on `dkms`, `gcc`, and `make`, but do not
  depend on distro kernel headers because PS5 custom kernels should provide
  matching headers at `/lib/modules/$(uname -r)/build`.
- Fedora/Bazzite releases also provide `akmod-ps5-linux-power`, the preferred
  module package for Bazzite/Fedora Atomic systems.
- `ps5_gpu_gov` also clamps computed load to `0.0..1.0`; sustained `vkmark`
  showed summed fdinfo counters can report above 100% when multiple DRM fdinfo
  entries overlap.
- Local validation after the current changes: `make`,
  `governors/ps5gov-smoke.sh`, `sudo -n make install-systemd`,
  `sudo -n systemctl restart ps5gov.service`, and `ps5govctl sensors`.
- This checkout is on the PS5 target itself; target deployment can be done
  locally with `make` and `sudo make install-systemd`.
- Installed locally on the PS5 target with `sudo -n make install-systemd`.
  `ps5boost.service` and `ps5fan.service` were disabled/stopped, and
  `ps5gov.service` was enabled/started.
- Installed lightweight benchmark packages for target validation:
  `mesa-utils`, `vulkan-tools`, and `vkmark`.
- Installed `ps5-icc-fan` through DKMS on the target and loaded
  `ps5_icc_fan`; `/dev/ps5-fan` exists after `modprobe`.
- Installed `ps5-smu` through DKMS on the target and loaded `ps5_smu`;
  `/dev/ps5-smu` exists and the tools prefer it over direct userspace PCI
  config-space SMN access.
- Added `/etc/modules-load.d/ps5-icc-fan.conf` so `ps5_icc_fan` loads after
  reboot. Verified by unloading the module, running
  `/usr/lib/systemd/systemd-modules-load`, and confirming `/dev/ps5-fan`
  returned.
- Built `dkms/ps5-icc-fan/ps5_fanctl` and updated root `make install` to copy it
  under `/usr/local/lib/ps5-linux-power/dkms/ps5-icc-fan/` when available, so
  installed `ps5gov-trace.sh` can capture EMC zones.
- Root service smoke test passed after install:
  `sudo -n /usr/local/lib/ps5-linux-power/governors/ps5gov-smoke.sh --service`.
  That restore test stops `ps5gov`, so the service was started again afterward.
- Generated binaries currently expected after local builds include root tool
  binaries, governor binaries, `dkms/ps5-icc-fan/ps5_fanctl`, and
  `dkms/ps5-smu/ps5_smuctl`.

PS5 target:

- SSH: `steam@10.0.1.41`.
- Test tree: `/home/steam/ps5-linux-power-test`.
- Installed service uses `/usr/local/lib/ps5-linux-power` and
  `/etc/ps5-linux-power/ps5gov.conf`.
- The Makefile intentionally migrates an old
  `/etc/ps5-linux-cpuclock/ps5gov.conf` into the new config path on first
  install.
- systemd service path: `/etc/systemd/system/ps5gov.service`.
- `ps5gov.service` is installed, enabled, and active after reboot.
- `ps5boost.service` and `ps5fan.service` are inactive; `ps5gov.service`
  conflicts with both.
- Current default policy is gaming-focused:
  - `PROFILE=performance`
  - `CPU_ARGS="-i 250 -d 6 -H 25 -M 12 -L 4"`
  - `GPU_ARGS="-P performance"`
  - `FAN_ENABLED=1`
  - `FAN_ARGS="-i 3000 -H 55 -L 45 -s auto"`
- `ps5_icc_fan` has been built/loaded successfully and `/dev/ps5-fan` works.
- `ps5_smu` has been built/loaded successfully and `/dev/ps5-smu` works.

Validated on target:

- Native build works with the kernel UAPI include fallback.
- GPU id is `0x13fb`.
- Idle governor state steps down: CPU around P5/1600 MHz, GPU target 1200 MHz,
  boost `0x0`.
- Headless CPU load (`openssl speed -multi 16`) ramps CPU to P0/3200 and boost
  to `0x1`.
- `ps5govctl restore` stops the service, clears boost, restores CPU/GPU, restores
  fan pattern 0, and service can be restarted.
- Reboot recovery is clean: after reboot, exactly one CPU/GPU/fan governor
  process is running and boost is `0x0`.
- DKMS fan read-only checks work: zone temp, mode get, servo get.
- Conservative fan writes work: pattern `0`, pattern `1`, target-temp `58`,
  restore pattern `0`.
- Staged fan curve test worked once: stage 3 around 65C, stage 2 around 61C,
  restore pattern 0 on exit.
- Root `ps5gov-trace.sh` captures EMC zones, CPU/GPU/fan state, and boost.
- Local trace script now also captures GPU load, configured load method, active
  load method, and raw fdinfo `drm-engine-gfx` counter. These changes were
  built/installed on the PS5 target and verified in `/run/ps5-power.gpu`.
- A 10-second root trace verified the new CSV columns:
  `/tmp/ps5gov-traces/ps5gov-trace-1781257111.csv`.
- A 45-second `vkmark --winsys wayland` trace verified fdinfo-visible GPU load:
  `vkmark` saw GPU id `0x13fb`, governor load reached 0.980, current/target/
  desired MHz reached 2230, and GPU boost reached `1`.
  Trace: `/tmp/ps5gov-traces/ps5gov-trace-1781257336.csv`.
- A sustained 15-minute `vkmark --winsys wayland` trace verified ramp behavior
  under longer load: 841 samples over 900 seconds, active method `fdinfo`,
  average load about 1.001 before clamping, max current/target/desired 2230 MHz,
  boost active for 834 samples, max temperature 73 C, no thermal cap.
  Trace: `/tmp/ps5gov-traces/ps5gov-trace-1781257417.csv`.
- After adding load clamping, a 60-second `vkmark --winsys wayland` regression
  trace showed max load 1.000, max current 2230 MHz, boost active, and no thermal
  cap. Trace: `/tmp/ps5gov-traces/ps5gov-trace-1781258458.csv`.
- A 10-second trace after DKMS fan module install and `ps5_fanctl` install
  verified EMC zone columns populate:
  `/tmp/ps5gov-traces/ps5gov-trace-1781261518.csv`.
- A 5-second trace after validating modules-load persistence again showed EMC
  zone columns populated:
  `/tmp/ps5gov-traces/ps5gov-trace-1781261815.csv`.
- `ps5gov-trace.sh` now supports `-n note` or `PS5GOV_TRACE_NOTE` and records a
  `note` CSV column, so real-game traces can identify the game/scene without
  renaming files during capture.
- Installed trace note support was verified with:
  `/tmp/ps5gov-traces/ps5gov-trace-1781262008.csv`.
- Installed `supertuxkart` for an unattended real-game validation path.
- A 15-minute `supertuxkart` profile trace verified real-game fdinfo load and
  moving-target behavior: 868 samples over 929 seconds, note
  `supertuxkart-lighthouse-profile`, active method `fdinfo`, average load 0.493,
  max load 0.738, current/target/desired MHz reached 2230, boost active for 816
  samples, max hwmon temp 72 C, max EMC zones 70.87/66.22/29.64/58.50 C, fan
  pattern 1, and no thermal cap. Trace:
  `/tmp/ps5gov-traces/ps5gov-trace-1781262531.csv`.
- A 30-minute commercial game trace with Shadow of the Tomb Raider Trial
  (`steam://rungameid/974630`, Proton, manually launched benchmark) verified
  heavy real-game behavior: 1622 samples over 1800 seconds, note
  `shadow-trial-manual-benchmark`, active method `fdinfo`, average load 0.972,
  max load 1.000, max hwmon temp 84 C, max EMC zones
  81.00/83.25/29.38/71.98 C, current/target/desired MHz reached 2230, boost
  active, the previous staged thermal policy capped the GPU for 746 samples, and
  fan pattern 1 was active. Trace:
  `/tmp/ps5gov-traces/ps5gov-trace-1781268618.csv`.

Important caveats:

- Basic GPU ramp validation passed with `vkmark --winsys wayland`: fdinfo load
  is visible, GPU clocks ramp to 2230 MHz, and boost becomes non-zero.
- Sustained synthetic, open-source game, and commercial game traces now validate
  fdinfo sampling, moving-target ramp, boost behavior, and natural warm thermal
  cap transitions. Subjective fan/noise feedback is still needed before
  finalizing default fan tuning.
- After changing script defaults, use `systemctl restart ps5gov`, not only
  `systemctl reload ps5gov`; reload keeps the existing parent shell process.

Next best work:

- Review real-game trace data against subjective fan noise. Shadow Trial reached
  84 C hwmon / 83.25 C EMC zone with fan pattern 1. Manual fan target testing
  found MAINSOC target `70 C` acceptable and able to hold roughly `70-72 C`
  without GPU thermal cap under that load, while `58 C` was too loud. Adopted a
  provisional measured-data curve:
  `45:78:0,60:74:0,72:70:1,82:68:1,88:66:1`.
- Capture additional 15-30 minute root traces for heavier real games as needed.
  Prefer the persistent bundle wrapper:
  `sudo ps5gov-collect-trace.sh -d 1800 -i 1 -n game-or-scene-name`.
- Best commercial target for repeatable heavier tuning is Shadow of the Tomb
  Raider: Definitive Edition, because it is a native Linux/Feral Vulkan game
  with a command-line benchmark path. Suggested trace bundle:
  `sudo ps5gov-collect-trace.sh -d 1800 -i 1 -n shadow-tomb-raider-benchmark`,
  then run `steam -applaunch 750920 -benchmark`.
- Current installed commercial target is Shadow of the Tomb Raider Trial
  (`974630`) through Proton. It has a launcher, so the validated workflow is:
  start the benchmark manually, then attach a trace bundle with
  `sudo ps5gov-collect-trace.sh -d 1800 -i 1 -n shadow-trial-smu-fan-cpu-validation`.
- Other commercial fallback candidates: Deus Ex: Mankind Divided, DiRT Rally,
  and Mad Max, all with reported benchmark/automation paths. Avoid making Total
  War the first choice because its benchmark is less command-line friendly.
- If a commercial game does not produce fdinfo-visible load, compare it against
  the validated `vkmark --winsys wayland` and SuperTuxKart paths before
  revisiting GPU load sampling.
- GPU traces include configured load method, active load method, the raw fdinfo
  `drm-engine-gfx` counter, and a note column so real-game runs can distinguish
  "governor did not ramp" from "current sampler saw no GPU work" and tie fan
  noise observations to a specific game/scene.
- Next code work should wait until a new real-game bundle plus subjective fan
  noise feedback shows a specific regression or tuning need.

## Current state

- [x] One-shot tools for CPU P-state, DF P-state, GPU GFXCLK, and MP1 boost.
- [x] CPU/GPU load-adaptive governors.
- [x] systemd service and install target.
- [x] `/etc/ps5-linux-power/ps5gov.conf` config file.
- [x] `auto`, `quiet`, `balanced`, and `performance` profiles, with
      `performance` as the default gaming profile.
- [x] `ps5govctl` for profile switching, config inspection, sensors, restore,
      restart, and boost recovery.
- [x] Shared boost-vote state so CPU and GPU governors do not fight each other.
- [x] Parent-level restore on governor shutdown: boost off, CPU P0, GPU 2000.
- [x] Local UAPI fallback headers for stripped/custom PS5 Linux images.
- [x] Packaging scaffolding for Debian/Ubuntu `.deb`, Fedora/RPM `.rpm`, and
      Arch `.pkg.tar.zst` artifacts.
- [x] Fedora/Bazzite akmods package path for optional kernel transports.
- [x] GitHub Actions release workflow that builds packages on `v*` tags and
      publishes artifacts with release-note install commands.

## External-reference-derived parts already used

- [x] Same userspace SMN mailbox transport through PCI config space
      `0000:00:00.0` index/data registers.
- [x] Same practical focus on GPU GFXCLK as the biggest power lever.
- [x] Same `/dev/mp1` boost envelope concept for peak-load behavior.
- [x] Same model of keeping DF control manual because DF writes are riskier.
- [x] GPU moving target frequency policy with configurable significant-change,
      normal ramp, and burst ramp thresholds.
- [x] GPU governor presets (`auto`, `quiet`, `balanced`, `performance`) so the
      service can use concise profile selection while preserving manual tuning.
- [x] GPU frequency range limits, matching the configurable min/max policy
      concept while keeping PS5-safe defaults.
- [x] Runtime high-performance override, implemented as a dependency-free
      `/run/ps5-power.performance` switch controlled by
      `ps5govctl performance on|off|status`.
- [x] Deferred direct D-Bus service and voltage/safe-point table for now:
      D-Bus adds packaging/dependency surface, and voltage points are not PS5
      validated.

## Safety policy

- [x] Do not run alongside `ps5boost.service` or `ps5fan.service`;
      `ps5gov.service` conflicts with both.
- [x] Keep DF out of the automatic governor.
- [x] Require `force` for direct DF/GPU writes.
- [x] Validate governor thresholds, intervals, down-counts, load method, and
      temperature source.
- [x] Refuse governor startup when `/dev/mp1` or the SMN PCI config path is
      missing.
- [x] Use gradual GPU thermal max-frequency reduction:
      above `85 C` clear boost, start around 2000 MHz, and step down while hot;
      below `75 C` restore the requested max and normal boost behavior.
- [x] Implement optional in-repo fan governor (`ps5_fan_gov`) with fan state
      exposure (`/run/ps5-power.fan`) and configurable temperature hysteresis.
- [x] Fan governor auto sensor mode tracks the hottest hwmon temperature, restores
      the default servo pattern safely, and fails safe to the cool servo pattern
      after repeated sensor read errors.
- [x] Reviewed `ps5-emc-re` fan/EMC reverse-engineering notes; they point to a
      better long-term fan path than simple userspace pattern toggling.
- [x] Align `ps5_fan_gov` with the current kernel `/dev/icc` UAPI:
      `ICC_FAN_CHANGE_SERVO_PATTERN` (`_IOW('I', 1, u8)`).
- [x] Make `ps5_fan_gov` prefer the optional DKMS `/dev/ps5-fan` UAPI and fall
      back to the legacy `/dev/icc` pattern ioctl.
- [x] Add optional staged fan curve support to `ps5_fan_gov`:
      `temp_up:target_temp:servo_pattern` with hysteresis.

## ps5-emc-re fan findings

Fan EMC/ICC references:

- Upstream research: `https://github.com/c0w-ar/ps5-emc-re`
- Local checkout used here: `/Users/bizkut/Downloads/PS5/Linux/ps5-emc-re`

That research shows fan control should eventually be EMC/ICC auto-servo control,
not just `/dev/icc` servo-pattern toggling:

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

Implemented direction:

- Current userspace `/dev/icc` only exposes `ICC_FAN_CHANGE_SERVO_PATTERN`
  (`_IOW('I', 1, u8)`), so `ps5_fan_gov` now treats `-f`, default, and cool
  choices as servo pattern numbers rather than a true fan on/off switch.
- `ps5_fan_gov` keeps simple two-state temperature hysteresis as the default
  failsafe/manual pattern policy.
- Optional staged fan curves are supported with
  `temp_up:target_temp:servo_pattern` points and hysteresis.
- The optional `ps5-icc-fan` DKMS module exposes a curated `/dev/ps5-fan` UAPI
  for EMC fan mode, zone temperature, servo status, servo pattern, and
  target-temperature updates when the target kernel exports `icc_query()`.
- When `/dev/ps5-fan` exists, `ps5_fan_gov` applies both EMC target temperature
  and servo pattern for curve stages. Without it, it falls back to legacy
  `/dev/icc` servo-pattern control.
- Prefer EMC `AUTO` mode plus target-temperature tuning over raw userspace fan
  pattern loops once real hardware traces confirm behavior.
- Treat P/I/limit servo writes as experimental until measured on real hardware;
  target temperature is the first setting worth testing.
- Do not expose a raw generic ICC query interface by default; expose curated fan
  operations so userspace cannot accidentally send power/shutdown/unknown ICC
  messages.

## Fan curve design

Goal: keep the default fan policy enabled and gaming-oriented with a
measured-data staged curve. The default service starts `ps5_fan_gov`; curve
stages drive EMC target temperature when `/dev/ps5-fan` is available and fall
back to servo-pattern selection when only legacy `/dev/icc` is available.

Implemented config shape:

```sh
FAN_CURVE="45:78:0,60:74:0,72:70:1,82:68:1,88:66:1"
FAN_HYSTERESIS=4
```

Each point is:

```text
temp_up_c:target_temp_c:servo_pattern
```

Default provisional staged curve:

| Stage | Up C | Down C | Servo pattern | EMC target C |
| --- | ---: | ---: | ---: | ---: |
| 0 | 45 | default | 0 | 78 |
| 1 | 60 | 56 | 0 | 74 |
| 2 | 72 | 68 | 1 | 70 |
| 3 | 82 | 78 | 1 | 68 |
| 4 | 88 | 84 | 1 | 66 |

Those numbers are provisional but based on measured PS5 behavior:

- Shadow Trial reached 84 C hwmon / 83.25 C EMC zone with the default cool
  pattern.
- MAINSOC target-temp `70 C` held roughly `70-72 C` under Shadow load with GPU
  boost active and no thermal cap.
- MAINSOC target-temp `58 C` was too loud.
- The curve keeps quiet/default behavior at low temperatures, uses 70 C as the
  main gaming-load target, and only asks for stronger cooling above the measured
  Shadow Trial range.

Validation required before making this default:

- Record idle desktop and real-game traces on real PS5 hardware. Sustained
  benchmark traces have already been captured with `vkmark --winsys wayland`;
  real-game traces have been captured with SuperTuxKart and Shadow Trial.
- Compare hottest hwmon temperature with DKMS EMC zone temperatures. Existing
  traces include both hwmon and EMC zones; Shadow Trial reached 84 C hwmon and
  EMC zones up to 83.25 C during sustained load.
- Confirm which servo pattern numbers map to default/cool/max behavior.
- Confirm target-temp writes affect EMC auto-servo behavior predictably.
- Adjust curve points from new user bundles and local traces if broader games
  show worse temperatures, performance loss, or unacceptable fan noise.

## Real PS5 validation checklist

- [x] `make clean && make` on the target custom kernel image.
- [x] `sudo make install-systemd && sudo systemctl daemon-reload`.
- [x] `sudo systemctl disable --now ps5boost`; target shows inactive.
- [x] `sudo systemctl enable --now ps5gov`; target shows active/enabled.
- [x] `ps5govctl sensors` runs on target through the smoke script.
- [x] `cat /sys/class/drm/card*/device/device` confirms the GPU id is `0x13fb`.
- [x] Under idle load, CPU/GPU clocks step down and boost state remains `0x0`
      on target; observed CPU P5/1600 MHz and GPU 1200 MHz.
- [x] Under headless CPU benchmark load, CPU ramps up and boost state becomes
      non-zero; `openssl speed -multi 16` drove CPU from P5/1600 to P0/3200 and
      boost state to `0x1`.
- [x] Under a GPU-visible game/benchmark session, GPU ramps up and boost state
      becomes non-zero. `vkmark --winsys wayland` on the target produced
      fdinfo-visible load, reached 2230 MHz, and set GPU boost non-zero.
- [x] At high temperature, boost is cleared and GPU max frequency is reduced.
      Shadow Trial naturally reached the previous thermal cap during a
      30-minute trace; current policy now uses gradual max-frequency reduction
      instead of fixed staged cap levels.
- [x] `sudo ps5govctl restore` stops the service and restores CPU P0/GPU 2000;
      verified boost `0x0`, CPU P0 request, GPU reset to 2000, fan pattern 0,
      then restarted `ps5gov.service`.
- [x] Reboot recovery is clean: service starts only once and no stale boost state
      remains after stop/start cycles. Verified after reboot: `ps5gov.service`
      active/enabled, `ps5boost.service` and `ps5fan.service` inactive, boost
      `0x0`, and exactly one CPU/GPU/fan governor process.
- [x] `grep ' icc_query$' /proc/kallsyms` confirms the target kernel can support
      the optional `ps5-icc-fan` DKMS module.
- [x] `make -C dkms/ps5-icc-fan` succeeds on the target kernel.
- [x] `sudo insmod ps5_icc_fan.ko` creates `/dev/ps5-fan`.
- [x] `ps5-icc-fan` is installed through DKMS and `modprobe ps5_icc_fan` creates
      `/dev/ps5-fan` on the running target kernel.
- [x] `/etc/modules-load.d/ps5-icc-fan.conf` loads `ps5_icc_fan` automatically;
      validated with `systemd-modules-load` on the running target.
- [x] `sudo ps5_fanctl temp 0`, `mode-get 0`, and `servo 0` return sane data
      before any fan writes are tested.
- [x] Conservative fan writes passed on target: pattern `0`, pattern `1`,
      MAINSOC target-temp `58`, then restore pattern `0`.
- [x] `ps5gov.service` now conflicts with `ps5fan.service`; target restart left
      `ps5fan.service` inactive while ps5gov fan control was active.

## Next engineering steps

- [x] Add structured GPU status output under `/run/ps5-power.gpu`.
- [x] Add structured CPU status output under `/run/ps5-power.cpu`.
- [x] Add optional logging rate limits for verbose hold messages.
- [x] Add a target-side smoke test script for install, config, sensors, restore,
      and service lifecycle checks.
- [x] Add a target-side DKMS fan validation script for symbol, module/device,
      read-only ioctl, and optional write checks.
- [x] Add a target-side trace capture script for fan/GPU tuning CSVs:
      service state, hottest hwmon temperature, optional EMC zone temperatures,
      CPU/GPU/fan runtime state, and boost state.
- [x] Add service-level reload for runtime profile/range changes without a full
      systemd restart; reload re-reads config and replaces child governors.
- [x] Add a target-side fan validation script that compares EMC auto/target-temp
      behavior against forced fan servo patterns after the DKMS UAPI is validated
      on real hardware.
- [x] Prototype DKMS fan module operations from `ps5-emc-re`: mode get/set, zone
      temperature get, servo current settings get, servo pattern set, and
      MAINSOC target-temp set.

## Next target-side validation

- [x] Build/load `ps5-icc-fan` module on real PS5 Linux and confirm
      `/dev/ps5-fan` works.
- [x] Run read-only fan checks first: zone temperature, mode get, servo get.
- [x] Test conservative write operations: pattern `0`, pattern `1`,
      target-temp `58`, then restore pattern `0`.
- [x] Run one staged curve test with conservative thresholds; on target it
      initialized at stage 3 around 65C, stepped down to stage 2 around 61C, and
      restored pattern 0 on exit.
- [x] Capture a short headless CPU benchmark trace with:
      hottest hwmon temp, EMC zone temp, CPU clocks, boost state, and fan state.
- [x] Capture GPU-visible sustained benchmark traces with: hottest hwmon temp,
      CPU/GPU clocks, boost state, and fan state. `vkmark --winsys wayland`
      produced a 900-second trace with 2230 MHz boost and no thermal cap.
- [x] Capture real game traces with: hottest hwmon temp, EMC zone temp, CPU/GPU
      clocks, boost state, and fan state. SuperTuxKart profile trace captured
      15 minutes with note `supertuxkart-lighthouse-profile`; Shadow Trial
      captured 30 minutes with note `shadow-trial-manual-benchmark`.
- [ ] Record subjective fan noise for the real-game trace.
- [x] Use `ps5gov-trace.sh` for short tuning runs with EMC zones, CPU/GPU clocks,
      boost, and fan state. Verified 10-second root trace on target.
- [x] Installed `ps5_fanctl` into the runtime library tree so installed
      `ps5gov-trace.sh` captures EMC zones instead of `?`.
- [x] Run longer 15-30 minute traces under benchmark load.
- [x] Validate GPU moving-target defaults against an fdinfo-visible sustained
      benchmark trace on real PS5 hardware.
- [x] Validate GPU moving-target defaults against a real game trace on real PS5
      hardware.
- [x] Promote a provisional fan curve based on existing measured data:
      `45:78:0,60:74:0,72:70:1,82:68:1,88:66:1`.
- [ ] Validate the provisional fan curve across broader games and user trace
      bundles; adjust if temperatures, performance, or noise regress.

## Next implementation work

- [x] Decide whether GPU busy sampling should stay fdinfo-based or move to a
      safer kernel/libdrm path for hardware busy sampling. Current decision:
      keep fdinfo plus debugfs fallback in userspace for now; do not add
      userspace MMIO busy sampling until real game/benchmark traces show both
      fdinfo and debugfs are inadequate.
- [x] Add shared-thermal CPU cooperation to `ps5_cpu_gov`.
      Rationale: CPU and GPU share the same SoC thermal envelope, fan path, and
      boost/power headroom. GPU-only thermal limiting is incomplete when the CPU
      remains at P0/3200 during sustained game load.
      Implemented conservative first policy:
      - below `75 C`: normal CPU load policy and boost can follow top-tier load.
      - `>= 85 C`: cap CPU max to P1/2560 MHz and suppress CPU boost.
      - `>= 90 C`: cap CPU max to P2/2327 MHz and suppress CPU boost.
      - below `75 C`: restore normal CPU max policy.
      Use the same temperature source style as GPU (`auto`, GPU hwmon, or
      `k10temp`) so CPU and GPU governors react to shared package/SoC
      temperature. CPU state now exposes raw demand, effective demand,
      temperature, thermal cap, and max allowed P-state.
- [ ] Validate shared-thermal CPU cooperation under Shadow Trial or a similar
      sustained game load. Compare CPU P-state, GPU thermal-cap samples, max
      hwmon/EMC temperature, average GPU load, and subjective performance/fan
      noise.
- [x] Add first-phase kernel-owned SMN mailbox transport as optional DKMS:
      `dkms/ps5-smu` creates `/dev/ps5-smu` and exposes curated ioctls for CPU
      P-state get/set, GPU GFXCLK get/set, and CPU/GPU rail voltage reads.
      Existing tools prefer `/dev/ps5-smu` when present and fall back to direct
      userspace PCI config-space SMN access otherwise.
- [x] Install `ps5-smu` through DKMS on the target, enable module autoload, and
      validate service operation through `/dev/ps5-smu`.
      Verified by unloading `ps5_smu`, running
      `/usr/lib/systemd/systemd-modules-load`, confirming `/dev/ps5-smu`
      returned, then restarting `ps5gov.service` and validating sensors.
- [ ] Extend or replace `/dev/mp1` boost ownership if consolidating boost into
      the same kernel-owned interface becomes necessary.

## Deferred by design

- [ ] Automatic DF governor. DF writes save power, but the failure mode is worse
      than CPU/GPU clock changes.
- [ ] Userspace MMIO mapping for GPU busy flags. It is not worth adding a second
      unsafe hardware access path while SMN is already the PoC risk.
- [ ] Supporting unknown AMD GPU ids automatically. The known PS5 id is `0x13fb`;
      fallback to a single AMD DRM card is enough for this repo.
