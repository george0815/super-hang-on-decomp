"""Register dataflow for the recompiled 68000 code.

For every function computes
  inputs[f]  registers/flags read before being written (its parameters)
  outputs[f] registers/flags some caller reads after the call (its results)
as bit masks. Data registers are tracked in three parts so byte and word
operations are not mistaken for uses of the whole register: dN low byte
(bit 3N), second byte (3N+1), upper word (3N+2). a0-a6 are bits 24-30, the
condition codes X N Z V C bits 32-36. a7 is the stack pointer and always
preserved.

This lets handwritten replacements be checked on their real interface
instead of every scratch register the original happened to leave behind.
"""

from m68k import (DN, AN, IND, POSTINC, PREDEC, DISP, INDEX, PCINDEX, IMM,
                  REGLIST, CCR, SR)

X, N, Z, V, C = (1 << 32, 1 << 33, 1 << 34, 1 << 35, 1 << 36)
FLAGS = X | N | Z | V | C
DREGS = (1 << 24) - 1
AREGS = 0x7f << 24
ALL = DREGS | AREGS | FLAGS
COND_USE = [0, 0, C | Z, C | Z, C, C, Z, Z, V, V, N, N, N | V, N | V, N | V | Z, N | V | Z]
PART = {'b': 1, 'w': 3, 'l': 7, None: 7}   # data register parts touched by a size


def dpart(n, sz):
    return PART[sz] << (3 * n)


def areg_bit(n):
    return 1 << (24 + n) if n < 7 else 0


def regmask_from_list(val):
    """movem register list (bit n = d0..d7, a0..a7) -> mask"""
    m = 0
    for n in range(8):
        if val & (1 << n):
            m |= dpart(n, 'l')
    for n in range(7):
        if val & (1 << (8 + n)):
            m |= areg_bit(n)
    return m



FLAG_NAMES = ['x', 'n', 'z', 'v', 'c']


def mask_names(m):
    out = []
    for n in range(8):
        p = (m >> (3 * n)) & 7
        if p:
            out.append('d%d%s' % (n, {1: '.b', 3: '.w', 7: ''}.get(p, '.%o' % p)))
    out += ['a%d' % n for n in range(7) if m & areg_bit(n)]
    out += [FLAG_NAMES[i] for i in range(5) if m & (1 << (32 + i))]
    return ' '.join(out) or '-'


def reg(o, sz='l'):
    if o.mode == DN:
        return dpart(o.reg, sz)
    if o.mode == AN:
        return areg_bit(o.reg)
    return 0


def addr_use(o):
    """registers read to form a memory operand's address"""
    m = 0
    if o.mode in (IND, POSTINC, PREDEC, DISP, INDEX):
        m |= areg_bit(o.reg)
    if o.mode in (INDEX, PCINDEX):
        if o.xreg < 8:
            m |= dpart(o.xreg, 'l' if o.xlong else 'w')
        else:
            m |= areg_bit(o.xreg - 8)
    return m


def addr_def(o):
    if o.mode in (POSTINC, PREDEC):
        return areg_bit(o.reg)
    return 0


def read(o, sz='l'):
    return reg(o, sz) | addr_use(o)


def use_def(i):
    """(use, def) masks for a non-call instruction."""
    m = i.mnem
    ops = i.ops
    sz = i.sz if i.sz in ('b', 'w', 'l') else 'l'
    use = dfn = 0

    def src(o, size=None):
        nonlocal use, dfn
        use |= read(o, size or sz) | addr_def(o)
        dfn |= addr_def(o)

    def dst_write(o, size=None):
        nonlocal use, dfn
        if o.mode == AN:
            dfn |= areg_bit(o.reg)
        else:
            dfn |= reg(o, size or sz)
        use |= addr_use(o) | addr_def(o)
        dfn |= addr_def(o)

    def rmw(o, size=None):
        nonlocal use
        use |= reg(o, size or sz) if o.mode != AN else areg_bit(o.reg)
        dst_write(o, size)

    if m in ('move', 'movea') and ops[1].mode in (CCR, SR):
        src(ops[0], 'w')
        dfn |= FLAGS
    elif m == 'move' and ops[0].mode == SR:
        use |= FLAGS
        dst_write(ops[1], 'w')
    elif m == 'move' and ops[1].mode == PREDEC and ops[1].reg == 7 and ops[0].mode in (DN, AN):
        # saving a register on the stack (like movem): not a real use
        use |= areg_bit(7)
        dfn |= N | Z | V | C
    elif m == 'move' and ops[0].mode == POSTINC and ops[0].reg == 7 and ops[1].mode == DN:
        # restoring it: the caller's value comes back, transparent
        dfn |= N | Z | V | C
    elif m == 'move':
        src(ops[0])
        dst_write(ops[1])
        dfn |= N | Z | V | C
    elif m == 'movea':
        src(ops[0])
        dfn |= areg_bit(ops[1].reg)
    elif m == 'moveq':
        dfn |= dpart(ops[1].reg, 'l') | N | Z | V | C
    elif m == 'lea':
        use |= addr_use(ops[0])
        dfn |= areg_bit(ops[1].reg)
    elif m == 'pea':
        use |= addr_use(ops[0])
    elif m == 'clr':
        dst_write(ops[0])
        dfn |= N | Z | V | C
    elif m in ('add', 'sub', 'addi', 'subi', 'addq', 'subq'):
        src(ops[0])
        if ops[1].mode == AN:
            use |= areg_bit(ops[1].reg)
            dfn |= areg_bit(ops[1].reg)
        else:
            rmw(ops[1])
            dfn |= FLAGS
    elif m in ('and', 'or', 'eor', 'andi', 'ori', 'eori'):
        if ops[1].mode in (CCR, SR):
            use |= FLAGS
            dfn |= FLAGS
        else:
            src(ops[0])
            rmw(ops[1])
            dfn |= N | Z | V | C
    elif m in ('cmp', 'cmpi', 'cmpm', 'tst'):
        for o in ops:
            src(o)
        dfn |= N | Z | V | C
    elif m == 'cmpa':
        src(ops[0])
        use |= areg_bit(ops[1].reg)
        dfn |= N | Z | V | C
    elif m in ('adda', 'suba'):
        src(ops[0])
        use |= areg_bit(ops[1].reg)
        dfn |= areg_bit(ops[1].reg)
    elif m in ('addx', 'subx', 'negx', 'abcd', 'sbcd', 'nbcd'):
        for o in ops[:-1]:
            src(o)
        rmw(ops[-1])
        use |= X | Z
        dfn |= FLAGS
    elif m in ('neg', 'not'):
        rmw(ops[0])
        dfn |= (FLAGS if m == 'neg' else N | Z | V | C)
    elif m == 'ext':
        r = ops[0].reg
        use |= dpart(r, 'b' if sz == 'w' else 'w')
        dfn |= dpart(r, sz) | N | Z | V | C
    elif m == 'swap':
        use |= dpart(ops[0].reg, 'l')
        dfn |= dpart(ops[0].reg, 'l') | N | Z | V | C
    elif m in ('mulu', 'muls'):
        src(ops[0], 'w')
        use |= dpart(ops[1].reg, 'w')
        dfn |= dpart(ops[1].reg, 'l') | N | Z | V | C
    elif m in ('divu', 'divs'):
        src(ops[0], 'w')
        use |= dpart(ops[1].reg, 'l')
        dfn |= dpart(ops[1].reg, 'l') | N | Z | V | C
    elif m == 'exg':
        use |= reg(ops[0]) | reg(ops[1])
        dfn |= reg(ops[0]) | reg(ops[1])
    elif m in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
        if len(ops) == 2:
            if ops[0].mode == DN:
                use |= dpart(ops[0].reg, 'b')
            rmw(ops[1])
        else:
            rmw(ops[0], 'w')
        if m in ('roxl', 'roxr'):
            use |= X
        dfn |= (N | Z | V | C) if m in ('rol', 'ror') else FLAGS
    elif m in ('btst', 'bset', 'bclr', 'bchg'):
        if ops[0].mode == DN:
            use |= dpart(ops[0].reg, 'b')
        if ops[1].mode == DN and ops[0].mode == IMM:
            # a constant bit number touches one part of the register only
            bit = ops[0].val & 31
            part = (1 if bit < 8 else 2 if bit < 16 else 4) << (3 * ops[1].reg)
            use |= part
            if m != 'btst':
                dfn |= part
        else:
            bsz = 'l' if ops[1].mode == DN else 'b'
            if m == 'btst':
                src(ops[1], bsz)
            else:
                rmw(ops[1], bsz)
        dfn |= Z
    elif m == 'tas':
        rmw(ops[0], 'b')
        dfn |= N | Z | V | C
    elif m == 'movem':
        a, b = ops
        if a.mode == REGLIST:
            # saving registers on the stack is not a real use of their values
            if not (b.mode == PREDEC and b.reg == 7):
                use |= regmask_from_list(a.val) if sz == 'l' else _word_list(a.val)
            use |= addr_use(b) | addr_def(b)
            dfn |= addr_def(b)
        else:
            use |= addr_use(a) | addr_def(a)
            dfn |= addr_def(a)
            # restoring them leaves the caller's values: transparent
            if not (a.mode == POSTINC and a.reg == 7):
                dfn |= regmask_from_list(b.val)   # words are sign-extended to 32 bits
    elif m in ('link', 'unlk'):
        use |= areg_bit(ops[0].reg)
        dfn |= areg_bit(ops[0].reg)
    elif i.cond is not None and i.flow is None:           # scc
        use |= COND_USE[i.cond]
        dst_write(ops[0], 'b')
    elif i.flow == 'dbcc':
        use |= COND_USE[i.cond] | dpart(ops[0].reg, 'w')
        dfn |= dpart(ops[0].reg, 'w')
    elif i.flow == 'bcc':
        use |= COND_USE[i.cond]
    elif m in ('nop', 'bra', 'jmp', 'rts', 'rte', 'rtr', 'reset', 'stop'):
        pass
    else:
        use |= ALL      # unknown: be conservative
    return use, dfn


def _word_list(val):
    m = 0
    for n in range(8):
        if val & (1 << n):
            m |= dpart(n, 'w')
    for n in range(7):
        if val & (1 << (8 + n)):
            m |= areg_bit(n)
    return m


class Flow:
    """Interprocedural register summaries.

    uexp[f]    registers read before written (the function's inputs)
    mustdef[f] registers written on every path to a return
    maydef[f]  registers written on some path
    outputs[f] registers a caller reads after the call, among those f may write
    """

    VECTORS = {0x6d2a, 0x6bac, 0x6da6, 0x200, 0x202}

    def __init__(self, rc):
        self.rc = rc
        self.dis = rc.dis
        self.ents = rc.entries()
        self.bodies = {f: rc.body(f) for f in self.ents}
        self._targets = {a: self._indirect_targets(a, i) for a, i in self.dis.insns.items()
                         if i.flow in ('jsr', 'jmp') and i.target is None}
        self.uexp = {f: 0 for f in self.ents}
        self.mustdef = {f: ALL for f in self.ents}   # refined downwards
        self.maydef = {f: 0 for f in self.ents}
        self.outputs = {f: 0 for f in self.ents}
        self._summaries()
        self._results()
        self._local_results()
        self.inputs = self.uexp

    # ----------------------------------------------------------- helpers

    def _succ(self, a, body):
        i = self.dis.insns[a]
        out = []
        if i.flow in ('bra', 'bcc', 'dbcc') or (i.flow == 'jmp' and i.target is not None):
            out.append(i.target)
        if i.flow not in ('bra', 'jmp', 'ret'):
            out.append(a + i.size)
        return [s for s in out if s in body]

    def _callees(self, a, i):
        if i.target is not None:
            return [i.target] if i.target in self.bodies else None
        t = self._targets.get(a)
        return [x for x in t if x in self.bodies] if t else None

    def _call_effect(self, a, i):
        """(uses, mustdef, maydef) of a call or indirect jump"""
        ts = self._callees(a, i)
        extra = read(i.ops[0]) if i.target is None else 0
        if not ts:
            return ALL, 0, ALL
        u, md, yd = 0, ALL, 0
        for t in ts:
            u |= self.uexp[t]
            md &= self.mustdef[t]
            yd |= self.maydef[t]
        return u | extra, md, yd

    def _indirect_targets(self, a, i):
        """possible targets of an indirect jsr/jmp, or None if unknown"""
        h = self.dis.h
        if a in h.ijmp:
            return list(h.ijmp[a])
        op = i.ops[0]
        base = None
        if op.mode == PCINDEX:
            base = op.val
        elif op.mode in (INDEX, IND, DISP):
            # the disassembler resolved these through a preceding lea
            k = a
            for _ in range(8):
                prev = [p for p in self.dis.insns if p < k]
                if not prev:
                    break
                k = max(prev)
                j = self.dis.insns[k]
                if j.mnem == 'lea' and j.ops[1].mode == AN and j.ops[1].reg == op.reg:
                    base = j.ops[0].addr(0) & 0xffffff
                    break
        if base is None:
            return None
        for site, start, count, stride in self.dis.tables:
            if base <= start <= base + 8:
                return [start + k * stride for k in range(count)]
        return None

    def _transfer(self, a, i, after, exit_live):
        """live-in of one instruction given live-out"""
        if i.flow == 'ret':
            return exit_live if i.mnem in ('rts', 'rtr') else 0
        if i.flow in ('bsr', 'jsr'):
            u, md, _ = self._call_effect(a, i)
            return u | (after & ~md)
        if i.flow == 'jmp' and i.target is None:      # tail call
            u, md, _ = self._call_effect(a, i)
            return u | (exit_live & ~md)
        if i.mnem == 'swap':
            # the halves trade places: each needs the other only if used
            n = i.ops[0].reg
            low, up = dpart(n, 'w'), dpart(n, 'l') & ~dpart(n, 'w')
            li = after & ~dpart(n, 'l') & ~(N | Z | V | C)
            if after & low:
                li |= up
            if after & up:
                li |= low
            return li
        u, d = use_def(i)
        return u | (after & ~d)

    def _liveness(self, f, exit_live):
        body = self.bodies[f]
        insns = self.dis.insns
        live = {a: 0 for a in body}
        order = sorted(body, reverse=True)
        changed = True
        while changed:
            changed = False
            for a in order:
                after = 0
                for s in self._succ(a, body):
                    after |= live[s]
                li = self._transfer(a, insns[a], after, exit_live)
                if li != live[a]:
                    live[a] = li
                    changed = True
        return live

    def _defs(self, f):
        """(mustdef, maydef) over all paths entry -> return"""
        body = self.bodies[f]
        insns = self.dis.insns
        must_in = {a: ALL for a in body}
        must_in[f] = 0
        may = 0
        must_exit = ALL
        changed = True
        order = sorted(body)
        while changed:
            changed = False
            for a in order:
                i = insns[a]
                if i.flow in ('bsr', 'jsr') or (i.flow == 'jmp' and i.target is None):
                    _, md, yd = self._call_effect(a, i)
                    d, ydef = md, yd
                else:
                    _, d = use_def(i)
                    ydef = d
                may |= ydef
                out = must_in[a] | d
                if i.flow == 'ret' or (i.flow == 'jmp' and i.target is None):
                    must_exit &= out
                for s in self._succ(a, body):
                    if s == f:
                        continue
                    nv = must_in[s] & out
                    if nv != must_in[s]:
                        must_in[s] = nv
                        changed = True
        return (must_exit if must_exit != ALL or True else 0), may

    # ------------------------------------------------------------ solve

    def _summaries(self):
        changed = True
        while changed:
            changed = False
            for f in self.ents:
                md, yd = self._defs(f)
                live = self._liveness(f, 0)
                if md != self.mustdef[f] or yd != self.maydef[f] or live[f] != self.uexp[f]:
                    self.mustdef[f], self.maydef[f] = md, yd
                    self.uexp[f] = live[f]
                    changed = True

    def _results(self):
        insns = self.dis.insns
        reached = set()
        self.unresolved_calls = False
        changed = True
        while changed:
            changed = False
            for f in self.ents:
                live = self._liveness(f, self.outputs[f])
                for a in self.bodies[f]:
                    i = insns[a]
                    if i.flow in ('bsr', 'jsr'):
                        after = live.get(a + i.size, 0)
                    elif i.flow == 'jmp' and i.target is None:
                        after = self.outputs[f]        # tail call: our caller's view
                    else:
                        continue
                    ts = self._callees(a, i)
                    if ts is None:
                        self.unresolved_calls = True
                        continue
                    for t in ts:
                        reached.add(t)
                        new = self.outputs[t] | (after & self.maydef[t])
                        if new != self.outputs[t]:
                            self.outputs[t] = new
                            changed = True
        for f in self.ents:
            if f not in reached and f not in self.VECTORS:
                self.outputs[f] = self.maydef[f]

    def _local_results(self):
        """local[f]: registers a caller reads after the call before the
        caller itself returns. The rest of outputs[f] only passes through
        callers' returns (often into code that reads leftover registers)."""
        insns = self.dis.insns
        self.local = {f: 0 for f in self.ents}
        for f in self.ents:
            live = self._liveness(f, 0)
            for a in self.bodies[f]:
                i = insns[a]
                if i.flow not in ('bsr', 'jsr'):
                    continue
                ts = self._callees(a, i) or []
                after = live.get(a + i.size, 0)
                for t in ts:
                    self.local[t] |= after & self.maydef[t]
        for f in self.ents:
            self.local[f] &= self.outputs[f]
