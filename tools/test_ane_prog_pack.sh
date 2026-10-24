#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
#
# test_ane_prog_pack.sh: builds tools/ane-prog-pack.c on a host without libdrm (a stub drm.h with the five macros ane_accel.h needs), packs the checked-in H14 add
# program (fixtures/h14-anec/add) with two input files, and checks the program file: the header, the six sections byte-equal to the fixture's section files
# (generic, kernel, descriptor, operation, procedure, tdprop at ids 1 to 5 and 7), the three buffers with their sizes and directions, the input data at
# 16 byte aligned offsets, and that every size and offset lies inside the file. A wrong input buffer id and an oversized input file must fail. Needs cc and python3.
# usage: sh tools/test_ane_prog_pack.sh   (from the repository root)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/stub"
printf '#include <linux/ioctl.h>\n#include <linux/types.h>\n#define DRM_IOCTL_BASE 0x64\n#define DRM_COMMAND_BASE 0x40\n#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)\n#define DRM_IOR(nr, t) _IOR(DRM_IOCTL_BASE, nr, t)\n#define DRM_IOW(nr, t) _IOW(DRM_IOCTL_BASE, nr, t)\n#define DRM_IOWR(nr, t) _IOWR(DRM_IOCTL_BASE, nr, t)\n' > "$tmp/stub/drm.h"
cc -O1 -I "$tmp/stub" -I "$root/libane" -I "$root/ane/src/uapi/drm" -o "$tmp/pack" "$here/ane-prog-pack.c" "$root/libane/ane_m2.c" "$root/libane/ane_sha256.c" 2> "$tmp/cc.err" ||
  { cat "$tmp/cc.err" >&2; echo "FAIL cannot build ane-prog-pack" >&2; exit 1; }
fx="$root/fixtures/h14-anec/add"
python3 - "$tmp" <<'EOF'
import struct, sys
t = sys.argv[1]
open(t + "/a.f16", "wb").write(struct.pack("<H", 0x3e00) * 16384)   # 1.5
open(t + "/b.f16", "wb").write(struct.pack("<H", 0x4080) * 16384)   # 2.25
open(t + "/big.f16", "wb").write(b"\0" * 32769)
EOF
"$tmp/pack" --anec "$fx/program-0.anec" --in 5="$tmp/a.f16" --in 6="$tmp/b.f16" -o "$tmp/prog.bin" > "$tmp/pack.out"
fails=0
if "$tmp/pack" --anec "$fx/program-0.anec" --in 4="$tmp/a.f16" -o "$tmp/x.bin" > /dev/null 2>&1; then echo "FAIL an input file for the output buffer was accepted"; fails=1; fi
if "$tmp/pack" --anec "$fx/program-0.anec" --in 5="$tmp/big.f16" -o "$tmp/x.bin" > /dev/null 2>&1; then echo "FAIL an input file larger than its buffer was accepted"; fails=1; fi
if "$tmp/pack" --anec "$tmp/a.f16" -o "$tmp/x.bin" > /dev/null 2>&1; then echo "FAIL a file that is not an ANEC was accepted"; fails=1; fi
python3 - "$tmp" "$fx" <<'EOF' || fails=1
import hashlib, struct, sys
t, fx = sys.argv[1], sys.argv[2]
b = open(t + "/prog.bin", "rb").read()
bad = 0
def chk(ok, what):
    global bad
    print(("  PASS " if ok else "  FAIL ") + what)
    bad += 0 if ok else 1
magic, ver, nsec, nio, fsize, rsv = struct.unpack_from("<IIIIQQ", b, 0)
chk(magic == 0x50454e41 and ver == 1 and nsec == 6 and nio == 3 and fsize == len(b) and rsv == 0, "header: ANEP, version 1, 6 sections, 3 buffers, size field = file size")
names = {1: "generic.bin", 2: "kernel.bin", 3: "descriptor.bin", 4: "operation.bin", 5: "procedure.bin", 7: "tdprop.bin"}
ids = []
for i in range(nsec):
    sid, z, size, off, z2 = struct.unpack_from("<IIQQQ", b, 0x20 + i * 0x20)
    ids.append(sid)
    ok = sid in names and off % 16 == 0 and off + size <= len(b) and z == 0 and z2 == 0
    ok = ok and b[off:off + size] == open(fx + "/" + names[sid], "rb").read()
    chk(ok, "section %d is byte-equal to %s" % (sid, names.get(sid)))
chk(sorted(ids) == [1, 2, 3, 4, 5, 7], "section ids 1, 2, 3, 4, 5 and 7, each once")
want = {5: (0, 0x3e00), 6: (0, 0x4080), 4: (1, None)}
for i in range(nio):
    bid, d, size, doff, dsz = struct.unpack_from("<IIQQQ", b, 0x20 + (nsec + i) * 0x20)
    wd, pat = want[bid]
    ok = d == wd and size == 32768
    if pat is None:
        ok = ok and doff == 0 and dsz == 0
    else:
        ok = ok and dsz == 32768 and doff % 16 == 0 and doff + dsz <= len(b) and b[doff:doff + dsz] == struct.pack("<H", pat) * 16384
    chk(ok, "buffer %d: direction %d, 32768 bytes, %s" % (bid, d, "zero filled" if pat is None else "input data from its file"))
sys.exit(1 if bad else 0)
EOF
[ "$fails" = 0 ] && echo "test_ane_prog_pack: all checks passed"
exit "$fails"
