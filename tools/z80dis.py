#!/usr/bin/env python3
"""Disassemble the Z80 sound driver (ROM $7D7F4, $1A24 bytes, loaded at Z80
address 0) by tracing from the reset and interrupt vectors.

usage: z80dis.py ROM OUT.s [extra entry points (hex)...]
"""

# jump tables of little-endian code pointers: (address, count)
JUMP_TABLES = [
    (0x0677, 28),   # song commands $E4-$FF
    (0x1066, 5),    # 68000 commands $B0-$B4
]
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import z80  # noqa: E402

DRV_ROM, DRV_LEN = 0x7D7F4, 0x1A24


def main():
    rom = open(sys.argv[1], 'rb').read()
    drv = rom[DRV_ROM:DRV_ROM + DRV_LEN]
    entries = [0x0000, 0x0038] + [int(x, 16) for x in sys.argv[3:]]
    for base, count in JUMP_TABLES:
        entries += [drv[base + 2 * k] | drv[base + 2 * k + 1] << 8 for k in range(count)]
    seen, labels, todo = {}, set(entries), list(entries)
    while todo:
        a = todo.pop()
        while a < len(drv) and a not in seen:
            n, text, flow, tgt = z80.decode(drv, a)
            seen[a] = (n, text)
            if tgt is not None and tgt < len(drv):
                todo.append(tgt)
                labels.add(tgt)
            a += n
            if flow in ('jp', 'ret', 'jpind', 'halt'):
                break
    out = []
    a = 0
    while a < len(drv):
        if a in labels:
            out.append('L%04X:' % a)
        if a in seen:
            n, text = seen[a]
            out.append('    %-24s ; %04X  %s' % (text, a, drv[a:a + n].hex()))
            a += n
        elif any(base <= a < base + 2 * count for base, count in JUMP_TABLES):
            out.append('    dw L%04X%s ; %04X' % (drv[a] | drv[a + 1] << 8, ' ' * 14, a))
            a += 2
        else:
            b = a
            while b < len(drv) and b not in seen and b not in labels and b - a < 16 and not any(
                    base <= b < base + 2 * count for base, count in JUMP_TABLES):
                b += 1
            out.append('    db %-21s ; %04X' % (','.join('$%02X' % c for c in drv[a:b]), a))
            a = b
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
    print('%d instructions, %d bytes of code' % (len(seen), sum(n for n, _ in seen.values())))


if __name__ == '__main__':
    main()
