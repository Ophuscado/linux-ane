# Chip promotion and community rows

How an opt-in chip in the README table becomes "enabled by default", how a
default-on chip can drop back to opt-in, and how to submit a row.

## Judged rows

`omarchy-ane-check --smoke` produces one JSON line per run: `name`, `chip`,
`die`, `sha256`, `golden_sha256`, `errors`, `min_ms`, `median_ms`. The `die`
field is the bound device's ANE die index, 0 except on an Ultra board's
second device ([docs/collector-die-field.md](collector-die-field.md)).

Each row also records `driver_source`: `intree` when the kernel's own ANE
driver ran (aurora-silicon/linux builds it under `kernel/drivers/accel/ane`),
`dkms` when the omarchy-ane DKMS module ran, and `none` when the kernel has
no ANE driver. A row without the field counts as `dkms`.

## Submitting a judged row for an untested chip

1. Install `omarchy-ane-dkms`.
2. Add the chip's opt-in key from the README table to
   `/etc/omarchy-mac-boot/dtb-overlays.opt-in`. For T6022 and T8112, run
   `sudo omarchy-ane-firmware-fetch` first.
3. `sudo omarchy-ane-dt apply`, then reboot.
4. From an [omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx)
   checkout, run `python3 scripts/collect_deep.py --ane-smoke --submit`.

The collector runs the smoke when the chip is idle (load < 0.5, PSI 0). Rows
without an installed driver or smoke attempt are not judged. On a kernel
that ships the ANE driver itself, do not install omarchy-ane-dkms:
`omarchy-ane-check` says so, and you build the userspace from source instead.

## Promotion

One passing community row promotes an untested chip. A passing row has:

- a ready `omarchy-ane-check`,
- the chip's driver loaded,
- 20 smoke calls (`add-fixture`) that match the golden bit for bit,
- no ANE, ANE-DART or ANE-mailbox fault.

A row from a machine without omarchy-ane installed is not judged and counts
neither for nor against promotion. If an opt-in chip has both passing and
failing rows, the result is `CONFLICT`; it does not promote until the failing
row is explained or superseded. H13 chips (T8103, T6000, T6001, T6002) use
the H13 smoke golden; H14 chips use the H14 golden. The Parakeet encoder hash
is a developer check, not a collector field.

`tools/promotion_check.py --remote` prints the live verdict. When it says
`PROMOTE`, a PR sets that chip's line in `packaging/dt/overlays` to
`enabled`.

## Regression

A chip that is on by default goes back to opt-in when its latest judged row
is not clean: `omarchy-ane-check` not ready, or the row has a fault line. The
chip stays opt-in until a clean row lands. `promotion_check.py` prints
`REVERT` for such a chip, and a PR sets its line back to `opt-in`.

In-tree and dkms rows are judged the same way; the `CONFLICT` and `REVERT`
rules do not change, and a `none` row is not judged. A chip that is on by
default and has a passing in-tree row gets the aurora device-tree change
described below on its own. On an in-tree kernel the chip runs only when
aurora's device tree enables its node.

## The aurora device-tree change

Promotion normally flips the overlay only. When a passing row is in-tree,
the chip also needs its ANE node enabled in the kernel's device trees, and
`.github/workflows/promotion.yml` makes that change in two jobs:

- `aurora-plan` builds the chip's board device trees from aurora-silicon/linux
  `aurora-wip` with `tools/aurora_dt.py`, which finds each disabled node the
  ANE node uses and removes the chip's `APPLE_ANE_UNTESTED` define when the
  tree has one, otherwise adds `status = "okay"`. This job runs code from the
  aurora tree, so it holds no credential.
- `aurora-pr` runs on a fresh runner and builds nothing. It checks that
  `aurora-wip` is still at the commit the plan used, redoes the change from
  the file at that commit, and stops before any write if the result differs
  from the plan. Only then does it push to the joshuaswarren fork, open a PR
  to aurora-silicon/linux, and comment once per PR on the in-tree tracking
  issue, the one step that uses the repository secret `AURORA_PR_TOKEN`. The
  token needs three permissions and no others: push to the fork, open a PR on
  aurora-silicon/linux, and comment there. Without the secret, and on a
  `dry_run` or `synthetic` dispatch, the step runs with `DRY_RUN=1` and only
  prints the diff, the PR text and the comment.

The gate judges a promotion PR on the base branch, where its flip is not
merged yet, and merges while the verdict and the targets stay the same, every
judged row keeps its outcome, and no new judged row fails. New passing rows
and rows that are not judged do not hold it back. Each run closes its own
open promotion PR (labeled `auto-promotion`, from this repository) when it no
longer proposes that chip.

## Chip landscape (M3 and later)

The macOS 27.0 ADTs give four shapes. The M3 family (H15) ANE is an ASC IOP
(`iop,ascwrap-v6`). The M4 family (H16) keeps `ane,t8020`. The MacBook Neo,
the M5 MacBook and T6050 (H17) use `ane,t8132exclave`, which the macOS kernel
binds to the same `H11ANEIn` class as `ane,t8020`, so `ane_t6021` is the
closer model for H16 and H17
([receipts/2026-10-02-neo-ane](../receipts/2026-10-02-neo-ane/README.md)).
The M6 Mac mini (T8152, H18) is an ASC IOP again, `iop-ane,ascwrap-v8`
([receipts/2026-10-02-ane-gen-h18](../receipts/2026-10-02-ane-gen-h18/README.md)).
The experimental bring-up modules for these live in `ane/h15`, `ane/h16` and
`ane/h18`; each directory's README-bringup.md says what its stages read,
write and refuse.
