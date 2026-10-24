#!/usr/bin/env python3
"""Print the instructions of functions: their own code, with jumps into
other functions' code noted rather than repeated.

usage: show.py ROM ADDR...
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k  # noqa: E402
from recomp import Recompiler  # noqa: E402
from shodis import Hints, ROOT  # noqa: E402


def main():
    rc = Recompiler(open(sys.argv[1], 'rb').read(), Hints.load(os.path.join(ROOT, 'config', 'hints.txt')))
    lab = rc.dis.labels()
    ents = rc.entries()
    for a in sys.argv[2:]:
        f = int(a, 16)
        body = rc.body(f)
        # code that is another function's own body is shown by reference
        others = set()
        for e in ents:
            if e != f and e in body:
                others |= rc.body(e)
        print('== %06X' % f)
        for k in sorted(body - others):
            i = rc.dis.insns[k]
            l = lab.get(k)
            print('%06X %-10s %s' % (k, (l + ':') if l and k != f else '', m68k.fmt_insn(i, lab.get)))
        shared = sorted(e for e in ents if e != f and e in body)
        if shared:
            print('   (continues into %s)' % ' '.join('%06X' % e for e in shared))


if __name__ == '__main__':
    main()
