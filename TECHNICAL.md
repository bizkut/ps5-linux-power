# Technical Notes

This document holds the lower-level details that are useful when developing,
tuning, or debugging `ps5-linux-cpuclock`.

For normal install and profile use, start with [README.md](README.md).

## Project Role

This repo is the userspace development and validation tree for PS5 APU power
control work. It provides small root-only tools for reading and changing CPU
P-states, GPU GFXCLK, DF/memory P-states, and the MP1 boost envelope, plus
load-adaptive governors that can run as a systemd service.

The long-term production target is the kernel driver/patch stack carried by
`ps5-linux-patches`. This repo remains useful for:

- verifying MP1/SMU commands on real PS5 hardware
- tuning CPU/GPU governor thresholds
- validating boost behavior under game and desktop workloads
- collecting telemetry for the kernel driver path
- recovering clocks/boost state during testing

## Safety Model

The userspace tools talk to the SMU MP1 mailbox through PCI config-space SMN
index/data registers on `0000:00:00.0`.

The tools serialize against each other with:

```text
/run/lock/ps5-power.lock
```

That prevents two userspace policy writers from colliding with each other, but
it does not take the kernel's internal SMN lock. Kernel users such as `k10temp`
or EDAC may still touch SMN at the same time. This is the main reason this
remains a userspace proof of concept instead of the final production mechanism.

The kernel driver version should use kernel SMN helpers/locking and expose stable
interfaces for clock, boost, fan, and telemetry control.

## Performance Impact

The build/profile workflow changes do not lower PS5 runtime performance by
themselves. They only decide which binaries, service files, and profiles are
installed.

The governors can lower clocks intentionally when the system is idle or lightly
loaded, then ramp back up under load. Under high load, the default gaming policy
selects high CPU stages, moves the GPU target upward, and can vote for MP1 boost.

Manual low clocks can reduce performance until restored:

```sh
sudo ./ps5_cpu set 0xff 0
sudo ./ps5_gpu reset
sudo ./ps5_df set 3 force
sudo ./ps5_boost off
```

## Build Details

The top-level `make` builds:

- `ps5_cpu`
- `ps5_gpu`
- `ps5_df`
- `ps5_boost`
- `governors/ps5_cpu_gov`
- `governors/ps5_gpu_gov`
- `governors/ps5_fan_gov`

The repo carries small fallback UAPI headers for stripped/custom PS5 Linux
images. If the target libc headers are missing Linux UAPI pieces such as
`linux/errno.h`, the Makefiles automatically add:

```text
/lib/modules/$(uname -r)/build/include/uapi
```

when that path exists.

## Manual Tools

All mailbox tools require root.

### CPU P-State

```sh
sudo ./ps5_cpu get 0
sudo ./ps5_cpu set 0xff 7
sudo ./ps5_cpu set 0xff 0
```

`set` takes a core bitmask. `0xff` means all eight cores.

Known P-states:

| P-state | Frequency |
| --- | --- |
| `0` | 3200 MHz |
| `1` | 2560 MHz |
| `2` | 2327 MHz |
| `3` | 1969 MHz |
| `4` | 1829 MHz |
| `5` | 1600 MHz |
| `6` | 1280 MHz |
| `7` | 800 MHz |

### GPU GFXCLK

```sh
sudo ./ps5_gpu get
sudo ./ps5_gpu set 1500 force
sudo ./ps5_gpu reset
```

The accepted range is currently `400..2380` MHz. GPU clock changes are usually
the largest power lever.

### DF / Memory Fabric

```sh
sudo ./ps5_df get
sudo ./ps5_df set 2 force
sudo ./ps5_df set 3 force
```

Use `df2` and `df3` only for normal testing. Lower DF states are experimental and
can wedge the mailbox until reboot.

### MP1 Boost

```sh
sudo ./ps5_boost on
sudo ./ps5_boost off
./ps5_boost status
```

The governors normally manage boost votes themselves. `ps5_boost` is mainly for
manual testing and recovery.

## Governor Behavior

The governor binaries live in `governors/`.

They monitor load and write only when the target changes by a meaningful amount.
At steady idle there is no continuous mailbox traffic. CPU uses fixed clock
stages. GPU uses a moving target frequency with burst ramping. Ramp-up is fast;
ramp-down uses hysteresis.

Built-in profiles:

| Profile | CPU policy | GPU policy |
| --- | --- | --- |
| `auto` | reserved adaptive profile, currently balanced policy | `-P auto` |
| `quiet` | slower sampling, higher load thresholds | `-P quiet` |
| `balanced` | lower-power daily profile | `-P balanced` |
| `performance` | default gaming profile, faster sampling, lower load thresholds | `-P performance` |

`performance` is the default service profile because this repo is tuned first
for gaming. `auto` currently uses the balanced CPU policy and the GPU governor's
`-P auto` preset, which is an alias for `balanced`.

Runtime CPU/GPU governor state is written to:

```text
/run/ps5-power.cpu
/run/ps5-power.gpu
```

`ps5govctl sensors` prints those fields with `cpu_state_` and `gpu_state_`
prefixes.

## Runtime Reload and Overrides

`ps5govctl profile <name>` writes:

```text
/etc/ps5-linux-cpuclock/ps5gov.conf
```

Then it reloads the running service. Reload keeps the parent service process
alive, re-reads config, and replaces the CPU/GPU/fan child governors with freshly
merged arguments. Use `restart` when you want a full systemd stop/start.

`ps5govctl performance on` is a Cyan-style runtime override. It writes:

```text
/run/ps5-power.performance
```

Then it reloads the service and forces the `performance` profile without
rewriting persistent config. `performance off` clears the override and reloads
back to the configured profile.

## Fan Implementation

Fan control is intentionally separate from CPU/GPU clock control, but the default
systemd service starts the fan governor alongside the CPU and GPU governors.

The EMC fan mode/servo direction is based on the public PS5 EMC reverse
engineering notes at <https://github.com/c0w-ar/ps5-emc-re>.

`ps5gov.conf` defaults to:

```sh
FAN_ENABLED=1
FAN_ARGS="-i 3000 -H 55 -L 45 -s auto"
```

That starts the fan governor by default but keeps the default EMC servo pattern
active below 55 C. At 55 C it switches to the cool pattern, then returns to the
default pattern after temperature falls below 45 C.

When `FAN_ENABLED=1`, `ps5_fan_gov` uses hysteresis (`-H` high temperature, `-L`
low temperature) to switch the EMC fan servo pattern exposed by the current
`/dev/icc` ioctl. Its `auto` sensor mode tracks the hottest readable hwmon
temperature, writes structured state to `/run/ps5-power.fan`, restores the
default pattern on normal exit, and fails safe to the cool pattern after repeated
sensor read errors.

For richer EMC fan control without patching kernel source, see the optional DKMS
module in `dkms/ps5-icc-fan`. It exposes a curated `/dev/ps5-fan` interface for
fan mode, thermal zone temperature, servo status, servo pattern, and target
temperature. It requires the target kernel to export `icc_query()`; check with:

```sh
grep ' icc_query$' /proc/kallsyms
```

When `/dev/ps5-fan` exists, `ps5_fan_gov` uses it for servo pattern changes and
falls back to the older `/dev/icc` pattern ioctl otherwise.

Fan curves are optional and provisional until tuned on real hardware:

```sh
FAN_CURVE="45:62:0,50:58:0,58:55:1,65:52:1,72:48:1"
FAN_HYSTERESIS=4
```

Each point is `temp_up:target_temp:servo_pattern`. With `/dev/ps5-fan`, the
governor sets EMC target temperature plus the servo pattern. Without it, the
curve still controls servo pattern only.

## Validation Notes

After building on the target:

```sh
governors/ps5gov-smoke.sh
sudo governors/ps5gov-smoke.sh --service
```

Default smoke mode checks scripts, binaries, config rendering, and sensor/control
plumbing without restarting services. `--service` reloads systemd, restarts
`ps5gov.service`, reads sensors, then runs `ps5govctl restore` to stop the
service and restore CPU/GPU defaults.

For target-side tuning traces:

```sh
sudo governors/ps5gov-trace.sh -d 300 -i 1
```

The trace CSV records hottest hwmon temperature, optional EMC zone temperatures
from `/dev/ps5-fan`, CPU/GPU/fan runtime state, boost state, and service state.

For DKMS fan behavior validation:

```sh
governors/ps5gov-fan-validate.sh
sudo governors/ps5gov-fan-validate.sh --write-tests
```

The read-only mode checks `/dev/ps5-fan` access. `--write-tests` compares
default/cool servo patterns and MAINSOC target-temperature behavior.

GPU ramp validation needs a game or benchmark session that shows fdinfo-visible
`drm-engine-gfx` activity. Headless `gamescope -- vkcube` launched Vulkan on the
PS5 test target but did not produce governor-visible GPU load.

## Production Driver Direction

The production-quality path is to move stable behavior into kernel patches:

- MP1 mailbox access under kernel locking
- PS5 CPU clock/boost controls
- GPU telemetry and GFXCLK reporting/control where appropriate
- safe hwmon/sysfs interfaces
- integration with the CachyOS BORE PS5 kernel profile

Userspace remains useful for experiments, service policy, and recovery tools,
but direct SMN writes from userspace should not be the final interface for broad
distribution.

## Caution Notes

- Tested on real PS5 hardware.
- Requires root for mailbox writes.
- Do not run multiple mailbox policy writers at once.
- Do not run multiple fan policy daemons at once.
- Keep DF testing conservative.
- Be ready to reboot when testing experimental mailbox commands.
