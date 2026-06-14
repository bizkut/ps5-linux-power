# ps5-linux-power

PS5 Linux power tools for gaming-focused CPU, GPU, boost, and fan control.

The main thing most users want is `ps5gov`: a systemd service that runs the CPU,
GPU, and fan governors together. It defaults to the `performance` profile because
this project is tuned first for gaming.

## What You Get

| Tool | What it is for |
| --- | --- |
| `ps5gov` | Automatic CPU/GPU/fan policy service |
| `ps5govctl` | Change profiles, reload config, inspect sensors, recover defaults |
| `ps5_cpu` | Manual CPU P-state testing |
| `ps5_gpu` | Manual GPU clock testing |
| `ps5_df` | Manual memory/fabric P-state testing |
| `ps5_boost` | Manual MP1 boost testing/recovery |

Use the governor service for normal use. The manual tools are mostly for testing,
tuning, and recovery.

The optional `dkms/ps5-smu` module provides a kernel-owned `/dev/ps5-smu`
transport for the validated CPU/GPU/voltage mailbox commands. When loaded, the
tools prefer it over direct userspace PCI config-space access.

## Quick Install

Prebuilt release packages are available for Debian/Ubuntu, Fedora, and Arch
systems, including Bazzite through `rpm-ostree`. Install commands are included
in each GitHub Release. Package details are documented in
[docs/PACKAGING.md](docs/PACKAGING.md).

Build and install the governor service on the PS5:

```sh
make
sudo make install-systemd
sudo systemctl daemon-reload
sudo systemctl enable --now ps5gov
```

Check that it is running:

```sh
systemctl status ps5gov
ps5govctl config
ps5govctl sensors
```

## Uninstall

First stop the governor and restore normal clocks:

```sh
sudo ps5govctl restore || true
sudo systemctl disable --now ps5gov || true
sudo systemctl daemon-reload
```

If you installed a release package, remove the package that matches your system:

```sh
# Debian / Ubuntu
sudo apt remove ps5-linux-power ps5-linux-power-dkms

# Fedora
sudo dnf remove ps5-linux-power ps5-linux-power-dkms akmod-ps5-linux-power

# Bazzite / rpm-ostree
sudo rpm-ostree uninstall ps5-linux-power akmod-ps5-linux-power
sudo systemctl reboot

# Arch
sudo pacman -R ps5-linux-power ps5-linux-power-dkms
```

If you installed manually with `sudo make install-systemd`, remove the installed
files directly:

```sh
sudo systemctl disable --now ps5gov || true
sudo rm -f /etc/systemd/system/ps5gov.service
sudo rm -f /usr/local/bin/ps5govctl
sudo rm -rf /usr/local/lib/ps5-linux-power
sudo systemctl daemon-reload
sudo systemctl reset-failed ps5gov || true
```

Optional DKMS/akmod cleanup:

```sh
sudo modprobe -r ps5_icc_fan ps5_smu 2>/dev/null || true
sudo dkms remove -m ps5-smu --all 2>/dev/null || true
sudo dkms remove -m ps5-icc-fan --all 2>/dev/null || true
```

The config is intentionally left behind so tuned profiles are not lost. Remove it
only when you want a full purge:

```sh
sudo rm -rf /etc/ps5-linux-power
sudo rm -rf /etc/ps5-linux-cpuclock   # old pre-rename config path, if present
```

## Default Behavior

The installed service defaults to:

```sh
PROFILE=performance
FAN_ENABLED=1
```

That means:

- CPU policy favors fast ramp-up for games.
- GPU policy uses the `performance` preset.
- Fan governor starts automatically.
- The fan still uses the default EMC pattern until temperature reaches the
  configured threshold.
- `ps5gov.service` conflicts with older standalone boost/fan services so only
  one policy owner is active.

Default fan settings:

```sh
FAN_ARGS="-i 3000 -H 55 -L 45 -s auto"
```

With those settings, the fan governor is enabled at boot, keeps the default fan
pattern below 55 C, switches to the cool pattern at 55 C, then restores the
default pattern below 45 C.

## Profiles

Change profile without editing config by hand:

```sh
sudo ps5govctl profile performance
sudo ps5govctl profile balanced
sudo ps5govctl profile quiet
sudo ps5govctl profile powersave
sudo ps5govctl profile auto
```

| Profile | Use it when |
| --- | --- |
| `performance` | Gaming default; fastest response and highest performance bias |
| `balanced` | Daily use with lower idle power/heat |
| `quiet` | Lower noise and slower ramp-up |
| `powersave` | Lowest idle bias; GPU can drop to `400 MHz` and boost response is conservative |
| `auto` | Reserved adaptive mode; currently maps to the balanced policy |

Temporary performance override:

```sh
sudo ps5govctl performance on
sudo ps5govctl performance off
ps5govctl performance status
```

`performance on` forces the performance profile until you turn it off. It does
not rewrite the saved profile in `/etc/ps5-linux-power/ps5gov.conf`.

## Common Operations

Reload config without a full service restart:

```sh
sudo ps5govctl reload
```

Show the active merged config:

```sh
ps5govctl config
```

Show current governor state:

```sh
ps5govctl sensors
```

Stop the governor and restore safe defaults:

```sh
sudo ps5govctl restore
```

`restore` stops `ps5gov`, clears boost, restores CPU P0, and resets GPU GFXCLK.

## Fan Control

Fan control is part of `ps5gov` by default.

The fan governor reads the hottest available temperature sensor by default and
switches the PS5 EMC fan servo pattern with hysteresis. It writes runtime state
to:

```sh
/run/ps5-power.fan
```

Optional staged curves are supported:

```sh
FAN_CURVE="45:78:0,60:74:0,72:70:1,82:68:1,88:66:1"
FAN_HYSTERESIS=4
```

Each curve point is:

```text
temperature_up:target_temperature:servo_pattern
```

The richer `/dev/ps5-fan` interface is provided by the optional DKMS module in
`dkms/ps5-icc-fan`. Without that module, the fan governor falls back to the older
`/dev/icc` interface.

The default curve is provisional but based on measured PS5 data: Shadow Trial
reached 84 C with the default cool pattern, target-temp `70 C` held roughly
`70-72 C` under that load, and `58 C` was too loud.

The EMC fan behavior is based on the public PS5 EMC reverse engineering work at
<https://github.com/c0w-ar/ps5-emc-re>.

## GPU Thermal Behavior

In the default `performance` GPU profile, thermal protection uses a gradual
max-frequency reduction:

| Temperature | Governor behavior |
| --- | --- |
| `> 85 C` | clears boost, starts from about `2000 MHz`, then gradually lowers the allowed max frequency |
| `< 75 C` | restores the requested max and normal boost behavior |

On this PS5, Shadow of the Tomb Raider Trial reached the warm cap during a
30-minute benchmark trace. Manual fan target testing found `70 C` to be a good
quiet target under that load: it held roughly `70-72 C` with GPU boost active
and no thermal cap, while `58 C` was too loud.

## CPU Thermal Behavior

CPU and GPU share the same SoC thermal envelope, so the CPU governor also watches
APU temperature. By default it keeps normal load-based CPU policy below `75 C`,
caps the fastest CPU state to P1/2560 at `85 C`, caps to P2/2327 at `90 C`, and
restores normal policy below `75 C`. CPU boost is disabled while a thermal cap is
active.

## Manual Clock Tools

For quick manual checks:

```sh
sudo ./ps5_cpu get 0
sudo ./ps5_gpu get
sudo ./ps5_df get
./ps5_boost status
```

Restore common defaults manually:

```sh
sudo ./ps5_cpu set 0xff 0
sudo ./ps5_gpu reset
sudo ./ps5_df set 3 force
sudo ./ps5_boost off
```

Stop `ps5gov` before doing manual clock experiments:

```sh
sudo systemctl stop ps5gov
```

Start it again when done:

```sh
sudo systemctl start ps5gov
```

## More Docs

- [docs/USAGE.md](docs/USAGE.md) has command examples.
- [docs/TECHNICAL.md](docs/TECHNICAL.md) has the safety model, implementation details, and
  validation notes.
- [governors/README.md](governors/README.md) documents governor-specific flags.
- [dkms/ps5-icc-fan/README.md](dkms/ps5-icc-fan/README.md) documents the optional
  DKMS fan module.
- [dkms/ps5-smu/README.md](dkms/ps5-smu/README.md) documents the optional DKMS
  SMU mailbox module.

## Status

This repo is a userspace development and validation tree for PS5 Linux power
management. It has been tested on real PS5 hardware, but direct mailbox control
still requires care. Do not run multiple power/fan policy daemons at the same
time.
