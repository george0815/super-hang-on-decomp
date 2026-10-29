#!/usr/bin/env python3
"""A small SPC700 assembler for the SNES port's sound program (snes/spc/).

usage: spc700.py SOURCE OUTPUT [-l LABELS]

Writes the assembled bytes from the first .org on (the image is loaded
there), and with -l the labels as ca65 constants (NAME = $XXXX).

Syntax: one statement a line, ';' comments, 'label:' definitions, and
'NAME = expr' constants. Directives: .org, .byte, .word, .incbin "file",
.res N. Operands are written as in the usual SPC700 syntax:
  #imm, dp, dp+x, dp+y, !abs, !abs+x, !abs+y, (x), (x)+, [dp+x], [dp]+y,
  dp.bit, a, x, y, ya, sp, labels for branches.
'dp' is a direct page address ($00-$FF; the program keeps P clear), '!abs'
a 16-bit one. Expressions: numbers ($hex, %bin, decimal), names, + - * /
<< >> & | ^, parentheses, and <expr / >expr for the low / high byte.
"""
import os
import re
import sys


class AsmError(Exception):
    pass


# (mnemonic, operand forms) -> opcode. Forms: 'a', 'x', 'y', 'ya', 'sp',
# 'psw', '#', 'dp', 'dp+x', 'dp+y', 'abs', 'abs+x', 'abs+y', '(x)', '(x)+',
# '(y)', '[dp+x]', '[dp]+y', 'dp.b', 'rel', '[abs+x]', 'c', 'abs.b'.
OPS = {
    ('mov', ('a', '#')): 0xE8, ('mov', ('a', '(x)')): 0xE6, ('mov', ('a', 'dp')): 0xE4,
    ('mov', ('a', 'dp+x')): 0xF4, ('mov', ('a', 'abs')): 0xE5, ('mov', ('a', 'abs+x')): 0xF5,
    ('mov', ('a', 'abs+y')): 0xF6, ('mov', ('a', '[dp]+y')): 0xF7, ('mov', ('a', '[dp+x]')): 0xE7,
    ('mov', ('a', '(x)+')): 0xBF,
    ('mov', ('x', '#')): 0xCD, ('mov', ('x', 'dp')): 0xF8, ('mov', ('x', 'dp+y')): 0xF9,
    ('mov', ('x', 'abs')): 0xE9,
    ('mov', ('y', '#')): 0x8D, ('mov', ('y', 'dp')): 0xEB, ('mov', ('y', 'dp+x')): 0xFB,
    ('mov', ('y', 'abs')): 0xEC,
    ('mov', ('dp', 'a')): 0xC4, ('mov', ('dp+x', 'a')): 0xD4, ('mov', ('abs', 'a')): 0xC5,
    ('mov', ('abs+x', 'a')): 0xD5, ('mov', ('abs+y', 'a')): 0xD6, ('mov', ('[dp]+y', 'a')): 0xD7,
    ('mov', ('[dp+x]', 'a')): 0xC7, ('mov', ('(x)', 'a')): 0xC6, ('mov', ('(x)+', 'a')): 0xAF,
    ('mov', ('dp', 'x')): 0xD8, ('mov', ('dp+y', 'x')): 0xD9, ('mov', ('abs', 'x')): 0xC9,
    ('mov', ('dp', 'y')): 0xCB, ('mov', ('dp+x', 'y')): 0xDB, ('mov', ('abs', 'y')): 0xCC,
    ('mov', ('a', 'x')): 0x7D, ('mov', ('a', 'y')): 0xDD, ('mov', ('x', 'a')): 0x5D,
    ('mov', ('y', 'a')): 0xFD, ('mov', ('x', 'sp')): 0x9D, ('mov', ('sp', 'x')): 0xBD,
    ('mov', ('dp', 'dp')): 0xFA, ('mov', ('dp', '#')): 0x8F,
    ('movw', ('ya', 'dp')): 0xBA, ('movw', ('dp', 'ya')): 0xDA,
    ('incw', ('dp',)): 0x3A, ('decw', ('dp',)): 0x1A, ('addw', ('ya', 'dp')): 0x7A,
    ('subw', ('ya', 'dp')): 0x9A, ('cmpw', ('ya', 'dp')): 0x5A,
    ('mul', ('ya',)): 0xCF, ('div', ('ya', 'x')): 0x9E,
    ('push', ('a',)): 0x2D, ('push', ('x',)): 0x4D, ('push', ('y',)): 0x6D, ('push', ('psw',)): 0x0D,
    ('pop', ('a',)): 0xAE, ('pop', ('x',)): 0xCE, ('pop', ('y',)): 0xEE, ('pop', ('psw',)): 0x8E,
    ('inc', ('a',)): 0xBC, ('inc', ('x',)): 0x3D, ('inc', ('y',)): 0xFC, ('inc', ('dp',)): 0xAB,
    ('inc', ('dp+x',)): 0xBB, ('inc', ('abs',)): 0xAC,
    ('dec', ('a',)): 0x9C, ('dec', ('x',)): 0x1D, ('dec', ('y',)): 0xDC, ('dec', ('dp',)): 0x8B,
    ('dec', ('dp+x',)): 0x9B, ('dec', ('abs',)): 0x8C,
    ('asl', ('a',)): 0x1C, ('asl', ('dp',)): 0x0B, ('lsr', ('a',)): 0x5C, ('lsr', ('dp',)): 0x4B,
    ('rol', ('a',)): 0x3C, ('rol', ('dp',)): 0x2B, ('ror', ('a',)): 0x7C, ('ror', ('dp',)): 0x6B,
    ('xcn', ('a',)): 0x9F,
    ('clrc', ()): 0x60, ('setc', ()): 0x80, ('notc', ()): 0xED, ('clrv', ()): 0xE0,
    ('clrp', ()): 0x20, ('setp', ()): 0x40, ('ei', ()): 0xA0, ('di', ()): 0xC0,
    ('nop', ()): 0x00, ('sleep', ()): 0xEF, ('stop', ()): 0xFF,
    ('ret', ()): 0x6F, ('reti', ()): 0x7F,
    ('jmp', ('abs',)): 0x5F, ('jmp', ('[abs+x]',)): 0x1F, ('call', ('abs',)): 0x3F,
    ('bra', ('rel',)): 0x2F, ('beq', ('rel',)): 0xF0, ('bne', ('rel',)): 0xD0,
    ('bcs', ('rel',)): 0xB0, ('bcc', ('rel',)): 0x90, ('bmi', ('rel',)): 0x30,
    ('bpl', ('rel',)): 0x10, ('bvs', ('rel',)): 0x70, ('bvc', ('rel',)): 0x50,
    ('cbne', ('dp', 'rel')): 0x2E, ('cbne', ('dp+x', 'rel')): 0xDE,
    ('dbnz', ('y', 'rel')): 0xFE, ('dbnz', ('dp', 'rel')): 0x6E,
}
# the ALU group: (a, #) (a, (x)) (a, dp) (a, dp+x) (a, abs) (a, abs+x)
# (a, abs+y) (a, [dp+x]) (a, [dp]+y) (dp, dp) (dp, #) at base + 8, 6, 4,
# $14, 5, $15, $16, 7, $17, 9, $18
for name, base in (('or', 0x00), ('and', 0x20), ('eor', 0x40), ('cmp', 0x60),
                   ('adc', 0x80), ('sbc', 0xA0)):
    for form, off in ((('a', '#'), 8), (('a', '(x)'), 6), (('a', 'dp'), 4),
                      (('a', 'dp+x'), 0x14), (('a', 'abs'), 5), (('a', 'abs+x'), 0x15),
                      (('a', 'abs+y'), 0x16), (('a', '[dp+x]'), 7), (('a', '[dp]+y'), 0x17),
                      (('dp', 'dp'), 9), (('dp', '#'), 0x18)):
        OPS[(name, form)] = base + off
OPS[('cmp', ('x', '#'))] = 0xC8
OPS[('cmp', ('x', 'dp'))] = 0x3E
OPS[('cmp', ('x', 'abs'))] = 0x1E
OPS[('cmp', ('y', '#'))] = 0xAD
OPS[('cmp', ('y', 'dp'))] = 0x7E
OPS[('cmp', ('y', 'abs'))] = 0x5E

BIT_OPS = {'set1': 0x02, 'clr1': 0x12, 'bbs': 0x03, 'bbc': 0x13}


class Asm:
    def __init__(self, path):
        self.path = path
        self.syms = {}
        self.labels = {}

    # ----------------------------------------------------------- expressions

    def value(self, text, need=True):
        text = text.strip()
        if text.startswith('<'):
            v = self.value(text[1:], need)
            return None if v is None else v & 0xFF
        if text.startswith('>'):
            v = self.value(text[1:], need)
            return None if v is None else v >> 8 & 0xFF
        expr = re.sub(r'\$([0-9A-Fa-f]+)', lambda m: str(int(m.group(1), 16)), text)
        expr = re.sub(r'%([01]+)', lambda m: str(int(m.group(1), 2)), expr)

        def name(m):
            n = m.group(0)
            if n in self.syms:
                return str(self.syms[n])
            if need:
                raise AsmError('unknown name %s' % n)
            return '0'
        expr = re.sub(r'[A-Za-z_@.][A-Za-z_0-9@.]*', name, expr)
        if not re.fullmatch(r'[0-9+\-*/<>&|^() ~]*', expr):
            raise AsmError('bad expression %s' % text)
        return int(eval(expr.replace('/', '//')))

    # ----------------------------------------------------------- operands

    def operand(self, text):
        """(form, value expression) of one operand."""
        t = text.strip()
        low = t.lower()
        if low in ('a', 'x', 'y', 'ya', 'sp', 'psw', 'c'):
            return low, None
        if low in ('(x)', '(x)+', '(y)'):
            return low, None
        if t.startswith('#'):
            return '#', t[1:]
        m = re.fullmatch(r'\[(.*)\+\s*x\]', t, re.I)
        if m:
            if m.group(1).strip().startswith('!'):
                return '[abs+x]', m.group(1).strip()[1:]
            return '[dp+x]', m.group(1)
        m = re.fullmatch(r'\[(.*)\]\s*\+\s*y', t, re.I)
        if m:
            return '[dp]+y', m.group(1)
        absolute = t.startswith('!')
        if absolute:
            t = t[1:]
        m = re.fullmatch(r'(.*?)\s*\+\s*([xy])', t, re.I)
        if m:
            return ('abs+' if absolute else 'dp+') + m.group(2).lower(), m.group(1)
        m = re.fullmatch(r'(.*)\.([0-7])', t)
        if m and not absolute:
            return 'dp.b', (m.group(1), int(m.group(2)))
        return ('abs' if absolute else 'dp'), t

    # ----------------------------------------------------------- statements

    def encode(self, mnem, ops, pc, final):
        forms = [self.operand(o) for o in ops]
        val = lambda e: self.value(e, final)
        if mnem in BIT_OPS:
            (form, (e, bit)) = forms[0]
            code = [BIT_OPS[mnem] | bit << 5, val(e) & 0xFF]
            if mnem in ('bbs', 'bbc'):
                code.append(self.rel(val(forms[1][1]), pc + 3, final))
            return code
        shape = tuple(f for f, _ in forms)
        if mnem in ('bra', 'beq', 'bne', 'bcs', 'bcc', 'bmi', 'bpl', 'bvs', 'bvc'):
            return [OPS[(mnem, ('rel',))], self.rel(val(forms[0][1]), pc + 2, final)]
        if mnem in ('cbne', 'dbnz'):
            first = forms[0][0]
            op = OPS[(mnem, (first, 'rel'))]
            if first == 'y':
                return [op, self.rel(val(forms[1][1]), pc + 2, final)]
            return [op, val(forms[0][1]) & 0xFF, self.rel(val(forms[1][1]), pc + 3, final)]
        if (mnem, shape) not in OPS:
            raise AsmError('no %s %s' % (mnem, ', '.join(shape)))
        op = OPS[(mnem, shape)]
        code = [op]
        if mnem == 'mov' and shape == ('dp', 'dp'):      # mov dst, src: src first
            return [op, val(forms[1][1]) & 0xFF, val(forms[0][1]) & 0xFF]
        if shape in (('dp', 'dp'),):
            return [op, val(forms[1][1]) & 0xFF, val(forms[0][1]) & 0xFF]
        if shape in (('dp', '#'),):                      # imm first
            return [op, val(forms[1][1]) & 0xFF, val(forms[0][1]) & 0xFF]
        for form, e in forms:
            if form in ('#', 'dp', 'dp+x', 'dp+y', '[dp+x]', '[dp]+y'):
                v = val(e)
                if final and form != '#' and not 0 <= v <= 0xFF:
                    raise AsmError('direct page address out of range: %s' % e)
                code.append(v & 0xFF)
            elif form in ('abs', 'abs+x', 'abs+y', '[abs+x]'):
                v = val(e)
                code += [v & 0xFF, v >> 8 & 0xFF]
        return code

    def rel(self, target, nxt, final):
        d = target - nxt
        if final and not -128 <= d <= 127:
            raise AsmError('branch out of range (%d)' % d)
        return d & 0xFF

    def lines(self):
        for n, raw in enumerate(open(self.path), 1):
            line = raw.split(';', 1)[0].rstrip()
            if line.strip():
                yield n, line

    def run(self, final):
        pc = None
        start = None
        out = bytearray()
        for n, line in self.lines():
            try:
                m = re.match(r'\s*([A-Za-z_@.][A-Za-z_0-9@.]*):\s*(.*)', line)
                if m:
                    self.syms[m.group(1)] = pc
                    self.labels[m.group(1)] = pc
                    line = m.group(2)
                    if not line.strip():
                        continue
                m = re.fullmatch(r'\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.*)', line)
                if m:
                    self.syms[m.group(1)] = self.value(m.group(2), final)
                    continue
                parts = line.strip().split(None, 1)
                mnem = parts[0].lower()
                args = parts[1] if len(parts) > 1 else ''
                if mnem == '.org':
                    org = self.value(args)
                    if pc is None:
                        pc = start = org
                    else:
                        if org < pc:
                            raise AsmError('.org backwards')
                        out += bytes(org - pc)
                        pc = org
                    continue
                if pc is None:
                    raise AsmError('code before .org')
                if mnem in ('.byte', '.word'):
                    size = 1 if mnem == '.byte' else 2
                    for e in split_args(args):
                        v = self.value(e, final)
                        out += v.to_bytes(size, 'little', signed=v < 0) if final else bytes(size)
                    pc += size * len(split_args(args))
                    continue
                if mnem == '.res':
                    k = self.value(args)
                    out += bytes(k)
                    pc += k
                    continue
                if mnem == '.incbin':
                    name = args.strip().strip('"')
                    data = open(os.path.join(os.path.dirname(self.path), name), 'rb').read() \
                        if not os.path.isabs(name) and not os.path.exists(name) else open(name, 'rb').read()
                    out += data
                    pc += len(data)
                    continue
                code = self.encode(mnem, split_args(args), pc, final)
                out += bytes(code)
                pc += len(code)
            except AsmError as err:
                raise AsmError('%s:%d: %s' % (self.path, n, err))
            except (KeyError, ValueError, TypeError) as err:
                raise AsmError('%s:%d: %r' % (self.path, n, err))
        return start, out


def split_args(text):
    """Operands split at top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def main():
    args = sys.argv[1:]
    labels = None
    if '-l' in args:
        k = args.index('-l')
        labels = args[k + 1]
        del args[k:k + 2]
    asm = Asm(args[0])
    try:
        asm.run(False)
        start, out = asm.run(True)
    except AsmError as err:
        sys.exit(str(err))
    open(args[1], 'wb').write(out)
    if labels:
        with open(labels, 'w') as f:
            f.write('; %s (tools/snes/spc700.py)\n' % args[0])
            f.write('SPC_ORG = $%04X\nSPC_SIZE = $%04X\n' % (start, len(out)))
            for name, v in sorted(asm.labels.items()):
                if not name.startswith('@'):
                    f.write('SPC_%s = $%04X\n' % (name.upper(), v))


if __name__ == '__main__':
    main()
