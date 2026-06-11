# Governors (userspace PoC)

Load-adaptive CPU + GPU governors — **no kernel module**. They sense load
without touching the mailbox (CPU via `/proc/stat`, GPU via the
`drm-engine-gfx` ns counter in fdinfo) and drive the clocks through the SMU MP1
mailbox directly over PCI config space (`../smn.h`). They write only when the
target changes by a meaningful amount, so at steady idle there is zero mailbox
traffic. CPU uses fixed P-state tiers. GPU uses a Cyan-style moving target
between 800..2000 MHz, with burst ramping to 2230 MHz only while top-tier load
is present. Ramp down uses hysteresis. At the top load tier they also vote for
`/dev/mp1` boost mode, matching the `ps5boost` performance envelope while load
is high. On exit (Ctrl+C / SIGTERM) each clears its boost vote and restores full
normal clock (CPU P0/3200, GPU 2000).

GPU thermal guard is borrowed from Cyan's model: above `-T`, boost is disabled
and the GPU is capped at 1500 MHz until temperature falls to `-R`. It reads the
GPU hwmon node when available and falls back to `k10temp` on PS5 kernels that do
not expose AMDGPU temperature.

There is **no DF governor** on purpose — DF writes can deadlock the mailbox.

## Build & run
```sh
make                         # from the repo root: builds the tools + these
cd governors
sudo ./ps5_cpu_gov -i 400 -d 4   # CPU: load >=40%->P0, >=20%->P2, >=8%->P5, else P7
sudo ./ps5_gpu_gov -P auto
```
Options: `-i <ms>` interval, `-d <n>` low-samples before one step down, `-v` verbose.
Load thresholds: `-H <percent>` high, `-M <percent>` mid, `-L <percent>` low.
GPU-only: `-P auto|quiet|balanced|performance` selects a built-in preset.
`auto` currently aliases `balanced` and is the default service profile.
`-A <MHz>` significant-change threshold, `-U <MHz>` normal ramp step,
`-b <MHz>` burst ramp step, `-B <n>` high-load samples before burst,
`-n <MHz>` minimum GPU target, `-x <MHz>` maximum GPU target,
`-T <C>` thermal cap temperature, `-R <C>` recovery temperature.

## Both at once
`run-governors.sh` starts both with **deliberately different intervals** (400 vs
700 ms). They also serialize on a shared `flock`, so they can't collide on the
mailbox:
```sh
sudo ./run-governors.sh      # foreground; Ctrl+C stops + restores clocks
```

## systemd
Install the governor binaries under `/usr/local/lib/ps5-linux-cpuclock` and the
unit as `/etc/systemd/system/ps5gov.service`. The default config is installed at
`/etc/ps5-linux-cpuclock/ps5gov.conf`:
```sh
sudo make install-systemd
sudo systemctl daemon-reload
sudo systemctl enable --now ps5gov
```
The unit requires `/dev/mp1` for boost control and conflicts with
`ps5boost.service`; use one MP1 power policy owner at a time.

Profiles:
```sh
sudo ps5govctl profile quiet
sudo ps5govctl profile balanced
sudo ps5govctl profile performance
sudo ps5govctl profile auto
ps5govctl config
```

Built-in profiles:

| Profile | CPU args | GPU args |
|---------|----------|----------|
| `auto` | `-i 400 -d 4 -H 40 -M 20 -L 8` | `-P auto` |
| `quiet` | `-i 600 -d 6 -H 55 -M 30 -L 12` | `-P quiet` |
| `balanced` | `-i 400 -d 4 -H 40 -M 20 -L 8` | `-P balanced` |
| `performance` | `-i 250 -d 6 -H 25 -M 12 -L 4` | `-P performance` |

`auto` is the default service profile. It currently aliases `balanced` for the
GPU preset and uses the balanced CPU policy; the name is reserved for future
adaptive selection without changing user config.

The CPU and GPU governors write structured runtime state to
`/run/ps5-power.cpu` and `/run/ps5-power.gpu`. `ps5govctl sensors` prints this
state with `cpu_state_` and `gpu_state_` prefixes. The GPU file includes preset,
load, temperature, current/target/desired MHz, effective range, boost, thermal
cap, and burst state.

Manual config:
```sh
sudoedit /etc/ps5-linux-cpuclock/ps5gov.conf
sudo systemctl restart ps5gov
```
Precedence is: systemd environment override, config file, then built-in profile
defaults.

Useful GPU options:
- `-A <MHz>`, `-U <MHz>`, `-b <MHz>`, `-B <n>`: significant-change threshold,
  normal ramp, burst ramp, and burst sample count.
- `-n <MHz>`, `-x <MHz>`: requested GPU target range, clamped to `400..2230`.
- `-S auto|gpu|k10temp`: temperature source.
- `-m fdinfo|debugfs|auto|busy`: load source. `busy` is a dependency-free debugfs attempt, not Cyan's libdrm MMIO sampler.

Target smoke checks:
```sh
./ps5gov-smoke.sh
sudo ./ps5gov-smoke.sh --service
```
Default mode checks local binaries, scripts, config rendering, and sensor output.
`--service` performs the root-only systemd restart/sensors/restore lifecycle.

## Fan governor

Fan control is opt-in through `FAN_ENABLED=1`. With the default `FAN_ENABLED=0`,
keep the normal platform fan policy service running. If `FAN_ENABLED=1`, stop
other fan policy daemons such as `ps5fan.service` so only one process owns
`/dev/icc` fan control. The default fan args are:
```sh
FAN_ARGS="-i 3000 -H 58 -L 48 -s auto"
```

`ps5_fan_gov` switches the EMC fan servo pattern exposed by `/dev/icc` with
hysteresis. `-H` is the temperature that selects the cool pattern; `-L` is the
lower temperature that restores the default pattern. `-a <pattern>` sets the
default pattern, `-C <pattern>` sets the cool pattern, and `-f <pattern>` forces
one pattern once. `-s auto` tracks the hottest readable hwmon temperature instead
of only `k10temp`. A named hwmon sensor or direct `/sys/.../temp*_input` path can
still be supplied.

The fan governor writes structured state to `/run/ps5-power.fan`; `ps5govctl
sensors` prints it with a `fan_state_` prefix. On normal exit it restores the
default pattern. If sensor reads fail repeatedly while the default pattern is
active, it fails safe by selecting the cool pattern.
