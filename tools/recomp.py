#!/usr/bin/env python3
"""Static recompiler: 68000 code in the ROM -> C.

Every function entry found by the disassembler becomes a C function
`void f_XXXXXX(void)`. Its body holds every instruction reachable from the
entry without a call (code shared between functions is duplicated, which
costs ~1.4x and avoids tail-call recursion). Branches become gotos, calls
become C calls that push the real return address on the emulated stack, and
`rts` pops it into cpu.ret so stack tricks unwind correctly: a caller
returns too if cpu.ret is not its own return address.

Indirect jumps and calls go through m68k_dispatch(), a switch over every
known entry point.

usage: recomp.py ROM OUTDIR
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68k  # noqa: E402
from m68k import (DN, AN, IND, POSTINC, PREDEC, DISP, INDEX, ABSW, ABSL,  # noqa: E402
                  PCDISP, PCINDEX, IMM, REGLIST, CCR, SR, USP)
from shodis import Disasm, Hints, ROOT  # noqa: E402
from flow import Flow, mask_names  # noqa: E402

SZ = {'b': 1, 'w': 2, 'l': 4}
RD = {1: 'rd8', 2: 'rd16', 4: 'rd32'}
WR = {1: 'wr8', 2: 'wr16', 4: 'wr32'}
SHIFT = {'asl': 'SH_ASL', 'asr': 'SH_ASR', 'lsl': 'SH_LSL', 'lsr': 'SH_LSR',
         'rol': 'SH_ROL', 'ror': 'SH_ROR', 'roxl': 'SH_ROXL', 'roxr': 'SH_ROXR'}


def hx(v):
    return '0x%Xu' % (v & 0xffffffff)


def mask(sz):
    return {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}[sz]


def xreg(o):
    r = 'cpu.d[%d]' % o.xreg if o.xreg < 8 else 'cpu.a[%d]' % (o.xreg - 8)
    return r if o.xlong else 'sx16(%s)' % r


class Unsupported(Exception):
    pass


# Approximate MC68000 timing (clock cycles), used only to pace interrupts in
# the native port; exact timing is not needed for correct game logic.
EA_T = {DN: (0, 0), AN: (0, 0), IND: (4, 8), POSTINC: (4, 8), PREDEC: (6, 10),
        DISP: (8, 12), INDEX: (10, 14), ABSW: (8, 12), ABSL: (12, 16),
        PCDISP: (8, 12), PCINDEX: (10, 14), IMM: (4, 8)}


def ea_t(o, sz):
    t = EA_T.get(o.mode)
    return 0 if t is None else t[sz == 4]


def cycles(i):
    m = i.mnem
    sz = SZ.get(i.sz, 2) if i.flow not in ('bra', 'bsr', 'bcc') else 2
    ops = i.ops
    L = sz == 4
    if m in ('move', 'movea'):
        return 4 + sum(ea_t(o, sz) for o in ops)
    if m == 'moveq':
        return 4
    if m in ('lea', 'pea'):
        return (4 if m == 'lea' else 12) + ea_t(ops[0], 2)
    if m in ('add', 'sub', 'and', 'or', 'eor', 'cmp', 'adda', 'suba', 'cmpa'):
        if ops[-1].mode in (DN, AN):
            return (8 if L else 4) + ea_t(ops[0], sz)
        return (12 if L else 8) + ea_t(ops[1], sz)
    if m in ('addi', 'subi', 'andi', 'ori', 'eori', 'cmpi'):
        if ops[1].mode == DN:
            return 16 if L else 8
        return (20 if L else 12) + ea_t(ops[1], sz)
    if m in ('addq', 'subq'):
        if ops[1].mode in (DN, AN):
            return 8 if L or ops[1].mode == AN else 4
        return (12 if L else 8) + ea_t(ops[1], sz)
    if m in ('clr', 'neg', 'negx', 'not', 'tst', 'scc', 'tas', 'nbcd') or (m[0] == 's' and i.cond is not None and not i.flow):
        if ops[0].mode == DN:
            return 6 if L else 4
        return (12 if L else 8) + ea_t(ops[0], sz)
    if m in ('mulu', 'muls'):
        return 54 + ea_t(ops[0], 2)
    if m == 'divu':
        return 140 + ea_t(ops[0], 2)
    if m == 'divs':
        return 158 + ea_t(ops[0], 2)
    if m in SHIFT:
        if len(ops) == 1:
            return 8 + ea_t(ops[0], 2)
        n = ops[0].val if ops[0].mode == IMM else 4
        return (8 if L else 6) + 2 * n
    if m in ('btst', 'bset', 'bclr', 'bchg'):
        return (4 if m == 'btst' else 8) + (4 if ops[0].mode == IMM else 0) + ea_t(ops[1], 1)
    if m == 'movem':
        regs = bin((ops[0] if ops[0].mode == REGLIST else ops[1]).val).count('1')
        mem = ops[1] if ops[0].mode == REGLIST else ops[0]
        return 8 + (8 if L else 4) * regs + ea_t(mem, 2)
    if m in ('abcd', 'sbcd'):
        return 6 if ops[0].mode == DN else 18
    if i.flow in ('bra', 'bcc'):
        return 10
    if i.flow == 'dbcc':
        return 10
    if i.flow == 'bsr':
        return 18
    if i.flow in ('jsr', 'jmp'):
        return (16 if i.flow == 'jsr' else 8) + ea_t(ops[0], 2)
    if m in ('rts', 'rtr'):
        return 16
    if m == 'rte':
        return 20
    if m in ('exg', 'ext', 'swap', 'nop'):
        return 6 if m == 'exg' else 4
    return 8


class FuncGen:
    """Generates C for one function body."""

    def __init__(self, rg, entry):
        self.rg = rg
        self.entry = entry
        self.lines = []
        self.tmp = 0

    def t(self):
        self.tmp += 1
        return 't%d' % self.tmp

    # -------------------------------------------------------- operands

    def addr(self, o, sz, out):
        """Emit code computing the address of a memory operand (applying
        any pre/post increment exactly once); return the C expression."""
        m = o.mode
        step = 2 if (o.reg == 7 and sz == 1) else sz
        if m == IND:
            return 'cpu.a[%d]' % o.reg
        if m == POSTINC:
            v = self.t()
            out.append('uint32_t %s = cpu.a[%d]; cpu.a[%d] += %d;' % (v, o.reg, o.reg, step))
            return v
        if m == PREDEC:
            out.append('cpu.a[%d] -= %d;' % (o.reg, step))
            v = self.t()
            out.append('uint32_t %s = cpu.a[%d];' % (v, o.reg))
            return v
        if m == DISP:
            return '(cpu.a[%d] + %s)' % (o.reg, hx(o.val))
        if m == INDEX:
            return '(cpu.a[%d] + %s + %s)' % (o.reg, hx(o.val), xreg(o))
        if m in (ABSW, ABSL):
            return hx(o.val)  # full 32 bits: lea/pea expose the upper byte
        if m == PCDISP:
            return hx(o.val)
        if m == PCINDEX:
            return '(%s + %s)' % (hx(o.val), xreg(o))
        raise Unsupported('addr mode %d' % m)

    def is_mem(self, o):
        return o.mode in (IND, POSTINC, PREDEC, DISP, INDEX, ABSW, ABSL, PCDISP, PCINDEX)

    def read(self, o, sz, out, ea=None):
        """Return C expression for operand value (zero-extended to size)."""
        m = o.mode
        if m == DN:
            return '(cpu.d[%d] & %s)' % (o.reg, mask(sz))
        if m == AN:
            return '(cpu.a[%d] & %s)' % (o.reg, mask(sz)) if sz != 4 else 'cpu.a[%d]' % o.reg
        if m == IMM:
            return hx(o.val & int(mask(sz)[:-1], 16))
        if ea is None:
            ea = self.addr(o, sz, out)
        v = self.t()
        out.append('uint32_t %s = %s(%s);' % (v, RD[sz], ea))
        return v

    def write(self, o, sz, val, out, ea=None):
        m = o.mode
        if m == DN:
            if sz == 4:
                out.append('cpu.d[%d] = %s;' % (o.reg, val))
            else:
                out.append('set_dn(%d, %d, %s);' % (o.reg, sz, val))
            return
        if m == AN:
            out.append('cpu.a[%d] = %s;' % (o.reg, val))
            return
        if ea is None:
            ea = self.addr(o, sz, out)
        out.append('%s(%s, %s);' % (WR[sz], ea, val))

    def rmw(self, o, sz, out):
        """For read-modify-write: return (value expr, ea or None)."""
        if self.is_mem(o):
            ea = self.addr(o, sz, out)
            if ea.startswith('(') or ea.startswith('cpu.'):
                v = self.t()
                out.append('uint32_t %s = %s;' % (v, ea))
                ea = v
            return self.read(o, sz, out, ea), ea
        return self.read(o, sz, out), None

    # ------------------------------------------------------ instructions

    def insn(self, i):
        out = []
        m = i.mnem
        sz = SZ.get(i.sz, 0) if i.flow not in ('bra', 'bsr', 'bcc') else 0
        ops = i.ops
        nxt = i.addr + i.size
        rg = self.rg

        if m in ('move', 'movea') and ops[1].mode in (CCR, SR, USP) or (
                m == 'move' and ops[0].mode in (SR, USP)):
            a, b = ops
            if b.mode == CCR:
                out.append('set_ccr(%s);' % self.read(a, 2, out))
            elif b.mode == SR:
                out.append('set_sr(%s);' % self.read(a, 2, out))
            elif a.mode == SR:
                self.write(b, 2, 'get_sr()', out)
            elif b.mode == USP:
                out.append('cpu.usp = cpu.a[%d];' % a.reg)
            else:
                out.append('cpu.a[%d] = cpu.usp;' % b.reg)
        elif m == 'move':
            v = self.read(ops[0], sz, out)
            r = self.t()
            out.append('uint32_t %s = f_logic(%d, %s);' % (r, sz, v))
            self.write(ops[1], sz, r, out)
        elif m == 'movea':
            v = self.read(ops[0], sz, out)
            out.append('cpu.a[%d] = %s;' % (ops[1].reg, 'sx16(%s)' % v if sz == 2 else v))
        elif m == 'moveq':
            out.append('cpu.d[%d] = f_logic(4, %s);' % (ops[1].reg, hx(ops[0].val)))
        elif m == 'lea':
            out.append('cpu.a[%d] = %s;' % (ops[1].reg, self.addr(ops[0], 4, out)))
        elif m == 'pea':
            out.append('push32(%s);' % self.addr(ops[0], 4, out))
        elif m == 'clr':
            self.write(ops[0], sz, '0', out)
            out.append('cpu.n = 0; cpu.z = 1; cpu.v = 0; cpu.c = 0;')
        elif m in ('add', 'sub', 'and', 'or', 'eor', 'addi', 'subi', 'andi', 'ori',
                   'eori', 'addq', 'subq', 'cmp', 'cmpi', 'cmpm', 'addx', 'subx'):
            base = m.rstrip('iq') if m not in ('ori', 'eori', 'andi') else m[:-1]
            if m == 'cmpm':
                base = 'cmp'
            a, b = ops
            if b.mode in (CCR, SR):
                v = self.read(a, sz, out)
                g = 'get_ccr()' if b.mode == CCR else 'get_sr()'
                op = {'and': '&', 'or': '|', 'eor': '^'}[base]
                out.append('%s(%s %s %s);' % ('set_ccr' if b.mode == CCR else 'set_sr', g, op, v))
            elif b.mode == AN:  # addq/subq to An: no flags, full 32 bits
                v = self.read(a, sz, out)
                out.append('cpu.a[%d] %s= %s;' % (b.reg, '+' if base == 'add' else '-', v))
            else:
                s = self.read(a, sz, out)
                d, ea = self.rmw(b, sz, out)
                r = self.t()
                if base == 'cmp':
                    out.append('f_cmp(%d, %s, %s);' % (sz, s, d))
                else:
                    fn = {'add': 'f_add', 'sub': 'f_sub', 'addx': 'f_addx', 'subx': 'f_subx'}.get(base)
                    if fn:
                        out.append('uint32_t %s = %s(%d, %s, %s);' % (r, fn, sz, s, d))
                    else:
                        op = {'and': '&', 'or': '|', 'eor': '^'}[base]
                        out.append('uint32_t %s = f_logic(%d, %s %s %s);' % (r, sz, d, op, s))
                    self.write(b, sz, r, out, ea)
        elif m in ('adda', 'suba', 'cmpa'):
            v = self.read(ops[0], sz, out)
            if sz == 2:
                v = 'sx16(%s)' % v
            if m == 'cmpa':
                out.append('f_cmp(4, %s, cpu.a[%d]);' % (v, ops[1].reg))
            else:
                out.append('cpu.a[%d] %s= %s;' % (ops[1].reg, '+' if m == 'adda' else '-', v))
        elif m == 'tst':
            v = self.read(ops[0], sz, out)
            out.append('f_logic(%d, %s);' % (sz, v))
        elif m in ('neg', 'negx', 'not'):
            d, ea = self.rmw(ops[0], sz, out)
            r = self.t()
            if m == 'not':
                out.append('uint32_t %s = f_logic(%d, ~%s);' % (r, sz, d))
            else:
                out.append('uint32_t %s = %s(%d, %s, 0);' % (r, 'f_sub' if m == 'neg' else 'f_subx', sz, d))
            self.write(ops[0], sz, r, out, ea)
        elif m == 'ext':
            n = ops[0].reg
            if i.sz == 'w':
                out.append('set_dn(%d, 2, f_logic(2, sx8(cpu.d[%d])));' % (n, n))
            else:
                out.append('cpu.d[%d] = f_logic(4, sx16(cpu.d[%d]));' % (n, n))
        elif m == 'swap':
            n = ops[0].reg
            out.append('cpu.d[%d] = f_logic(4, cpu.d[%d] << 16 | cpu.d[%d] >> 16);' % (n, n, n))
        elif m == 'exg':
            a, b = ops
            ra = ('cpu.d[%d]' if a.mode == DN else 'cpu.a[%d]') % a.reg
            rb = ('cpu.d[%d]' if b.mode == DN else 'cpu.a[%d]') % b.reg
            out.append('uint32_t t = %s; %s = %s; %s = t;' % (ra, ra, rb, rb))
        elif m in ('mulu', 'muls', 'divu', 'divs'):
            v = self.read(ops[0], 2, out)
            n = ops[1].reg
            out.append('cpu.d[%d] = f_%s(%s, cpu.d[%d]);' % (n, m, v, n))
        elif m in SHIFT:
            if len(ops) == 1:  # memory, by one
                d, ea = self.rmw(ops[0], 2, out)
                r = self.t()
                out.append('uint32_t %s = f_shift(%s, 2, %s, 1);' % (r, SHIFT[m], d))
                self.write(ops[0], 2, r, out, ea)
            else:
                c, dst = ops
                cnt = str(c.val) if c.mode == IMM else '(int)(cpu.d[%d] & 63)' % c.reg
                out.append('set_dn(%d, %d, f_shift(%s, %d, cpu.d[%d], %s));' % (
                    dst.reg, sz, SHIFT[m], sz, dst.reg, cnt))
        elif m in ('btst', 'bset', 'bclr', 'bchg'):
            bit, dst = ops
            width = 32 if dst.mode == DN else 8
            b = str(bit.val % width) if bit.mode == IMM else '(cpu.d[%d] & %d)' % (bit.reg, width - 1)
            bsz = 4 if dst.mode == DN else 1
            if m == 'btst':
                v = self.read(dst, bsz, out)
                out.append('cpu.z = ((%s >> %s) & 1) == 0;' % (v, b))
            else:
                d, ea = self.rmw(dst, bsz, out)
                out.append('cpu.z = ((%s >> %s) & 1) == 0;' % (d, b))
                op = {'bset': '%s | (1u << %s)', 'bclr': '%s & ~(1u << %s)', 'bchg': '%s ^ (1u << %s)'}[m]
                self.write(dst, bsz, op % (d, b), out, ea)
        elif m in ('abcd', 'sbcd'):
            a, b = ops
            s = self.read(a, 1, out)
            d, ea = self.rmw(b, 1, out)
            r = self.t()
            out.append('uint32_t %s = f_%s(%s, %s);' % (r, m, s, d))
            self.write(b, 1, r, out, ea)
        elif m == 'nbcd':
            d, ea = self.rmw(ops[0], 1, out)
            r = self.t()
            out.append('uint32_t %s = f_nbcd(%s);' % (r, d))
            self.write(ops[0], 1, r, out, ea)
        elif m == 'tas':
            d, ea = self.rmw(ops[0], 1, out)
            out.append('f_logic(1, %s);' % d)
            self.write(ops[0], 1, '%s | 0x80u' % d, out, ea)
        elif m[0] == 's' and i.cond is not None and i.flow is None:
            self.write(ops[0], 1, 'cond(%d) ? 0xffu : 0u' % i.cond, out)
        elif m == 'movem':
            self.movem(i, sz, out)
        elif m == 'link':
            r = ops[0].reg
            out.append('push32(cpu.a[%d]); cpu.a[%d] = cpu.a[7]; cpu.a[7] += sx16(%s);' % (
                r, r, hx(ops[1].val)))
        elif m == 'unlk':
            r = ops[0].reg
            out.append('cpu.a[7] = cpu.a[%d]; cpu.a[%d] = pop32();' % (r, r))
        elif m in ('nop', 'reset'):
            pass
        elif m == 'stop':
            out.append('set_sr(%s); m68k_stop();' % hx(ops[0].val))
        # ------------------------------------------------- control flow
        elif i.flow == 'dbcc':
            n = ops[0].reg
            out.append('if (!cond(%d)) {' % i.cond)
            out.append('    uint32_t c = (cpu.d[%d] - 1) & 0xffffu; set_dn(%d, 2, c);' % (n, n))
            out.append('    if (c != 0xffffu) %s' % self.goto(i.target))
            out.append('}')
        elif i.flow == 'bcc':
            out.append('if (cond(%d)) %s' % (i.cond, self.goto(i.target)))
        elif i.flow == 'bra' or (i.flow == 'jmp' and i.target is not None):
            out.append(self.goto(i.target))
        elif i.flow in ('bsr', 'jsr'):
            if i.target is not None:
                out.append('push32(%s); %s(); if (cpu.ret != %s) return;' % (
                    hx(nxt), rg.fname(i.target), hx(nxt)))
            else:
                ea = self.addr(ops[0], 4, out)
                out.append('push32(%s); m68k_dispatch(%s); if (cpu.ret != %s) return;' % (
                    hx(nxt), ea, hx(nxt)))
        elif i.flow == 'jmp':
            ea = self.addr(ops[0], 4, out)
            out.append('m68k_dispatch(%s); return;' % ea)
        elif m == 'rts':
            out.append('cpu.ret = pop32(); return;')
        elif m == 'rtr':
            out.append('set_ccr(pop16()); cpu.ret = pop32(); return;')
        elif m == 'rte':
            out.append('{ uint16_t sr = pop16(); cpu.ret = pop32(); set_sr(sr); } return;')
        else:
            raise Unsupported(m68k.fmt_insn(i))
        return out

    def movem(self, i, sz, out):
        src, dst = i.ops
        to_mem = src.mode == REGLIST
        regs = [n for n in range(16) if (src if to_mem else dst).val & (1 << n)]
        mem = dst if to_mem else src

        def r(n):
            return 'cpu.d[%d]' % n if n < 8 else 'cpu.a[%d]' % (n - 8)

        if to_mem:
            if mem.mode == PREDEC:
                # stored highest register first going down; values are the
                # pre-instruction register contents
                out.append('uint32_t ea = cpu.a[%d];' % mem.reg)
                for n in reversed(regs):
                    out.append('ea -= %d; %s(ea, %s);' % (sz, WR[sz], r(n)))
                out.append('cpu.a[%d] = ea;' % mem.reg)
            else:
                ea = self.addr(mem, sz, out)
                out.append('uint32_t ea = %s;' % ea)
                for n in regs:
                    out.append('%s(ea, %s); ea += %d;' % (WR[sz], r(n), sz))
        else:
            if mem.mode == POSTINC:
                out.append('uint32_t ea = cpu.a[%d];' % mem.reg)
            else:
                out.append('uint32_t ea = %s;' % self.addr(mem, sz, out))
            for n in regs:
                v = '%s(ea)' % RD[sz]
                if sz == 2:
                    v = 'sx16(%s)' % v
                out.append('%s = %s; ea += %d;' % (r(n), v, sz))
            if mem.mode == POSTINC:
                out.append('cpu.a[%d] = ea;' % mem.reg)

    def goto(self, t):
        if t != self.entry and t in self.rg.hand:
            # a handwritten function: tail call it rather than copy its original
            return '{ %s(); return; }' % self.rg.fname(t)
        return 'goto L_%06X;' % t

    # ------------------------------------------------------------- body

    def generate(self, name=None, storage=''):
        x = self.rg.dis
        body = self.rg.body(self.entry, self.rg.hand)
        order = sorted(body)
        out = ['%svoid %s(void)' % (storage, name or self.rg.fname(self.entry)), '{']
        if self.entry != order[0]:
            out.append('    goto L_%06X;' % self.entry)
        for k, a in enumerate(order):
            i = x.insns[a]
            out.append('L_%06X: /* %s */' % (a, m68k.fmt_insn(i).replace('*/', '* /')))
            out.append('    HOOK(0x%06X, %d);' % (a, cycles(i)))
            try:
                code = self.insn(i)
            except Unsupported as e:
                code = ['m68k_fault("unsupported: %s", %s);' % (str(e).replace('"', "'"), hx(a))]
            if code:
                out.append('    { ' + ' '.join(code) + ' }' if len(code) <= 3 and
                           sum(len(c) for c in code) < 100 else
                           '    {\n' + '\n'.join('        ' + c for c in code) + '\n    }')
            ends = i.flow in ('bra', 'ret') or (i.flow == 'jmp')
            if not ends:
                nxt = a + i.size
                if k + 1 >= len(order) or order[k + 1] != nxt:
                    if nxt in body or nxt in self.rg.hand:
                        out.append('    ' + self.goto(nxt))
                    else:
                        out.append('    m68k_fault("ran off end of code", %s); return;' % hx(nxt))
        out.append('}')
        return '\n'.join(out)


NOVERIFY = set()


def load_handwritten(path, with_ignore=False):
    """config/handwritten.txt: ADDR c_function_name [ignore=part,part...] [noverify]
    ignore lists outputs left out of verification (each needs a reason in a
    comment): e.g. d4.hi, d1.b1, x. noverify: verify builds always run the
    original (for code that never returns, like the main loop)"""
    out, ignore = {}, {}
    if os.path.exists(path):
        for ln in open(path):
            ln = ln.split(';', 1)[0].split('#', 1)[0].strip()
            if ln:
                f = ln.split()
                out[int(f[0], 16)] = f[1]
                for extra in f[2:]:
                    if extra.startswith('ignore='):
                        ignore[int(f[0], 16)] = parse_parts(extra[7:])
                    elif extra == 'noverify':
                        NOVERIFY.add(int(f[0], 16))
    return (out, ignore) if with_ignore else out


def parse_parts(text):
    from flow import dpart, areg_bit, X, N, Z, V, C
    m = 0
    for p in text.split(','):
        p = p.strip().lower()
        if p in ('x', 'n', 'z', 'v', 'c'):
            m |= {'x': X, 'n': N, 'z': Z, 'v': V, 'c': C}[p]
        elif p[0] == 'a':
            m |= areg_bit(int(p[1]))
        else:
            reg, _, part = p.partition('.')
            n = int(reg[1])
            m |= {'b0': 1, 'b1': 2, 'hi': 4, '': 7}[part] << (3 * n)
    return m


class Recompiler:
    def __init__(self, rom, hints):
        self.rom = rom
        self.dis = Disasm(rom, hints)
        self.dis.trace()
        self.hints = hints
        self.names = self.dis.labels()
        self.hand = set()   # handwritten entries (set by write)

    def fname(self, a):
        return 'f_%06X' % a

    def body(self, f, stop=()):
        """addresses reachable from f without a call; entries in stop (other
        than f) are left out, to be reached by a tail call instead"""
        x = self.dis
        seen = set()
        st = [f]
        while st:
            a = st.pop()
            if a in seen or a not in x.insns or (a != f and a in stop):
                continue
            seen.add(a)
            i = x.insns[a]
            if i.flow in ('bra', 'bcc', 'dbcc') or (i.flow == 'jmp' and i.target is not None):
                st.append(i.target)
            if i.flow in ('bra', 'jmp', 'ret'):
                continue
            st.append(a + i.size)
        return seen

    def entries(self):
        return sorted(f for f in self.dis.funcs if f in self.dis.insns)

    def write(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        ents = self.entries()
        aliases = self.hints.aliases
        with open(os.path.join(outdir, 'sho_funcs.h'), 'w') as h:
            h.write('/* generated by tools/recomp.py - do not edit */\n')
            h.write('#ifndef SHO_FUNCS_H\n#define SHO_FUNCS_H\n\n')
            for f in ents:
                n = self.names.get(f)
                h.write('void %s(void);%s\n' % (self.fname(f), ('  /* %s */' % n) if n else ''))
            h.write('\nint sho_is_entry(uint32_t addr);\n#endif\n')
        hand, ignore = load_handwritten(os.path.join(ROOT, 'config', 'handwritten.txt'), True)
        flow = Flow(self)
        self.hand = set(hand)
        with open(os.path.join(outdir, 'interfaces.txt'), 'w') as itf:
            itf.write('# function  inputs | outputs read by callers | outputs that only pass through\n'
                      '# callers\' returns (from tools/flow.py; verification compares the first set)\n')
            for f in ents:
                transit = flow.outputs[f] & ~flow.local[f]
                itf.write('%06X  %-40s | %-30s | %s\n' % (f, mask_names(flow.inputs[f]),
                                                         mask_names(flow.local[f]), mask_names(transit)))
        with open(os.path.join(outdir, 'sho_code.c'), 'w') as c:
            c.write('/* generated by tools/recomp.py - do not edit */\n')
            c.write('#include "m68k_rt.h"\n#include "sho_funcs.h"\n')
            if hand:
                c.write('#ifdef SHO_VERIFY\n#include "verify.h"\n#endif\n')
            c.write('\nvoid m68k_stop(void);\n\n')
            for f in ents:
                if f in hand:
                    # replaced by handwritten C; the original is kept only to
                    # verify against (tests/replay.c, built with SHO_VERIFY)
                    name = hand[f]
                    c.write('void %s(void);\n' % name)
                    # the handwritten function does the work; the wrapper
                    # returns like the original (pops the caller's address)
                    c.write('static void h_%06X(void) { %s(); cpu.ret = pop32(); }\n' % (f, name))
                    c.write('#ifdef SHO_VERIFY\n')
                    c.write(FuncGen(self, f).generate('o_%06X' % f, 'static '))
                    if f in NOVERIFY:
                        c.write('\nvoid %s(void) { (void)h_%06X; o_%06X(); }\n' % (self.fname(f), f, f))
                    else:
                        c.write('\nvoid %s(void) { verify_call(0x%06X, "%s", o_%06X, h_%06X, 0x%XULL); }\n' % (
                            self.fname(f), f, name, f, f, flow.local[f] & ~ignore.get(f, 0)))
                    c.write('#else\nvoid %s(void) { h_%06X(); }\n#endif\n\n' % (self.fname(f), f))
                    continue
                c.write(FuncGen(self, f).generate())
                c.write('\n\n')
            c.write('void m68k_dispatch(uint32_t addr)\n{\n    switch (addr & 0xffffff) {\n')
            for f in ents:
                c.write('    case 0x%06X: %s(); return;\n' % (f, self.fname(f)))
            for ram, rom in sorted(aliases.items()):
                c.write('    case 0x%06X: %s(); return;\n' % (ram, self.fname(rom)))
            c.write('    }\n    m68k_fault("dispatch to unknown address", addr);\n}\n\n')
            c.write('int sho_is_entry(uint32_t addr)\n{\n    switch (addr & 0xffffff) {\n')
            for f in ents:
                c.write('    case 0x%06X:\n' % f)
            c.write('        return 1;\n    }\n    return 0;\n}\n')
        with open(os.path.join(outdir, 'entries.txt'), 'w') as e:
            for f in ents:
                e.write('%06X\n' % f)
        print('%d functions written to %s' % (len(ents), outdir))


def main():
    rom = open(sys.argv[1], 'rb').read()
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build', 'recomp')
    Recompiler(rom, Hints.load(os.path.join(ROOT, 'config', 'hints.txt'))).write(out)


if __name__ == '__main__':
    main()
