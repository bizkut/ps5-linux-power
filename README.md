# ps5-linux-cpuclock

PS5 CPU/GPU clock, boost, and telemetry tooling for Linux.

This repository is the userspace development and validation tree for PS5 APU
power-control work. It provides small root-only tools for reading and changing
CPU P-states, GPU GFXCLK, DF/memory P-states, and the MP1 boost envelope, plus
load-adaptive governors that can be used as a systemd service.

The long-term production target is the kernel driver/patch stack carried by
`ps5-linux-patches`. This repo remains useful for testing mailbox behavior,
tuning policies, and validating governor logic before moving the stable pieces
into the kernel.

## What It Does

| Component | Purpose | Typical use |
| --- | --- | --- |
| `ps5_cpu` | Read/set CPU core P-state | Manual CPU clock testing |
| `ps5_gpu` | Read/set GPU GFXCLK | Manual GPU cap/testing |
| `ps5_df` | Read/set DF memory/fabric P-state | Experimental power testing |
| `ps5_boost` | Toggle/query `/dev/mp1` boost envelope | Recovery and manual boost tests |
| `ps5gov` | CPU/GPU load-adaptive governors | Daily userspace policy service |
| `ps5govctl` | Configure, inspect, and recover governors | Service operations |

All mailbox tools require root.

## Current Role

This is not the final production driver. It is the userspace control and tuning
environment for PS5 Linux power management.

Use this repo to:

- verify MP1/SMU commands on real PS5 hardware
- tune CPU/GPU governor thresholds
- validate boost behavior under game and desktop workloads
- collect telemetry for the kernel driver patch
- recover clocks/boost state during testing

For packaged CachyOS BORE kernel builds, use the patch flow in
`ps5-linux-patches` and `ps5-linux-image`. This repository supplies the behavior
and driver logic that gets adapted into those patches.

## Safety Model

The userspace tools talk to the SMU MP1 mailbox through PCI config-space SMN
index/data registers on `0000:00:00.0`.

The tools serialize against each other with:

```text
/run/lock/ps5-power.lock
```

That prevents two userspace policy writers from colliding, but it does not take
the kernel's internal SMN lock. Kernel users such as `k10temp` or EDAC may still
touch SMN at the same time. That is the main reason this remains a userspace
PoC instead of the final production mechanism.

The kernel driver version should use the kernel SMN helpers/locking and expose
stable interfaces for clock, boost, and telemetry control.

## Performance Impact

The build/profile workflow changes do not lower PS5 runtime performance by
themselves. They only decide which kernel/profile gets built.

The governors can lower clocks intentionally when the system is idle or lightly
loaded, then ramp back up under load. That may reduce idle power and heat. Under
high load, the default governor policy selects high CPU stages, moves the GPU
target upward, and can vote for MP1 boost, so performance should recover when
demand is present.

Manual low clocks can reduce performance until restored:

```sh
sudo ./ps5_cpu set 0xff 0
sudo ./ps5_gpu reset
sudo ./ps5_df set 3 force
sudo ./ps5_boost off
```

## Build

```sh
make
```

This builds:

- `ps5_cpu`
- `ps5_gpu`
- `ps5_df`
- `ps5_boost`
- `governors/ps5_cpu_gov`
- `governors/ps5_gpu_gov`
- `governors/ps5_fan_gov`

The repo carries small fallback UAPI headers for stripped/custom PS5 Linux
images. If the target's libc headers are missing Linux UAPI pieces such as
`linux/errno.h`, the Makefiles automatically add
`/lib/modules/$(uname -r)/build/include/uapi` when it exists.

## Manual Tools

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

Use `df2` and `df3` only for normal testing. Lower DF states are experimental
and can wedge the mailbox until reboot.

### MP1 Boost

```sh
sudo ./ps5_boost on
sudo ./ps5_boost off
./ps5_boost status
```

The governors normally manage boost votes themselves. `ps5_boost` is mainly for
manual testing and recovery.

## Governors

The governor binaries live in `governors/`.

They monitor load and write only when the target changes by a meaningful amount.
At steady idle there is no continuous mailbox traffic. CPU uses fixed clock
stages; GPU uses a moving target frequency with burst ramping. Ramp-up is fast;
ramp-down uses hysteresis.

Run in the foreground:

```sh
cd governors
sudo ./run-governors.sh
```

Install as a service:

```sh
sudo make install-systemd
sudo systemctl daemon-reload
sudo systemctl enable --now ps5gov
```

Configuration is installed at:

```text
/etc/ps5-linux-cpuclock/ps5gov.conf
```

Built-in profiles:

- `auto`
- `quiet`
- `balanced`
- `performance`

`performance` is the default service profile because this repo is tuned first for
gaming. `auto` currently uses the balanced CPU policy and the GPU governor's
`-P auto` preset, which is an alias for `balanced`; the name is reserved so it
can become truly adaptive later without changing user config.

The GPU preset can still be narrowed with `GPU_ARGS`, including `-n <MHz>` and
`-x <MHz>` for a requested min/max GPU target range. Runtime CPU/GPU governor
state is written to `/run/ps5-power.cpu` and `/run/ps5-power.gpu`, then shown by
`ps5govctl sensors`.

Profile summary:

| Profile | CPU policy | GPU policy |
| `auto` | reserved adaptive profile, currently balanced policy | `-P auto` |
| `quiet` | slower sampling, higher load thresholds | `-P quiet` |
| `balanced` | lower-power daily profile | `-P balanced` |
| `performance` | default gaming profile, faster sampling, lower load thresholds | `-P performance` |

Common operations:

```sh
sudo ps5govctl profile quiet
sudo ps5govctl profile balanced
sudo ps5govctl profile performance
sudo ps5govctl profile auto
sudo ps5govctl performance on
sudo ps5govctl performance off
ps5govctl performance status
sudo ps5govctl reload
ps5govctl config
ps5govctl sensors
sudo ps5govctl stop-boost
sudo ps5govctl restore
```

`profile` writes `/etc/ps5-linux-cpuclock/ps5gov.conf` and reloads the running
service. Reload keeps the parent service process alive, re-reads config, and
replaces the CPU/GPU/fan child governors with freshly merged arguments. Use
`restart` when you want a full systemd stop/start.

`performance on` is a Cyan-style runtime override. It writes
`/run/ps5-power.performance`, reloads the service, and forces the `performance`
profile without rewriting persistent config. `performance off` clears the
override and reloads back to the configured profile.

`restore` stops the governor service, clears boost, restores CPU P0, and resets
GPU GFXCLK to 2000 MHz.

## Smoke Test

After building on the target, run:

```sh
governors/ps5gov-smoke.sh
```

That checks scripts, binaries, config rendering, and sensor/control plumbing
without restarting services. On a real PS5 install, the root-only lifecycle check
is:

```sh
sudo governors/ps5gov-smoke.sh --service
```

`--service` reloads systemd, restarts `ps5gov.service`, reads sensors, then runs
`ps5govctl restore` to stop the service and restore CPU/GPU defaults.

For target-side tuning traces:

```sh
governors/ps5gov-trace.sh -d 300 -i 1
```

The trace CSV records hottest hwmon temperature, optional EMC zone temperatures
from `/dev/ps5-fan`, CPU/GPU/fan runtime state, boost state, and service state.
Run it as root, or make `/dev/ps5-fan` readable, if you want EMC zone columns
instead of `?`.
For GPU governor validation, use a game or benchmark session that shows
fdinfo-visible `drm-engine-gfx` activity; headless `gamescope -- vkcube` launched
Vulkan on the PS5 test target but did not produce governor-visible GPU load.

For DKMS fan behavior validation, run read-only checks first:

```sh
governors/ps5gov-fan-validate.sh
```

After `/dev/ps5-fan` reads are sane, gated writes can compare default/cool servo
patterns and MAINSOC target-temperature behavior:

```sh
sudo governors/ps5gov-fan-validate.sh --write-tests
```

## Fan Policy

Fan control is intentionally separate from CPU/GPU clock control.
The EMC fan mode/servo direction is based on the public PS5 EMC reverse
engineering notes at `https://github.com/c0w-ar/ps5-emc-re`.

`ps5gov.conf` defaults to:

```text
FAN_ENABLED=1
FAN_ARGS="-i 3000 -H 55 -L 45 -s auto"
```

That starts the fan governor by default but keeps the default EMC servo pattern
active below 55 C. At 55 C it switches to the cool pattern, then returns to the
default pattern after temperature falls below 45 C. Do not run multiple fan
policy daemons at the same time; `ps5gov.service` conflicts with
`ps5fan.service` for this reason.
If an older config file omits `FAN_ENABLED`, the service still defaults to
enabled.

When `FAN_ENABLED=1`, `ps5_fan_gov` uses hysteresis (`-H` high temperature,
`-L` low temperature) to switch the EMC fan servo pattern exposed by the current
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

## Production Driver Direction

The production-quality path is to move the stable parts of this behavior into
kernel patches:

- MP1 mailbox access under kernel locking
- PS5 CPU clock/boost controls
- GPU telemetry and GFXCLK reporting/control where appropriate
- safe hwmon/sysfs interfaces
- integration with the CachyOS BORE PS5 kernel profile

Userspace remains useful for experiments, service policy, and recovery tools,
but direct SMN writes from userspace should not be the final interface for broad
distribution.

## Notes

- Tested on real PS5 hardware.
- Requires root for mailbox writes.
- Do not run multiple mailbox policy writers at once.
- Keep DF testing conservative.
- Be ready to reboot when testing experimental mailbox commands.
