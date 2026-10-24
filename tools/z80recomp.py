#!/usr/bin/env python3
"""Static recompiler for the Z80 sound driver -> C (temporary stage).

Same scheme as tools/recomp.py: one C function per call target, jumps
become gotos, calls push the real return address and `ret` pops it into
z80.ret so stack tricks (pop of a return address) unwind correctly.

usage: z80recomp.py ROM OUTDIR
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import z80  # noqa: E402
from z80dis import DRV_ROM, DRV_LEN, JUMP_TABLES  # noqa: E402

# entry points the host calls directly (see src/runtime/sound_host.c)
HOST_ENTRIES = [0x0000, 0x0038, 0x001F, 0x0027, 0x0086, 0x0091, 0x0C37, 0x1015, 0x1070, 0x11E8]

CCS = ['nz', 'z', 'nc', 'c', 'po', 'pe', 'p', 'm']
R8 = {'a': 'z80.a', 'b': 'z80.b', 'c': 'z80.c', 'd': 'z80.d', 'e': 'z80.e', 'h': 'z80.h', 'l': 'z80.l'}
R16R = {'bc': 'Z_BC', 'de': 'Z_DE', 'hl': 'Z_HL', 'af': 'Z_AF', 'sp': 'z80.sp', 'ix': 'z80.ix', 'iy': 'z80.iy'}
R16W = {'bc': 'z_set_bc(%s);', 'de': 'z_set_de(%s);', 'hl': 'z_set_hl(%s);', 'af': 'z_set_af(%s);',
        'sp': 'z80.sp = %s;', 'ix': 'z80.ix = %s;', 'iy': 'z80.iy = %s;'}


class Unsupported(Exception):
    pass


def num(s):
    return int(s[1:], 16)


def mem_addr(s):
    """address expression for a memory operand, or None"""
    m = re.match(r'^\((ix|iy)([+-]\d+)\)$', s)
    if m:
        return '(uint16_t)(z80.%s + %d)' % (m.group(1), int(m.group(2)))
    if s in ('(hl)', '(bc)', '(de)'):
        return R16R[s[1:-1]]
    m = re.match(r'^\(\$([0-9A-F]+)\)$', s)
    if m:
        return '0x%04X' % int(m.group(1), 16)
    return None


def rd8(s):
    if s in R8:
        return R8[s]
    if s in ('ixh', 'iyh'):
        return '(uint8_t)(z80.%s >> 8)' % s[:2]
    if s in ('ixl', 'iyl'):
        return '(uint8_t)z80.%s' % s[:2]
    a = mem_addr(s)
    if a:
        return 'z80_read(%s)' % a
    if s.startswith('$'):
        return '0x%02X' % num(s)
    raise Unsupported(s)


def wr8(s, v):
    if s in R8:
        return '%s = %s;' % (R8[s], v)
    if s in ('ixh', 'iyh'):
        return 'z80.%s = (uint16_t)((z80.%s & 0xff) | (%s) << 8);' % (s[:2], s[:2], v)
    if s in ('ixl', 'iyl'):
        return 'z80.%s = (uint16_t)((z80.%s & 0xff00) | (%s));' % (s[:2], s[:2], v)
    a = mem_addr(s)
    if a:
        return 'z80_write(%s, %s);' % (a, v)
    raise Unsupported(s)


def emit(text, nxt, fname):
    parts = text.split(' ', 1)
    m = parts[0]
    ops = parts[1].split(',') if len(parts) > 1 else []
    out = []
    if m == 'ld':
        d, s = ops
        if d in R16W and (s.startswith('$') or s in R16R or s.startswith('($')):
            if s.startswith('($'):
                out.append(R16W[d] % ('z_rd16(0x%04X)' % int(s[2:-1], 16)))
            elif s.startswith('$'):
                out.append(R16W[d] % ('0x%04X' % num(s)))
            else:
                out.append(R16W[d] % R16R[s])
        elif d.startswith('($') and s in R16R:
            out.append('z_wr16(0x%04X, %s);' % (int(d[2:-1], 16), R16R[s]))
        else:
            out.append(wr8(d, rd8(s)))
    elif m in ('add', 'adc', 'sbc') and ops[0] in ('hl', 'ix', 'iy'):
        d, s = ops
        if m == 'add':
            out.append(R16W[d] % ('z_add16(%s, %s)' % (R16R[d], R16R[s])))
        elif m == 'sbc':
            out.append(R16W[d] % ('z_sbc16(%s, %s)' % (R16R[d], R16R[s])))
        else:
            raise Unsupported(text)
    elif m in ('add', 'adc', 'sub', 'sbc', 'and', 'xor', 'or', 'cp'):
        s = ops[-1]
        v = rd8(s)
        if m == 'add':
            out.append('z80.a = z_add8(z80.a, %s, 0);' % v)
        elif m == 'adc':
            out.append('z80.a = z_add8(z80.a, %s, z80.f & ZF_C);' % v)
        elif m == 'sub':
            out.append('z80.a = z_sub8(z80.a, %s, 0);' % v)
        elif m == 'sbc':
            out.append('z80.a = z_sub8(z80.a, %s, z80.f & ZF_C);' % v)
        elif m == 'cp':
            out.append('z_cp(z80.a, %s);' % v)
        else:
            op = {'and': '&', 'xor': '^', 'or': '|'}[m]
            out.append('z80.a = z_logic((uint8_t)(z80.a %s %s), %d);' % (op, v, m == 'and'))
    elif m in ('inc', 'dec'):
        s = ops[0]
        if s in R16R:
            out.append(R16W[s] % ('(uint16_t)(%s %s 1)' % (R16R[s], '+' if m == 'inc' else '-')))
        else:
            out.append('{ uint8_t v = z_%s8(%s); %s }' % (m, rd8(s), wr8(s, 'v')))
    elif m in ('bit', 'set', 'res'):
        n, s = int(ops[0]), ops[1]
        if m == 'bit':
            out.append('z_bit(%d, %s);' % (n, rd8(s)))
        elif m == 'set':
            out.append(wr8(s, '(uint8_t)(%s | 0x%02X)' % (rd8(s), 1 << n)))
        else:
            out.append(wr8(s, '(uint8_t)(%s & 0x%02X)' % (rd8(s), ~(1 << n) & 0xff)))
    elif m in ('rr', 'srl'):
        s = ops[0]
        out.append('{ uint8_t v = z_%s(%s); %s }' % (m, rd8(s), wr8(s, 'v')))
    elif m == 'rrca':
        out.append('z_rrca();')
    elif m == 'push':
        out.append('z_push(%s);' % R16R[ops[0]])
    elif m == 'pop':
        out.append(R16W[ops[0]] % 'z_pop()')
    elif m == 'ex':
        if ops == ['de', 'hl']:
            out.append('{ uint8_t t = z80.d; z80.d = z80.h; z80.h = t; t = z80.e; z80.e = z80.l; z80.l = t; }')
        elif ops == ['af', "af'"]:
            out.append('{ uint8_t t = z80.a; z80.a = z80.a2; z80.a2 = t; t = z80.f; z80.f = z80.f2; z80.f2 = t; }')
        elif ops == ['(sp)', 'hl']:
            out.append('{ uint16_t t = z_rd16(z80.sp); z_wr16(z80.sp, Z_HL); z_set_hl(t); }')
        else:
            raise Unsupported(text)
    elif m == 'ldi':
        out.append('z_ldi(1);')
    elif m == 'ldir':
        out.append('do z_ldi(1); while (Z_BC);')
    elif m in ('di', 'ei', 'im', 'nop'):
        pass
    elif m in ('jp', 'jr') and len(ops) == 1 and ops[0].startswith('$'):
        out.append('goto L_%04X;' % num(ops[0]))
    elif m in ('jp', 'jr') and len(ops) == 2:
        out.append('if (z_cond(%d)) goto L_%04X;' % (CCS.index(ops[0]), num(ops[1])))
    elif m == 'djnz':
        out.append('if (--z80.b) goto L_%04X;' % num(ops[0]))
    elif m == 'call':
        t = num(ops[-1])
        call = 'z_push(0x%04X); %s(); if (z80.ret != 0x%04X) return;' % (nxt, fname(t), nxt)
        if len(ops) == 2:
            out.append('if (z_cond(%d)) { %s }' % (CCS.index(ops[0]), call))
        else:
            out.append(call)
    elif m == 'ret':
        if ops:
            out.append('if (z_cond(%d)) { z80.ret = z_pop(); return; }' % CCS.index(ops[0]))
        else:
            out.append('z80.ret = z_pop(); return;')
    elif m == 'jp' and ops[0] in ('(hl)', '(ix)', '(iy)'):
        out.append('z80_dispatch(%s); return;' % R16R[ops[0][1:-1]])
    else:
        raise Unsupported(text)
    return out


def main():
    rom = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    drv = rom[DRV_ROM:DRV_ROM + DRV_LEN]

    # trace
    entries = set(HOST_ENTRIES)
    for base, count in JUMP_TABLES:
        entries |= {drv[base + 2 * k] | drv[base + 2 * k + 1] << 8 for k in range(count)}
    insns = {}
    todo = list(entries)
    while todo:
        a = todo.pop()
        while a < len(drv) and a not in insns:
            n, text, flow, tgt = z80.decode(drv, a)
            insns[a] = (n, text, flow, tgt)
            if flow == 'call' and tgt is not None:
                entries.add(tgt)
            if tgt is not None and tgt < len(drv):
                todo.append(tgt)
            a += n
            if flow in ('jp', 'ret', 'jpind', 'halt'):
                break

    def fname(a):
        return 'z_%04X' % a

    # "ld hl,nn / push hl" pushes a continuation that a handler reached via
    # jp (hl) returns into; treat nn as code belonging to the pushing function
    pushed = {}
    for a, (n, text, flow, tgt) in insns.items():
        m = re.match(r'^ld hl,\$([0-9A-F]{4})$', text)
        if not m:
            continue
        nxt = insns.get(a + n, (0, '', None, None))
        # directly followed by push hl, or by a jump to code that starts so
        if nxt[1] == 'push hl' or (nxt[2] == 'jp' and nxt[3] is not None and
                                   insns.get(nxt[3], (0, ''))[1] == 'push hl'):
            pushed[a] = int(m.group(1), 16)
    todo = list(pushed.values())
    while todo:
        a = todo.pop()
        while a < len(drv) and a not in insns:
            n, text, flow, tgt = z80.decode(drv, a)
            insns[a] = (n, text, flow, tgt)
            if flow == 'call' and tgt is not None:
                entries.add(tgt)
            if tgt is not None and tgt < len(drv):
                todo.append(tgt)
            a += n
            if flow in ('jp', 'ret', 'jpind', 'halt'):
                break

    def body(entry):
        seen, st, conts = set(), [entry], set()
        while st:
            a = st.pop()
            if a in seen or a not in insns:
                continue
            seen.add(a)
            n, text, flow, tgt = insns[a]
            if a in pushed:
                conts.add(pushed[a])
                st.append(pushed[a])
            if flow in ('jp', 'jr') and tgt is not None:
                st.append(tgt)
            if flow in ('jp', 'ret', 'jpind', 'halt'):
                continue
            st.append(a + n)
        return seen, conts

    os.makedirs(outdir, exist_ok=True)
    ents = sorted(entries)
    lines = ['/* generated by tools/z80recomp.py - do not edit */',
             '#include "z80_rt.h"', '',
             '#ifdef Z80_TRACE',
             '#include <stdio.h>',
             'extern int z80_trace;',
             '#define ZTRACE(pc) do { if (z80_trace) fprintf(stderr, "z80 %04X a=%02X f=%02X bc=%04X de=%04X hl=%04X ix=%04X sp=%04X\\n", pc, z80.a, z80.f, Z_BC, Z_DE, Z_HL, z80.ix, z80.sp); } while (0)',
             '#else',
             '#define ZTRACE(pc) ((void)0)',
             '#endif', '']
    lines += ['void %s(void);' % fname(e) for e in ents] + ['']
    for e in ents:
        b, conts = body(e)
        b = sorted(b)
        lines.append('void %s(void)\n{' % fname(e))
        if b[0] != e:
            lines.append('    goto L_%04X;' % e)
        for k, a in enumerate(b):
            n, text, flow, tgt = insns[a]
            lines.append('L_%04X: /* %s */' % (a, text))
            lines.append('    ZTRACE(0x%04X);' % a)
            try:
                code = emit(text, a + n, fname)
            except Unsupported as ex:
                code = ['z80_fault("unsupported: %s", 0x%04X);' % (ex, a)]
            if flow == 'jpind' and conts:
                # the handler may return into a continuation pushed here
                code = [code[0].replace(' return;', '')] + [
                    'if (z80.ret == 0x%04X) goto L_%04X;' % (c, c) for c in sorted(conts)] + ['return;']
            if code:
                lines.append('    ' + ' '.join(code))
            if flow not in ('jp', 'ret', 'jpind', 'halt'):
                nx = a + n
                if k + 1 >= len(b) or b[k + 1] != nx:
                    lines.append('    goto L_%04X;' % nx if nx in b else
                                 '    z80_fault("ran off end", 0x%04X); return;' % nx)
        lines.append('}\n')
    lines.append('void z80_dispatch(uint16_t addr)\n{\n    switch (addr) {')
    lines += ['    case 0x%04X: %s(); return;' % (e, fname(e)) for e in ents]
    lines.append('    }\n    z80_fault("dispatch to unknown address", addr);\n}')
    open(os.path.join(outdir, 'z80_code.c'), 'w').write('\n'.join(lines) + '\n')
    with open(os.path.join(outdir, 'z80_funcs.h'), 'w') as h:
        h.write('/* generated by tools/z80recomp.py - do not edit */\n')
        h.write(''.join('void %s(void);\n' % fname(e) for e in ents))
    print('%d Z80 functions, %d instructions' % (len(ents), len(insns)))


if __name__ == '__main__':
    main()
