# PS5 power control — userspace PoC (no kernel module)

Read and set the PS5 APU's power knobs (CPU P-state, DF/memory P-state, GPU
GFXCLK) from Linux. The one-shot tools talk to the SMU MP1 mailbox directly over
the PCI config space of `0000:00:00.0` (SMN index `@0xB8`, data `@0xBC`) — the
same transport the
[cyan-skillfish-governor](https://github.com/filippor/cyan-skillfish-governor)
uses, verified working on PS5. The load-adaptive governors can also enter the
kernel `/dev/mp1` boost envelope at the top load tier.

> **Proof of concept.** It works, but see *Caveat* below. For the safer,
> kernel-locked variant see the module-based tools.

| Tool | What | Power impact |
|------|------|--------------|
| `ps5_cpu` | CPU core P-state set/get + rail-voltage read | tiny at idle |
| `ps5_df`  | DF (memory/fabric) P-state set/get | ~19 W (df3→df2) |
| `ps5_gpu` | GPU GFXCLK set/get | ~30 W (2000→1500) ← biggest |
| `ps5_boost` | `/dev/mp1` boost envelope on/off/status | top-tier performance |
| `ps5gov` | systemd CPU/GPU governors with boost-on-load | adaptive |

Measured on a real PS5, kernel 7.0.10, GPU id `0x13fb`. Run as **root**. Use at
your own risk.

## Build & run
```sh
make                      # just gcc — builds the tools + governors/
sudo ./ps5_gpu get        # gfxclk=2000 MHz
sudo ./ps5_gpu set 1500 force
sudo ./ps5_cpu set 0xff 7 # all cores -> P7 (800 MHz)
sudo ./ps5_df  set 2 force # df3 -> df2  (~-19 W)
sudo ./ps5_boost off      # clear /dev/mp1 boost envelope
```
No kernel headers are required; the repo carries small fallback UAPI definitions
for stripped/custom PS5 Linux images. Root is required.

## CPU — `ps5_cpu`  (msg 0x0b set / 0x0c get)
```sh
sudo ./ps5_cpu get 0            # core 0: P0 | cpu rail ~974 mV, gpu rail ~993 mV
sudo ./ps5_cpu set 0xff 7       # ALL cores -> P7 (bitmask 0xff)
sudo ./ps5_cpu set 0x01 0       # core 0 only -> P0
```
SET takes a core **bitmask** (core0=0x01 … all=0xff); GET takes a core **id** (0..7).
Voltage is **read-only** (the SMU sets CPU VID per P-state). P-states:
`0=3200 1=2560 2=2327 3=1969 4=1829 5=1600 6=1280 7=800` MHz.

## DF (memory/fabric) — `ps5_df`  (msg 0x12 set / 0x13 get)
```sh
sudo ./ps5_df get               # df_pstate=3
sudo ./ps5_df set 2 force       # df3 -> df2 (~-19 W); 'force' required
sudo ./ps5_df set 3 force       # back to default
```
df3=1200/875, df2=750/425 (FCLK/UCLK MHz). ⚠ **df0/df1 are EXPERIMENTAL** — they
can deadlock the SMU mailbox (recovery = reboot). Use df2/df3.

## GPU — `ps5_gpu`  (msg 0x0e set / 0x0f get)
```sh
sudo ./ps5_gpu get              # gfxclk=2000 MHz
sudo ./ps5_gpu set 1500 force   # 2000 -> 1500 (~-30 W); 'force' required
sudo ./ps5_gpu reset            # back to 2000 MHz
```
Range 400..2380 MHz. GFXCLK is the dominant power lever.

## Boost — `ps5_boost`
```sh
sudo ./ps5_boost on       # enter /dev/mp1 boost envelope
sudo ./ps5_boost off      # exit boost envelope and clear shared vote state
./ps5_boost status        # boost_state=0x0 or non-zero
```
The governors normally manage boost votes themselves. This helper is mainly for
manual recovery and service restore paths.

## Governors
Load-adaptive CPU + GPU governors live in [`governors/`](governors/). They lower
clocks at idle, ramp up immediately under load, and vote `/dev/mp1` boost mode on
at the top load tier:

- CPU high load: boost on + P0 (`3200+ MHz` envelope)
- GPU high load: boost on + `2230 MHz`
- GPU warm: boost off + 2000 MHz cap
- GPU hot: boost off + 1500 MHz cap
- GPU critical: boost off + 1200 MHz cap until recovery (`k10temp` fallback if needed)
- lower load: boost vote cleared, normal adaptive stages resume
- stop/exit: boost vote cleared, CPU P0 and GPU 2000 restored

Foreground:
```sh
cd governors
sudo ./run-governors.sh
```

systemd:
```sh
sudo make install-systemd
sudo systemctl daemon-reload
sudo systemctl disable --now ps5boost
sudo systemctl enable --now ps5gov
```

Profiles and thresholds are tuned through `/etc/ps5-linux-cpuclock/ps5gov.conf`
or the helper:
```sh
sudo ps5govctl profile quiet
sudo ps5govctl profile balanced
sudo ps5govctl profile performance
ps5govctl config
ps5govctl sensors
sudo ps5govctl stop-boost
sudo ps5govctl restore
```

Built-in profiles:

| Profile | CPU policy | GPU policy | Thermal |
|---------|------------|------------|---------|
| `quiet` | `-i 600 -d 6 -H 55 -M 30 -L 12` | `-i 900 -d 5 -H 65 -M 30 -L 8` | `-T 80 -R 70` |
| `balanced` | `-i 400 -d 4 -H 40 -M 20 -L 8` | `-i 700 -d 3 -H 50 -M 20 -L 5` | `-T 85 -R 75` |
| `performance` | `-i 250 -d 6 -H 25 -M 12 -L 4` | `-i 350 -d 5 -H 30 -M 12 -L 3` | `-T 90 -R 80` |

`ps5gov.service` conflicts with `ps5boost.service`; use one MP1 power policy
owner at a time.

`ps5govctl sensors` prints service state, boost vote state, available CPU/GPU
readbacks, effective governor config, and every exposed hwmon temperature. Use
`stop-boost` to clear only the `/dev/mp1` boost envelope. Use `restore` to stop
`ps5gov`, clear boost, set CPU P0, and reset GPU to 2000 MHz.

## Caveat (why this is a PoC)
SMN access is a two-step (index→data) sequence on a register pair **shared by
in-kernel SMN users** (e.g. `k10temp`, EDAC). The tools take an `flock`
(`/run/lock/ps5-power.lock`) so they don't collide with **each other**, but they
cannot take the kernel's SMN lock, so a tiny race window against in-kernel users
remains. Never observed to misbehave in practice on PS5, but the kernel-module
variant (which uses `amd_smn_read/write` under the kernel lock) is the safe path
if you care.

Also: the one-shot CLIs do not auto-restore. To undo:
`ps5_cpu set 0xff 0`, `ps5_gpu reset`, `ps5_df set 3 force`.

## Safety
- `force` is required for DF and GPU writes; both do read-before-write + poll/verify.
- The MP1 mailbox is **shared** — never run two policy writers at once.
- `ps5gov` needs `/dev/mp1` for boost control.
- DF writes are the fragile part; keep to df2/df3 and be ready to reboot.
