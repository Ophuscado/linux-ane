# ane/h15 — OPT-IN EXPERIMENTAL H15 (M3) ANE bring-up module

Nothing in this directory is installed, autoloaded or shipped as
working support. `ane_h15.ko` is a bring-up tool for the first owner
of an M3 (T8122 / T6030 / T6031) Linux box. T6034 is a fourth data
row in the same module (j514m / j516m). Facts and the b0 family
decision: `receipts/2026-10-03-ane-h15/README.md`. The structural
template is `ane/h16/`.

## What it does

- `stage=0` or `stage=dt` (safe, no MMIO): parse the running DT and
  log every word group, every PMGR offset, every ADT-derived base
  address the module would touch in stages 1–3, with its evidence
  tier and source. No `of_iomap`, no `pm_runtime_get`.
- `stage=1` or `stage=status` (default, safe): enable the ANE power
  domains named in the device node, wait for every PMGR ANE state
  word to read ACTUAL=0xf, re-read all five, then log the value
  and ACTUAL of each one. The pmgr window is mapped and read; the
  engine window is **not** touched at stage 1.
- `stage=2` or `stage=wrapper`: refused unless `confirm_read=1`. The
  CPU_STATUS and RVBAR words are MEASURED addresses (ADT 27.0 reg[0] and
  reg[1]), but their roles are INFERENCE. Without `confirm_read=1` the
  stage prints the word list and refuses before any engine access.
  The probe has already powered the ANE domains by then, as it does
  for stage 1.
  With `confirm_read=1` it runs the read-only preflight from the b0
  plan. It first compares the live reg[0] and reg[1] resources with the SoC
  row and refuses on a mismatch. Then it runs the same ACTUAL=0xf gate as stage 1. It reads CPU_STATUS,
  RVBAR (two 32-bit reads) and the two mailbox CTRL words. Each address
  is logged before its read, so a hang leaves the last address in the
  log. Reads stay inside the two windows the ADT publishes for the ANE
  node: the wrapper (engine + 0x1400000, 0x6c000 bytes) and the RVBAR
  block (engine + 0x1050000, 0x4000 bytes). The code refuses CPU_CONTROL,
  the CoreSight line and the T6021 fatal range whatever the SoC row
  says. Nothing is written and no CPU is released. The result line
  compares CPU_STATUS with the value a stopped T6021 core reads (0x2a).
  The meaning of the bits on H15 is not measured, so the line only says
  whether the values are equal.
- `stage=3` or `stage=boot`: refused without `fw_path=` and
  `confirm_boot=1`. With them, the stage prints one line per
  still-unmeasured fact from the `ane_h15_facts` table (stub
  firmware identity, mailbox base and IRQ roles,
  segment-ranges/DART geometry, RVBAR compose value, SCRATCH wake
  word) and stops with `verdict=REFUSED reason=unfilled-facts`.
  The table is data: a later change fills a row only with the
  receipt named in the row. Even with every row filled, THIS
  build implements no boot path and releases no CPU; it refuses
  with `reason=facts-filled-boot-path-not-built`. When iBoot
  preloads firmware into ANE DRAM, the live ADT's
  `segment-ranges` describes it; on a measured H15 boot the same
  preload + map + CPU-release path that the T6021 W-series uses
  is the only mechanically proven route, and a volunteer
  recording it is what fills the missing rows in the b0 table.
- `stage=4` or `stage=dart` (read only, `confirm_read=1` required): reads
  the ANE DART, never writes. After the stage 1 ACTUAL=0xf gate it reads
  PARAMS3 of the four 0x4000 windows the ADT lists (engine + 0x1800000,
  +0x1810000, +0x1820000, +0x1804000), and for every window whose version
  equals window 0's it reads PARAMS4, PROTECT, PROTECT_LOCK, the ENABLE_STREAMS
  words and TCR (plus TTBR where TCR is set) of streams 0 to 63. It logs
  each non-zero stream and `live_streams=N locked_windows=N`. `PASS
  reason=dart-live-stream-found` means iBoot left a stream enabled, with
  translation on and a valid TTBR. `PASS reason=dart-no-stream-enabled`
  means nothing is enabled: a started core could not fetch its firmware. It
  does not show whether the mapping covers the firmware, whether the core would
  run, or anything about the mailbox. On the M6 (T8152) the same kind of read (PR 183 and 185)
  found every ENABLE_STREAMS word 0: iBoot left no stream enabled.
- `stage=5` or `stage=run`: refused without `confirm_run=1`, with
  `run_wait_ms` outside 10 to 2000 (default 300), and on any SoC row whose
  `rvbar_expect` is 0 (only t6030 has a measured RVBAR, from stage 2 on a
  16-inch M3 Pro: `0x0000010000000001`). It is the only stage that writes.
  After the stage 1 ACTUAL=0xf gate it reads CPU_STATUS, CPU_CONTROL, RVBAR
  and both mailbox CTRL words and writes only if the core is stopped (STOPPED
  set, RUNNING clear), RUN is clear, RVBAR equals `rvbar_expect` and both
  mailbox words read EMPTY, and the stage 4 scan finds a live DART stream
  (`run_unmapped=1` overrides this and expects silence). With `map_fw=1` it
  first reads CPU_STATUS and refuses unless the core is stopped (attaching replaces the
  DART state a running core fetches through), then gives the core a DART mapping, the M6
  module's pattern (PR 185): it
  attaches the ANE to its DART through the iommu API (so the `apple-dart`
  driver programs the DART and enables the stream) and `iommu_map()`s the two
  firmware segments iBoot preloaded, TEXT read only at the RVBAR entry address
  and DATA read+write right behind it, then runs the scan, which must now find
  the stream. The physical addresses come from `fw_seg=TEXTPA:SIZE,DATAPA:SIZE`,
  taken from the live ADT of the same boot (`ane/h15/h15-segments.py ADT.bin [RVBAR]`, which also refuses a dump whose TEXT remap is not the RVBAR entry), because
  iBoot picks them and the Linux device tree does not carry them for the ANE
  (m1n1 writes asc-firmware regions only for dcp, dcpext and ave). Each segment
  must be 16 KiB aligned, at most 64 MiB, disjoint, and outside system RAM,
  or the stage refuses before touching hardware. `map_fw=1` needs `iommus` in
  the ANE node: `ane/h15/t6030-ane-dart-experimental.dts` (DART instance 0
  enabled, stream 0 inferred). The mapping is undone after a clean stop and
  kept when the core does not stop. Then it sets CPU_CONTROL bit 4 (the macOS
  `_runCPU(true)` read-OR-write), watches CPU_STATUS and `MBOX_I2A_CTRL` every
  10 ms for `run_wait_ms`, pops at most four I2A messages (one 64-bit read per receive register) and logs
  the raw words (it sends nothing, never answers a HELLO), and stops the core the macOS
  way (clear bit 4, then bit 5). It never writes RVBAR, the mailbox or the
  DART. If the core does not report STOPPED afterwards it keeps the power
  domains and mappings, logs `reboot required`, `remove()` refuses and the module is pinned
  with `__module_get` (an `rmmod` would otherwise free the code under a running core).

- `stage=6` or `stage=rtkit`: stage 5 (all of its gates, `map_fw`, `fw_seg`) plus an RTKit management
  session over the ASC mailbox, behind `confirm_run=1` AND `confirm_rtkit=1`. It is the first stage that
  writes the mailbox: the two send registers (A2I_SEND0 at wrapper + 0x8800, then A2I_SEND1 at + 0x8808, 64-bit;
  the apple-mailbox v4 layout the other T6030 ASCs use, INFERRED for the ANE, never written on an H15 part), at most 16
  messages, never while A2I_CTRL reads FULL, only management-endpoint messages. After RUN it answers the firmware's
  HELLO (version 12; refused if the firmware's range excludes it) and endpoint map, starts the system endpoints it
  announced, answers the IOP power ON acknowledgement with AP power ON, starts the application endpoints once that is
  acknowledged, then shuts the session down before it clears RUN: `rtkit_stop=1` (default) AP QUIESCED then IOP SLEEP,
  restartable; `rtkit_stop=2` OFF then OFF, the core cannot be restarted until a reboot. It answers no buffer request
  (the crash log request stays unanswered) and no message of another endpoint. `rtkit_wait_ms` (100 to 10000, default
  2000) bounds the start phase. Verdicts: `PASS rtkit-session-and-shutdown`; `FAIL rtkit-no-hello` (the firmware
  said nothing: the mailbox layout or the transport may be wrong, nothing was answered), `rtkit-session-incomplete`,
  `rtkit-shutdown-incomplete` (the core is still stopped by clearing RUN), or `core-not-stopped` (reboot). The M6
  runs this session on its Postoffice (ane/h18); the H15 transport is the unknown. **Run it only after a stage 5 result
  with a mailbox message in it.**

## Build (host cross build)

    make -C ane/h15 KERNELDIR=<kernel tree> ARCH=arm64 \
         CROSS_COMPILE=aarch64-linux-gnu- W=1 modules

The build was verified W=1-clean against the M2 3-1 tree in the
macstudio ALARM chroot and against a josh/ane-driver-aurora worktree
on the lab CT. The module is not part of `make all`, `dkms.conf`
or the package.

The host self test:

    make -C ane/h15 check

compiles `ane_h15_adt_test.c` (the pure ADT walker in
`ane_h15_adt.h`) and runs it on a hand-built IODeviceTree-shaped
blob.

The userspace sim (no kernel build, no hardware; the PMP vehicle
harness pattern):

    make -C ane/h15 sim-test

compiles the real module sources against `sim/shim.h` and asserts
the stage behavior: stage=0 does zero MMIO, stage=1 passes the
ACTUAL=0xf gate and reads only pmgr words, stages 2 and 3 refuse,
and the stage=3 refusal enumerates exactly the unfilled rows of
the fact table. The named negative controls each exit 0 only when
the module fails the induced fault correctly:

    make -C ane/h15 sim-negative-ps-stuck       # ANE_CPU word stuck at 0x30
    make -C ane/h15 sim-negative-wrong-stage    # unknown stage, wrong optin
    make -C ane/h15 sim-negative-truncated-dt   # reg property missing
    make -C ane/h15 sim-negative-nopsguard      # a guard-deleted build must fail
    make -C ane/h15 sim-negative-read-ps-stuck  # stage 2 with a stuck pmgr word reads nothing
    make -C ane/h15 sim-negative-read-forbidden # stage 2 words outside the ADT windows are refused
    make -C ane/h15 sim-negative-read-window    # stage 2 refuses a reg[0] that is not the SoC row's window
    make -C ane/h15 sim-mutation-read-noguard   # three mutants of the stage 2 gates must fail

A sim is not silicon: a green sim proves control flow and gate
logic in the module code, not any M3 hardware behavior.

## Device tree (one overlay per SoC)

`ane/h15/t8122-ane-experimental.dts`,
`ane/h15/t6030-ane-experimental.dts`,
`ane/h15/t6031-ane-experimental.dts` — opt-in key
`ane-h15-experimental`. The t8122 and t6030 overlays also add the
ANE_MPM / ANE_CPU / ANE_TD / ANE_BASE power-controller nodes that
the aurora t8122-pmgr.dtsi and t6030-pmgr.dtsi do not yet carry
(only `ps_ane_sys` is in the kernel tree today; the b0 receipt
describes the four missing ps words). The t6031 overlay targets
the existing ps nodes by path. None of these overlays are
installed by the package. They ship the four ANE DART nodes as
`status = "disabled"` and the `ane` node without an `iommus`
property. Stage 0 and stage 1 need no DART, and enabled DART nodes
hung a T6030 at boot through equivalent nodes. `omarchy-ane-probe`
reports the ANE node as `not-exposed` until the DART work lands.
That is expected. The first M3 owner:

1. Compile against the aurora tree with the system dtc 1.7.2+:
   `dtc -@ -I dts -O dtb -o ane-h15-t8122.dtbo t8122-ane-experimental.dts`
   (dtc has no `-O dtbo` output format; the `.dtbo` name is enough).
2. Copy the `dtbo` into the overlay directory with owner-only
   permissions.
3. Add the line `ane-h15-experimental` to
   `/etc/omarchy-mac-boot/dtb-overlays.opt-in`.
4. Reboot.

T6034 boards (j514m, j516m) use the t6031 ADT nodes — a t6034
owner copies `t6031-ane-experimental.dts`, swaps
`apple,t6031-ane` → `apple,t6034-ane`, and keeps the ane-type at
`0xe0`. The four-row module (`ane_h15_of_match[]`) is already
prepared to bind either compatible.

The shipped **data-only** overlays in `packaging/dt/`:

- `packaging/dt/t8122-ane-dataonly.dts`
- `packaging/dt/t6030-ane-dataonly.dts`
- `packaging/dt/t6031-ane-dataonly.dts`

carry the `omarchy,data-only` root property and follow the
established convention (t8132-ane-dataonly.dts): `omarchy-ane-data-
tXXXX` with `omarchy,ane-data-only` compatible, status `disabled`,
and the ADT-derived `ane-compatible`, `ane-reg`, `ane-interrupts`,
`dart-compatible`, `dart-reg`, `dart-interrupt`, `ane-type`
fields. These are validated by `tools/test_ane_overlays.py
--data-only` against the aurora-silicon/linux `aurora-wip` board
trees via `.github/workflows/aurora-dtbs.yml`; no M3 driver binds
them.

## Capture before stage 2 (no register access)

    bash ane/h15/h15-capture.sh | tee h15-capture.txt

The script reads the running device tree, `/proc/interrupts`,
`/proc/iomem`, the genpd summary and dmesg. It loads no module and
writes no file. It records the firmware version string that selects the
image iBoot preloads (H1), the ANE, DART and mailbox nodes with their
interrupt lines (H2), and the reserved firmware regions with their
iommu addresses (part of H3). It does not fill H3: the ADT
segment-ranges, the ASC tunables and the DART sid, vm-base and vm-size
are not in the Linux device tree and need the boot ADT. Send the output
file with the stage results.

## Bring-up protocol for the first owner

Run in this order; stop at the first unexpected result and send
the dmesg back.

1. **stage=0** (insmod, no MMIO). Expected: every word group
   printed with tier `0` (MEASURED_GUARD) for the five pmgr
   words, tier `1` (ADDR_MEASURED) for CPU_STATUS / RVBAR, tier
   `2` (INFERRED) for the mbox ctrl / doorbell, tier `3`
   (FORBIDDEN) for the CoreSight line. The RESULT line reads
   `verdict=PASS reason=dt-parse-only`. Send `dmesg | grep
   ane_h15`.
2. **stage=1** (`insmod ane_h15.ko optin=t8122 stage=1`). Expected:
   five pmgr ACTUAL=0xf log lines, then five
   `ane_h15 word=ANE_… pa=… value=… actual=0xf pass=true` lines,
   and `verdict=PASS reason=ps-guard+reads`. Send `dmesg | grep
   ane_h15`.
3. **stage=2** (`stage=2` or `stage=wrapper`). Without
   `confirm_read=1` the expected result is `verdict=REFUSED`. With
   `confirm_read=1` (run it only after the capture step below and
   stages 0 and 1), expect five `ane_h15 read` lines, five
   `ane_h15 word=` lines, one `ane_h15 preflight` line and
   `verdict=PASS reason=preflight-read-only`. Send `dmesg | grep
   ane_h15`. If the machine hangs, the last `ane_h15 read` line names
   the address.
4. **stage=3** (`stage=3 confirm_boot=1 fw_path=…`): the refusal
   prints one line per unfilled row of the `ane_h15_facts` table
   (`ane_h15 hole=H… …`), then
   `verdict=REFUSED reason=unfilled-facts`. Send the dmesg.
5. **stage=4** (`stage=4 confirm_read=1`, T6030, T8122 and T6031 rows).
   Read only. Expect a few `ane_h15 read DART` group lines, one
   `ane_h15 dart inst=… params3=…` line per window, a `streams=` line, one line
   per non-zero stream, `ane_h15 dart summary live_streams=… locked_windows=…`
   and `verdict=PASS reason=dart-live-stream-found` or
   `dart-no-stream-enabled`. Both are results. Send the whole `dmesg | grep
   ane_h15`. Do not run stage 5 on the strength of this alone: it needs a mapping we
   have not built (stage 6) unless this stage reports a live stream.
6. **stage=5** (`stage=5 confirm_run=1`, T6030 only today). Run it only after
   stages 0, 1, 2 and 4 passed on the same boot, stage 4 reported a live stream
   (or the maintainers said GO on the issue for another reason). Expected, in dmesg: the pmgr lines, six `ane_h15 read`
   lines (CPU_STATUS, CPU_CONTROL, RVBAR_LO, RVBAR_HI, the two mailbox CTRL
   words), `ane_h15 write CPU_CONTROL=0x10 (RUN)`, a few `t+10ms
   cpu_status=…` lines, optionally `ane_h15 msg0 raw0=… raw1=…`, then
   `write CPU_CONTROL=… (clear RUN)` and one `ane_h15 run started_seen=…`
   line. `verdict=PASS reason=core-started-and-stopped` (or
   `core-never-left-stopped`) means the core is stopped again and the
   module can be removed. `verdict=FAIL reason=core-not-stopped` means
   reboot. `verdict=FAIL reason=preflight-not-as-iboot-left-it` means
   nothing was written. Send the whole `dmesg | grep ane_h15`.
7. Remove the opt-in key and reboot before any other experiment.

## Stop rules

- A hang at any stage: power button, remove the opt-in key,
  reboot. The module cannot be unloaded after `stage=boot`
  releases the CPU.
- Any external abort, SError or DART fault in dmesg: stop, keep
  the log, do not retry the same stage.
- Do not add IRQs or mailbox interrupts to the overlay: which of
  the two ADT ANE lines is the mailbox recv line is not
  established, and a mismapped mailbox IRQ stormed at ~700 kHz
  on T6021 (`receipts/2026-09-30-t6021-stock-mailbox`).

## What a volunteer can run safely now

Two commands, no MMIO beyond a read of a kernel-mapped PMGR
window:

    insmod ane/h15/ane_h15.ko optin=t8122 stage=0
    dmesg | grep ane_h15

    insmod ane/h15/ane_h15.ko optin=t8122 stage=1
    dmesg | grep ane_h15

The read-only preflight (reads four words and one 64-bit word in the
engine window, writes nothing):

    insmod ane/h15/ane_h15.ko optin=t6030 stage=2 confirm_read=1
    dmesg | grep ane_h15

Substitute `t6030` or `t6031` (or `t6034` with a t6031-style
overlay). The other stages refuse; the runbook protocol above
explains the refused verbiage.

The read-only DART scan:

    insmod ane/h15/ane_h15.ko optin=t6030 stage=4 confirm_read=1
    dmesg | grep ane_h15

The run stage (writes CPU_CONTROL; T6030 only, refuses without a live DART
stream, only after the maintainers' GO). With the DART overlay and `fw_seg`:

    insmod ane/h15/ane_h15.ko optin=t6030 stage=5 confirm_run=1 map_fw=1 fw_seg=TEXTPA:SIZE,DATAPA:SIZE
    dmesg | grep ane_h15

Without them (only if stage 4 found a live stream):

    insmod ane/h15/ane_h15.ko optin=t6030 stage=5 confirm_run=1
    dmesg | grep ane_h15

## Not verified (honest limits)

- No M3 silicon has run any of this. The register map is
  ADT-derived (the macOS 27.0 IPSW ADT summary, not disassembled
  kernelcache or iBoot). The role of every word in the engine
  aperture is INFERENCE on H15.
- Whether iBoot preloads the ANE firmware on an M3 Linux boot
  at all is not known (the b0 doc records the stub macOS
  version as "unknown"); the module's stage=3 plan to map the
  preload at the ADT remap IOVAs is the same mechanical path
  the T6021 W-series proved, but on M3 it has not run.
- `map_fw=1`: no H15 run. The DART overlay has not been booted; an enabled DART node hung a
  T6030 at boot before (PR 173) and this one relies on `power-domains` to prevent that. Stream 0 is
  inferred. Whether iBoot's firmware phys addresses repeat between boots is not known, which is why
  `fw_seg` must come from the same boot's ADT. Whether the T6030 firmware lays TEXT and DATA out
  like the M6 (DATA IOVA = vm-base + TEXT size) is inferred; `h15-segments.py` refuses a dump where it
  does not hold. Instance 0 of the four DART windows may not be enough for the firmware fetch (an
  open question on the M6).
- Stage 5 reads CPU_CONTROL and writes it (bits 4 and 5 only). Nobody has run
  that on an H15 part. The M6 (T8152) result it is modelled on: the same RUN
  write cleared the STOPPED bit and left the core with no message, with the
  DART unprogrammed. A started H15 core may do the same. A "core-started-and-
  stopped" result with no message is a valid, useful result.
- The mailbox message pop (wrapper + 0x8830 and + 0x8838) is the apple-mailbox
  v4 layout inferred from the other T6030 ASCs. It is read only when
  `MBOX_I2A_CTRL` loses its EMPTY bit, so a wrong layout costs nothing unless
  the core posts a message.
- The 27.0 CSNE host contract (init structure, channel-manager
  table, CONFIG_GET / PLATFORM_INFO / BUILDINFO opcodes) is not
  derived; the smoke stops at the boot stage refusal.
- The DAPF windows of `dart-ane` are not programmed by Linux
  (known gap since the T6021 work; `omarchy-ane/docs/t6021-ane-
  bringup-findings.md` §18).
