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

No kernel headers are required. The repo carries small fallback UAPI headers for
stripped/custom PS5 Linux images.

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

`auto` is the default service profile. It currently uses the balanced CPU policy
and the GPU governor's `-P auto` preset, which is an alias for `balanced`; the
name is reserved so it can become truly adaptive later without changing user
config.

The GPU preset can still be narrowed with `GPU_ARGS`, including `-n <MHz>` and
`-x <MHz>` for a requested min/max GPU target range. Runtime CPU/GPU governor
state is written to `/run/ps5-power.cpu` and `/run/ps5-power.gpu`, then shown by
`ps5govctl sensors`.

Profile summary:

| Profile | CPU policy | GPU policy |
| `auto` | balanced defaults | `-P auto` |
| `quiet` | slower sampling, higher load thresholds | `-P quiet` |
| `balanced` | default daily profile | `-P balanced` |
| `performance` | faster sampling, lower load thresholds | `-P performance` |

Common operations:

```sh
sudo ps5govctl profile quiet
sudo ps5govctl profile balanced
sudo ps5govctl profile performance
sudo ps5govctl profile auto
ps5govctl config
ps5govctl sensors
sudo ps5govctl stop-boost
sudo ps5govctl restore
```

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

## Fan Policy

Fan control is intentionally separate from CPU/GPU clock control.

`ps5gov.conf` defaults to:

```text
FAN_ENABLED=0
```

Enable fan governor testing only when you explicitly want this service to own fan
policy. Do not run multiple fan policy daemons at the same time.

When `FAN_ENABLED=1`, `ps5_fan_gov` uses hysteresis (`-H` high temperature,
`-L` low temperature) to switch the EMC fan servo pattern exposed by the current
`/dev/icc` ioctl. Its `auto` sensor mode tracks the hottest readable hwmon
temperature, writes structured state to `/run/ps5-power.fan`, restores the
default pattern on normal exit, and fails safe to the cool pattern after repeated
sensor read errors.

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
