# T6022 (M2 Ultra, J475d): the ANE DARTs in ANE_SYS, 2026-10-29

The first boot of a J475d with the `t6022-ane` overlay (main at `ae18911`) panicked; with the three ANE DARTs moved to
the ANE_SYS power domain it boots, and `ane_t6021` runs the smoke fixture bit-exact.

## The panic (overlay as shipped)

Kernel 7.1.13 (linux-asahi 7.1.13-3 base), 2.66 s after start, on most boots (one boot in about eleven got through):

```
Asynchronous SError Interrupt
CPU: 10 UID: 0 PID: 484 Comm: (udev-worker) Tainted: G S M  7.1.13 #1 PREEMPT(lazy)
Hardware name: Apple Inc. Mac14,14/J475d, BIOS 2026.07 07/01/2026
 arm64_serror_panic+0x78/0x90
 do_serror+0x38/0x60
 el1h_64_error+0x84/0x88
 apple_dart_probe+0x170/0x980 [apple_dart] (P)
 platform_probe+0x88/0xc0
 ...
 apple_dart_driver_init+0x2c/0x3fd0 [apple_dart]
```

The shipped overlay puts dart0 (0x285800000) in pmp (power-controller@2c8) and dart1/dart2 in ANE_CPU (0x2e0).

## Where macOS powers them (live IORegistry, macOS 15, same machine, 2026-10-28)

- `/arm-io/dart-ane0@85800000`: reg 0x285800000, 0x285810000, 0x285820000 (and 0x285804000), each 0x4000,
  interrupt 885, `compatible` dart,t8110, `power-gates` = 0x1fb. (`dart-ane1`: the same at 0x2285800000, 0x100001fb.)
- pmgr `devices`: 0x1fb = `ANE-SYS-DART`, flags 0x10, no register (psreg 0, slot 0), parent 0x3c.
  0x3c = `ANE_SYS`, psreg 6 (base 0x200) slot 12: pmgr offset 0x260. `ANE_CPU` (0x4c, offset 0x2e0) is a child of
  `ANE_SYS`; the set and base domains 0x70-0x76 (0x4000-0x4030) are children of `ANE_SYS` or of each other.

So all three DART windows sit behind ANE_SYS, which iBoot leaves off on this machine at boot.

## With the DARTs in ANE_SYS (this change)

Same kernel and machine, the overlay with `power-domains = <&ps_ane_sys>` on the three DARTs:

- boot clean; `apple-dart 28580[012]0000.iommu: DART [pagesize 4000, 16 streams, bypass support: 1, bypass forced: 0,
  locked: 0, AS 42 -> 42] initialized`; `284000000.ane: Adding to iommu group 0`
- `ane_t6021` (ae18911) loaded by hand: firmware t602x_ane0_fw_selene_rc4x (13.5 pin) booted, handshake complete,
  chman table validated, `/dev/accel/accel0`
- `omarchy-ane-smoke`: add-fixture on t6022, 20/20 calls bit-exact (sha256 94041b7c...de0b, the golden), min 0.398 ms,
  median 0.418 ms

The dtc that applied the overlay was 1.7.2; every phandle in the result resolved (the DARTs at the
power-controller@260 phandle, AIC and ANE_CPU unchanged).
