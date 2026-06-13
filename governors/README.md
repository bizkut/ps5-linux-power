# Governors (userspace PoC)

Load-adaptive CPU + GPU governors — **no kernel module**. They sense load
without touching the mailbox (CPU via `/proc/stat`, GPU via the
`drm-engine-gfx` ns counter in fdinfo) and drive the clocks through the SMU MP1
mailbox directly over PCI config space (`../smn.h`). They write only when the
target changes by a meaningful amount, so at steady idle there is zero mailbox
traffic. CPU uses fixed P-state tiers. GPU uses a moving target
between 800..2000 MHz, with burst ramping to 2230 MHz only while top-tier load
is present. Ramp down uses hysteresis. At the top load tier they also vote for
`/dev/mp1` boost mode, matching the `ps5boost` performance envelope while load
is high. On exit (Ctrl+C / SIGTERM) each clears its boost vote and restores full
normal clock (CPU P0/3200, GPU 2000).

GPU thermal guard uses gradual max-frequency reduction. In the default
`performance` profile, `-T 85 -R 75` means temps above `85 C` clear boost, start
from about `2000 MHz`, and then gradually lower the allowed max frequency while
the GPU remains hot. Below `75 C`, normal boost behavior resumes. The governor
reads the GPU hwmon node when available and falls back to `k10temp` on PS5
kernels that do not expose AMDGPU temperature.

CPU shared-thermal cooperation uses the same temperature source style. By
default, `-T 85 -R 75 -C 90` lets normal CPU load policy run below `75 C`, caps
the fastest CPU state to P1/2560 at `85 C`, caps to P2/2327 at `90 C`, and
clears the cap below `75 C`. CPU boost is not requested while a thermal cap is
active.

There is **no DF governor** on purpose — DF writes can deadlock the mailbox.

## Build & run
```sh
make                         # from the repo root: builds the tools + these
cd governors
sudo ./ps5_cpu_gov -i 400 -d 4   # CPU: load >=40%->P0, >=20%->P2, >=8%->P5, else P7
sudo ./ps5_gpu_gov -P auto
```
Options: `-i <ms>` interval, `-d <n>` low-samples before one step down,
`-v` verbose, `-q <n>` verbose hold-log interval.
Load thresholds: `-H <percent>` high, `-M <percent>` mid, `-L <percent>` low.
GPU-only: `-P auto|quiet|balanced|performance` selects a built-in preset.
`performance` is the default service profile for gaming; `auto` currently aliases
`balanced`.
CPU thermal options: `-T <C>` hot cap temperature, `-R <C>` recovery
temperature, `-C <C>` critical cap temperature, `-S auto|gpu|k10temp`.
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
Install the governor binaries under `/usr/local/lib/ps5-linux-power` and the
unit as `/etc/systemd/system/ps5gov.service`. The default config is installed at
`/etc/ps5-linux-power/ps5gov.conf`:
```sh
sudo make install-systemd
sudo systemctl daemon-reload
sudo systemctl enable --now ps5gov
```
The unit requires `/dev/mp1` for boost control and conflicts with
`ps5boost.service` and `ps5fan.service`; use one MP1 power policy owner and one
fan policy owner at a time.

Profiles:
```sh
sudo ps5govctl profile quiet
sudo ps5govctl profile powersave
sudo ps5govctl profile balanced
sudo ps5govctl profile performance
sudo ps5govctl profile auto
sudo ps5govctl performance on
sudo ps5govctl performance off
ps5govctl performance status
sudo ps5govctl reload
ps5govctl config
```

Built-in profiles:

| Profile | CPU args | GPU args |
|---------|----------|----------|
| `auto` | `-i 400 -d 4 -H 40 -M 20 -L 8` | `-P auto` |
| `quiet` | `-i 600 -d 6 -H 55 -M 30 -L 12` | `-P quiet` |
| `balanced` | `-i 400 -d 4 -H 40 -M 20 -L 8` | `-P balanced` |
| `performance` | `-i 250 -d 6 -H 25 -M 12 -L 4` | `-P performance` |

`performance` is the default service profile because this repo is tuned first for
gaming. `auto` currently aliases `balanced` for the GPU preset and uses the
balanced CPU policy; the name is reserved for future adaptive selection without
changing user config.
`performance on` is a runtime override backed by
`/run/ps5-power.performance`; it forces the performance profile until
`performance off` reloads the service back to the configured profile.

The CPU and GPU governors write structured runtime state to
`/run/ps5-power.cpu` and `/run/ps5-power.gpu`. `ps5govctl sensors` prints this
state with `cpu_state_` and `gpu_state_` prefixes. The CPU file includes load,
raw and effective demand P-state, current/target P-state, temperature, thermal
cap, max allowed P-state, boost, and downshift streak. The GPU file includes
preset, load, configured and active load method, raw fdinfo `drm-engine-gfx`
counter, temperature, current/target/desired MHz, effective range, boost,
thermal cap, and burst state. GPU load is clamped to `0.0..1.0` before policy
and status output because summed fdinfo counters can exceed wall time when
multiple DRM fdinfo entries expose overlapping `drm-engine-gfx` counters.

Manual config:
```sh
sudoedit /etc/ps5-linux-power/ps5gov.conf
sudo systemctl reload ps5gov
```
Reload keeps the parent `ps5gov.service` process alive, re-reads the config, and
replaces the CPU/GPU/fan child governors with freshly merged arguments. Use
`sudo systemctl restart ps5gov` only when you need a full service restart.
Precedence is: explicit process environment, config file, then built-in profile
defaults.

Useful GPU options:
- `-A <MHz>`, `-U <MHz>`, `-b <MHz>`, `-B <n>`: significant-change threshold,
  normal ramp, burst ramp, and burst sample count.
- `-n <MHz>`, `-x <MHz>`: requested GPU target range, clamped to `400..2230`.
- `-S auto|gpu|k10temp`: temperature source.
- `-m fdinfo|debugfs|auto|busy`: load source. `busy` is a dependency-free debugfs attempt, not a libdrm/MMIO sampler.

Target smoke checks:
```sh
./ps5gov-smoke.sh
sudo ./ps5gov-smoke.sh --service
./ps5gov-trace.sh -d 300 -i 1
sudo ./ps5gov-trace.sh -d 1800 -i 1 -n "game scene"
./ps5gov-fan-validate.sh
sudo ./ps5gov-fan-validate.sh --write-tests
```
Default mode checks local binaries, scripts, config rendering, and sensor output.
`--service` performs the root-only systemd restart/sensors/restore lifecycle.
The trace script writes a CSV for fan/GPU tuning with hwmon temperature, optional
EMC zone temperatures, CPU/GPU/fan state, boost state, service state, and GPU
load sampler diagnostics. Use `-n` or `PS5GOV_TRACE_NOTE` to add a note column
for the game, scene, or benchmark phase being captured.
Run it as root, or make `/dev/ps5-fan` readable, to capture EMC zone
temperatures.
GPU ramp validation needs an fdinfo-visible game or benchmark workload; headless
`gamescope -- vkcube` did not exercise the current fdinfo sampler on the PS5 test
target. Before adding a riskier MMIO busy sampler, capture traces that
show whether fdinfo or the debugfs fallback actually sees the workload.
The fan validation script runs read-only `/dev/ps5-fan` checks by default;
`--write-tests` is gated and compares default/cool pattern and target-temp writes.

## Fan governor

The EMC fan mode/servo details are based on the public PS5 EMC reverse
engineering notes at `https://github.com/c0w-ar/ps5-emc-re`.

Fan control is enabled by default through `FAN_ENABLED=1`. Stop other fan policy
daemons such as `ps5fan.service` so only one process owns `/dev/icc` fan control.
The default fan args are:
```sh
FAN_ARGS="-i 3000 -H 55 -L 45 -s auto"
```
If an older config file omits `FAN_ENABLED`, `run-governors.sh` still defaults to
enabled.

`ps5_fan_gov` switches the EMC fan servo pattern exposed by `/dev/icc` with
hysteresis. `-H` is the temperature that selects the cool pattern; `-L` is the
lower temperature that restores the default pattern. With the defaults, pattern 0
stays active below 55 C, pattern 1 is selected at 55 C, and pattern 0 is restored
below 45 C. `-a <pattern>` sets the
default pattern, `-C <pattern>` sets the cool pattern, and `-f <pattern>` forces
one pattern once. `-s auto` tracks the hottest readable hwmon temperature instead
of only `k10temp`. A named hwmon sensor or direct `/sys/.../temp*_input` path can
still be supplied.
`-q <n>` limits repetitive verbose `hold` messages while preserving immediate
toggle/failsafe/restore logs.

Optional staged fan curve:
```sh
FAN_CURVE="45:78:0,60:74:0,72:70:1,82:68:1,88:66:1"
FAN_HYSTERESIS=4
```
Each point is `temp_up:target_temp:servo_pattern`. With the DKMS `/dev/ps5-fan`
device, curve stages set EMC target temperature and servo pattern. Without it,
the governor still uses the curve's servo pattern through legacy `/dev/icc`.
The default curve is provisional but based on current PS5 traces: Shadow Trial
reached 84 C with the default cool pattern, target-temp `70 C` held roughly
`70-72 C` under that load, and `58 C` was too loud.

For user reports, collect a persistent bundle while the workload is running:
```sh
sudo ./ps5gov-collect-trace.sh -d 1800 -i 1 -n "game scene"
```
The script writes a CSV plus config/sensor snapshots and prints a `.tar.gz`
bundle path under `~/ps5gov-traces`.

The fan governor writes structured state to `/run/ps5-power.fan`; `ps5govctl
sensors` prints it with a `fan_state_` prefix. On normal exit it restores the
default pattern. If sensor reads fail repeatedly while the default pattern is
active, it fails safe by selecting the cool pattern.
