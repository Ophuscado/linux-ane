#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Print the fw_seg= argument for ane_h15 stage=5 map_fw=1 from an ADT dump.

Usage: h15-segments.py ADT.bin

ADT.bin must be the live ADT of the SAME boot you will insmod on: iBoot picks the physical addresses when it
loads the firmware. Whether those addresses repeat from one boot to the next is NOT KNOWN, so do not reuse a
dump from another boot. With m1n1's proxyclient attached to that boot, the dump is
    python3 -c "from m1n1.setup import *; open('adt.bin', 'wb').write(u.get_adt())"
(proxyutils.get_adt reads devtree at boot_args.devtree). Never from macOS.

The /arm-io/ane node's segment-ranges holds two 32-byte little-endian records {phys, iova offset, remap iova,
size | flags << 32}. This script walks the ADT without m1n1's python modules, prints the records and the
fw_seg= argument, and refuses anything that is not exactly two records, a layout where remap != vm-base + offset,
or a DATA offset that is not the TEXT size (ane_h15 derives both IOVAs from the RVBAR entry and would be wrong).
Checked against the saved M6 ADT: it prints fw_seg=100014e8000:c4000,100030c4000:2ac000, w7C's decode. Nothing
is written.
"""
import struct
import sys


def parse(b, off=0):
    nprops, nchild = struct.unpack_from('<II', b, off)
    off += 8
    props = {}
    for _ in range(nprops):
        name = b[off:off + 32].split(b'\0')[0].decode('latin1')
        size = struct.unpack_from('<I', b, off + 32)[0] & 0x7fffffff
        off += 36
        props[name] = b[off:off + size]
        off += (size + 3) & ~3
    kids = []
    for _ in range(nchild):
        kid, off = parse(b, off)
        kids.append(kid)
    return {'props': props, 'kids': kids}, off


def find(node, *path):
    for want in path:
        for k in node['kids']:
            if k['props'].get('name', b'').split(b'\0')[0].decode() == want:
                node = k
                break
        else:
            return None
    return node


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    root, _ = parse(data)
    ane = find(root, 'arm-io', 'ane')
    if ane is None:
        sys.exit('no /arm-io/ane node in this ADT')
    seg = ane['props'].get('segment-ranges')
    if seg is None or len(seg) != 64:
        sys.exit(f'segment-ranges is {"absent" if seg is None else str(len(seg)) + " bytes"}, expected exactly two 32-byte records')
    recs = [struct.unpack_from('<QQQQ', seg, 32 * i) for i in range(2)]
    for i, (phys, off, remap, sz) in enumerate(recs):
        print(f'record {i}: phys={phys:#x} iova_offset={off:#x} remap={remap:#x} size={sz & 0xffffffff:#x} flags={sz >> 32:#x}')
    text, data_ = recs
    vm_base = text[2] - text[1]
    for _, off, remap, _ in recs:
        if remap != vm_base + off:
            sys.exit('remap != vm_base + offset: the layout differs from the M6, ane_h15 would derive wrong IOVAs')
    if data_[1] != text[3] & 0xffffffff:
        sys.exit('DATA offset is not the TEXT size: ane_h15 puts DATA directly behind TEXT and would be wrong')
    print(f'fw_seg={text[0]:x}:{text[3] & 0xffffffff:x},{data_[0]:x}:{data_[3] & 0xffffffff:x}')


if __name__ == '__main__':
    main()
