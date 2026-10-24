#!/usr/bin/env python3
"""Tracing disassembler for Super Hang-On (Mega Drive).

Follows control flow from the vector table plus manual hints
(config/hints.txt), resolves jump tables, and emits Motorola-syntax
assembly plus a coverage report.

usage: shodis.py ROM [--asm OUT.s] [--report] [--syms OUT.txt]
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


class Hints:
    def __init__(self):
        self.code = set()        # extra code entry points
        self.jtbl = []           # (addr, count, stride): each slot is code
        self.ptrs = []           # (addr, count): table of longword code pointers
        self.names = {}          # addr -> symbol name
        self.data = []           # (start, end, name) known data regions
        self.noret = set()       # functions that never return to caller
        self.aliases = {}        # runtime (RAM) address -> ROM address of copied code
        self.ijmp = {}           # indirect jump/call site -> possible targets

    @classmethod
    def load(cls, path):
        h = cls()
        if not os.path.exists(path):
            return h
        for ln in open(path):
            ln = ln.split(';', 1)[0].strip()
            if not ln:
                continue
            f = ln.split()
            k, a = f[0], int(f[1], 16)
            if k == 'code':
                h.code.add(a)
                if len(f) > 2:
                    h.names[a] = f[2]
            elif k == 'name':
                h.names[a] = f[2]
            elif k == 'jtbl':
                h.jtbl.append((a, int(f[2], 0), int(f[3], 0) if len(f) > 3 else 4))
            elif k == 'ptrs':
                h.ptrs.append((a, int(f[2], 0)))
            elif k == 'data':
                h.data.append((a, int(f[2], 16), f[3] if len(f) > 3 else None))
            elif k == 'alias':
                h.aliases[int(f[2], 16)] = a
            elif k == 'ijmp':
                h.ijmp[a] = [int(t, 16) for t in f[2:]]
                h.code.update(h.ijmp[a])
            elif k == 'noret':
                h.noret.add(a)
            else:
                raise SystemExit('bad hint: ' + ln)
        return h


class Disasm:
    def __init__(self, rom, hints):
        self.rom = rom
        self.h = hints
        self.insns = {}           # addr -> Insn
        self.owner = {}           # byte addr -> insn addr (for overlap checks)
        self.funcs = set()
        self.branch_targets = set()
        self.data_refs = set()
        self.tables = []          # (site, base, count, stride)
        self.unresolved = []      # (addr, text)
        self.conflicts = []

    def in_rom(self, a):
        return 0 <= a < len(self.rom)

    def trace(self):
        vec = struct.unpack_from('>64I', self.rom, 0)
        todo = []
        for n, v in enumerate(vec[1:], 1):
            if self.in_rom(v) and v >= 0x200:
                todo.append(v)
                self.funcs.add(v)
        todo += sorted(self.h.code)
        self.funcs |= self.h.code
        for a, cnt, stride in self.h.jtbl:
            self.tables.append((None, a, cnt, stride))
            for k in range(cnt):
                todo.append(a + k * stride)
                self.funcs.add(a + k * stride)
        for a, cnt in self.h.ptrs:
            self.data_refs.add(a)
            for k in range(cnt):
                p = struct.unpack_from('>I', self.rom, a + 4 * k)[0]
                todo.append(p)
                self.funcs.add(p)
        while todo:
            self._block(todo.pop(), todo)

    def _block(self, a, todo):
        lea = {}  # areg -> constant address loaded in this block
        while self.in_rom(a) and a not in self.insns:
            if a in self.owner:
                self.conflicts.append(a)
                return
            i = m68k.decode(self.rom, a)
            if i is None:
                self.conflicts.append(a)
                return
            self.insns[a] = i
            for b in range(a, a + i.size):
                self.owner[b] = a
            self._refs(i)
            f = i.flow
            end = a + i.size
            if i.mnem == 'lea' and i.ops[0].mode in (m68k.ABSL, m68k.ABSW, m68k.PCDISP):
                lea[i.ops[1].reg] = i.ops[0].addr(0) & 0xffffff
            elif i.ops and i.ops[-1].mode == m68k.AN:
                lea.pop(i.ops[-1].reg, None)
            if f in ('bra', 'bcc', 'dbcc', 'bsr') or (f in ('jmp', 'jsr') and i.target is not None):
                t = i.target
                if self.in_rom(t):
                    todo.append(t)
                    (self.funcs if f in ('bsr', 'jsr') else self.branch_targets).add(t)
                if f in ('bra', 'jmp'):
                    return
                if f in ('bsr', 'jsr') and t in self.h.noret:
                    return
            elif f in ('jmp', 'jsr'):
                op = i.ops[0]
                base = None
                if op.mode == m68k.PCINDEX:
                    base = op.val
                elif op.mode in (m68k.INDEX, m68k.IND, m68k.DISP) and op.reg in lea:
                    base = lea[op.reg] + (op.val if op.mode != m68k.IND else 0)
                if a in self.h.ijmp:
                    for t in self.h.ijmp[a]:
                        todo.append(t)
                        self.funcs.add(t)
                elif base is not None and op.mode in (m68k.IND, m68k.DISP):
                    todo.append(base)
                    self.funcs.add(base)
                elif base is not None:
                    self._jump_table(a, end, base, todo)
                else:
                    self.unresolved.append((a, m68k.fmt_insn(i)))
                if f == 'jmp':
                    return
            elif f in ('ret', 'trap') and i.mnem not in ('trap',):
                return
            a = end

    def _jump_table(self, site, end, base, todo):
        if any(t[1] == base for t in self.tables):
            return
        # slot width: bra.w tables use 4-byte slots, bra.s/bsr.s tables 2-byte
        w = self.rom[base:base + 2]
        stride = 2 if w[0] in (0x60, 0x61) and w[1] != 0 else 4
        k = base
        # skip slots overlapping the dispatching instruction (index 0 unused)
        while k < end:
            k += stride
        start = k
        while self.in_rom(k + stride):
            w = self.rom[k:k + 2]
            if stride == 4 and w not in (b'\x60\x00', b'\x4e\x75'):
                break
            if stride == 2 and not (w[0] in (0x60, 0x61) and w[1] not in (0, 0xff)):
                break
            todo.append(k)
            self.funcs.add(k)
            k += stride
        self.tables.append((site, start, (k - start) // stride, stride))

    def _refs(self, i):
        for o in i.ops:
            if o.mode in (m68k.ABSL, m68k.PCDISP, m68k.PCINDEX) or (
                    o.mode == m68k.ABSW and o.val < 0x8000):
                t = o.val & 0xffffff
                if 0x200 <= t < len(self.rom) and i.flow not in ('jmp', 'jsr'):
                    self.data_refs.add(t)

    # ---------------------------------------------------------------- output

    def labels(self):
        lab = {}
        for a in self.data_refs:
            if a not in self.owner or self.owner[a] == a:
                lab[a] = ('loc_%X' if a in self.insns else 'dat_%X') % a
        for a in self.branch_targets:
            lab[a] = 'loc_%X' % a
        for a in self.funcs:
            lab[a] = 'sub_%X' % a
        for _, base, _, _ in self.tables:
            lab.setdefault(base, 'jt_%X' % base)
        for a, n in self.h.names.items():
            lab[a] = n
        # drop labels that fall inside an instruction
        return {a: n for a, n in lab.items()
                if a not in self.owner or self.owner[a] == a}

    def write_asm(self, path):
        lab = self.labels()
        get = lab.get
        out = ['; Super Hang-On (Mega Drive) - generated by tools/shodis.py',
               '; do not edit: regenerate from config/hints.txt', '']
        a = 0
        n = len(self.rom)
        # vector table + header are data
        while a < n:
            if a in lab:
                out.append('%s:' % lab[a])
            i = self.insns.get(a)
            if i is not None:
                out.append('\t%s' % m68k.fmt_insn(i, get))
                a += i.size
                continue
            # data run up to next label / instruction
            b = a + 1
            while b < n and b not in self.insns and b not in lab and b - a < 0x10000:
                b += 1
            out += self._data(a, b)
            a = b
        open(path, 'w').write('\n'.join(out) + '\n')

    def _data(self, a, b):
        lines = []
        if b - a >= 256:
            lines.append('\tincbin\t"baserom.md",$%X,$%X' % (a, b - a))
            return lines
        for k in range(a, b, 16):
            chunk = self.rom[k:min(b, k + 16)]
            lines.append('\tdc.b\t' + ','.join('$%02X' % c for c in chunk))
        return lines

    def report(self):
        code_end = 0x10000
        code = sum(1 for b in range(0x200, code_end) if b in self.owner)
        print('instructions : %d' % len(self.insns))
        print('functions    : %d' % len(self.funcs))
        print('jump tables  : %d' % len(self.tables))
        print('code bytes   : %d / %d of $200-$FFFF (%.1f%%)' % (
            code, code_end - 0x200, 100.0 * code / (code_end - 0x200)))
        if self.conflicts:
            print('decode conflicts: ' + ' '.join('%X' % c for c in sorted(set(self.conflicts))))
        for a, t in self.unresolved:
            print('unresolved   : %06X  %s' % (a, t))
        # list gaps in the code region
        gaps = []
        a = 0x200
        while a < code_end:
            if a in self.owner:
                a += 1
                continue
            b = a
            while b < code_end and b not in self.owner:
                b += 1
            gaps.append((a, b))
            a = b
        big = [g for g in gaps if g[1] - g[0] >= 16]
        print('gaps >=16 bytes in code region: %d' % len(big))
        for g in big:
            print('  %06X-%06X  %5d' % (g[0], g[1], g[1] - g[0]))

    def write_syms(self, path):
        lab = self.labels()
        with open(path, 'w') as f:
            for a in sorted(lab):
                f.write('%06X %s\n' % (a, lab[a]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--hints', default=os.path.join(ROOT, 'config', 'hints.txt'))
    ap.add_argument('--asm')
    ap.add_argument('--syms')
    ap.add_argument('--report', action='store_true')
    args = ap.parse_args()
    rom = open(args.rom, 'rb').read()
    d = Disasm(rom, Hints.load(args.hints))
    d.trace()
    if args.asm:
        d.write_asm(args.asm)
    if args.syms:
        d.write_syms(args.syms)
    if args.report or not (args.asm or args.syms):
        d.report()


if __name__ == '__main__':
    main()
