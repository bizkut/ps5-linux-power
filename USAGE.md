# Usage — PS5 power control (userspace PoC)

All commands run as **root** and talk to the SMU MP1 mailbox over PCI config
space — no kernel module, no `insmod`.

## ⚠️ Before you start — one mailbox, one user at a time
The SMU mailbox is **shared**. Only ONE thing may drive it at a time. On this box
the kernel-module governors run as the `ps5gov` service, so **stop it first**:

```sh
sudo systemctl stop ps5gov         # free the mailbox for these tools
# ... test the PoC tools ...
sudo systemctl start ps5gov        # hand it back when done
```
Also: don't run the module CLIs (`../ps5_cpu` etc.), `ps5boost`, or two of these
tools at the same time. The PoC tools serialize against *each other* via an
`flock`, but not against the kernel modules.

## Build
```sh
make           # gcc only -> ps5_cpu, ps5_df, ps5_gpu, governors/ps5_*_gov
```

## Read everything (safe, no writes)
```sh
sudo ./ps5_cpu get 0     # core 0: P0 | cpu rail ~974 mV, gpu rail ~993 mV
sudo ./ps5_gpu get       # gfxclk=2000 MHz
sudo ./ps5_df  get       # df_pstate=3
```

## CPU P-state — `ps5_cpu`
`set` takes a core **bitmask** (core0=`0x01`, core1=`0x02`, … all=`0xff`);
`get` takes a core **id** (0..7).
```sh
sudo ./ps5_cpu set 0xff 7    # ALL cores -> P7 (800 MHz, coolest)
sudo ./ps5_cpu set 0xff 5    # ALL cores -> P5 (1600 MHz)
sudo ./ps5_cpu set 0xff 0    # ALL cores -> P0 (3200 MHz, default)
sudo ./ps5_cpu set 0x01 0    # only core 0 -> P0
```
P-states: `0=3200 1=2560 2=2327 3=1969 4=1829 5=1600 6=1280 7=800` MHz.
Voltage is read-only (the SMU picks CPU VID per P-state).

## GPU GFXCLK — `ps5_gpu`  (biggest power lever, ~-30 W at 1500)
`force` is required to write; `set` without it does a dry-run.
```sh
sudo ./ps5_gpu set 1500 force   # 2000 -> 1500 MHz  (~-30 W; the sweet spot)
sudo ./ps5_gpu set 800 force    # 800 MHz (much cooler)
sudo ./ps5_gpu reset            # back to 2000 MHz (default)
```
Range 400..2380 MHz. Each write is poll-verified.

## DF (memory/fabric) — `ps5_df`  (~-19 W at df2)
```sh
sudo ./ps5_df set 2 force    # df3 -> df2 (~-19 W)
sudo ./ps5_df set 3 force    # back to default
```
⚠️ **Only use df2/df3.** `df0`/`df1` are EXPERIMENTAL and have **deadlocked the
mailbox** in testing — recovery needs a **reboot**. The tool warns and still lets
you try with `force`, but don't unless you're ready to reboot.

## Governors (automatic, load-adaptive)
Drive the clocks by load. Ramp up immediately, ramp down with hysteresis,
restore full clock on Ctrl+C.
```sh
cd governors
sudo ./ps5_cpu_gov -v               # CPU only, verbose (Ctrl+C to stop)
sudo ./ps5_gpu_gov -v               # GPU only, verbose
sudo ./run-governors.sh             # BOTH together (staggered intervals)
```
Flags: `-i <ms>` interval, `-d <n>` low-samples before stepping down, `-v` verbose.
To install as a boot service, edit the two paths in `governors/ps5gov.service`
then `sudo cp` it to `/etc/systemd/system/` and `systemctl enable --now`.
(Don't run a PoC governor and the module `ps5gov` at the same time.)

## Undo / restore defaults
```sh
sudo ./ps5_cpu set 0xff 0    # CPU -> P0 / 3200
sudo ./ps5_gpu reset         # GPU -> 2000
sudo ./ps5_df  set 3 force   # DF  -> df3   (only if you changed it)
```
A reboot also restores everything to defaults.

## Troubleshooting
- **"open … config (need root)"** → run with `sudo`.
- **status `0xFC` / "busy" / hangs** → the mailbox is stuck (almost always a DF
  write). Reboot.
- **values don't change** → something else is driving the mailbox (the `ps5gov`
  service, a module CLI, or `ps5boost`). Stop it first.
