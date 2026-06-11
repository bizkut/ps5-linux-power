# Governors (userspace PoC)

Load-adaptive CPU + GPU governors — **no kernel module**. They sense load
without touching the mailbox (CPU via `/proc/stat`, GPU via the
`drm-engine-gfx` ns counter in fdinfo) and drive the clocks through the SMU MP1
mailbox directly over PCI config space (`../smn.h`). They write only when the
target stage changes, so at steady idle there is zero mailbox traffic. Ramp
up is immediate; ramp down uses hysteresis. At the top load tier they also vote
for `/dev/mp1` boost mode, matching the `ps5boost` performance envelope while
load is high. On exit (Ctrl+C / SIGTERM) each clears its boost vote and restores
full normal clock (CPU P0/3200, GPU 2000).

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
sudo ./ps5_gpu_gov -i 700 -d 3 -H 50 -M 20 -L 5 -T 85 -R 75
```
Options: `-i <ms>` interval, `-d <n>` low-samples before one step down, `-v` verbose.
Load thresholds: `-H <percent>` high, `-M <percent>` mid, `-L <percent>` low.
GPU-only: `-T <C>` thermal cap temperature, `-R <C>` recovery temperature.

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
ps5govctl config
```

Built-in profiles:

| Profile | CPU args | GPU args |
|---------|----------|----------|
| `quiet` | `-i 600 -d 6 -H 55 -M 30 -L 12` | `-i 900 -d 5 -H 65 -M 30 -L 8 -T 80 -R 70 -S auto -m fdinfo` |
| `balanced` | `-i 400 -d 4 -H 40 -M 20 -L 8` | `-i 700 -d 3 -H 50 -M 20 -L 5 -T 85 -R 75 -S auto -m fdinfo` |
| `performance` | `-i 250 -d 6 -H 25 -M 12 -L 4` | `-i 350 -d 5 -H 30 -M 12 -L 3 -T 90 -R 80 -S auto -m fdinfo` |

Manual config:
```sh
sudoedit /etc/ps5-linux-cpuclock/ps5gov.conf
sudo systemctl restart ps5gov
```
Precedence is: systemd environment override, config file, then built-in profile
defaults.

Useful GPU options:
- `-S auto|gpu|k10temp`: temperature source.
- `-m fdinfo|debugfs|auto|busy`: load source. `busy` is a dependency-free debugfs attempt, not Cyan's libdrm MMIO sampler.
