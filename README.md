# ps5-linux-cpuclock

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

## Quick Install

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
sudo ps5govctl profile auto
```

| Profile | Use it when |
| --- | --- |
| `performance` | Gaming default; fastest response and highest performance bias |
| `balanced` | Daily use with lower idle power/heat |
| `quiet` | Lower noise and slower ramp-up |
| `auto` | Reserved adaptive mode; currently maps to the balanced policy |

Temporary performance override:

```sh
sudo ps5govctl performance on
sudo ps5govctl performance off
ps5govctl performance status
```

`performance on` forces the performance profile until you turn it off. It does
not rewrite the saved profile in `/etc/ps5-linux-cpuclock/ps5gov.conf`.

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
FAN_CURVE="45:62:0,50:58:0,58:55:1,65:52:1,72:48:1"
FAN_HYSTERESIS=4
```

Each curve point is:

```text
temperature_up:target_temperature:servo_pattern
```

The richer `/dev/ps5-fan` interface is provided by the optional DKMS module in
`dkms/ps5-icc-fan`. Without that module, the fan governor falls back to the older
`/dev/icc` interface.

The EMC fan behavior is based on the public PS5 EMC reverse engineering work at
<https://github.com/c0w-ar/ps5-emc-re>.

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

## Validation

Run the local smoke checks:

```sh
governors/ps5gov-smoke.sh
```

Run the service lifecycle check on the PS5:

```sh
sudo governors/ps5gov-smoke.sh --service
```

Collect a tuning trace:

```sh
sudo governors/ps5gov-trace.sh -d 300 -i 1
```

Validate fan behavior:

```sh
governors/ps5gov-fan-validate.sh
sudo governors/ps5gov-fan-validate.sh --write-tests
```

## Troubleshooting

| Problem | What to do |
| --- | --- |
| `need root` | Run the command with `sudo` |
| Values do not change | Stop other policy services, then retry |
| Mailbox busy or stuck | Stop competing tools; reboot if the mailbox is wedged |
| Fan policy conflicts | Disable older `ps5fan.service` and let `ps5gov` own fan policy |
| GPU does not ramp in a test | Use a real game/benchmark with fdinfo-visible GPU load |

## More Docs

- [USAGE.md](USAGE.md) has command examples.
- [TECHNICAL.md](TECHNICAL.md) has the safety model, implementation details, and
  lower-level tuning notes.
- [governors/README.md](governors/README.md) documents governor-specific flags.
- [dkms/ps5-icc-fan/README.md](dkms/ps5-icc-fan/README.md) documents the optional
  DKMS fan module.

## Status

This repo is a userspace development and validation tree for PS5 Linux power
management. It has been tested on real PS5 hardware, but direct mailbox control
still requires care. Do not run multiple power/fan policy daemons at the same
time.
