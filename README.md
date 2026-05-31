# PS5 CPU & memory/fabric P-state control (Linux)

Set and read PS5 CPU core P-states and the DF (memory/fabric) P-state from
Linux via the SMU MP1 mailbox. There is no cpufreq/amd_pstate/amdgpu DPM
on PS5-Linux, so this drives the SMU directly from small out-of-tree kernel
modules (amd_smn_read/write). No kernel rebuild needed.

**What actually saves power:** lowering the CPU P-state changes frequency
only, not voltage (all P-states share VID = 1.05 V), so it barely helps. The
real lever is the DF/memory P-state: df3 → df2 cut wall power by ~15% on the
tested unit. Below df2 the GPU/SoC dominates and Linux can't touch it.

## Files
| File | What |
|------|------|
| `ps5_corepstate_ctl.c` | kernel module: CPU core P-state, `/dev/ps5cpc` (GET + guarded SET) |
| `ps5_cpc.c` | CLI for the CPU module |
| `pstate_msr.c` | read-only verify: active CPU P-state + table (freq and VID) |
| `ps5_dfpstate_ctl.c` | kernel module: DF P-state, `/dev/ps5dfc` (GET + guarded SET) |
| `ps5_dfc.c` | CLI for the DF module |
| `ps5_dffreq_probe.c` | read-only: FCLK/UCLK per DF P-state (dumps to dmesg) |
| `Makefile` | builds everything |

## Build
```sh
make                # all .ko + ps5_cpc + pstate_msr + ps5_dfc
sudo modprobe msr   # pstate_msr needs /dev/cpu/*/msr
```

## CPU core P-state
```sh
sudo insmod ps5_corepstate_ctl.ko
sudo ./ps5_cpc set 0xff 7      # all 8 cores -> P7 (800 MHz)
sudo ./pstate_msr defs         # verify: MHz + VID per P-state
sudo ./ps5_cpc set 0xff 0      # back to P0 (full clock)
sudo rmmod ps5_corepstate_ctl  # also auto-restores every touched core to P0
```
Protocol (FW 4.51): RequestCorePstate 0x0b (set), QueryCorePstate 0x0c (read),
arg = (core_sel & 0xff) | ((pstate & 0xf) << 16).
**Gotcha:** SET's core_sel is a bitmask (core0=0x01 … all=0xff); QUERY's is
a core id (0..7).

CPU P-state table (measured via PStateDef MSRs):
```
P0 3200  P1 2560  P2 2327  P3 1969  P4 1829  P5 1600  P6 1280  P7 800 MHz
all P-states @ VID 1.05 V   (frequency-only; no voltage change)
```

## DF (memory/fabric) P-state — the real power lever
```sh
sudo insmod ps5_dffreq_probe.ko   # one-shot: prints FCLK/UCLK per state to dmesg
dmesg | tail; sudo rmmod ps5_dffreq_probe

sudo insmod ps5_dfpstate_ctl.ko   # captures current df-state as restore baseline
sudo ./ps5_dfc get                # -> df_pstate=3 (Linux default = 1200/875)
sudo ./ps5_dfc set 2 force        # df3 -> df2 (750/425) : ~15% less wall power
sudo ./ps5_dfc set 3 force        # back to default
sudo rmmod ps5_dfpstate_ctl       # auto-restores the baseline
```
Protocol (FW 4.51): RequestDfPstate 0x12 (set), QueryDfPstate 0x13 (read),
arg = df_pstate 0..3. Sony's "mempstate" is inverted: df_pstate = 3 - mempstate.
FCLK/UCLK read-only via TESTSMC 0x38/0x39.

DF P-state table (measured):
```
df0 250/225   df1 250/225   df2 750/425   df3 1200/875   (FCLK/UCLK MHz)
```

## Results (wall power, CPU pinned at 800 MHz)
```
df3  1200/875  -> 129 W   (default)
df2   750/425  -> 110 W   (-19 W, ~15%)   <- best safe steady state
df1   250/225  -> 105 W   (only -5 W more, and unsafe)
```

## Safety notes
- SMN errors return -EIO, never panic.
- DF/CPU modules auto-restore on rmmod (CPU → P0, DF → boot baseline).
- DF writes need an explicit force arg; default is a no-op.
- The MP1 mailbox is shared never run boost (UniversalMode) or two of these
  modules' writes at the same time.
- Tested on a real PS5, Ubuntu 26.04, self-built kernel 7.0.10. Use at your own risk.
