#!/usr/bin/env python3
"""
The SNES port against the native port, game frame by game frame: the
Genesis RAM as each game frame ends, from livetest (SHO_FRAMEDUMP: MD/f_N.ram)
and from snesrun -F (SN/shot_fN.ram). Prints the frames that differ, and the
addresses by the first frame they differ in.

usage: framecmp.py MD SN

Left out: bytes that depend on where the VBlanks fall in a frame, which
differs on the SNES (its frames take longer, so its VBlanks land inside
them). The VBlank's DMA stream ($FFC7CC-$FFC7FF) is counted apart.
"""
import collections
import os
import sys

TIMING = (set(range(0x0100, 0x0200))        # the stack
          | {0x0500, 0x0501}                # VBlank counter
          | set(range(0x0582, 0x058A))      # HUD digit scratch
          | {0x05F2, 0x05F3}
          | {0x094B + 0x18 * i for i in range(12)}  # tile update counts
          | set(range(0xC708, 0xC70E))      # palette flag, last DMA command
          | {0xC724, 0xC725}                # race VBlank phase
          | {0xC7EA, 0xC7EB}                # DMA slot pending
          | {0xC89A, 0xC89B}                # parts HUD redraw flag
          | set(range(0xE900, 0xEB00)))     # HBlank/VBlank buffers
DMA_STREAM = set(range(0xC7CC, 0xC800))

md, sn = sys.argv[1], sys.argv[2]
total = collections.Counter()
first = {}
k = bad = 0
while os.path.exists('%s/f_%d.ram' % (md, k)) and os.path.exists('%s/shot_f%d.ram' % (sn, k)):
    r = open('%s/f_%d.ram' % (md, k), 'rb').read()
    s = open('%s/shot_f%d.ram' % (sn, k), 'rb').read()
    diff = [a for a in range(0x10000) if a not in TIMING and r[a] != s[a]]
    if diff:
        bad += 1
        if bad <= 3:
            print('  frame %d ($FFC726 = %d): %s%s' % (
                k, r[0xC726], ' '.join('%04X:%02x/%02x' % (a, r[a], s[a]) for a in diff[:10]),
                ' ...' if len(diff) > 10 else ''))
        for a in diff:
            total[a] += 1
            first.setdefault(a, k)
    k += 1
print('  %d of %d game frames compared, %d differ' % (k, len(os.listdir(md)), bad))
other = sorted((a for a in total if a not in DMA_STREAM), key=lambda a: first[a])
if other:
    print('  outside the DMA stream:')
    for a in other[:25]:
        print('    %04X: %d frames, first %d' % (a, total[a], first[a]))
