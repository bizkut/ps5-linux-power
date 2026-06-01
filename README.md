# PS5 power control — userspace PoC (no kernel module)

Read and set the PS5 APU's power knobs (CPU P-state, DF/memory P-state, GPU
GFXCLK) from Linux **without any kernel module**. The tools talk to the SMU MP1
mailbox directly over the PCI config space of `0000:00:00.0` (SMN index `@0xB8`,
data `@0xBC`) — the same transport the
[cyan-skillfish-governor](https://github.com/filippor/cyan-skillfish-governor)
uses, verified working on PS5.

> **Proof of concept.** It works, but see *Caveat* below. For the safer,
> kernel-locked variant see the module-based tools.

| Tool | What | Power impact |
|------|------|--------------|
| `ps5_cpu` | CPU core P-state set/get + rail-voltage read | tiny at idle |
| `ps5_df`  | DF (memory/fabric) P-state set/get | ~19 W (df3→df2) |
| `ps5_gpu` | GPU GFXCLK set/get | ~30 W (2000→1500) ← biggest |

Measured on a real PS5, kernel 7.0.10, GPU id `0x13fb`. Run as **root**. Use at
your own risk.

## Build & run
```sh
make                      # just gcc — builds the 3 tools + governors/
sudo ./ps5_gpu get        # gfxclk=2000 MHz
sudo ./ps5_gpu set 1500 force
sudo ./ps5_cpu set 0xff 7 # all cores -> P7 (800 MHz)
sudo ./ps5_df  set 2 force # df3 -> df2  (~-19 W)
```
No `insmod`, no `/dev`, no kernel headers — a single `gcc` and root is enough.

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

## Governors
Load-adaptive CPU + GPU governors live in [`governors/`](governors/) — same
userspace transport, no module. See its README.

## Caveat (why this is a PoC)
SMN access is a two-step (index→data) sequence on a register pair **shared by
in-kernel SMN users** (e.g. `k10temp`, EDAC). The tools take an `flock`
(`/run/lock/ps5-power.lock`) so they don't collide with **each other**, but they
cannot take the kernel's SMN lock, so a tiny race window against in-kernel users
remains. Never observed to misbehave in practice on PS5, but the kernel-module
variant (which uses `amd_smn_read/write` under the kernel lock) is the safe path
if you care.

Also: these are one-shot CLIs — no auto-restore. To undo:
`ps5_cpu set 0xff 0`, `ps5_gpu reset`, `ps5_df set 3 force`.

## Safety
- `force` is required for DF and GPU writes; both do read-before-write + poll/verify.
- The MP1 mailbox is **shared** — never run two writers (or `ps5boost`) at once.
- DF writes are the fragile part; keep to df2/df3 and be ready to reboot.
