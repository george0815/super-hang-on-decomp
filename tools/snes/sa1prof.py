#!/usr/bin/env python3
"""Functions by share of the SA-1's cycles, from a SA1PROF file written by the
snes9x core with tools/snes/snes9x-sa1prof.patch: sa1prof.py FILE [N]"""
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
c = collections.Counter(); total = 0
for k in range(0, len(data), 8):
    a, n = struct.unpack_from('<II', data, k)
    i = bisect.bisect_right(addrs, a) - 1
    c[starts[i][1] if i >= 0 else '?'] += n
    total += n
for name, k in c.most_common(int(sys.argv[2]) if len(sys.argv) > 2 else 60):
    print('%5.1f%%  %s' % (100.0 * k / total, name))
