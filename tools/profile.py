#!/usr/bin/env python3
"""Summarise a build/profile run: which original functions still execute.

Each instruction is credited to the function whose own code it is (code
shared with a called entry belongs to that entry, as in show.py).

usage: profile.py ROM profile.txt [N]   top N functions by instructions run
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recomp import Recompiler  # noqa: E402
from shodis import Hints, ROOT  # noqa: E402


def main():
    rc = Recompiler(open(sys.argv[1], 'rb').read(), Hints.load(os.path.join(ROOT, 'config', 'hints.txt')))
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    ents = rc.entries()
    bodies = {e: rc.body(e) for e in ents}
    owner = {}
    # smallest body first, so an address goes to the innermost entry
    for e in sorted(ents, key=lambda e: len(bodies[e])):
        for a in bodies[e]:
            owner.setdefault(a, e)
    count, run = {}, {}
    for line in open(sys.argv[2]):
        a, c = line.split()
        a, c = int(a, 16), int(c)
        e = owner.get(a)
        count[e] = count.get(e, 0) + c
        run.setdefault(e, set()).add(a)
    total = sum(count.values())
    print('%d original instructions run' % total)
    print('entry    insns run   share  covered/size')
    for e in sorted(count, key=count.get, reverse=True)[:n]:
        size = len(bodies[e]) if e in bodies else 0
        print('%-8s %10d  %5.1f%%  %4d/%d' % ('%06X' % e if e is not None else '?', count[e],
                                           100.0 * count[e] / total, len(run[e]), size))


if __name__ == '__main__':
    main()
