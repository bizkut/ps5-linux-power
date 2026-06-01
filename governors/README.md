# Governors (userspace PoC)

Load-adaptive CPU + GPU governors — **no kernel module**. They sense load
without touching the mailbox (CPU via `/proc/stat`, GPU via the
`drm-engine-gfx` ns counter in fdinfo) and drive the clocks through the SMU MP1
mailbox directly over PCI config space (`../smn.h`). They write only when the
target stage changes, so at steady idle there is zero mailbox traffic. Ramp
up is immediate; ramp down uses hysteresis. On exit (Ctrl+C / SIGTERM)
each restores full clock (CPU P0/3200, GPU 2000).

There is **no DF governor** on purpose — DF writes can deadlock the mailbox.

## Build & run
```sh
make                         # from the repo root: builds the tools + these
cd governors
sudo ./ps5_cpu_gov -i 400 -d 4   # CPU: load >=40%->P0, >=20%->P2, >=8%->P5, else P7
sudo ./ps5_gpu_gov -i 700 -d 3   # GPU: load >=50%->2000, >=20%->1500, >=5%->1200, else 800
```
Options: `-i <ms>` interval, `-d <n>` low-samples before one step down, `-v` verbose.

## Both at once
`run-governors.sh` starts both with **deliberately different intervals** (400 vs
700 ms). They also serialize on a shared `flock`, so they can't collide on the
mailbox:
```sh
sudo ./run-governors.sh      # foreground; Ctrl+C stops + restores clocks
```
