#!/usr/bin/env python3
"""Functions by share of the SA-1's cycles, from a SA1PROF file written by the
snes9x core with tools/snes/snes9x-sa1prof.patch: sa1prof.py FILE [N]

Each line: share of the cycles, times the function's first instruction ran
(its calls, for a function entered at the top), cycles per call, name."""
import re, struct, collections, bisect, sys
dbg = open('build/snes/shangon.dbg').read()
starts = []
for m in re.finditer(r'sym\tid=\d+,name="([^"@][^"]*)",addrsize=\w+,(?:size=\d+,)?scope=\d+,def=[\d+]+(?:,ref=[\d+]+)?,val=(0x[0-9A-F]+),seg=\d+,type=lab', dbg):
    a = int(m.group(2), 16)
    if a >= 0xC00000 or a < 0x400000:
        starts.append((a, m.group(1)))
starts.sort()
addrs = [a for a, _ in starts]
data = open(sys.argv[1], 'rb').read()
cycles = collections.Counter()
calls = collections.Counter()
total = 0
for k in range(0, len(data), 12):
    a, n, count = struct.unpack_from('<III', data, k)
    i = bisect.bisect_right(addrs, a) - 1
    name = starts[i][1] if i >= 0 else '?'
    cycles[name] += n
    if i >= 0 and a == starts[i][0]:
        calls[name] += count
    total += n
for name, n in cycles.most_common(int(sys.argv[2]) if len(sys.argv) > 2 else 60):
    c = calls[name]
    print('%5.1f%% %8d %9s  %s' % (100.0 * n / total, c, '%.0f' % (n / c) if c else '-', name))
