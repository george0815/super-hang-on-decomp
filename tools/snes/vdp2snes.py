#!/usr/bin/env python3
"""
Convert a Mega Drive VDP snapshot (tests/livetest.c writes them next to its
screenshots: VRAM, CRAM, VSRAM, registers) into SNES data for the screen
viewer (snes/snap.s): mode 1 tiles, BG1/BG2 tile maps, CGRAM, OAM and
per-line HDMA tables for the scroll registers.

usage: vdp2snes.py SNAPSHOT.vdp OUTDIR [CROP]

CROP is how many pixels of the Genesis's 320 are cut off on the left to fit
the SNES's 256 (default 32: the middle of the screen).

Mapping:
  plane A, or the window plane on the lines it covers -> BG1
  plane B -> BG2
  sprites -> drawn, then cut into 16x16 (or 32x32) OBJs on a grid
  CRAM lines 0-3 -> BG palettes 0-3 and OBJ palettes 0-3
  priority: sprite high/low -> OBJ priority 3/2, which sits between the BG
  layers' high and low tiles the way the VDP orders its planes
"""
import os
import struct
import sys

# SNES VRAM layout (word addresses)
BG_TILES = 0x0000      # up to $3800 words: 896 tiles, shared by BG1 and BG2
WINDOW_MAP = 0x3800    # 64x32
PLANE_A_MAP = 0x4000   # 64x64
PLANE_B_MAP = 0x5000   # 64x64
OBJ_TILES = 0x6000     # 512 tiles
MAX_BG_TILES = 0x3800 // 16
MAX_OBJ_TILES = 512


def tile_to_snes(md):
    """32 bytes of Mega Drive 4bpp (nibble packed, left pixel high) to SNES
    4bpp planar (planes 0/1 interleaved by row, then planes 2/3)."""
    return planar([[(md[r * 4 + x // 2] >> (4 if x % 2 == 0 else 0)) & 15 for x in range(8)] for r in range(8)])


def planar(rows):
    """8 rows of 8 pixel values to a SNES 4bpp tile."""
    out = bytearray(32)
    for r, pix in enumerate(rows):
        for plane in range(4):
            b = 0
            for x in range(8):
                b |= ((pix[x] >> plane) & 1) << (7 - x)
            out[(plane // 2) * 16 + r * 2 + (plane & 1)] = b
    return bytes(out)


def sprite_pixels(vram, attr, w, h):
    """A sprite's pixels, flips applied; its tiles go down the columns."""
    img = [[0] * (8 * w) for _ in range(8 * h)]
    for col in range(w):
        for row in range(h):
            t = ((attr & 0x7FF) + col * h + row) & 0x7FF
            for r in range(8):
                for c in range(8):
                    b = vram[t * 32 + r * 4 + c // 2]
                    img[row * 8 + r][col * 8 + c] = (b >> 4 if c % 2 == 0 else b) & 15
    if attr & 0x800:
        img = [row[::-1] for row in img]
    if attr & 0x1000:
        img = img[::-1]
    return img


def alloc_units(used, size):
    """First free size x size block of 2x2-tile units (8 units per row of
    the OBJ tile grid); None when full."""
    for u in range(len(used)):
        if u % 8 + size > 8 or u // 8 + size > len(used) // 8 or (size == 2 and (u % 2 or u // 8 % 2)):
            continue
        block = [u + dy * 8 + dx for dy in range(size) for dx in range(size)]
        if not any(used[b] for b in block):
            for b in block:
                used[b] = True
            return u
    return None


def sprite_canvas(vram, sprites, crop):
    """The sprite layer as the VDP draws it: per screen pixel, the first
    sprite's (palette, priority, value), or None."""
    canvas = [[None] * 256 for _ in range(224)]
    link, seen = 0, set()
    while link not in seen and len(seen) < 80:
        seen.add(link)
        y, size, nxt, attr, x = struct.unpack_from('>HBBHH', vram, sprites + 8 * link)
        y, x = (y & 0x3FF) - 128, (x & 0x1FF) - 128 - crop
        w, h = (size >> 2 & 3) + 1, (size & 3) + 1
        img = sprite_pixels(vram, attr, w, h)
        for r in range(8 * h):
            for c in range(8 * w):
                sx, sy = x + c, y + r
                if img[r][c] and 0 <= sx < 256 and 0 <= sy < 224 and canvas[sy][sx] is None:
                    canvas[sy][sx] = (attr >> 13 & 3, attr >> 15, img[r][c])
        link = nxt & 0x7F
        if link == 0:
            break
    return canvas


def sprite_objs(canvas, box):
    """The sprite layer cut on a box x box grid: one OBJ per palette and
    priority in each box that has pixels. Aligned boxes never overlap, so a
    line needs at most 256 / 8 OBJ tiles (the PPU fetches 34). Returns the
    OAM entries, high table bits, tiles and the tile units used."""
    objs, high = [], []
    obj_data = bytearray(MAX_OBJ_TILES * 32)
    used = [False] * (MAX_OBJ_TILES // 4)        # 2x2-tile units, 8 per row
    for by in range(0, 224, box):
        for bx in range(0, 256, box):
            kinds = sorted({canvas[y][x][:2] for y in range(by, min(by + box, 224))
                            for x in range(bx, bx + box) if canvas[y][x]})
            for pal, prio in kinds:
                pix = [[canvas[y][x][2] if y < 224 and canvas[y][x] and canvas[y][x][:2] == (pal, prio) else 0
                        for x in range(bx, bx + box)] for y in range(by, by + box)]
                unit = alloc_units(used, box // 16)
                if unit is None:
                    print('vdp2snes: out of OBJ tiles', file=sys.stderr)
                    continue
                t = (unit // 8) * 32 + (unit % 8) * 2          # top-left tile
                for ty in range(box // 8):
                    for tx in range(box // 8):
                        n = t + ty * 16 + tx
                        obj_data[n * 32:n * 32 + 32] = planar([pix[ty * 8 + r][tx * 8:tx * 8 + 8] for r in range(8)])
                objs.append(struct.pack('<BBBB', bx, by, t & 0xFF, (3 if prio else 2) << 4 | pal << 1 | t >> 8))
                high.append(2 if box == 32 else 0)
    return objs, high, obj_data, used


def color_to_snes(w):
    """CRAM word ----BBB-GGG-RRR- to SNES -BBBBBGGGGGRRRRR."""
    def five(v3):
        return v3 << 2 | v3 >> 1
    r, g, b = (w >> 1) & 7, (w >> 5) & 7, (w >> 9) & 7
    return five(r) | five(g) << 5 | five(b) << 10


class Tiles:
    """Tiles used, numbered in order of first use."""
    def __init__(self, vram, limit, what):
        self.vram, self.limit, self.what, self.index, self.data = vram, limit, what, {}, []

    def get(self, md_tile):
        if md_tile not in self.index:
            if len(self.data) >= self.limit:
                raise SystemExit('%s: more than %d tiles' % (self.what, self.limit))
            self.index[md_tile] = len(self.data)
            self.data.append(tile_to_snes(self.vram[md_tile * 32:md_tile * 32 + 32]))
        return self.index[md_tile]


def snes_entry(md, tiles):
    """Name table entry: MD p ll v h ttttttttttt -> SNES v h p ppp tttttttttt."""
    t = tiles.get(md & 0x7FF)
    return (md >> 12 & 1) << 15 | (md >> 11 & 1) << 14 | (md >> 15) << 13 | (md >> 13 & 3) << 10 | t


def snes_map(vram, base, width, height, tiles):
    """A MD name table (row major) as SNES 32x32 screens: left/right, then
    top/bottom."""
    out = [0] * (64 * 64 if height > 32 else 64 * 32)
    for row in range(height):
        for col in range(width):
            md = struct.unpack_from('>H', vram, (base + 2 * (row * width + col)) & 0xFFFF)[0]
            screen = (col >> 5) + (2 if row >= 32 else 0)
            out[screen * 1024 + (row & 31) * 32 + (col & 31)] = snes_entry(md, tiles)
    return struct.pack('<%dH' % len(out), *out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    snap = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    crop = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    vram = snap[:0x10000]
    cram = struct.unpack_from('<64H', snap, 0x10000)
    reg = snap[0x10100:0x10120]
    line_vsram = struct.unpack_from('<448H', snap, 0x10120)   # per line: A, B

    plane_a = (reg[2] & 0x38) << 10
    plane_b = (reg[4] & 7) << 13
    window = (reg[3] & 0x3C) << 10
    sprites = (reg[5] & 0x7E) << 9
    hscroll = (reg[13] & 0x3F) << 10
    width = {0: 32, 1: 64, 3: 128}[reg[16] & 3]
    height = {0: 32, 1: 64, 3: 128}[reg[16] >> 4 & 3]
    if width != 64 or height != 64:
        sys.exit('only 64x64 planes are handled (got %dx%d)' % (width, height))
    if reg[11] & 4:
        sys.exit('2-cell vertical scroll is not handled')
    if reg[17]:
        sys.exit('horizontal window split is not handled')
    window_down = reg[18] & 0x80
    window_rows = reg[18] & 0x1F

    bg = Tiles(vram, MAX_BG_TILES, 'BG')
    map_a = snes_map(vram, plane_a, 64, 64, bg)
    map_b = snes_map(vram, plane_b, 64, 64, bg)
    map_w = snes_map(vram, window, 64, 32, bg)

    # per-line scroll; 225 lines, the first (never shown) a copy of the next
    hmode = reg[11] & 3
    lines = []
    for line in range(224):
        hs = {0: 0, 2: line & ~7, 3: line}[hmode] * 4
        ha, hb = struct.unpack_from('>HH', vram, hscroll + hs)
        in_window = (line >> 3 >= window_rows) if window_down else (line >> 3 < window_rows)
        if in_window:
            a = (WINDOW_MAP >> 10 << 2 | 1, crop & 0x3FF, 0x3FF)    # not scrolled
        else:
            a = (PLANE_A_MAP >> 10 << 2 | 3, (crop - ha) & 0x3FF, (line_vsram[2 * line] - 1) & 0x3FF)
        b = ((crop - hb) & 0x3FF, (line_vsram[2 * line + 1] - 1) & 0x3FF)
        lines.append((a, b))
    lines.insert(0, lines[0])

    def hdma(entries, fmt):
        data = bytearray()
        for start in range(0, len(entries), 127):
            chunk = entries[start:start + 127]
            data.append(0x80 | len(chunk))
            for e in chunk:
                data += struct.pack(fmt, *e)
        return bytes(data + b'\0')

    bg1sc = hdma([(a[0],) for a, b in lines], '<B')
    bg1ofs = hdma([(a[1], a[2]) for a, b in lines], '<HH')
    bg2ofs = hdma([b for a, b in lines], '<HH')

    # sprites: drawn, then cut into 16x16 OBJs, or 32x32 when that needs
    # more than 128
    canvas = sprite_canvas(vram, sprites, crop)
    for box in (16, 32):
        objs, high, obj_data, used = sprite_objs(canvas, box)
        if len(objs) <= 128:
            break
    if len(objs) > 128:
        print('vdp2snes: %d OBJs, the first 128 kept' % len(objs), file=sys.stderr)
        objs, high = objs[:128], high[:128]
    oam = bytearray(b''.join(objs) + b'\0\xF0\0\0' * (128 - len(objs)))   # unused: y = 240
    hi = bytearray(32)
    for k, bits in enumerate(high):
        hi[k // 4] |= bits << (2 * (k % 4))
    oam += hi
    obj_count = sum(used) * 4

    cgram = [0] * 256
    for k in range(64):
        cgram[k] = cgram[128 + k] = color_to_snes(cram[k])
    cgram[0] = color_to_snes(cram[reg[7] & 0x3F])   # the backdrop

    os.makedirs(out, exist_ok=True)
    files = {
        'bg_tiles.bin': b''.join(bg.data),
        'obj_tiles.bin': bytes(obj_data),
        'map_a.bin': map_a, 'map_b.bin': map_b, 'map_w.bin': map_w,
        'cgram.bin': struct.pack('<256H', *cgram), 'oam.bin': bytes(oam),
        'hdma_bg1sc.bin': bg1sc, 'hdma_bg1ofs.bin': bg1ofs, 'hdma_bg2ofs.bin': bg2ofs,
    }
    for name, data in files.items():
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
    print('vdp2snes: %d BG tiles, %d OBJ tiles, %d OBJs' % (len(bg.data), obj_count, len(objs)))


if __name__ == '__main__':
    main()
