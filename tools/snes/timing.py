#!/usr/bin/env python3
"""Video frames per game frame on the SNES, by game mode, from snesrun -T
(make snes-shots TIMING=file); with a livetest SHO_TIMING file as a second
argument, the same for the Genesis next to it.

usage: timing.py SNES_TIMING [GENESIS_TIMING]"""
import collections, sys

MODES = ['logo', 'title', 'course select', 'music select', 'race start', 'demo',
         'race', 'goal', 'ranking', 'command', 'shop', 'password', 'options',
         'ranking insert']

def load(path):
    video = collections.Counter(); game = collections.Counter()
    last = None
    for line in open(path):
        f, count, mode = line.split()
        mode = (int(mode, 16) & 0x7FFC) // 4
        name = MODES[mode] if mode < len(MODES) else hex(mode)
        video[name] += 1
        count = int(count)
        if last is not None and count != last:
            game[name] += (count - last) & 0xFF
        last = count
    return video, game

runs = [load(p) for p in sys.argv[1:]]
print('%-15s' % 'mode' + ''.join('%10s %8s %6s' % ('video', 'game', 'ratio') for _ in runs))
for name in MODES:
    if not any(v[name] for v, _ in runs):
        continue
    line = '%-15s' % name
    for v, g in runs:
        line += '%10d %8d %6s' % (v[name], g[name], '%.2f' % (v[name] / g[name]) if g[name] else '-')
    print(line)
