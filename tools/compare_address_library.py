"""Compare two Address Library files (formats 1/2 and 5) by ID.

Usage: python compare_address_library.py <old.bin> <new.bin> [ids.txt]

Reports how many old IDs survive, how many neighbouring pairs keep an identical gap (high = same
numbering, ~0 = renumbered), and, given a file of IDs your plugin uses (one per line, or grep output
of RELOCATION_ID/REL::ID), which of them are missing in the new file.
Reads the files you installed; copies nothing.
"""
import struct, sys, re


def load(path):
    b = open(path, 'rb').read()
    fmt = struct.unpack_from('<i', b, 0)[0]
    if fmt == 5:
        ver = struct.unpack_from('<4I', b, 4)
        n = struct.unpack_from('<i', b, 92)[0]
        arr = struct.unpack_from('<%dI' % n, b, 96)
        return fmt, ver, {i: o for i, o in enumerate(arr) if o}
    if fmt not in (1, 2):
        sys.exit(f'{path}: unsupported format {fmt}')
    ver = struct.unpack_from('<4i', b, 4)
    nl = struct.unpack_from('<i', b, 20)[0]
    p = 24 + nl
    ps, n = struct.unpack_from('<ii', b, p)
    p += 8
    m, pid, po = {}, 0, 0
    sizes = {0: 8, 2: 1, 3: 1, 4: 2, 5: 2, 6: 2, 7: 4}
    for _ in range(n):
        t = b[p]; p += 1
        lo, hi = t & 15, t >> 4
        v = int.from_bytes(b[p:p + sizes.get(lo, 0)], 'little'); p += sizes.get(lo, 0)
        i = {0: v, 1: pid + 1, 2: pid + v, 3: pid - v, 4: pid + v, 5: pid - v, 6: v, 7: v}[lo]
        tmp = po // ps if hi & 8 else po
        h = hi & 7
        v = int.from_bytes(b[p:p + sizes.get(h, 0)], 'little'); p += sizes.get(h, 0)
        o = {0: v, 1: tmp + 1, 2: tmp + v, 3: tmp - v, 4: tmp + v, 5: tmp - v, 6: v, 7: v}[h]
        if hi & 8:
            o *= ps
        m[i] = o; pid, po = i, o
    return fmt, ver, m


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fa, va, a = load(sys.argv[1])
    fb, vb, b = load(sys.argv[2])
    print(f'old: format {fa} version {".".join(map(str, va))} ids {len(a)} max {max(a)}')
    print(f'new: format {fb} version {".".join(map(str, vb))} ids {len(b)} max {max(b)}')
    common = sorted((o, i) for i, o in a.items() if i in b)
    gaps = [(x, y) for x, y in zip(common, common[1:]) if y[0] - x[0] < 0x4000]
    same = sum(1 for (o1, i1), (o2, i2) in gaps if b[i2] - b[i1] == o2 - o1)
    print(f'old IDs present in new: {len(common)} of {len(a)} ({100 * len(common) / len(a):.1f}%)')
    print(f'neighbour gaps identical: {same} of {len(gaps)} ({100 * same / max(len(gaps), 1):.1f}%)')
    if len(sys.argv) > 3:
        ids = sorted({int(x) for x in re.findall(r'\b\d{1,7}\b', open(sys.argv[3]).read())} & set(a))
        miss = [i for i in ids if i not in b]
        print(f'your IDs found in old library: {len(ids)}; missing in new: {len(miss)}')
        for i in miss:
            print(f'  MISSING {i}')


if __name__ == '__main__':
    main()
