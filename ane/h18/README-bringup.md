# ane/h18: OPT-IN EXPERIMENTAL H18 ANE0 bring-up for the T8152 (Mac mini M6, J873g)

Nothing in this directory is installed, autoloaded or shipped as working support. No M6 has run this module. It has no
firmware path: no IORVBAR write, no CPU RUN, no mailbox write. The ANE firmware is preloaded by iBoot (the ADT
`/arm-io/ane` carries `segment-ranges`), and booting it is a later change.

## What it does

`ane_h18.ko` binds to the `apple,t8152-ane` node of `t8152-j873-ane.dtsi` and only when loaded with `optin=t8152`.

- `stage=dt` (default): check every device tree window and property against the SoC row (`ane_h18_soc.c`) and log
  them. No mapping and no MMIO.
- `stage=psd`: read only. Map the two PSD slot windows (slots 30 and 31) and read VALID, then STATUS when VALID is
  set. The proven PSD probe does the same two reads. No write.
- `stage=power`: needs `confirm_power=1`. Request PSD state `psd_state` (module default 0, the stage script passes 1) on the first `psd_slots`
  slots (default 1, slot 30 only) and wait for each acknowledge as `drivers/pmdomain/apple/pmgr-psd.c` does. Only
  after every requested slot acknowledged, read CPU_STATUS, ASC_STATUS and IORVBAR (and the Postoffice status word
  of three candidate channels with `read_mbox=1`). Then request state 6 again unless `keep_on=1`. Every write and
  read address is logged before the access, so a hang leaves the last address in the log.

The module never reads or writes CPU_CONTROL (ASC + 0x44) and writes nothing in the ASC, IORVBAR or mailbox windows.

## Why the ANE is powered by PSD slots

The live M6 ADT lists `power-gates`, `clock-gates` and `service-gates` of `/arm-io/ane` as 30 and 31 (ANE1: 32 and
33). Gate values under 170 are PSD slot numbers, and the kernel tree already drives PSD slots for the USB ports and the
GPU (`t8152-j873-psd.dtsi`, `t8152-j873-psd-gpu.dtsi`). The ADT has no PMGR power state for the ANE. A slot window is
0x100 bytes at 0x303a10000 + (slot << 8): slot 30 is 0x303a11e00 and slot 31 is 0x303a11f00.

## Not known

- The PSD state each ANE slot needs. The kext decode (`w74-t8152-ane-rows.md`) names slot 30 ANE0_CPU_BLOCKIN and slot
  31 ANE0_SYS_0, reads "on" as request value 1 (or 2) from the shared threshold lists, and says which value the ANE
  driver passes is not decoded. The GPU uses 0 for its cluster slot and 2 for its ASC slots, the USB blocks 0. The stage
  script requests state 1 on slot 30 first; `psd_state` takes 0 to 5 without a rebuild.
- Whether the ASC wrapper answers once slots 30 and 31 run. The ANE also has `clock-ids` 329, 328 and 375, which this
  module does not touch.
- The Postoffice channel of this ANE. The T8152 driver deliverable gives `msgbox_using_postoffice + (msgbox_mailbox_num
  << 1)`, which is channel 25 for mailbox number 12. A review reads the same expression as channel 12. The SMC node
  fits neither. `read_mbox=1` reads the status word of channels 12, 24 and 25, and the log decides.
- Everything about the DART. It is `dart,gen3` with manual availability, and the first read of such a DART hangs the
  bus until its power chain is up. The overlay has no DART node.

## Build and sim

Build the module against the kernel tree of the Image that runs on the M6, on a build host (the vermagic must match):

    make -C ane/h18 KERNELDIR=<kernel tree> O=<its out dir> ARCH=arm64 LLVM=1 W=1 modules

The userspace sim needs only a C compiler:

    make -C ane/h18 sim-all

`sim-test` has the positive controls (zero bus accesses at `dt`; at `psd` no write; at `power` the exact write sequence,
the reads only after the acknowledge, three candidate mailbox addresses). The negative controls
(`sim-negative-psd-never`, `-psd-wrong`, `-dt`, `-args`) each exit 0 only when the module fails the induced fault
correctly: a slot that never acknowledges, a slot that acknowledges another state, a tampered window or property
(including mailbox number 0), a wrong opt-in key, an unknown stage or a bad argument. Five mutation targets delete
one check each from a copy of the module and must fail the harness. A sim is not silicon: it proves control flow and
gating, not any M6 behavior.

## Stage script

`m6-ane-stage.sh <1|2|3> <boot-id prefix> [GO [state]]` runs one stage as root and collects the log in a new directory under
`/var/tmp/m6-ane`. It refuses on a kernel release other than 7.1.12-aurora-m6usb-ARCH, a boot id that does not match,
a missing ANE node, a module whose sha256 is not the pinned one, and a module that is already loaded. It enforces the
order per boot: step 2 needs a step 1 PASS, step 3 needs a step 2 PASS and the literal argument `GO`, and step 3 runs once
per boot. The boot id prefix has at least 8 characters. The script refuses when any of its override variables is set in
the environment. It reads the verdict only from RESULT lines that the run added to the kernel log, for its own stage. A
step 3 PASS with a `did not return to state 6` line exits 3. `test-stage-script.sh` checks all of this off device with fake `insmod`, `dmesg` and `lsmod`.

## Device tree

`t8152-j873-ane.dtsi` adds the `ane@419600000` node under `/soc` of `t8152-j873.dts`. Every window is a live ADT value
plus 0x200000000. `t8152-j873-ane.dts` includes both files, as `t8152-j873-gpupsd.dts` does for the GPU. The node has
no `power-domains` property, because attaching a PSD domain writes the slot request before any stage decides.

## Bring-up order

Stop at the first unexpected result and keep the log. A hang costs one reboot: hold the power button and reload.

1. `insmod ane_h18.ko optin=t8152` (stage `dt`). Expect `RESULT stage=dt ... verdict=PASS reason=dt-windows-match`.
2. `insmod ane_h18.ko optin=t8152 stage=psd`. Expect one `psd0` and one `psd1` line with their valid and status words.
3. `insmod ane_h18.ko optin=t8152 stage=power confirm_power=1 psd_state=1`. Slot 30 only. Expect `psd0 ... state 1
   acknowledged`, then the ASC words, then the return to state 6.
4. Only after step 3 passed: `psd_slots=2`, then `read_mbox=1`.
