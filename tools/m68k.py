"""Minimal, exact MC68000 instruction decoder.

Produces structured instructions (mnemonic, size, operands, control flow)
that both the disassembler and the C translator consume. Only the plain
68000 instruction set is supported; anything else decodes as None.
"""

import struct

# Effective-address modes
DN, AN, IND, POSTINC, PREDEC, DISP, INDEX, ABSW, ABSL, PCDISP, PCINDEX, IMM = range(12)
# Pseudo operands
REGLIST, CCR, SR, USP, BRANCH, QUICK = range(100, 106)

CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq',
      'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']

SIZES = {1: 'b', 2: 'w', 4: 'l'}


class Op:
    __slots__ = ('mode', 'reg', 'val', 'xreg', 'xlong', 'size')

    def __init__(self, mode, reg=0, val=0, xreg=None, xlong=False, size=None):
        self.mode, self.reg, self.val = mode, reg, val
        self.xreg, self.xlong, self.size = xreg, xlong, size

    def addr(self, pc_base):
        """Static address referenced by this operand, if any."""
        if self.mode in (ABSW, ABSL):
            return self.val
        if self.mode in (PCDISP, PCINDEX):
            return pc_base + self.val
        if self.mode == BRANCH:
            return self.val
        return None


class Insn:
    __slots__ = ('addr', 'size', 'mnem', 'sz', 'ops', 'flow', 'target', 'cond')

    def __init__(self, addr, mnem, sz=None, ops=(), flow=None, cond=None):
        self.addr, self.mnem, self.sz, self.ops = addr, mnem, sz, list(ops)
        self.flow, self.cond = flow, cond
        self.size = 0
        self.target = None


class Reader:
    def __init__(self, data, pc):
        self.d, self.pc = data, pc

    def w(self):
        v = struct.unpack_from('>H', self.d, self.pc)[0]
        self.pc += 2
        return v

    def sw(self):
        v = struct.unpack_from('>h', self.d, self.pc)[0]
        self.pc += 2
        return v

    def l(self):
        v = struct.unpack_from('>I', self.d, self.pc)[0]
        self.pc += 4
        return v


def _sx8(v):
    return v - 0x100 if v & 0x80 else v


def _ea(r, mode, reg, size, allow):
    """Decode an effective address. allow is a string of permitted classes:
    d=Dn a=An m=memory p=pc-relative i=immediate."""
    if mode == 0:
        if 'd' not in allow:
            raise ValueError
        return Op(DN, reg)
    if mode == 1:
        if 'a' not in allow or size == 1:
            raise ValueError
        return Op(AN, reg)
    if 'm' not in allow and mode < 7:
        raise ValueError
    if mode == 2:
        return Op(IND, reg)
    if mode == 3:
        return Op(POSTINC, reg)
    if mode == 4:
        return Op(PREDEC, reg)
    if mode == 5:
        return Op(DISP, reg, r.sw())
    if mode == 6:
        return _index(r, INDEX, reg)
    if reg == 0:
        if 'm' not in allow:
            raise ValueError
        return Op(ABSW, val=r.sw() & 0xffffffff, size='w')
    if reg == 1:
        if 'm' not in allow:
            raise ValueError
        return Op(ABSL, val=r.l(), size='l')
    if reg == 2:
        if 'p' not in allow:
            raise ValueError
        base = r.pc
        return Op(PCDISP, base, r.sw())
    if reg == 3:
        if 'p' not in allow:
            raise ValueError
        base = r.pc
        o = _index(r, PCINDEX, 0)
        o.reg = base
        return o
    if reg == 4:
        if 'i' not in allow:
            raise ValueError
        if size == 1:
            return Op(IMM, val=r.w() & 0xff)
        if size == 2:
            return Op(IMM, val=r.w())
        return Op(IMM, val=r.l())
    raise ValueError


def _index(r, mode, reg):
    ext = r.w()
    if ext & 0x0700:
        raise ValueError  # 68020 full/scaled formats
    xreg = (ext >> 12) & 0xf  # 0-7 Dn, 8-15 An
    return Op(mode, reg, _sx8(ext & 0xff), xreg, bool(ext & 0x800))


EA_ALL = 'dampi'
EA_DATA = 'dmpi'      # data addressing
EA_ALTMEM = 'm'       # memory alterable (no pc, no imm)
EA_DALT = 'dm'        # data alterable
EA_CTRL = 'mp'        # control (filtered further below)


def _ctrl(op):
    if op.mode in (POSTINC, PREDEC, DN, AN, IMM):
        raise ValueError
    return op


def decode(data, pc):
    """Decode one instruction at pc. Returns Insn or None."""
    r = Reader(data, pc)
    try:
        i = _decode(r, pc)
    except (ValueError, struct.error):
        return None
    if i is None:
        return None
    i.size = r.pc - pc
    # pc-relative operands carry their extension-word address in .reg;
    # fold it in so .val is the absolute base address
    for o in i.ops:
        if o.mode in (PCDISP, PCINDEX):
            o.val = (o.reg + o.val) & 0xffffff
            o.reg = 0
    if i.flow in ('jmp', 'jsr', 'bra', 'bsr', 'bcc', 'dbcc'):
        t = i.ops[-1]
        if t.mode == BRANCH:
            i.target = t.val
        elif t.mode in (ABSW, ABSL):
            i.target = t.val & 0xffffff
        elif t.mode == PCDISP:
            i.target = t.val
    return i


def _decode(r, pc):
    op = r.w()
    hi = op >> 12
    reg9 = (op >> 9) & 7
    mode = (op >> 3) & 7
    reg = op & 7
    ss = (op >> 6) & 3

    if hi == 0:
        return _line0(r, pc, op, reg9, mode, reg, ss)
    if hi in (1, 2, 3):
        size = {1: 1, 3: 2, 2: 4}[hi]
        src = _ea(r, mode, reg, size, EA_ALL)
        dmode = (op >> 6) & 7
        if dmode == 1:
            if size == 1:
                return None
            return Insn(pc, 'movea', SIZES[size], [src, Op(AN, reg9)])
        dst = _ea(r, dmode, reg9, size, EA_DALT)
        return Insn(pc, 'move', SIZES[size], [src, dst])
    if hi == 4:
        return _line4(r, pc, op, reg9, mode, reg, ss)
    if hi == 5:
        if ss == 3:
            cond = (op >> 8) & 0xf
            if mode == 1:
                disp = r.sw()
                return Insn(pc, 'db' + CC[cond], None,
                            [Op(DN, reg), Op(BRANCH, val=(pc + 2 + disp) & 0xffffff)],
                            flow='dbcc', cond=cond)
            dst = _ea(r, mode, reg, 1, EA_DALT)
            return Insn(pc, 's' + CC[cond], 'b', [dst], cond=cond)
        q = reg9 or 8
        size = 1 << ss
        dst = _ea(r, mode, reg, size, 'dam')
        return Insn(pc, 'subq' if op & 0x100 else 'addq', SIZES[size],
                    [Op(IMM, val=q), dst])
    if hi == 6:
        cond = (op >> 8) & 0xf
        d8 = op & 0xff
        if d8 == 0:
            disp, sz = r.sw(), 'w'
        else:
            disp, sz = _sx8(d8), 's'
        tgt = (pc + 2 + disp) & 0xffffff
        if cond == 0:
            return Insn(pc, 'bra', sz, [Op(BRANCH, val=tgt)], flow='bra')
        if cond == 1:
            return Insn(pc, 'bsr', sz, [Op(BRANCH, val=tgt)], flow='bsr')
        return Insn(pc, 'b' + CC[cond], sz, [Op(BRANCH, val=tgt)], flow='bcc', cond=cond)
    if hi == 7:
        if op & 0x100:
            return None
        return Insn(pc, 'moveq', 'l', [Op(IMM, val=_sx8(op & 0xff) & 0xffffffff), Op(DN, reg9)])
    if hi in (8, 9, 0xb, 0xc, 0xd):
        return _arith(r, pc, op, hi, reg9, mode, reg, ss)
    if hi == 0xe:
        return _shift(r, pc, op, reg9, mode, reg, ss)
    return None  # line A / line F


def _line0(r, pc, op, reg9, mode, reg, ss):
    if op & 0x100:
        if mode == 1:  # movep
            opm = (op >> 6) & 7
            sz = 'w' if opm in (4, 6) else 'l'
            m = Op(DISP, reg, r.sw())
            if opm >= 6:
                return Insn(pc, 'movep', sz, [Op(DN, reg9), m])
            return Insn(pc, 'movep', sz, [m, Op(DN, reg9)])
        name = ['btst', 'bchg', 'bclr', 'bset'][ss]
        allow = EA_DATA if ss == 0 else EA_DALT
        dst = _ea(r, mode, reg, 1, allow)
        return Insn(pc, name, 'l' if mode == 0 else 'b', [Op(DN, reg9), dst])
    kind = reg9
    if kind == 4:  # static bit ops
        name = ['btst', 'bchg', 'bclr', 'bset'][ss]
        bit = r.w() & 0xff
        allow = 'dmp' if ss == 0 else EA_DALT
        dst = _ea(r, mode, reg, 1, allow)
        return Insn(pc, name, 'l' if mode == 0 else 'b', [Op(IMM, val=bit), dst])
    names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
    if kind not in names or ss == 3:
        return None
    name = names[kind]
    if kind in (0, 1, 5) and mode == 7 and reg == 4:
        if ss == 0:
            return Insn(pc, name, 'b', [Op(IMM, val=r.w() & 0xff), Op(CCR)])
        if ss == 1:
            return Insn(pc, name, 'w', [Op(IMM, val=r.w()), Op(SR)])
        return None
    size = 1 << ss
    src = _ea(r, 7, 4, size, 'i')
    dst = _ea(r, mode, reg, size, EA_DALT)
    return Insn(pc, name, SIZES[size], [src, dst])


def _reglist(mask, predec):
    if predec:  # reversed bit order
        mask = int('{:016b}'.format(mask)[::-1], 2)
    return Op(REGLIST, val=mask)


def _line4(r, pc, op, reg9, mode, reg, ss):
    if op & 0x100:
        if ss == 3:
            src = _ctrl(_ea(r, mode, reg, 4, EA_CTRL))
            return Insn(pc, 'lea', 'l', [src, Op(AN, reg9)])
        if ss == 2:
            src = _ea(r, mode, reg, 2, EA_DATA)
            return Insn(pc, 'chk', 'w', [src, Op(DN, reg9)])
        return None
    sub = (op >> 8) & 0xf
    if sub == 0:
        if ss == 3:
            return Insn(pc, 'move', 'w', [Op(SR), _ea(r, mode, reg, 2, EA_DALT)])
        size = 1 << ss
        return Insn(pc, 'negx', SIZES[size], [_ea(r, mode, reg, size, EA_DALT)])
    if sub == 2:
        if ss == 3:
            return None
        size = 1 << ss
        return Insn(pc, 'clr', SIZES[size], [_ea(r, mode, reg, size, EA_DALT)])
    if sub == 4:
        if ss == 3:
            return Insn(pc, 'move', 'w', [_ea(r, mode, reg, 2, EA_DATA), Op(CCR)])
        size = 1 << ss
        return Insn(pc, 'neg', SIZES[size], [_ea(r, mode, reg, size, EA_DALT)])
    if sub == 6:
        if ss == 3:
            return Insn(pc, 'move', 'w', [_ea(r, mode, reg, 2, EA_DATA), Op(SR)])
        size = 1 << ss
        return Insn(pc, 'not', SIZES[size], [_ea(r, mode, reg, size, EA_DALT)])
    if sub == 8:
        if ss == 0:
            return Insn(pc, 'nbcd', 'b', [_ea(r, mode, reg, 1, EA_DALT)])
        if ss == 1:
            if mode == 0:
                return Insn(pc, 'swap', 'w', [Op(DN, reg)])
            return Insn(pc, 'pea', 'l', [_ctrl(_ea(r, mode, reg, 4, EA_CTRL))])
        if mode == 0:
            return Insn(pc, 'ext', 'w' if ss == 2 else 'l', [Op(DN, reg)])
        mask = r.w()
        dst = _ea(r, mode, reg, 2, 'm')
        if dst.mode in (POSTINC,):
            return None
        return Insn(pc, 'movem', 'w' if ss == 2 else 'l',
                    [_reglist(mask, dst.mode == PREDEC), dst])
    if sub == 0xa:
        if ss == 3:
            if op == 0x4afc:
                return Insn(pc, 'illegal', flow='trap')
            return Insn(pc, 'tas', 'b', [_ea(r, mode, reg, 1, EA_DALT)])
        size = 1 << ss
        return Insn(pc, 'tst', SIZES[size], [_ea(r, mode, reg, size, EA_DALT)])
    if sub == 0xc:
        if ss in (2, 3):
            mask = r.w()
            src = _ea(r, mode, reg, 2, 'mp')
            if src.mode == PREDEC:
                return None
            return Insn(pc, 'movem', 'w' if ss == 2 else 'l', [src, _reglist(mask, False)])
        return None
    if sub == 0xe:
        if ss == 1:
            lo = op & 0x3f
            if lo < 0x10:
                return Insn(pc, 'trap', None, [Op(IMM, val=lo & 0xf)], flow='trap')
            if 0x10 <= lo < 0x18:
                return Insn(pc, 'link', 'w', [Op(AN, reg), Op(IMM, val=r.w())])
            if 0x18 <= lo < 0x20:
                return Insn(pc, 'unlk', None, [Op(AN, reg)])
            if 0x20 <= lo < 0x28:
                return Insn(pc, 'move', 'l', [Op(AN, reg), Op(USP)])
            if 0x28 <= lo < 0x30:
                return Insn(pc, 'move', 'l', [Op(USP), Op(AN, reg)])
            simple = {0x4e70: ('reset', None), 0x4e71: ('nop', None),
                      0x4e73: ('rte', 'ret'), 0x4e75: ('rts', 'ret'),
                      0x4e76: ('trapv', None), 0x4e77: ('rtr', 'ret')}
            if op in simple:
                n, f = simple[op]
                return Insn(pc, n, None, [], flow=f)
            if op == 0x4e72:
                return Insn(pc, 'stop', None, [Op(IMM, val=r.w())], flow='ret')
            return None
        if ss == 2:
            return Insn(pc, 'jsr', None, [_ctrl(_ea(r, mode, reg, 4, EA_CTRL))], flow='jsr')
        if ss == 3:
            return Insn(pc, 'jmp', None, [_ctrl(_ea(r, mode, reg, 4, EA_CTRL))], flow='jmp')
    return None


def _arith(r, pc, op, hi, reg9, mode, reg, ss):
    opm = (op >> 6) & 7
    if hi == 8 or hi == 0xc:
        base = 'or' if hi == 8 else 'and'
        if opm == 3:
            n = 'divu' if hi == 8 else 'mulu'
            return Insn(pc, n, 'w', [_ea(r, mode, reg, 2, EA_DATA), Op(DN, reg9)])
        if opm == 7:
            n = 'divs' if hi == 8 else 'muls'
            return Insn(pc, n, 'w', [_ea(r, mode, reg, 2, EA_DATA), Op(DN, reg9)])
        if opm == 4 and mode in (0, 1):
            n = 'sbcd' if hi == 8 else 'abcd'
            if mode == 0:
                return Insn(pc, n, 'b', [Op(DN, reg), Op(DN, reg9)])
            return Insn(pc, n, 'b', [Op(PREDEC, reg), Op(PREDEC, reg9)])
        if hi == 0xc and opm in (5, 6) and mode in (0, 1):
            if opm == 5 and mode == 0:
                return Insn(pc, 'exg', 'l', [Op(DN, reg9), Op(DN, reg)])
            if opm == 5 and mode == 1:
                return Insn(pc, 'exg', 'l', [Op(AN, reg9), Op(AN, reg)])
            if opm == 6 and mode == 1:
                return Insn(pc, 'exg', 'l', [Op(DN, reg9), Op(AN, reg)])
            return None
        size = 1 << (opm & 3)
        if opm < 3:
            return Insn(pc, base, SIZES[size], [_ea(r, mode, reg, size, EA_DATA), Op(DN, reg9)])
        return Insn(pc, base, SIZES[size], [Op(DN, reg9), _ea(r, mode, reg, size, 'm')])
    if hi in (9, 0xd):
        base = 'sub' if hi == 9 else 'add'
        if opm in (3, 7):
            size = 2 if opm == 3 else 4
            return Insn(pc, base + 'a', SIZES[size], [_ea(r, mode, reg, size, EA_ALL), Op(AN, reg9)])
        size = 1 << (opm & 3)
        if opm >= 4 and mode in (0, 1):
            n = base + 'x'
            if mode == 0:
                return Insn(pc, n, SIZES[size], [Op(DN, reg), Op(DN, reg9)])
            return Insn(pc, n, SIZES[size], [Op(PREDEC, reg), Op(PREDEC, reg9)])
        if opm < 3:
            return Insn(pc, base, SIZES[size], [_ea(r, mode, reg, size, EA_ALL), Op(DN, reg9)])
        return Insn(pc, base, SIZES[size], [Op(DN, reg9), _ea(r, mode, reg, size, 'm')])
    # hi == 0xb
    if opm in (3, 7):
        size = 2 if opm == 3 else 4
        return Insn(pc, 'cmpa', SIZES[size], [_ea(r, mode, reg, size, EA_ALL), Op(AN, reg9)])
    size = 1 << (opm & 3)
    if opm < 3:
        return Insn(pc, 'cmp', SIZES[size], [_ea(r, mode, reg, size, EA_ALL), Op(DN, reg9)])
    if mode == 1:
        return Insn(pc, 'cmpm', SIZES[size], [Op(POSTINC, reg), Op(POSTINC, reg9)])
    return Insn(pc, 'eor', SIZES[size], [Op(DN, reg9), _ea(r, mode, reg, size, EA_DALT)])


def _shift(r, pc, op, reg9, mode, reg, ss):
    names = ['as', 'ls', 'rox', 'ro']
    if ss == 3:
        if op & 0x800:
            return None
        kind = (op >> 9) & 3
        d = 'l' if op & 0x100 else 'r'
        return Insn(pc, names[kind] + d, 'w', [_ea(r, mode, reg, 2, 'm')])
    kind = (op >> 3) & 3
    d = 'l' if op & 0x100 else 'r'
    size = 1 << ss
    if op & 0x20:
        cnt = Op(DN, reg9)
    else:
        cnt = Op(IMM, val=reg9 or 8)
    return Insn(pc, names[kind] + d, SIZES[size], [cnt, Op(DN, reg)])


# ---------------------------------------------------------------- formatting

def regname(n):
    return ('d%d' % n) if n < 8 else ('a%d' % (n - 8) if n != 15 else 'sp')


def areg(n):
    return 'sp' if n == 7 else 'a%d' % n


def fmt_reglist(mask):
    out = []
    i = 0
    while i < 16:
        if mask & (1 << i):
            j = i
            while j + 1 < 16 and mask & (1 << (j + 1)) and (j + 1) != 8:
                j += 1
            out.append(regname(i) if i == j else '%s-%s' % (regname(i), regname(j)))
            i = j + 1
        else:
            i += 1
    return '/'.join(out)


def hexv(v):
    return ('$%X' % v) if v > 9 or v < 0 else str(v)


def fmt_op(o, label=None, sz=None):
    """Format operand in vasm/asm68k Motorola syntax. label(addr)->str|None."""
    lab = (lambda a: label(a) if label else None)
    m = o.mode
    if m == DN:
        return 'd%d' % o.reg
    if m == AN:
        return areg(o.reg)
    if m == IND:
        return '(%s)' % areg(o.reg)
    if m == POSTINC:
        return '(%s)+' % areg(o.reg)
    if m == PREDEC:
        return '-(%s)' % areg(o.reg)
    if m == DISP:
        return '%s(%s)' % (sdisp(o.val), areg(o.reg))
    if m == INDEX:
        return '%s(%s,%s.%s)' % (sdisp(o.val), areg(o.reg), regname(o.xreg), 'l' if o.xlong else 'w')
    if m == ABSW:
        a = o.val & 0xffffffff
        n = lab(a & 0xffffff) if a < 0x8000 else None
        return '(%s).w' % (n or '$%X' % a)  # >= $8000 is sign-extended $FFFFxxxx
    if m == ABSL:
        n = lab(o.val & 0xffffff) if o.val < 0x1000000 else None
        return '(%s).l' % (n or '$%X' % o.val)
    if m == PCDISP:
        n = lab(o.val)
        return '%s(pc)' % (n or '$%X' % o.val)
    if m == PCINDEX:
        n = lab(o.val)
        return '%s(pc,%s.%s)' % (n or '$%X' % o.val, regname(o.xreg), 'l' if o.xlong else 'w')
    if m == IMM:
        return '#%s' % hexv(o.val)
    if m == REGLIST:
        return fmt_reglist(o.val)
    if m == CCR:
        return 'ccr'
    if m == SR:
        return 'sr'
    if m == USP:
        return 'usp'
    if m == BRANCH:
        return lab(o.val) or '$%X' % o.val
    raise ValueError(m)


def sdisp(v):
    if v < 0:
        return '-$%X' % -v
    return '$%X' % v if v > 9 else str(v)


def fmt_insn(i, label=None):
    m = i.mnem
    if i.sz and m not in ('moveq',):
        m += '.' + i.sz
    return ('%-8s %s' % (m, ','.join(fmt_op(o, label) for o in i.ops))).rstrip()
