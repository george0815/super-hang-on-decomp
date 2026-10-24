#!/usr/bin/env python3
"""Call graph queries for planning rewrites.

usage: callgraph.py ROM ADDR...   list every function reachable from ADDRs
                                  (calls and indirect jumps) with size and
                                  interface, smallest first
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recomp import Recompiler, load_handwritten  # noqa: E402
from shodis import Hints, ROOT  # noqa: E402
from flow import Flow, mask_names  # noqa: E402


def build(rom_path):
    rc = Recompiler(open(rom_path, 'rb').read(), Hints.load(os.path.join(ROOT, 'config', 'hints.txt')))
    return rc, Flow(rc)


def callees(fl, f):
    insns = fl.dis.insns
    out = set()
    for a in fl.bodies[f]:
        i = insns[a]
        if i.flow in ('jsr', 'bsr') or (i.flow == 'jmp' and i.target is None):
            ts = [i.target] if i.target is not None else (fl._targets.get(a) or [])
            out |= {t for t in ts if t in fl.bodies}
    return out


def subtree(fl, roots):
    seen, st = set(), list(roots)
    while st:
        f = st.pop()
        if f in seen:
            continue
        seen.add(f)
        st += list(callees(fl, f))
    return seen


def main():
    rc, fl = build(sys.argv[1])
    hand = load_handwritten(os.path.join(ROOT, 'config', 'handwritten.txt'))
    st = subtree(fl, [int(a, 16) for a in sys.argv[2:]])
    todo = [f for f in st if f not in hand]
    print('%d functions, %d not yet handwritten' % (len(st), len(todo)))
    for f in sorted(st, key=lambda f: len(fl.bodies[f])):
        print('%06X %4d %s in: %-28s out: %s' % (f, len(fl.bodies[f]), 'H' if f in hand else ' ',
                                                 mask_names(fl.inputs[f]), mask_names(fl.outputs[f])))


if __name__ == '__main__':
    main()
