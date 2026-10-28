#!/usr/bin/env python3
"""
Check the SNES maps against the Genesis name tables, in a BW-RAM dump of the
SNES build (snesrun -B or -b; 256 KB): each entry of a map row the VDP layer
has converted (its flag in rows_a/rows_b/rows_w clear) is turned back into a
Genesis entry through the tile slots (md2snes) and compared with the one in
the VRAM copy (bank $42). Rows still waiting to be converted are left out.

usage: mapcheck.py DUMP.bwram [build/snes/shangon.dbg]
       mapcheck.py PREFIX_fN.maps [...]: snesrun -F's frame dump (banks $41
       and $42: .maps and .vram)
"""
import re
import struct
import sys

dbg = open(sys.argv[2] if len(sys.argv) > 2 else 'build/snes/shangon.dbg').read()
sym = {m.group(1): int(m.group(2), 16) for m in re.finditer(
    r'sym\tid=\d+,name="([^"]+)",addrsize=\w+,(?:size=\d+,)?scope=\d+,def=[\d+]+'
    r'(?:,ref=[\d+]+)?,val=(0x[0-9A-F]+),seg=\d+,type=lab', dbg)}
if sys.argv[1].endswith('.maps'):
    base = sys.argv[1][:-len('.maps')]
    bw = bytes(0x10000) + open(base + '.maps', 'rb').read() + open(base + '.vram', 'rb').read()
else:
    bw = open(sys.argv[1], 'rb').read()


def at(name):
    return sym[name] - 0x400000


VRAM = 0x20000
md2snes = struct.unpack_from('<2048H', bw, at('md2snes'))
tile_of_slot = {s - 1: t for t, s in enumerate(md2snes) if s}
rows = bw[at('rows_a'):at('rows_a') + 160]

# (name, Genesis table, SNES map, rows, first flag)
planes = [('A', 0xC000, at('map_a'), 64, 0), ('B', 0xE000, at('map_b'), 64, 64),
          ('W', 0xD000, at('map_w'), 32, 128)]
bad = checked = 0
for name, table, snes_map, nrows, flag0 in planes:
    for row in range(nrows):
        if rows[flag0 + row]:
            continue                    # waiting to be converted
        for col in range(64):
            g = struct.unpack_from('>H', bw, VRAM + table + row * 128 + col * 2)[0]
            index = (1024 if col & 32 else 0) | (2048 if row & 32 else 0) \
                | (row & 31) * 32 | (col & 31)
            e = struct.unpack_from('<H', bw, snes_map + index * 2)[0]
            checked += 1
            want_slot = md2snes[g & 0x7FF] - 1
            if want_slot < 0:
                want_slot = 0           # (slots ran out: slot 0)
            want = (((g >> 12) & 1) << 15 | ((g >> 11) & 1) << 14 | (g >> 15) << 13
                    | ((g >> 13) & 3) << 10 | want_slot)
            if e != want:
                bad += 1
                if bad <= 10:
                    print('%s row %2d col %2d: Genesis %04X (tile %03X), SNES %04X (slot %03X = tile %s), want %04X'
                          % (name, row, col, g, g & 0x7FF, e, e & 0x3FF,
                             '%03X' % tile_of_slot[e & 0x3FF] if (e & 0x3FF) in tile_of_slot else '?', want))
print('%d entries checked, %d wrong' % (checked, bad))
sys.exit(1 if bad else 0)
