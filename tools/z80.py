"""Z80 instruction decoder (documented instruction set plus the common
undocumented IXH/IXL forms). Used to study the sound driver.

decode(mem, pc) -> (length, text, flow, target)
  flow: None, 'jp', 'jr', 'call', 'ret', 'reti', 'rst', 'jpind', 'halt'
  conditional jumps/calls/returns use the same flow names; `cond` is in text.
"""

R8 = ['b', 'c', 'd', 'e', 'h', 'l', '(hl)', 'a']
R16 = ['bc', 'de', 'hl', 'sp']
R16AF = ['bc', 'de', 'hl', 'af']
CC = ['nz', 'z', 'nc', 'c', 'po', 'pe', 'p', 'm']
ALU = ['add a,', 'adc a,', 'sub ', 'sbc a,', 'and ', 'xor ', 'or ', 'cp ']
ROT = ['rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'sll', 'srl']


def _s8(v):
    return v - 256 if v & 0x80 else v


def h8(v):
    return '$%02X' % v


def h16(v):
    return '$%04X' % v


def decode(mem, pc):
    def b(k):
        return mem[(pc + k) & 0xffff] if (pc + k) < len(mem) else 0

    def w(k):
        return b(k) | b(k + 1) << 8

    op = b(0)
    if op in (0xdd, 0xfd):
        return _index(mem, pc, 'ix' if op == 0xdd else 'iy', b, w)
    if op == 0xcb:
        o = b(1)
        x, y, z = o >> 6, (o >> 3) & 7, o & 7
        if x == 0:
            return 2, '%s %s' % (ROT[y], R8[z]), None, None
        return 2, '%s %d,%s' % (['', 'bit', 'res', 'set'][x], y, R8[z]), None, None
    if op == 0xed:
        return _ed(b, w)

    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if x == 0:
        if z == 0:
            if y == 0:
                return 1, 'nop', None, None
            if y == 1:
                return 1, "ex af,af'", None, None
            t = (pc + 2 + _s8(b(1))) & 0xffff
            if y == 2:
                return 2, 'djnz %s' % h16(t), 'jr', t
            if y == 3:
                return 2, 'jr %s' % h16(t), 'jp', t
            return 2, 'jr %s,%s' % (CC[y - 4], h16(t)), 'jr', t
        if z == 1:
            if q == 0:
                return 3, 'ld %s,%s' % (R16[p], h16(w(1))), None, None
            return 1, 'add hl,%s' % R16[p], None, None
        if z == 2:
            forms = ['ld (bc),a', 'ld a,(bc)', 'ld (de),a', 'ld a,(de)']
            if p < 2:
                return 1, forms[p * 2 + q], None, None
            if p == 2:
                return 3, ('ld (%s),hl' if q == 0 else 'ld hl,(%s)') % h16(w(1)), None, None
            return 3, ('ld (%s),a' if q == 0 else 'ld a,(%s)') % h16(w(1)), None, None
        if z == 3:
            return 1, '%s %s' % ('inc' if q == 0 else 'dec', R16[p]), None, None
        if z == 4:
            return 1, 'inc %s' % R8[y], None, None
        if z == 5:
            return 1, 'dec %s' % R8[y], None, None
        if z == 6:
            return 2, 'ld %s,%s' % (R8[y], h8(b(1))), None, None
        return 1, ['rlca', 'rrca', 'rla', 'rra', 'daa', 'cpl', 'scf', 'ccf'][y], None, None
    if x == 1:
        if op == 0x76:
            return 1, 'halt', 'halt', None
        return 1, 'ld %s,%s' % (R8[y], R8[z]), None, None
    if x == 2:
        return 1, '%s%s' % (ALU[y], R8[z]), None, None
    # x == 3
    if z == 0:
        return 1, 'ret %s' % CC[y], 'cret', None
    if z == 1:
        if q == 0:
            return 1, 'pop %s' % R16AF[p], None, None
        return 1, ['ret', 'exx', 'jp (hl)', 'ld sp,hl'][p], ['ret', None, 'jpind', None][p], None
    if z == 2:
        return 3, 'jp %s,%s' % (CC[y], h16(w(1))), 'jr', w(1)
    if z == 3:
        if y == 0:
            return 3, 'jp %s' % h16(w(1)), 'jp', w(1)
        if y == 2:
            return 2, 'out (%s),a' % h8(b(1)), None, None
        if y == 3:
            return 2, 'in a,(%s)' % h8(b(1)), None, None
        return 1, [None, None, None, None, 'ex (sp),hl', 'ex de,hl', 'di', 'ei'][y], None, None
    if z == 4:
        return 3, 'call %s,%s' % (CC[y], h16(w(1))), 'call', w(1)
    if z == 5:
        if q == 0:
            return 1, 'push %s' % R16AF[p], None, None
        if p == 0:
            return 3, 'call %s' % h16(w(1)), 'call', w(1)
        return 1, 'db %s' % h8(op), None, None
    if z == 6:
        return 2, '%s%s' % (ALU[y], h8(b(1))), None, None
    return 1, 'rst %s' % h8(y * 8), 'call', y * 8


def _ed(b, w):
    o = b(1)
    x, y, z = o >> 6, (o >> 3) & 7, o & 7
    p, q = y >> 1, y & 1
    if x == 1:
        if z == 0:
            return 2, 'in %s,(c)' % (R8[y] if y != 6 else 'f'), None, None
        if z == 1:
            return 2, 'out (c),%s' % (R8[y] if y != 6 else '0'), None, None
        if z == 2:
            return 2, '%s hl,%s' % ('sbc' if q == 0 else 'adc', R16[p]), None, None
        if z == 3:
            return 4, ('ld (%s),%s' if q == 0 else 'ld %s,(%s)') % (
                (h16(w(2)), R16[p]) if q == 0 else (R16[p], h16(w(2)))), None, None
        if z == 4:
            return 2, 'neg', None, None
        if z == 5:
            return 2, 'reti' if y == 1 else 'retn', 'ret', None
        if z == 6:
            return 2, 'im %s' % ['0', '0/1', '1', '2'][y & 3], None, None
        return 2, ['ld i,a', 'ld r,a', 'ld a,i', 'ld a,r', 'rrd', 'rld', 'nop', 'nop'][y], None, None
    if x == 2 and z <= 3 and y >= 4:
        return 2, [['ldi', 'cpi', 'ini', 'outi'], ['ldd', 'cpd', 'ind', 'outd'],
                   ['ldir', 'cpir', 'inir', 'otir'], ['lddr', 'cpdr', 'indr', 'otdr']][y - 4][z], None, None
    return 2, 'db $ED,%s' % h8(o), None, None


def _index(mem, pc, ix, b, w):
    o = b(1)
    if o == 0xcb:
        d = _s8(b(2))
        o2 = b(3)
        x, y, z = o2 >> 6, (o2 >> 3) & 7, o2 & 7
        ref = '(%s%+d)' % (ix, d)
        if x == 0:
            return 4, '%s %s' % (ROT[y], ref), None, None
        return 4, '%s %d,%s' % (['', 'bit', 'res', 'set'][x], y, ref), None, None
    if o == 0xe9:
        return 2, 'jp (%s)' % ix, 'jpind', None
    # reuse the base decoder with HL -> IX substitution
    sub = bytes([o, b(2), b(3), b(4)])
    uses_mem = o in (0x34, 0x35, 0x36) or (0x40 <= o < 0x80 and o != 0x76 and ((o & 7) == 6 or ((o >> 3) & 7) == 6)) or (
        0x80 <= o < 0xc0 and (o & 7) == 6)
    if uses_mem:
        d = _s8(b(2))
        ref = '(%s%+d)' % (ix, d)
        if o == 0x36:
            return 4, 'ld %s,%s' % (ref, h8(b(3))), None, None
        # memory operand form: the displacement byte sits between the
        # opcode and any immediate data
        n, text, flow, tgt = decode(bytes([o, b(3), b(4), 0]), 0)
        text = text.replace('(hl)', ref)
        return 2 + n, text, flow, tgt
    n, text, flow, tgt = decode(sub, 0)
    if 'hl' in text or text.split(' ')[0] in ('inc', 'dec', 'ld', 'add', 'sub', 'and', 'or', 'xor', 'cp', 'adc', 'sbc'):
        text = text.replace('hl', ix)
        if o not in (0xeb,):  # ex de,hl is not affected
            text = text.replace(' h,', ' %sh,' % ix).replace(',h', ',%sh' % ix).replace(' l,', ' %sl,' % ix).replace(',l', ',%sl' % ix)
    return 1 + n, text, flow, tgt
