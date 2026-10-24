#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the ANE DART nodes of a built T8152 DTB against the live ADT.

Usage: h18-dart-check.py ADT.bin DTB

For /arm-io/dart-ane instances 0 to 3 (the ADT reg windows 0 to 3, plus the 0x200000000 translation of the board
device tree) the script requires in the DTB: a node at that address with the same size, the ADT interrupt, and, where the
node has apple,tunable, exactly the ADT dart-tunables-instance-N records (24 byte records: u32 offset, u32 size,
u64 mask, u64 value). It also requires that the ane node's iommus names every instance with stream 0, and that any
other stream is a stream of the ADT iommu-parent of the ane node (mapper-ane reg 0, mapper-ane-mpm reg 11) on every
instance. It needs dtc on the PATH and prints one line per check; the exit status is 1 on the first mismatch. Nothing is written.
Checked: the DTBs built from t8152-j873-ane-dart.dts and t8152-j873-ane-dart-full.dts against the saved M6 ADT.
"""
import re
import struct
import subprocess
import sys

TRANSLATION = 0x200000000


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


def nm(n):
    return n['props'].get('name', b'').split(b'\0')[0].decode()


def find(node, *path):
    for want in path:
        node = next((k for k in node['kids'] if nm(k) == want), None)
        if node is None:
            return None
    return node


def adt_view(path):
    with open(path, 'rb') as f:
        root, _ = parse(f.read())
    dart = find(root, 'arm-io', 'dart-ane')
    ane = find(root, 'arm-io', 'ane')
    if dart is None or ane is None:
        sys.exit('no /arm-io/dart-ane or /arm-io/ane in the ADT')
    reg = [struct.unpack_from('<QQ', dart['props']['reg'], 16 * i) for i in range(len(dart['props']['reg']) // 16)]
    tun = []
    for i in range(4):
        b = dart['props'].get('dart-tunables-instance-%d' % i)
        if b is None:
            sys.exit('the ADT has no dart-tunables-instance-%d' % i)
        tun.append([struct.unpack_from('<IIQQ', b, o) for o in range(0, len(b), 24)])
    phandle = {struct.unpack('<I', m['props']['AAPL,phandle'])[0]: struct.unpack('<I', m['props']['reg'])[0]
               for m in dart['kids'] if 'reg' in m['props']}
    ip = struct.unpack('<Q', ane['props']['iommu-parent'])[0]
    streams = {phandle[ip & 0xffffffff], phandle[ip >> 32]}
    return reg, struct.unpack_from('<I', dart['props']['interrupts'])[0], tun, streams


def dtb_nodes(path):
    txt = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', path], capture_output=True, text=True, check=True).stdout
    nodes = {}
    for m in re.finditer(r'\n\t\t(iommu@[0-9a-f]+) \{\n(.*?)\n\t\t\};', txt, re.S):
        nodes[m.group(1)] = m.group(2)
    ane = re.search(r'\n\t\tane@419600000 \{\n(.*?)\n\t\t\};', txt, re.S)
    return nodes, ane.group(1) if ane else ''


def cells(body, prop):
    m = re.search(r'\b' + re.escape(prop) + r' = <(.*?)>;', body, re.S)
    return [int(x, 16) for x in m.group(1).split()] if m else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    reg, irq, tun, streams = adt_view(sys.argv[1])
    nodes, ane = dtb_nodes(sys.argv[2])
    bad = 0

    def say(ok, msg):
        nonlocal bad
        bad += not ok
        print(('ok   ' if ok else 'FAIL ') + msg)

    phand = {}
    for i in range(4):
        addr = reg[i][0] + TRANSLATION
        body = nodes.get('iommu@%x' % addr)
        if body is None:
            say(False, 'instance %d: no node at %#x' % (i, addr))
            continue
        r = cells(body, 'reg') or []
        say(r[:4] == [addr >> 32, addr & 0xffffffff, 0, reg[i][1]], 'instance %d: reg %#x size %#x' % (i, addr, reg[i][1]))
        say(cells(body, 'interrupts')[1] == irq, 'instance %d: interrupt %d' % (i, irq))
        t = cells(body, 'apple,tunable')
        want = [x for rec in tun[i] for x in (rec[0], rec[2], rec[3])]
        say(t is None or t == want, 'instance %d: %s' % (i, 'no tunables in the node' if t is None else
                                                          '%d tunable records equal the ADT' % len(tun[i])))
        phand[cells(body, 'phandle')[0]] = i
    iom = cells(ane, 'iommus') or []
    pairs = list(zip(iom[0::2], iom[1::2]))
    for i in range(4):
        have = {s for p, s in pairs if phand.get(p) == i}
        say(0 in have and have <= streams, 'ane iommus: instance %d has streams %s (ADT iommu-parent streams %s)' %
            (i, sorted(have), sorted(streams)))
    say(len({tuple(sorted(s for p, s in pairs if phand.get(p) == i)) for i in range(4)}) == 1,
        'ane iommus: the same streams on every instance')
    sys.exit(1 if bad else 0)


main()
