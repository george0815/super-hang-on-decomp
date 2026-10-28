#!/usr/bin/env python3
"""The game's frame count ($FFC726) and mode ($FFC704) at every VBlank of a
gpgx recording (build/ref/NAME.vtrace), in the format of snesrun -T, so
tools/snes/timing.py can set the SNES against the real Genesis (livetest's
own timing leaves out the time the C takes).

usage: vtrace_timing.py NAME.vtrace OUT"""
import gzip, sys

HEADER = 4 * 3 + 4 * 18 + 32 + 4 + 128 + 128 + 8   # see tools/trace/gpgx_fncap.c
RECORD = HEADER + 0x10000
with gzip.open(sys.argv[1], 'rb') as f, open(sys.argv[2], 'w') as out:
    n = 0
    while True:
        r = f.read(RECORD)
        if len(r) < RECORD:
            break
        ram = r[HEADER:]
        out.write('%d %u %04X\n' % (n, ram[0xC726], ram[0xC704] << 8 | ram[0xC705]))
        n += 1
