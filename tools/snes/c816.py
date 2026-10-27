#!/usr/bin/env python3
"""
A compiler from the game's C (src/game) to 65816 assembly (ca65) for the
SNES port's SA-1.

usage: c816.py OUT.s SEGMENT FILE.c [--skip NAME,...] [--cpp-arg ARG]... [--trace]

--trace makes every function note its address in a ring buffer
(trace_ring in snes/rt.s), for debugging.

The game code reads and writes the Genesis's memory through rd8/rd16/
rd32 and wr8/wr16/wr32; here those become accesses to the Genesis RAM
mirror (BW-RAM bank $40, big-endian, the data bank while game code runs),
to the copy of the Genesis ROM (MDROM), or calls into the runtime for the
hardware (VDP ports and DMA, the Z80's mailbox, the pads; snes/rt.s).

Conventions:
  - native mode, A and X/Y 16-bit, the direct page at SA1_DP, data bank $40
  - calls are jsl/rtl; arguments are pushed left to right (8- and 16-bit
    types take 2 bytes, 32-bit ones 4: high word first), the caller pops
  - results: 8/16-bit in A; 32-bit in A (low) and CHI (high)
  - locals live on the stack; statics in BW-RAM (CBSS), constant tables
    in ROM (CRODATA)
  - C's integer rules are followed: int is 32-bit. Expressions whose
    value is known to fit 16 bits, or whose low 16 bits are all that is
    used, are computed in 16 bits.

Functions named *_68k (adapters for the recompiled original) and those in
--skip are left out; calls to functions not defined here go to symbols
of that name (the runtime provides those not compiled).
"""
import os
import subprocess
import sys

from pycparser import c_ast, c_parser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')

# compiler temporaries on the direct page (snes/inc/dp.inc)
CHI, CT0, CT1, CT2, CPTR, CT3 = 'z:CHI', 'z:CT0', 'z:CT1', 'z:CT2', 'z:CPTR', 'z:CT3'


class CompileError(Exception):
    pass


# ---------------------------------------------------------------- types

class T:
    """A C type: int (size 1/2/4, signed), void, ptr, array, struct, func."""
    def __init__(self, kind, size=0, signed=False, target=None, n=0, fields=None,
                 ret=None, params=None):
        self.kind, self.size, self.signed = kind, size, signed
        self.target, self.n, self.fields = target, n, fields
        self.ret, self.params = ret, params

    def __repr__(self):
        if self.kind == 'int':
            return '%sint%d' % ('' if self.signed else 'u', self.size * 8)
        return self.kind

    @property
    def is_int(self):
        return self.kind == 'int'

    def sizeof(self):
        if self.kind == 'int':
            return self.size
        if self.kind == 'ptr' or self.kind == 'func':
            return 4
        if self.kind == 'array':
            return self.n * self.target.sizeof()
        if self.kind == 'struct':
            return sum(f[1].sizeof() for f in self.fields)
        raise CompileError('sizeof %s' % self.kind)

    def slot(self):
        """Bytes the value takes on the stack or in a 32/16-bit register."""
        return 4 if (self.kind == 'int' and self.size == 4) or self.kind in ('ptr', 'func') else 2


INT = T('int', 4, True)
UINT = T('int', 4, False)
VOID = T('void')
BASE = {
    ('uint8_t',): T('int', 1, False), ('int8_t',): T('int', 1, True),
    ('uint16_t',): T('int', 2, False), ('int16_t',): T('int', 2, True),
    ('uint32_t',): UINT, ('int32_t',): INT, ('size_t',): UINT,
    ('int',): INT, ('signed',): INT, ('signed', 'int'): INT, ('unsigned',): UINT,
    ('unsigned', 'int'): UINT, ('long',): INT, ('unsigned', 'long'): UINT,
    ('char',): T('int', 1, True), ('unsigned', 'char'): T('int', 1, False),
    ('signed', 'char'): T('int', 1, True),
    ('short',): T('int', 2, True), ('unsigned', 'short'): T('int', 2, False),
    ('void',): VOID,
}


def promote(t):
    if t.is_int and t.size < 4:
        return INT
    return t


def arith(a, b):
    a, b = promote(a), promote(b)
    if a.kind != 'int' or b.kind != 'int':
        return UINT
    if not a.signed or not b.signed:
        return UINT
    return INT


# A value class: what is known of an expression's value. bits 8/16/32,
# signed: the value is the zero (unsigned) or sign extension of that many
# bits.
class V:
    def __init__(self, bits, signed):
        self.bits, self.signed = bits, signed

    def __repr__(self):
        return '%s%d' % ('s' if self.signed else 'u', self.bits)

    def fits_u16(self):
        return self.bits <= 16 and not self.signed

    def fits_s16(self):
        return (self.signed and self.bits <= 16) or (not self.signed and self.bits <= 8)

    def narrow(self):
        return self.bits <= 16


def const_class(c):
    if 0 <= c <= 0xFF:
        return V(8, False)
    if 0 <= c <= 0xFFFF:
        return V(16, False)
    if -0x8000 <= c < 0:
        return V(16, True)
    return V(32, c < 0)


def type_class(t):
    if t.is_int:
        return V(t.size * 8, t.signed)
    return V(32, False)


# ---------------------------------------------------------------- compiler

class Function:
    def __init__(self, name, ftype, symbol):
        self.name, self.type, self.symbol = name, ftype, symbol


class Var:
    """A variable: a stack slot (offset from the frame base), a static
    (symbol), or a parameter."""
    def __init__(self, name, t, where, offset=0, symbol=None):
        self.name, self.type, self.where = name, t, where
        self.offset, self.symbol = offset, symbol


class Compiler:
    def __init__(self, path, segment, skip, cpp_args, trace=False):
        self.trace = trace
        self.path = path
        self.base = os.path.splitext(os.path.basename(path))[0]
        self.segment = segment
        self.skip = set(skip)
        self.cpp_args = cpp_args
        self.out = []
        self.data = []          # CRODATA lines
        self.bss = []           # CBSS lines
        self.init = []          # initialised statics: (symbol, size, value)
        self.typedefs = {}
        self.structs = {}
        self.funcs = {}         # name -> Function (declared or defined)
        self.defined = set()
        self.globals = {}       # name -> Var (file scope)
        self.label_n = 0
        self.extern = set()
        self.export = set()
        self.declared = set()   # statics emitted already

    # ------------------------------------------------------------ output

    def emit(self, line):
        self.out.append('    ' + line)

    def label(self, name):
        self.out.append('%s:' % name)

    def new_label(self):
        self.label_n += 1
        return '@L%d' % self.label_n

    # ------------------------------------------------------------ types

    def resolve(self, node):
        """C type of a declaration's type node."""
        if isinstance(node, c_ast.TypeDecl):
            return self.resolve(node.type)
        if isinstance(node, c_ast.IdentifierType):
            names = tuple(n for n in node.names if n not in ('const', 'volatile'))
            if names in BASE:
                return BASE[names]
            if len(names) == 1 and names[0] in self.typedefs:
                return self.typedefs[names[0]]
            raise CompileError('unknown type %s' % ' '.join(names))
        if isinstance(node, c_ast.PtrDecl):
            return T('ptr', target=self.resolve(node.type))
        if isinstance(node, c_ast.ArrayDecl):
            elem = self.resolve(node.type)
            n = self.const_eval(node.dim) if node.dim is not None else 0
            return T('array', target=elem, n=n)
        if isinstance(node, c_ast.FuncDecl):
            ret = self.resolve(node.type)
            params = []
            if node.args:
                for p in node.args.params:
                    if isinstance(p, c_ast.EllipsisParam):
                        continue
                    pt = self.resolve(p.type)
                    if pt.kind == 'void':
                        continue
                    if pt.kind == 'array':
                        pt = T('ptr', target=pt.target)
                    params.append((p.name, pt))
            return T('func', ret=ret, params=params)
        if isinstance(node, c_ast.Struct):
            if node.decls is None:
                return self.structs[node.name]
            fields = [(d.name, self.resolve(d.type)) for d in node.decls]
            st = T('struct', fields=fields)
            if node.name:
                self.structs[node.name] = st
            return st
        if isinstance(node, c_ast.Typename):
            return self.resolve(node.type)
        raise CompileError('type node %s' % type(node).__name__)

    # ------------------------------------------------------------ constants

    def const_eval(self, e):
        """The value of a constant expression, or None."""
        if isinstance(e, c_ast.Constant):
            if e.type in ('int', 'unsigned int', 'long int', 'unsigned long int', 'long long int'):
                s = e.value.rstrip('uUlL')
                return int(s, 0) if not (s.startswith('0') and len(s) > 1 and s[1] not in 'xXbB') else int(s, 8)
            if e.type == 'char':
                return ord(e.value[1:-1].encode().decode('unicode_escape'))
            return None
        if isinstance(e, c_ast.UnaryOp):
            v = self.const_eval(e.expr) if e.op != 'sizeof' else None
            if e.op == 'sizeof':
                return self.sizeof_expr(e.expr)
            if v is None:
                return None
            return {'-': -v, '+': v, '~': ~v, '!': int(not v)}.get(e.op)
        if isinstance(e, c_ast.BinaryOp):
            a, b = self.const_eval(e.left), self.const_eval(e.right)
            if a is None or b is None:
                return None
            ops = {'+': a + b, '-': a - b, '*': a * b, '&': a & b, '|': a | b, '^': a ^ b,
                   '<<': a << b, '>>': a >> b, '==': int(a == b), '!=': int(a != b),
                   '<': int(a < b), '>': int(a > b), '<=': int(a <= b), '>=': int(a >= b),
                   '&&': int(bool(a and b)), '||': int(bool(a or b))}
            if e.op in ('/', '%'):
                if b == 0:
                    return None
                q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
                return q if e.op == '/' else a - q * b
            return ops.get(e.op)
        if isinstance(e, c_ast.Cast):
            v = self.const_eval(e.expr)
            if v is None:
                return None
            t = self.resolve(e.to_type)
            if t.is_int:
                v &= (1 << (8 * t.size)) - 1
                if t.signed and v >= 1 << (8 * t.size - 1):
                    v -= 1 << (8 * t.size)
            return v
        if isinstance(e, c_ast.TernaryOp):
            c = self.const_eval(e.cond)
            if c is None:
                return None
            return self.const_eval(e.iftrue if c else e.iffalse)
        return None

    def sizeof_expr(self, e):
        if isinstance(e, c_ast.Typename):
            return self.resolve(e).sizeof()
        return self.type_of(e).sizeof()

    # ------------------------------------------------------------ scopes

    def lookup(self, name):
        for scope in reversed(self.scopes):
            if name in scope:
                return scope[name]
        if name in self.globals:
            return self.globals[name]
        return None

    # ------------------------------------------------------------ types of expressions

    def type_of(self, e):
        if isinstance(e, c_ast.Constant):
            if e.type == 'string':
                return T('ptr', target=T('int', 1, True))
            v = self.const_eval(e)
            if 'unsigned' in e.type or (e.value.lower().endswith('u')):
                return UINT
            return INT if v is None or v < 0x80000000 else UINT
        if isinstance(e, c_ast.ID):
            v = self.lookup(e.name)
            if v is not None:
                return v.type
            if e.name in self.funcs:
                return self.funcs[e.name].type
            raise CompileError('unknown name %s' % e.name)
        if isinstance(e, c_ast.Cast):
            return self.resolve(e.to_type)
        if isinstance(e, c_ast.UnaryOp):
            if e.op in ('!',):
                return INT
            if e.op == 'sizeof':
                return UINT
            if e.op == '&':
                return T('ptr', target=self.type_of(e.expr))
            if e.op == '*':
                t = self.type_of(e.expr)
                return t.target
            if e.op in ('p++', 'p--', '++', '--'):
                return self.type_of(e.expr)
            return promote(self.type_of(e.expr))
        if isinstance(e, c_ast.BinaryOp):
            if e.op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
                return INT
            lt, rt = self.type_of(e.left), self.type_of(e.right)
            if lt.kind in ('ptr', 'array') and e.op in ('+', '-'):
                return T('ptr', target=lt.target)
            if e.op in ('<<', '>>'):
                return promote(lt)
            return arith(lt, rt)
        if isinstance(e, c_ast.Assignment):
            return self.type_of(e.lvalue)
        if isinstance(e, c_ast.TernaryOp):
            return arith(self.type_of(e.iftrue), self.type_of(e.iffalse))
        if isinstance(e, c_ast.FuncCall):
            name = e.name.name if isinstance(e.name, c_ast.ID) else None
            if name in INTRINSIC_TYPES:
                return INTRINSIC_TYPES[name]
            ft = self.type_of(e.name)
            if ft.kind == 'ptr':
                ft = ft.target
            return ft.ret
        if isinstance(e, c_ast.ArrayRef):
            return self.type_of(e.name).target
        if isinstance(e, c_ast.StructRef):
            st = self.type_of(e.name)
            if e.type == '->':
                st = st.target
            for fname, ftype in st.fields:
                if fname == e.field.name:
                    return ftype
            raise CompileError('no field %s' % e.field.name)
        if isinstance(e, c_ast.ExprList):
            return self.type_of(e.exprs[-1])
        raise CompileError('type of %s' % type(e).__name__)

    def vclass(self, e):
        """What is known of e's value (see V)."""
        c = self.const_eval(e)
        if c is not None:
            return const_class(c)
        if isinstance(e, c_ast.ID):
            return type_class(self.type_of(e))
        if isinstance(e, c_ast.Cast):
            t = self.resolve(e.to_type)
            if not t.is_int:
                return V(32, False)
            inner = self.vclass(e.expr)
            if t.size < 4:
                if inner.bits <= t.size * 8 and inner.signed == t.signed:
                    return inner
                if not inner.signed and inner.bits < t.size * 8:
                    return inner
                return V(t.size * 8, t.signed)
            if inner.bits < 32 and (t.signed or not inner.signed):
                return inner
            return V(32, t.signed)
        if isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID):
            n = e.name.name
            if n == 'rd8':
                return V(8, False)
            if n == 'rd16':
                return V(16, False)
            if n == 'sx16':
                return V(16, True)
            if n == 'sx8':
                return V(8, True)
            return type_class(self.type_of(e))
        if isinstance(e, c_ast.BinaryOp):
            if e.op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
                return V(8, False)
            l, r = self.vclass(e.left), self.vclass(e.right)
            t = self.type_of(e)
            if e.op == '&':
                cands = [x for x in (l, r) if not x.signed and x.bits <= 16]
                if cands:
                    return min(cands, key=lambda x: x.bits)
                if l.narrow() and r.narrow() and l.signed and r.signed:
                    return V(16, True)
            if e.op in ('|', '^'):
                if l.fits_u16() and r.fits_u16():
                    return V(max(l.bits, r.bits), False)
                if l.fits_s16() and r.fits_s16():
                    return V(16, True)
            if e.op == '>>':
                if l.narrow():
                    return l
            if e.op == '%' and l.fits_u16() and r.fits_u16():
                return r if r.bits >= l.bits else l
            if e.op == '/' and l.fits_u16() and r.fits_u16():
                return l
            return type_class(t)
        if isinstance(e, c_ast.UnaryOp):
            if e.op == '!':
                return V(8, False)
            if e.op == '~':
                inner = self.vclass(e.expr)
                if inner.signed and inner.narrow():
                    return V(16, True)
            if e.op in ('p++', 'p--', '++', '--', '*'):
                return type_class(self.type_of(e))
            return type_class(self.type_of(e))
        if isinstance(e, c_ast.TernaryOp):
            a, b = self.vclass(e.iftrue), self.vclass(e.iffalse)
            if a.fits_u16() and b.fits_u16():
                return V(16, False)
            if a.fits_s16() and b.fits_s16():
                return V(16, True)
            return type_class(self.type_of(e))
        return type_class(self.type_of(e))

    # ------------------------------------------------------------ stack

    def push16(self):
        self.emit('pha')
        self.depth += 2

    def push32(self):
        self.emit('pei (CHI)')
        self.emit('pha')
        self.depth += 4

    def pop(self, n):
        """Drop n bytes, keeping A."""
        if n == 0:
            return
        if n <= 6:
            for _ in range(n // 2):
                self.emit('plx')
        else:
            self.emit('tay')
            self.emit('tsc')
            self.emit('clc')
            self.emit('adc #%d' % n)
            self.emit('tcs')
            self.emit('tya')
        self.depth -= n

    def sref(self, var, extra=0):
        """The stack-relative operand of a stack variable."""
        off = var.offset + self.depth + 1 + extra
        if off > 255:
            raise CompileError('frame too big in %s' % self.fn.name)
        return '%d, s' % off

    # ------------------------------------------------------------ values

    def extend(self, cls):
        """A holds a value of class cls (<= 16 bits): fill CHI."""
        if cls.bits == 32:
            return
        if cls.signed:
            lab = self.new_label()
            self.emit('ldx #0')
            self.emit('cmp #$8000')
            self.emit('bcc %s' % lab)
            self.emit('dex')
            self.label(lab)
            self.emit('stx CHI')
        else:
            self.emit('stz CHI')

    def gen(self, e, need):
        """Code for e: its low 16 bits in A (need 16), or all 32 in A and CHI
        (need 32). Returns e's value class."""
        cls = self.vclass(e)
        c = self.const_eval(e)
        if c is not None:
            self.emit('lda #$%04X' % (c & 0xFFFF))
            if need == 32:
                hi = (c >> 16) & 0xFFFF
                if hi:
                    self.emit('ldx #$%04X' % hi)
                    self.emit('stx CHI')
                else:
                    self.emit('stz CHI')
            return cls
        got = self.gen_value(e, need)
        if need == 32 and got == 16:
            self.extend(cls)
        return cls

    def gen_value(self, e, need):
        """As gen, without constants; returns how many bits it produced (16
        if only A holds the value: then e's class is narrow or need is 16)."""
        m = getattr(self, 'g_' + type(e).__name__, None)
        if m is None:
            raise CompileError('cannot compile %s' % type(e).__name__)
        return m(e, need)

    # --- names

    def g_ID(self, e, need):
        var = self.lookup(e.name)
        if var is None:
            if e.name in self.funcs:
                f = self.funcs[e.name]
                self.ref_func(f)
                self.emit('lda #.loword(%s)' % f.symbol)
                self.emit('ldx #^%s' % f.symbol)
                self.emit('stx CHI')
                return 32
            raise CompileError('unknown %s' % e.name)
        t = var.type
        if t.kind == 'array':
            self.address_of(e)
            return 32
        return self.load_var(var, need)

    def load_var(self, var, need):
        t = var.type
        size = t.size if t.is_int else 4
        if var.where == 'stack':
            if size == 4:
                if need == 32:
                    self.emit('lda %s' % self.sref(var, 2))
                    self.emit('sta CHI')
                self.emit('lda %s' % self.sref(var))
                return 32 if need == 32 else 16
            self.emit('lda %s' % self.sref(var))
            return 16
        sym = var.symbol
        if size == 4:
            if need == 32:
                self.emit('lda f:%s + 2' % sym)
                self.emit('sta CHI')
            self.emit('lda f:%s' % sym)
            return 32 if need == 32 else 16
        if size == 2:
            self.emit('lda f:%s' % sym)
            return 16
        self.emit('lda f:%s' % sym)
        if t.signed:
            self.sign8()
        else:
            self.emit('and #$00FF')
        return 16

    def sign8(self):
        lab = self.new_label()
        self.emit('and #$00FF')
        self.emit('cmp #$0080')
        self.emit('bcc %s' % lab)
        self.emit('ora #$FF00')
        self.label(lab)

    def store_var(self, var, cls_bits):
        """Store A (and CHI) into var, truncating."""
        t = var.type
        size = t.size if t.is_int else 4
        if var.where == 'stack':
            if size == 4:
                self.emit('sta %s' % self.sref(var))
                self.emit('ldx CHI')
                self.emit('pha')
                self.depth += 2
                self.emit('txa')
                self.emit('sta %s' % self.sref(var, 2))
                self.emit('pla')
                self.depth -= 2
                return
            if size == 1:
                if t.signed:
                    self.sign8()
                else:
                    self.emit('and #$00FF')
            self.emit('sta %s' % self.sref(var))
            return
        sym = var.symbol
        if size == 4:
            self.emit('sta f:%s' % sym)
            self.emit('ldx CHI')
            self.emit('pha')
            self.emit('txa')
            self.emit('sta f:%s + 2' % sym)
            self.emit('pla')
            return
        if size == 2:
            self.emit('sta f:%s' % sym)
            return
        self.emit('sep #$20')
        self.emit('sta f:%s' % sym)
        self.emit('rep #$20')
        if t.signed:
            self.sign8()
        else:
            self.emit('and #$00FF')

    # --- casts

    def g_Cast(self, e, need):
        t = self.resolve(e.to_type)
        if t.kind == 'void':
            self.gen(e.expr, 16)
            return 16
        if not t.is_int:
            self.gen(e.expr, 32)
            return 32
        inner = self.vclass(e.expr)
        if t.size == 4:
            self.gen(e.expr, need)
            return 32 if need == 32 else 16
        self.gen(e.expr, 16)
        if t.size == 1:
            if not (inner.bits <= 8 and inner.signed == t.signed):
                if t.signed:
                    self.sign8()
                else:
                    self.emit('and #$00FF')
        return 16

    # --- memory

    def g_FuncCall(self, e, need):
        name = e.name.name if isinstance(e.name, c_ast.ID) else None
        args = e.args.exprs if e.args else []
        if name in ('rd8', 'rd16', 'rd32'):
            return self.gen_read(int(name[2:]), args[0], need)
        if name in ('wr8', 'wr16', 'wr32'):
            self.gen_write(int(name[2:]), args[0], args[1])
            return 16
        if name in ('sx8', 'sx16'):
            self.gen(args[0], 16)
            if name == 'sx8':
                self.sign8()
            return 16
        if name == 'get_sr':
            self.emit('jsl rt_get_sr')
            return 16
        if name == 'set_sr':
            self.gen(args[0], 16)
            self.emit('jsl rt_set_sr')
            return 16
        if name == 'm68k_idle':
            self.emit('jsl rt_idle')
            return 16
        if name == 'm68k_fault':
            self.emit('jsl rt_fault')
            return 16
        return self.gen_call(e, need)

    def addr_const(self, a):
        return self.const_eval(a)

    def addr_pattern(self, a):
        """A cheaper form of an address: ('ram', base, e) for a RAM constant
        plus a 16-bit offset, ('rom', base, e) for a ROM table plus an
        unsigned 16-bit offset, ('var', var, c) for a 32-bit variable plus
        a small constant; or None."""
        parts = []              # (sign, term)

        def flat(x, sign):
            if isinstance(x, c_ast.BinaryOp) and x.op in ('+', '-'):
                flat(x.left, sign)
                flat(x.right, sign if x.op == '+' else -sign)
            elif isinstance(x, c_ast.Cast) and self.resolve(x.to_type).is_int \
                    and self.resolve(x.to_type).size == 4:
                flat(x.expr, sign)
            else:
                parts.append((sign, x))
        flat(a, 1)
        consts = [(sg, x) for sg, x in parts if self.const_eval(x) is not None]
        rest = [(sg, x) for sg, x in parts if self.const_eval(x) is None]
        base = sum(sg * self.const_eval(x) for sg, x in consts) if consts else 0
        if len(rest) == 1:
            sign, e = rest[0]
            cls = self.vclass(e)
            # RAM: accesses stay in its 64 KB, so 16 bits of offset do
            if 0xFF0000 <= base <= 0xFFFFFF:
                return ('ram', base, e if sign > 0 else c_ast.UnaryOp('-', e))
            if sign < 0:
                return None
            if 0 <= base < 0x400000 and cls.fits_u16() and consts:
                return ('rom', base, e)
            if isinstance(e, c_ast.ID) and -0x80 <= base <= 0x7F:
                var = self.lookup(e.name)
                if var is not None and var.type.is_int and var.type.size == 4:
                    return ('var', var, base)
        return None

    def var_word(self, var, hi):
        """The operand of a 32-bit variable's low (or high) word."""
        if var.where == 'stack':
            return self.sref(var, 2 if hi else 0)
        return 'f:%s%s' % (var.symbol, ' + 2' if hi else '')

    def gen_read(self, size, a, need):
        addr = self.addr_const(a)
        if addr is not None:
            addr &= 0xFFFFFF
            if addr >= 0xFF0000 or (0xE00000 <= addr):
                op = 'a:$%04X' % (addr & 0xFFFF)
                return self.read_at(size, op, op_hi='a:$%04X' % ((addr + 2) & 0xFFFF), need=need)
            if addr < 0x400000:
                op = 'f:MDROM + $%06X' % addr
                return self.read_at(size, op, op_hi='f:MDROM + $%06X' % (addr + 2), need=need)
            # hardware
            self.emit('lda #$%04X' % (addr & 0xFFFF))
            self.emit('ldx #$%04X' % (addr >> 16))
            self.emit('stx CHI')
            self.emit('jsl rt_read%d' % size)
            return 32 if size == 32 and need == 32 else 16
        pat = self.addr_pattern(a)
        if pat is not None and pat[0] == 'ram':
            self.gen(pat[2], 16)
            if pat[1] & 0xFFFF:
                self.emit('clc')
                self.emit('adc #$%04X' % (pat[1] & 0xFFFF))
            self.emit('tax')
            return self.read_at(size, 'a:0, x', 'a:2, x', need)
        if pat is not None and pat[0] == 'rom':
            self.gen(pat[2], 16)
            self.emit('tax')
            return self.read_at(size, 'f:MDROM + $%06X, x' % pat[1],
                                'f:MDROM + $%06X, x' % (pat[1] + 2), need)
        if pat is not None and pat[0] == 'var':
            var, c = pat[1], pat[2]
            done, slow, rom = self.new_label(), self.new_label(), self.new_label()
            self.emit('lda %s' % self.var_word(var, True))
            self.emit('cmp #$00FF')
            self.emit('bne %s' % rom)
            self.emit('lda %s' % self.var_word(var, False))
            self.emit('tax')
            self.read_at(size, 'a:$%04X, x' % (c & 0xFFFF), 'a:$%04X, x' % ((c + 2) & 0xFFFF), need)
            self.emit('bra %s' % done)
            self.label(rom)             # ROM: through CPTR
            self.emit('cmp #$0040')
            self.emit('bcs %s' % slow)
            self.emit('adc #^MDROM')
            self.emit('sta CPTR + 2')
            self.emit('lda %s' % self.var_word(var, False))
            self.emit('clc')
            self.emit('adc #$%04X' % (c & 0xFFFF))
            self.emit('sta CPTR')
            if c < 0 or c > 0:
                lab = self.new_label()
                if c >= 0:
                    self.emit('bcc %s' % lab)
                    self.emit('inc CPTR + 2')
                else:
                    self.emit('bcs %s' % lab)
                    self.emit('dec CPTR + 2')
                self.label(lab)
            if size == 8:
                self.emit('lda [CPTR]')
                self.emit('and #$00FF')
            elif size == 16:
                self.emit('lda [CPTR]')
                self.emit('xba')
            else:
                if need == 32:
                    self.emit('lda [CPTR]')
                    self.emit('xba')
                    self.emit('sta CHI')
                self.emit('ldy #2')
                self.emit('lda [CPTR], y')
                self.emit('xba')
            self.emit('bra %s' % done)
            self.label(slow)
            self.gen(a, 32)
            self.emit('jsl rt_read%d' % size)
            self.label(done)
            return 32 if size == 32 and need == 32 else 16
        # computed: RAM fast path, else the runtime
        self.gen(a, 32)
        done, slow = self.new_label(), self.new_label()
        self.emit('ldx CHI')
        self.emit('cpx #$00FF')
        self.emit('bne %s' % slow)
        self.emit('tax')
        if size == 8:
            self.emit('lda a:0, x')
            self.emit('and #$00FF')
        elif size == 16:
            self.emit('lda a:0, x')
            self.emit('xba')
        else:
            self.emit('lda a:0, x')
            self.emit('xba')
            self.emit('sta CHI')
            self.emit('lda a:2, x')
            self.emit('xba')
        self.emit('bra %s' % done)
        self.label(slow)
        self.emit('jsl rt_read%d' % size)
        self.label(done)
        return 32 if size == 32 and need == 32 else 16

    def read_at(self, size, op, op_hi, need):
        if size == 8:
            self.emit('lda %s' % op)
            self.emit('and #$00FF')
            return 16
        if size == 16:
            self.emit('lda %s' % op)
            self.emit('xba')
            return 16
        if need == 32:
            self.emit('lda %s' % op)
            self.emit('xba')
            self.emit('sta CHI')
        self.emit('lda %s' % op_hi)
        self.emit('xba')
        return 32 if need == 32 else 16

    def gen_write(self, size, a, v):
        addr = self.addr_const(a)
        if addr is not None:
            addr &= 0xFFFFFF
            if addr >= 0xE00000:
                self.gen(v, 32 if size == 32 else 16)
                self.write_at(size, 'a:$%04X' % (addr & 0xFFFF), 'a:$%04X' % ((addr + 2) & 0xFFFF))
                return
            if addr in (0xC00000, 0xC00002) and size in (16, 32):
                self.gen(v, 32 if size == 32 else 16)
                self.emit('jsl rt_vdp_data%d' % size)
                return
            if addr in (0xC00004, 0xC00006) and size in (16, 32):
                self.gen(v, 32 if size == 32 else 16)
                self.emit('jsl rt_vdp_ctrl%d' % size)
                return
            # hardware (or ROM: ignored by the runtime)
            self.gen(v, 32 if size == 32 else 16)
            self.emit('ldx #$%04X' % (addr & 0xFFFF))
            self.emit('stx CT0')
            self.emit('ldx #$%04X' % (addr >> 16))
            self.emit('stx CT1')
            self.emit('jsl rt_write%d' % size)
            return
        pat = self.addr_pattern(a)
        if pat is not None and pat[0] == 'ram':
            self.gen(pat[2], 16)
            if pat[1] & 0xFFFF:
                self.emit('clc')
                self.emit('adc #$%04X' % (pat[1] & 0xFFFF))
            self.push16()
            self.gen(v, 32 if size == 32 else 16)
            self.emit('plx')
            self.depth -= 2
            self.write_at(size, 'a:0, x', 'a:2, x')
            return
        if pat is not None and pat[0] == 'var':
            var, c = pat[1], pat[2]
            self.gen(v, 32 if size == 32 else 16)
            lab_slow, lab_done = self.new_label(), self.new_label()
            self.push16()
            self.emit('lda %s' % self.var_word(var, True))
            self.emit('cmp #$00FF')
            self.emit('bne %s' % lab_slow)
            self.emit('lda %s' % self.var_word(var, False))
            self.emit('tax')
            self.emit('pla')
            self.depth -= 2
            self.write_at(size, 'a:$%04X, x' % (c & 0xFFFF), 'a:$%04X, x' % ((c + 2) & 0xFFFF))
            self.emit('bra %s' % lab_done)
            self.label(lab_slow)
            self.depth += 2             # (the value is still on the stack)
            self.emit('lda %s' % self.var_word(var, False))
            self.emit('clc')
            self.emit('adc #$%04X' % (c & 0xFFFF))
            self.emit('sta CT0')
            self.emit('lda %s' % self.var_word(var, True))
            self.emit('adc #$%04X' % (0xFFFF if c < 0 else 0))
            self.emit('sta CT1')
            self.emit('pla')
            self.depth -= 2
            self.emit('jsl rt_write%d' % size)
            self.label(lab_done)
            return
        # computed: the address (kept on the stack), the value, then RAM
        # directly or the runtime
        self.gen(a, 32)
        self.push32()
        self.gen(v, 32 if size == 32 else 16)
        lab_slow, lab_done = self.new_label(), self.new_label()
        self.emit('plx')
        self.emit('stx CT0')
        self.emit('plx')
        self.emit('stx CT1')
        self.depth -= 4
        self.emit('cpx #$00FF')
        self.emit('bne %s' % lab_slow)
        self.emit('ldx CT0')
        self.write_at(size, 'a:0, x', 'a:2, x')
        self.emit('bra %s' % lab_done)
        self.label(lab_slow)
        self.emit('jsl rt_write%d' % size)
        self.label(lab_done)

    def write_at(self, size, op, op_hi):
        if size == 8:
            self.emit('sep #$20')
            self.emit('sta %s' % op)
            self.emit('rep #$20')
        elif size == 16:
            self.emit('xba')
            self.emit('sta %s' % op)
        else:
            self.emit('xba')
            self.emit('sta %s' % op_hi)
            self.emit('lda CHI')
            self.emit('xba')
            self.emit('sta %s' % op)

    # --- calls

    def ref_func(self, f):
        if f.name not in self.defined:
            self.extern.add(f.symbol)

    def gen_call(self, e, need):
        args = e.args.exprs if e.args else []
        name = e.name.name if isinstance(e.name, c_ast.ID) else None
        var = self.lookup(name) if name else None
        if name and var is None and name in self.funcs:
            f = self.funcs[name]
            ft = f.type
            target = None
        else:
            ft = self.type_of(e.name)
            if ft.kind == 'ptr':
                ft = ft.target
            target = e.name
            f = None
        params = ft.params
        if len(params) != len(args):
            raise CompileError('call to %s: %d arguments for %d' % (name, len(args), len(params)))
        total = 0
        for (pname, pt), arg in zip(params, args):
            if pt.slot() == 4:
                self.gen(arg, 32)
                self.push32()
                total += 4
            else:
                self.gen(arg, 16)
                if pt.is_int and pt.size == 1:
                    if pt.signed:
                        self.sign8()
                    else:
                        self.emit('and #$00FF')
                self.push16()
                total += 2
        if f is not None:
            self.ref_func(f)
            self.emit('jsl %s' % f.symbol)
        else:
            # through a pointer: a table entry or variable (24-bit address)
            self.gen(target, 32)
            self.emit('sta CPTR')
            self.emit('lda CHI')
            self.emit('sta CPTR + 2')
            self.emit('jsl rt_call_ptr')
        self.pop(total)
        r = ft.ret
        if r.kind == 'void':
            return 16
        if r.is_int and r.size == 4 or r.kind == 'ptr':
            return 32 if need == 32 else 16
        return 16

    # --- operators

    def simple(self, e):
        """An operand usable directly by adc/sbc/and/... in 16 bits, or None."""
        c = self.const_eval(e)
        if c is not None:
            return '#$%04X' % (c & 0xFFFF)
        if isinstance(e, c_ast.ID):
            var = self.lookup(e.name)
            if var and var.type.is_int and var.type.size >= 2:
                if var.where == 'stack':
                    return self.sref(var)
                return 'f:%s' % var.symbol
        return None

    def g_BinaryOp(self, e, need):
        op = e.op
        if op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
            t, f = self.new_label(), self.new_label()
            self.gen_cond(e, f)
            self.emit('lda #1')
            self.emit('bra %s' % t)
            self.label(f)
            self.emit('lda #0')
            self.label(t)
            return 16
        cls = self.vclass(e)
        t = self.type_of(e)
        if t.kind == 'ptr':
            return self.gen_ptr_arith(e)
        wide = need == 32 and not cls.narrow()
        if op in ('+', '-', '&', '|', '^'):
            return self.gen_bitarith(e, op, wide)
        if op == '*':
            return self.gen_mul(e, wide)
        if op == '<<':
            return self.gen_shl(e, wide)
        if op == '>>':
            return self.gen_shr(e, need)
        if op in ('/', '%'):
            return self.gen_div(e, op, need)
        raise CompileError('operator %s' % op)

    def gen_bitarith(self, e, op, wide):
        ins = {'+': 'adc', '-': 'sbc', '&': 'and', '|': 'ora', '^': 'eor'}[op]
        pre = {'+': 'clc', '-': 'sec'}.get(op)
        if not wide:
            r = self.simple(e.right)
            if r is not None:
                self.gen(e.left, 16)
                if pre:
                    self.emit(pre)
                self.emit('%s %s' % (ins, r))
                return 16
            self.gen(e.left, 16)
            self.push16()
            self.gen(e.right, 16)
            if op == '-':
                self.emit('sta CT0')
                self.emit('pla')
                self.depth -= 2
                self.emit('sec')
                self.emit('sbc CT0')
                return 16
            if pre:
                self.emit(pre)
            self.emit('%s 1, s' % ins)
            self.pop(2)
            return 16
        self.gen(e.left, 32)
        self.push32()
        self.gen(e.right, 32)
        if op == '-':
            self.emit('sta CT0')
            self.emit('lda CHI')
            self.emit('sta CT1')
            self.emit('pla')
            self.emit('sec')
            self.emit('sbc CT0')
            self.emit('sta CT0')
            self.emit('pla')
            self.depth -= 4
            self.emit('sbc CT1')
            self.emit('sta CHI')
            self.emit('lda CT0')
            return 32
        if pre:
            self.emit(pre)
        self.emit('%s 1, s' % ins)
        self.emit('sta CT0')
        self.emit('lda CHI')
        self.emit('%s 3, s' % ins)
        self.emit('sta CHI')
        self.emit('lda CT0')
        self.pop(4)
        return 32

    def gen_mul(self, e, wide):
        if self.const_eval(e.left) is not None and self.const_eval(e.right) is None:
            e = c_ast.BinaryOp('*', e.right, e.left)
        l, r = self.vclass(e.left), self.vclass(e.right)
        c = self.const_eval(e.right)
        if c is not None and c > 0 and c & (c - 1) == 0 and wide:
            self.gen(e.left, 32)
            for _ in range(c.bit_length() - 1):
                self.emit('asl')
                self.emit('rol CHI')
            return 32
        if c is not None and c > 0 and c & (c - 1) == 0 and not wide:
            self.gen(e.left, 16)
            for _ in range(c.bit_length() - 1):
                self.emit('asl')
            return 16
        if c is not None and 0 < c <= 64 and not wide:
            # small constants: shifts and adds
            self.gen(e.left, 16)
            self.emit('sta CT0')
            self.emit('lda #0')
            bit = 0
            first = True
            for k in range(c.bit_length() - 1, -1, -1):
                if not first:
                    self.emit('asl')
                if c >> k & 1:
                    self.emit('clc')
                    self.emit('adc CT0')
                first = False
            return 16
        if l.fits_s16() and r.fits_s16() or l.fits_u16() and r.fits_u16():
            # 16 x 16: the SA-1's multiplier (signed; unsigned corrected)
            self.gen(e.left, 16)
            self.push16()
            self.gen(e.right, 16)
            self.emit('tax')
            self.emit('pla')
            self.depth -= 2
            self.emit('jsl %s' % ('rt_muls16' if l.fits_s16() and r.fits_s16() else 'rt_mulu16'))
            return 32 if wide else 16
        self.gen(e.left, 32)
        self.push32()
        self.gen(e.right, 32)
        self.emit('jsl rt_mul32')       # (stack operand) * A:CHI -> A:CHI
        self.pop(4)
        return 32 if wide else 16

    def gen_shl(self, e, wide):
        c = self.const_eval(e.right)
        if not wide:
            self.gen(e.left, 16)
            if c is not None:
                if c >= 16:
                    self.emit('lda #0')
                for _ in range(min(c, 16) if c < 16 else 0):
                    self.emit('asl')
                return 16
            self.push16()
            self.gen(e.right, 16)
            self.emit('tax')
            self.emit('pla')
            self.depth -= 2
            self.emit('jsl rt_shl16')
            return 16
        self.gen(e.left, 32)
        if c is not None:
            if c >= 16:
                self.emit('sta CHI')
                self.emit('lda #0')
                c -= 16
            for _ in range(c):
                self.emit('asl')
                self.emit('rol CHI')
            return 32
        self.push32()
        self.gen(e.right, 16)
        self.emit('tax')
        self.emit('pla')
        self.emit('ply')
        self.depth -= 4
        self.emit('sty CHI')
        self.emit('jsl rt_shl32')
        return 32

    def gen_shr(self, e, need):
        l = self.vclass(e.left)
        t = promote(self.type_of(e.left))
        c = self.const_eval(e.right)
        if l.narrow():
            self.gen(e.left, 16)
            if c is not None:
                if l.signed:
                    for _ in range(min(c, 15)):
                        self.emit('cmp #$8000')
                        self.emit('ror')
                else:
                    if c >= 16:
                        self.emit('lda #0')
                    elif c >= 8:
                        self.emit('xba')
                        self.emit('and #$00FF')
                        for _ in range(c - 8):
                            self.emit('lsr')
                    else:
                        for _ in range(c):
                            self.emit('lsr')
                return 16
            self.push16()
            self.gen(e.right, 16)
            self.emit('tax')
            self.emit('pla')
            self.depth -= 2
            self.emit('jsl %s' % ('rt_asr16' if l.signed else 'rt_lsr16'))
            return 16
        self.gen(e.left, 32)
        if c is not None:
            if c >= 16:
                self.emit('lda CHI')
                if t.signed:
                    lab = self.new_label()
                    self.emit('ldx #0')
                    self.emit('cmp #$8000')
                    self.emit('bcc %s' % lab)
                    self.emit('dex')
                    self.label(lab)
                    self.emit('stx CHI')
                    for _ in range(c - 16):
                        self.emit('cmp #$8000')
                        self.emit('ror')
                else:
                    self.emit('stz CHI')
                    for _ in range(c - 16):
                        self.emit('lsr')
                return 32 if need == 32 else 16
            for _ in range(c):
                if t.signed:
                    self.emit('pha')
                    self.emit('lda CHI')
                    self.emit('cmp #$8000')
                    self.emit('ror')
                    self.emit('sta CHI')
                    self.emit('pla')
                    self.emit('ror')
                else:
                    self.emit('lsr CHI')
                    self.emit('ror')
            return 32 if need == 32 else 16
        self.push32()
        self.gen(e.right, 16)
        self.emit('tax')
        self.emit('pla')
        self.emit('ply')
        self.depth -= 4
        self.emit('sty CHI')
        self.emit('jsl %s' % ('rt_asr32' if t.signed else 'rt_lsr32'))
        return 32 if need == 32 else 16

    def gen_div(self, e, op, need):
        l, r = self.vclass(e.left), self.vclass(e.right)
        t = arith(self.type_of(e.left), self.type_of(e.right))
        c = self.const_eval(e.right)
        if l.fits_u16() and r.fits_u16():
            if c is not None and c > 0 and c & (c - 1) == 0:
                self.gen(e.left, 16)
                if op == '/':
                    for _ in range(c.bit_length() - 1):
                        self.emit('lsr')
                else:
                    self.emit('and #$%04X' % (c - 1))
                return 16
            self.gen(e.left, 16)
            self.push16()
            self.gen(e.right, 16)
            self.emit('tax')
            self.emit('pla')
            self.depth -= 2
            self.emit('jsl %s' % ('rt_divu16' if op == '/' else 'rt_modu16'))
            return 16
        self.gen(e.left, 32)
        self.push32()
        self.gen(e.right, 32)
        fn = {('/', True): 'rt_divs32', ('/', False): 'rt_divu32',
              ('%', True): 'rt_mods32', ('%', False): 'rt_modu32'}[(op, t.signed)]
        self.emit('jsl %s' % fn)        # (stack operand) op A:CHI
        self.pop(4)
        return 32 if need == 32 else 16

    def gen_ptr_arith(self, e):
        lt = self.type_of(e.left)
        size = lt.target.sizeof()
        self.gen(e.left, 32)
        self.push32()
        self.gen(e.right, 16)
        for _ in range(size.bit_length() - 1):
            self.emit('asl')
        if size & (size - 1):
            raise CompileError('pointer arithmetic on %d-byte elements' % size)
        if e.op == '+':
            self.emit('clc')
            self.emit('adc 1, s')
            self.emit('sta CT0')
            self.emit('lda #0')
            self.emit('adc 3, s')
        else:
            self.emit('sta CT0')
            self.emit('lda 1, s')
            self.emit('sec')
            self.emit('sbc CT0')
            self.emit('sta CT0')
            self.emit('lda 3, s')
            self.emit('sbc #0')
        self.emit('sta CHI')
        self.emit('lda CT0')
        self.pop(4)
        return 32

    def g_UnaryOp(self, e, need):
        op = e.op
        if op == '-':
            cls = self.vclass(e)
            if need == 32 and not cls.narrow():
                self.gen(e.expr, 32)
                self.emit('eor #$FFFF')
                self.emit('clc')
                self.emit('adc #1')
                self.emit('sta CT0')
                self.emit('lda CHI')
                self.emit('eor #$FFFF')
                self.emit('adc #0')
                self.emit('sta CHI')
                self.emit('lda CT0')
                return 32
            self.gen(e.expr, 16)
            self.emit('eor #$FFFF')
            self.emit('inc')
            return 16
        if op == '~':
            if need == 32:
                self.gen(e.expr, 32)
                self.emit('eor #$FFFF')
                self.emit('tax')
                self.emit('lda CHI')
                self.emit('eor #$FFFF')
                self.emit('sta CHI')
                self.emit('txa')
                return 32
            self.gen(e.expr, 16)
            self.emit('eor #$FFFF')
            return 16
        if op == '+':
            return self.gen_value(e.expr, need)
        if op == '!':
            t, f = self.new_label(), self.new_label()
            self.gen_cond(e.expr, t)       # true -> fall through to 0
            self.emit('lda #0')
            self.emit('bra %s' % f)
            self.label(t)
            self.emit('lda #1')
            self.label(f)
            return 16
        if op == 'sizeof':
            self.emit('lda #%d' % self.sizeof_expr(e.expr))
            return 16
        if op == '&':
            self.address_of(e.expr)
            return 32
        if op == '*':
            return self.gen_deref_read(e.expr, need)
        if op in ('++', '--', 'p++', 'p--'):
            return self.gen_incdec(e, need)
        raise CompileError('unary %s' % op)

    # --- lvalues

    def address_of(self, e):
        """A:CHI = the SNES address of an lvalue (stack bank 0, statics)."""
        if isinstance(e, c_ast.ID):
            var = self.lookup(e.name)
            if var is None:
                raise CompileError('address of %s' % e.name)
            if var.where == 'stack':
                self.emit('tsc')
                self.emit('clc')
                self.emit('adc #%d' % (var.offset + self.depth + 1))
                self.emit('stz CHI')
                return
            self.emit('lda #.loword(%s)' % var.symbol)
            self.emit('ldx #^%s' % var.symbol)
            self.emit('stx CHI')
            return
        if isinstance(e, c_ast.ArrayRef):
            bt = self.type_of(e.name)
            size = bt.target.sizeof()
            self.address_of_base(e.name)
            self.push32()
            self.gen(e.subscript, 16)
            self.scale(size)
            self.emit('clc')
            self.emit('adc 1, s')
            self.emit('sta CT0')
            self.emit('lda #0')
            self.emit('adc 3, s')
            self.emit('sta CHI')
            self.emit('lda CT0')
            self.pop(4)
            return
        if isinstance(e, c_ast.StructRef):
            st = self.type_of(e.name)
            if e.type == '->':
                self.gen(e.name, 32)
                st = st.target
            else:
                self.address_of(e.name)
            off = 0
            for fname, ftype in st.fields:
                if fname == e.field.name:
                    break
                off += ftype.sizeof()
            if off:
                self.emit('clc')
                self.emit('adc #%d' % off)
                lab = self.new_label()
                self.emit('bcc %s' % lab)
                self.emit('inc CHI')
                self.label(lab)
            return
        if isinstance(e, c_ast.UnaryOp) and e.op == '*':
            self.gen(e.expr, 32)
            return
        raise CompileError('address of %s' % type(e).__name__)

    def address_of_base(self, e):
        t = self.type_of(e)
        if t.kind == 'array':
            self.address_of(e)
        else:
            self.gen(e, 32)

    def scale(self, size):
        if size & (size - 1) == 0:
            for _ in range(size.bit_length() - 1):
                self.emit('asl')
            return
        self.emit('sta CT0')
        self.emit('lda #0')
        for k in range(size.bit_length() - 1, -1, -1):
            if k != size.bit_length() - 1:
                self.emit('asl')
            if size >> k & 1:
                self.emit('clc')
                self.emit('adc CT0')

    def load_ptr(self, t, need):
        """Read the value of type t at the SNES address in CPTR."""
        size = t.size if t.is_int else 4
        if size == 4:
            if need == 32:
                self.emit('ldy #2')
                self.emit('lda [CPTR], y')
                self.emit('sta CHI')
            self.emit('lda [CPTR]')
            return 32 if need == 32 else 16
        self.emit('lda [CPTR]')
        if size == 1:
            if t.signed:
                self.sign8()
            else:
                self.emit('and #$00FF')
        return 16

    def store_ptr(self, t):
        size = t.size if t.is_int else 4
        if size == 4:
            self.emit('sta [CPTR]')
            self.emit('ldy #2')
            self.emit('pha')
            self.emit('lda CHI')
            self.emit('sta [CPTR], y')
            self.emit('pla')
        elif size == 2:
            self.emit('sta [CPTR]')
        else:
            self.emit('sep #$20')
            self.emit('sta [CPTR]')
            self.emit('rep #$20')

    def set_cptr(self):
        self.emit('sta CPTR')
        self.emit('lda CHI')
        self.emit('sta CPTR + 2')

    def gen_deref_read(self, pe, need):
        t = self.type_of(pe).target
        self.gen(pe, 32)
        self.set_cptr()
        return self.load_ptr(t, need)

    def g_ArrayRef(self, e, need):
        t = self.type_of(e)
        if t.kind == 'array':
            self.address_of(e)
            return 32
        # constant tables indexed: long indexed by X
        base = e.name
        bt = self.type_of(base)
        if isinstance(base, c_ast.ID) and bt.kind == 'array':
            var = self.lookup(base.name)
            if var is not None and var.where == 'static':
                size = t.sizeof() if t.kind != 'struct' else t.sizeof()
                self.gen(e.subscript, 16)
                self.scale(size)
                self.emit('tax')
                return self.load_indexed(var.symbol, t, need)
        self.address_of(e)
        self.set_cptr()
        return self.load_ptr(t, need)

    def load_indexed(self, sym, t, need, off=0):
        size = t.size if t.is_int else 4
        if size == 4:
            if need == 32:
                self.emit('lda f:%s + %d, x' % (sym, off + 2))
                self.emit('sta CHI')
            self.emit('lda f:%s + %d, x' % (sym, off))
            return 32 if need == 32 else 16
        self.emit('lda f:%s + %d, x' % (sym, off))
        if size == 1:
            if t.signed:
                self.sign8()
            else:
                self.emit('and #$00FF')
        return 16

    def g_StructRef(self, e, need):
        t = self.type_of(e)
        # table[i].field
        if e.type == '.' and isinstance(e.name, c_ast.ArrayRef) and isinstance(e.name.name, c_ast.ID):
            var = self.lookup(e.name.name.name)
            st = self.type_of(e.name)
            if var is not None and var.where == 'static':
                off = 0
                for fname, ftype in st.fields:
                    if fname == e.field.name:
                        break
                    off += ftype.sizeof()
                self.gen(e.name.subscript, 16)
                self.scale(st.sizeof())
                self.emit('tax')
                return self.load_indexed(var.symbol, t, need, off)
        self.address_of(e)
        self.set_cptr()
        return self.load_ptr(t, need)

    def g_Assignment(self, e, need):
        op = e.op
        lv = e.lvalue
        lt = self.type_of(lv)
        if op == '=':
            rhs = e.rvalue
        else:
            rhs = c_ast.BinaryOp(op[:-1], lv, e.rvalue)
            if lt.is_int and lt.size < 4:
                rhs = c_ast.Cast(c_ast.Typename(None, [], None, self.typename_node(lt)), rhs)
        width = 32 if lt.slot() == 4 else 16
        if isinstance(lv, c_ast.ID):
            var = self.lookup(lv.name)
            self.gen(rhs, width)
            self.store_var(var, width)
            return width
        # through memory: the address first
        self.address_of(lv)
        self.push32()
        self.gen(rhs, width)
        self.emit('tax')
        self.emit('lda 1, s')
        self.emit('sta CPTR')
        self.emit('lda 3, s')
        self.emit('sta CPTR + 2')
        self.emit('txa')
        self.pop(4)
        self.store_ptr(lt)
        return width

    def typename_node(self, t):
        names = {(1, False): 'uint8_t', (1, True): 'int8_t', (2, False): 'uint16_t',
                 (2, True): 'int16_t', (4, False): 'uint32_t', (4, True): 'int32_t'}[(t.size, t.signed)]
        return c_ast.TypeDecl(None, [], None, c_ast.IdentifierType([names]))

    def gen_incdec(self, e, need):
        lv = e.expr
        t = self.type_of(lv)
        step = 1
        if t.kind == 'ptr':
            step = t.target.sizeof()
        sign = '+' if e.op in ('++', 'p++') else '-'
        post = e.op.startswith('p')
        one = c_ast.Constant('int', str(step))
        new = c_ast.Assignment(sign + '=', lv, one)
        if post:
            # the old value: read, then update
            width = 32 if t.slot() == 4 else 16
            self.gen(lv, width)
            if width == 32:
                self.push32()
            else:
                self.push16()
            self.g_Assignment(new, width)
            self.emit('pla')
            self.depth -= 2
            if width == 32:
                self.emit('ply')
                self.depth -= 2
                self.emit('sty CHI')
            return width
        return self.g_Assignment(new, need)

    def g_TernaryOp(self, e, need):
        f, end = self.new_label(), self.new_label()
        width = 32 if need == 32 and not self.vclass(e).narrow() else 16
        self.gen_cond(e.cond, f)
        self.gen(e.iftrue, width)
        self.emit('jmp %s' % end)
        self.label(f)
        self.gen(e.iffalse, width)
        self.label(end)
        return width

    def g_ExprList(self, e, need):
        for x in e.exprs[:-1]:
            self.gen(x, 16)
        return self.gen_value(e.exprs[-1], need) if self.const_eval(e.exprs[-1]) is None else self.gen(e.exprs[-1], need) and 16

    def g_Constant(self, e, need):
        if e.type == 'string':
            raise CompileError('string constants are not supported')
        raise CompileError('constant %s' % e.value)

    # ------------------------------------------------------------ conditions

    def gen_cond(self, e, false_label):
        """Jump to false_label if e is false (zero); fall through if true."""
        if isinstance(e, c_ast.BinaryOp):
            if e.op == '&&':
                self.gen_cond(e.left, false_label)
                self.gen_cond(e.right, false_label)
                return
            if e.op == '||':
                t = self.new_label()
                f2 = self.new_label()
                self.gen_cond(e.left, f2)
                self.emit('jmp %s' % t)
                self.label(f2)
                self.gen_cond(e.right, false_label)
                self.label(t)
                return
            if e.op in ('==', '!=', '<', '>', '<=', '>='):
                self.gen_compare(e, false_label)
                return
        if isinstance(e, c_ast.UnaryOp) and e.op == '!':
            t = self.new_label()
            self.gen_cond(e.expr, t)
            self.emit('jmp %s' % false_label)
            self.label(t)
            return
        c = self.const_eval(e)
        if c is not None:
            if not c:
                self.emit('jmp %s' % false_label)
            return
        cls = self.vclass(e)
        width = 16 if cls.narrow() else 32
        self.gen(e, width)
        lab = self.new_label()
        if width == 32:
            self.emit('ora CHI')
        else:
            self.emit('cmp #0')
        self.emit('bne %s' % lab)
        self.emit('jmp %s' % false_label)
        self.label(lab)

    def gen_compare(self, e, false_label):
        op = e.op
        l, r = e.left, e.right
        lc, rc = self.vclass(l), self.vclass(r)
        cl, cr = self.const_eval(l), self.const_eval(r)
        t = arith(self.type_of(l), self.type_of(r))
        # constants adopt the other side's class when they fit
        if cr is not None:
            rc = lc if self.const_fits(cr, lc) else const_class(cr)
        if cl is not None:
            lc = rc if self.const_fits(cl, rc) else const_class(cl)
        if op in ('==', '!='):
            if lc.narrow() and rc.narrow() and (lc.signed == rc.signed or
                                               (lc.fits_u16() and rc.fits_u16()) or
                                               (lc.fits_s16() and rc.fits_s16())):
                self.cmp16(l, r)
                ok = self.new_label()
                self.emit('%s %s' % ('beq' if op == '==' else 'bne', ok))
                self.emit('jmp %s' % false_label)
                self.label(ok)
                return
            self.cmp32_eq(l, r, op, false_label)
            return
        if lc.fits_u16() and rc.fits_u16():
            mode = 'u16'
        elif lc.fits_s16() and rc.fits_s16() and t.signed:
            mode = 's16'
        else:
            mode = 's32' if t.signed else 'u32'
        if mode in ('u16', 's16'):
            self.cmp16(l, r, signed=(mode == 's16'))
        else:
            self.cmp32(l, r, signed=(mode == 's32'))
        # flags: unsigned: carry = l >= r; signed: N (after fix-up) = l < r
        ok, no = self.new_label(), self.new_label()
        if mode in ('u16', 'u32'):
            if op == '<':
                self.emit('bcc %s' % ok)
            elif op == '>=':
                self.emit('bcs %s' % ok)
            elif op == '>':
                self.emit('beq %s' % no)
                self.emit('bcs %s' % ok)
            elif op == '<=':
                self.emit('bcc %s' % ok)
                self.emit('beq %s' % ok)
        else:
            # signed: N set if l < r; CT2 zero if l == r
            if op == '<':
                self.emit('bmi %s' % ok)
            elif op == '>=':
                self.emit('bpl %s' % ok)
            elif op == '>':
                self.emit('bmi %s' % no)
                self.emit('lda CT2')
                self.emit('bne %s' % ok)
            elif op == '<=':
                self.emit('bmi %s' % ok)
                self.emit('lda CT2')
                self.emit('beq %s' % ok)
        self.label(no)
        self.emit('jmp %s' % false_label)
        self.label(ok)

    def far(self, label):
        """A short branch target that jumps on to label (which may be far)."""
        lab = self.new_label()
        self.pending_far.append((lab, label))
        return lab

    def const_fits(self, c, cls):
        if cls.bits == 32:
            return True
        lo = -(1 << (cls.bits - 1)) if cls.signed else 0
        hi = (1 << (cls.bits - 1)) - 1 if cls.signed else (1 << cls.bits) - 1
        return lo <= c <= hi

    def cmp16(self, l, r, signed=False):
        """Compare l with r in 16 bits: unsigned leaves carry/zero as cmp;
        signed leaves N set if l < r and CT2 = l - r."""
        rs = self.simple(r)
        if rs is not None:
            self.gen(l, 16)
            if signed:
                lab = self.new_label()
                self.emit('sec')
                self.emit('sbc %s' % rs)
                self.emit('sta CT2')
                self.emit('bvc %s' % lab)
                self.emit('eor #$8000')
                self.label(lab)
                self.emit('ora #0')
            else:
                self.emit('cmp %s' % rs)
            return
        self.gen(l, 16)
        self.push16()
        self.gen(r, 16)
        self.emit('sta CT0')
        self.emit('pla')
        self.depth -= 2
        if signed:
            lab = self.new_label()
            self.emit('sec')
            self.emit('sbc CT0')
            self.emit('sta CT2')
            self.emit('bvc %s' % lab)
            self.emit('eor #$8000')
            self.label(lab)
            self.emit('ora #0')
        else:
            self.emit('cmp CT0')

    def cmp32(self, l, r, signed):
        """l - r in 32 bits: unsigned leaves carry (l >= r) and zero (l == r);
        signed leaves N set if l < r and CT2 nonzero if l != r."""
        self.gen(l, 32)
        self.push32()
        self.gen(r, 32)
        self.emit('sta CT0')
        self.emit('lda CHI')
        self.emit('sta CT1')
        self.emit('pla')
        self.emit('sec')
        self.emit('sbc CT0')
        self.emit('sta CT2')
        self.emit('pla')
        self.depth -= 4
        self.emit('sbc CT1')
        if signed:
            lab = self.new_label()
            self.emit('bvc %s' % lab)
            self.emit('eor #$8000')
            self.label(lab)
            self.emit('php')
            self.emit('ora CT2')       # (zero test of the whole difference)
            self.emit('sta CT2')
            self.emit('plp')
        else:
            lab = self.new_label()
            self.emit('php')
            self.emit('ora CT2')
            self.emit('sta CT2')
            self.emit('bne %s' % lab)
            self.emit('plp')
            self.emit('sep #$02')      # zero
            self.emit('bra %s' % (lab + 'e'))
            self.label(lab)
            self.emit('plp')
            self.emit('rep #$02')      # not zero
            self.label(lab + 'e')

    def cmp32_eq(self, l, r, op, false_label):
        self.gen(l, 32)
        self.push32()
        self.gen(r, 32)
        ok = self.new_label()
        self.emit('eor 1, s')
        self.emit('sta CT0')
        self.emit('lda CHI')
        self.emit('eor 3, s')
        self.emit('ora CT0')
        self.pop(4)
        self.emit('tax')            # (flags of the or)
        self.emit('%s %s' % ('beq' if op == '==' else 'bne', ok))
        self.emit('jmp %s' % false_label)
        self.label(ok)

    # ------------------------------------------------------------ statements

    def stmt(self, s):
        m = getattr(self, 's_' + type(s).__name__, None)
        if m is None:
            # an expression statement
            self.gen(s, 16)
            return
        m(s)

    def s_Compound(self, s):
        self.scopes.append({})
        for item in s.block_items or []:
            self.stmt(item)
        self.scopes.pop()

    def s_Decl(self, s):
        t = self.resolve(s.type)
        if 'static' in s.storage or 'extern' in s.storage:
            sym = '%s__%s__%s' % (self.base, self.fn.name, s.name)
            self.declare_static(s, t, sym)
            self.scopes[-1][s.name] = Var(s.name, t, 'static', symbol=sym)
            return
        if t.kind in ('array', 'struct'):
            raise CompileError('local arrays are not supported (%s in %s)' % (s.name, self.fn.name))
        if t.is_int and t.size == 4 and t.signed and s.name in self.narrow_ints:
            t = T('int', 2, True)       # a counter that stays in 16 bits
        var = self.alloc_local(s.name, t)
        self.scopes[-1][s.name] = var
        if s.init is not None:
            self.gen(s.init, 32 if t.slot() == 4 else 16)
            self.store_var(var, 0)

    def alloc_local(self, name, t):
        size = t.slot()
        off = self.frame_used
        self.frame_used += size
        self.frame_max = max(self.frame_max, self.frame_used)
        return Var(name, t, 'stack', offset=off)

    def s_If(self, s):
        f = self.new_label()
        self.gen_cond(s.cond, f)
        self.stmt(s.iftrue)
        if s.iffalse is not None:
            end = self.new_label()
            self.emit('jmp %s' % end)
            self.label(f)
            self.stmt(s.iffalse)
            self.label(end)
        else:
            self.label(f)

    def s_While(self, s):
        top, end = self.new_label(), self.new_label()
        self.label(top)
        self.gen_cond(s.cond, end)
        self.loops.append((end, top))
        self.stmt(s.stmt)
        self.loops.pop()
        self.emit('jmp %s' % top)
        self.label(end)

    def s_DoWhile(self, s):
        top, cont, end = self.new_label(), self.new_label(), self.new_label()
        self.label(top)
        self.loops.append((end, cont))
        self.stmt(s.stmt)
        self.loops.pop()
        self.label(cont)
        self.gen_cond(s.cond, end)
        self.emit('jmp %s' % top)
        self.label(end)

    def s_For(self, s):
        self.scopes.append({})
        if s.init is not None:
            if isinstance(s.init, c_ast.DeclList):
                for d in s.init.decls:
                    self.s_Decl(d)
            else:
                self.stmt(s.init)
        top, cont, end = self.new_label(), self.new_label(), self.new_label()
        self.label(top)
        if s.cond is not None:
            self.gen_cond(s.cond, end)
        self.loops.append((end, cont))
        self.stmt(s.stmt)
        self.loops.pop()
        self.label(cont)
        if s.next is not None:
            self.stmt(s.next)
        self.emit('jmp %s' % top)
        self.label(end)
        self.scopes.pop()

    def s_Break(self, s):
        self.emit('jmp %s' % self.loops[-1][0])

    def s_Continue(self, s):
        for brk, cont in reversed(self.loops):
            if cont is not None:
                self.emit('jmp %s' % cont)
                return
        raise CompileError('continue outside a loop')

    def s_Return(self, s):
        if s.expr is not None:
            r = self.fn_type.ret
            width = 32 if r.slot() == 4 else 16
            self.gen(s.expr, width)
            if r.is_int and r.size == 1:
                if r.signed:
                    self.sign8()
                else:
                    self.emit('and #$00FF')
            elif r.is_int and r.size == 2 and width == 16:
                pass
        self.emit('jmp @return')

    def s_Label(self, s):
        self.label('@%s' % s.name)
        if s.stmt is not None:
            self.stmt(s.stmt)

    def s_Goto(self, s):
        self.emit('jmp @%s' % s.name)

    def s_EmptyStatement(self, s):
        pass

    def s_Switch(self, s):
        cls = self.vclass(s.cond)
        width = 16 if cls.narrow() else 32
        if width == 32:
            raise CompileError('switch on a 32-bit value')
        self.gen(s.cond, 16)
        tmp = self.alloc_local('@switch', INT if False else T('int', 2, False))
        self.store_var(tmp, 16)
        end = self.new_label()
        cases = []
        default = None
        body = s.stmt.block_items or []
        for item in body:
            if isinstance(item, c_ast.Case):
                cases.append((self.const_eval(item.expr), self.new_label(), item))
            elif isinstance(item, c_ast.Default):
                default = (self.new_label(), item)
        for val, lab, item in cases:
            nxt = self.new_label()
            self.emit('lda %s' % self.sref(tmp))
            self.emit('cmp #$%04X' % (val & 0xFFFF))
            self.emit('bne %s' % nxt)
            self.emit('jmp %s' % lab)
            self.label(nxt)
        self.emit('jmp %s' % (default[0] if default else end))
        self.loops.append((end, None))
        for item in body:
            if isinstance(item, c_ast.Case):
                lab = [c[1] for c in cases if c[2] is item][0]
                self.label(lab)
                for st in item.stmts or []:
                    self.stmt(st)
            elif isinstance(item, c_ast.Default):
                self.label(default[0])
                for st in item.stmts or []:
                    self.stmt(st)
            else:
                self.stmt(item)
        self.loops.pop()
        self.label(end)

    # ------------------------------------------------------------ statics

    def declare_static(self, d, t, sym):
        if sym in self.declared:
            return
        self.declared.add(sym)
        if t.kind == 'array' and t.n == 0 and isinstance(d.init, c_ast.InitList):
            n = k = 0
            for item in d.init.exprs:     # unsized: as many as initialised
                if isinstance(item, c_ast.NamedInitializer):
                    k = self.const_eval(item.name[0])
                k += 1
                n = max(n, k)
            t.n = n
        const = 'const' in (d.quals or []) or self.is_const_type(d.type)
        if d.init is not None and const:
            self.data.append('%s:' % sym)
            self.emit_data(t, d.init)
        else:
            self.bss.append('%s: .res %d' % (sym, max(t.sizeof(), 2)))
            if d.init is not None:
                v = self.const_eval(d.init)
                if v is None:
                    raise CompileError('initialiser of %s' % sym)
                self.init.append((sym, t.sizeof(), v))

    def is_const_type(self, node):
        while node is not None:
            if hasattr(node, 'quals') and node.quals and 'const' in node.quals:
                return True
            node = getattr(node, 'type', None)
            if isinstance(node, (c_ast.IdentifierType, c_ast.Struct)):
                return False
        return False

    def emit_data(self, t, init):
        if t.kind == 'array':
            items = {}
            k = 0
            for item in (init.exprs if isinstance(init, c_ast.InitList) else []):
                if isinstance(item, c_ast.NamedInitializer):
                    k = self.const_eval(item.name[0])
                    item = item.expr
                items[k] = item
                k += 1
            for k in range(t.n):
                if k in items:
                    self.emit_data(t.target, items[k])
                else:
                    self.data.append('    .res %d, 0' % t.target.sizeof())
            return
        if t.kind == 'struct':
            items = init.exprs
            for (fname, ft), item in zip(t.fields, items):
                self.emit_data(ft, item)
            return
        if t.kind in ('ptr', 'func') or (t.kind == 'int' and t.size == 4 and isinstance(init, c_ast.ID)):
            if isinstance(init, c_ast.ID) and init.name in self.funcs:
                f = self.funcs[init.name]
                self.ref_func(f)
                self.data.append('    .faraddr %s' % f.symbol)
                self.data.append('    .byte 0')
                return
            v = self.const_eval(init)
            self.data.append('    .dword $%08X' % (v & 0xFFFFFFFF))
            return
        if isinstance(init, c_ast.ID) and init.name in self.funcs:
            f = self.funcs[init.name]
            self.ref_func(f)
            self.data.append('    .faraddr %s' % f.symbol)
            self.data.append('    .byte 0')
            return
        v = self.const_eval(init)
        if v is None:
            raise CompileError('table value')
        directive = {1: '.byte', 2: '.word', 4: '.dword'}[t.size]
        mask = (1 << (8 * t.size)) - 1
        self.data.append('    %s $%0*X' % (directive, 2 * t.size, v & mask))

    # ------------------------------------------------------------ functions

    def function(self, fd):
        name = fd.decl.name
        ft = self.resolve(fd.decl.type)
        f = self.funcs[name]
        self.fn = f
        self.fn_type = ft
        self.depth = 0
        self.frame_used = 0
        self.frame_max = 0
        self.loops = []
        self.pending_far = []
        self.scopes = [{}]
        # parameters: above the locals and the return address
        params = ft.params
        body_out = []
        saved_out = self.out
        self.out = body_out
        # parameter offsets are fixed after the frame size is known: use
        # placeholders resolved below
        pvars = []
        for pname, pt in params:
            v = Var(pname, pt, 'stack', offset=0)
            pvars.append(v)
            self.scopes[0][pname] = v
        self.param_vars = pvars
        self.narrow_ints = self.find_narrow_ints(fd.body)
        self.stmt(fd.body)
        frame = self.frame_max
        # now fix the parameter offsets: frame + 3 (return) + the later ones
        self.out = saved_out
        offs = []
        acc = frame + 3
        for v in reversed(pvars):
            v.offset = acc
            acc += v.type.slot()
        # recompile with the right offsets (simplest: compile again)
        self.out = []
        self.label_n_saved = self.label_n
        self.depth = 0
        self.frame_used = 0
        self.frame_max = 0
        self.loops = []
        self.pending_far = []
        self.scopes = [{}]
        for v in pvars:
            self.scopes[0][v.name] = v
        self.frame_fixed = frame
        self.stmt(fd.body)
        body = self.out
        self.out = saved_out
        self.out.append('')
        self.out.append('.proc %s' % f.symbol)
        if self.trace:
            for line in ('php', 'rep #$30', 'pha', 'phx', 'lda f:trace_pos', 'tax',
                         'lda #.loword(%s)' % f.symbol, 'sta f:trace_ring, x', 'inx', 'inx',
                         'txa', 'and #$03FE', 'sta f:trace_pos', 'plx', 'pla', 'plp'):
                self.emit(line)
        if frame:
            self.emit('tsc')
            self.emit('sec')
            self.emit('sbc #%d' % frame)
            self.emit('tcs')
        self.out.extend(body)
        self.label('@return')
        if frame:
            self.emit('tay')
            self.emit('tsc')
            self.emit('clc')
            self.emit('adc #%d' % frame)
            self.emit('tcs')
            self.emit('tya')
        self.emit('rtl')
        for lab, target in self.pending_far:
            self.label(lab)
            self.emit('jmp %s' % target)
        self.out.append('.endproc')

    def find_narrow_ints(self, body):
        """int locals that are loop counters kept within 16 bits: every
        plain assignment is of a small value, every other change is ++,
        -- or += / -= a small constant, it is never address-taken, and a
        loop condition bounds it by a value below $7F00."""
        decls, bad, bounded = {}, set(), set()
        comp = self

        def small(e, names):
            c = comp.const_eval(e)
            if c is not None:
                return -0x7F00 <= c <= 0x7F00
            if isinstance(e, c_ast.ID) and e.name in names:
                return True
            try:
                v = comp.vclass_guess(e, names)
            except CompileError:
                return False
            return v

        class Visit(c_ast.NodeVisitor):
            def visit_Decl(self, n):
                if n.name and isinstance(n.type, c_ast.TypeDecl) and isinstance(n.type.type, c_ast.IdentifierType) \
                        and n.type.type.names in (['int'], ['int32_t']) and not n.storage:
                    decls[n.name] = n
                self.generic_visit(n)

        Visit().visit(body)
        names = set(decls)

        def bounds(cond):
            """Names the loop condition keeps within small values."""
            out = set()
            if isinstance(cond, c_ast.BinaryOp) and cond.op in ('<', '<=', '>', '>=', '!='):
                for var, other in ((cond.left, cond.right), (cond.right, cond.left)):
                    if isinstance(var, c_ast.ID) and var.name in names:
                        c = comp.const_eval(other)
                        if (c is not None and -0x7F00 <= c <= 0x7F00) or (c is None and small(other, names)):
                            out.add(var.name)
            elif isinstance(cond, c_ast.BinaryOp) and cond.op == '&&':
                out |= bounds(cond.left) | bounds(cond.right)
            return out

        loops = []              # names bounded by each enclosing loop

        class Uses(c_ast.NodeVisitor):
            def changed(self, name):
                if not any(name in b for b in loops):
                    bad.add(name)   # grows outside a loop that bounds it

            def visit_Assignment(self, n):
                if isinstance(n.lvalue, c_ast.ID) and n.lvalue.name in names:
                    if n.op == '=':
                        if not small(n.rvalue, names):
                            bad.add(n.lvalue.name)
                    elif n.op in ('+=', '-='):
                        c = comp.const_eval(n.rvalue)
                        if c is None or abs(c) > 0x100:
                            bad.add(n.lvalue.name)
                        self.changed(n.lvalue.name)
                    else:
                        bad.add(n.lvalue.name)
                self.generic_visit(n)

            def visit_UnaryOp(self, n):
                if n.op == '&' and isinstance(n.expr, c_ast.ID) and n.expr.name in names:
                    bad.add(n.expr.name)
                if n.op in ('++', '--', 'p++', 'p--') and isinstance(n.expr, c_ast.ID) \
                        and n.expr.name in names:
                    self.changed(n.expr.name)
                self.generic_visit(n)

            def loop(self, n, cond):
                loops.append(bounds(cond) if cond is not None else set())
                self.generic_visit(n)
                loops.pop()

            def visit_For(self, n):
                self.loop(n, n.cond)

            def visit_While(self, n):
                self.loop(n, n.cond)

            def visit_DoWhile(self, n):
                self.loop(n, n.cond)

        Uses().visit(body)
        for n, d in decls.items():
            if d.init is not None and not small(d.init, names):
                bad.add(n)
        return {n for n in names if n not in bad}

    def vclass_guess(self, e, names):
        """Whether e's value fits 16 signed bits, counting the names as
        such (before their slots exist)."""
        if isinstance(e, c_ast.ID) and e.name in names:
            return True
        if isinstance(e, c_ast.BinaryOp) and e.op in ('+', '-'):
            return False
        if isinstance(e, c_ast.BinaryOp) and e.op in ('&', '>>', '%', '/', '|', '^', '<', '>', '==', '!=', '<=', '>=', '&&', '||'):
            l = self.vclass_guess(e.left, names) if not isinstance(e.left, c_ast.Constant) else True
            r = self.vclass_guess(e.right, names) if not isinstance(e.right, c_ast.Constant) else True
            if e.op == '&' and (l or r):
                return True
            return l and r
        try:
            v = self.vclass(e)
        except CompileError:
            return False
        return v.fits_s16() or (v.bits <= 15)

    # ------------------------------------------------------------ file

    def compile(self):
        cpp = ['cpp', '-P', '-nostdinc', '-DSNES',
               '-I', os.path.join(HERE, 'include'), '-I', os.path.join(ROOT, 'src', 'game')]
        cpp += self.cpp_args + [self.path]
        text = subprocess.run(cpp, check=True, capture_output=True, text=True).stdout
        ast = c_parser.CParser().parse(text, self.path)
        fdefs = []
        for ext in ast.ext:
            if isinstance(ext, c_ast.Typedef):
                self.typedefs[ext.name] = self.resolve(ext.type)
            elif isinstance(ext, c_ast.Decl):
                if isinstance(ext.type, c_ast.FuncDecl):
                    self.declare_func(ext.name, ext, 'static' in ext.storage)
                elif isinstance(ext.type, c_ast.Struct) and ext.name is None:
                    self.resolve(ext.type)
                elif ext.name:
                    t = self.resolve(ext.type)
                    if 'extern' in ext.storage:
                        sym = ext.name
                        self.extern.add(sym)
                    else:
                        sym = '%s__%s' % (self.base, ext.name) if 'static' in ext.storage else ext.name
                        if 'static' not in ext.storage:
                            self.export.add(sym)
                        self.declare_static(ext, t, sym)
                    self.globals[ext.name] = Var(ext.name, t, 'static', symbol=sym)
            elif isinstance(ext, c_ast.FuncDef):
                self.declare_func(ext.decl.name, ext.decl, 'static' in ext.decl.storage)
                fdefs.append(ext)
        bodies = {fd.decl.name: fd for fd in fdefs}
        for fd in fdefs:
            name = fd.decl.name
            if (name.endswith('_68k') and 'static' in fd.decl.storage) or name in self.skip \
                    or self.uses_cpu(fd):
                continue
            self.defined.add(name)
        # functions calling a static one left out go too
        statics = {fd.decl.name for fd in fdefs if 'static' in fd.decl.storage}
        changed = True
        while changed:
            changed = False
            for name in list(self.defined):
                if any(ref in statics and ref not in self.defined and ref not in self.skip
                       for ref in self.calls(bodies[name])):
                    self.defined.discard(name)
                    changed = True
        # adapters the game code itself calls stay (their register
        # bookkeeping is a no-op here)
        changed = True
        while changed:
            changed = False
            for name in list(self.defined):
                for ref in self.calls(bodies[name]):
                    if ref in bodies and ref not in self.defined and ref not in self.skip \
                            and not self.uses_cpu(bodies[ref]):
                        self.defined.add(ref)
                        changed = True
        for fd in fdefs:
            name = fd.decl.name
            if name not in self.defined:
                continue
            f = self.funcs[name]
            self.export.add(f.symbol)       # (statics too: hand code may call them)
            try:
                self.function(fd)
            except CompileError as err:
                raise CompileError('%s: %s' % (name, err))

    def calls(self, node):
        found = []

        class Visit(c_ast.NodeVisitor):
            def visit_FuncCall(self, n):
                if isinstance(n.name, c_ast.ID):
                    found.append(n.name.name)
                self.generic_visit(n)
        Visit().visit(node)
        return found

    def uses_cpu(self, node):
        found = []

        class Visit(c_ast.NodeVisitor):
            def visit_ID(self, n):
                if n.name in ('cpu', 'f_add', 'f_cmp', 'pop16'):
                    found.append(n.name)
        Visit().visit(node)
        return bool(found)

    def declare_func(self, name, decl, static):
        if name in INTRINSIC_TYPES:
            return
        t = self.resolve(decl.type)
        if name in self.funcs:
            return
        sym = '%s__%s' % (self.base, name) if static else name
        self.funcs[name] = Function(name, t, sym)

    def write(self, outpath):
        lines = ['; Compiled from %s by tools/snes/c816.py' % os.path.relpath(self.path, ROOT),
                 '.p816', '.smart', '.include "inc/dp.inc"', '.include "inc/macros.inc"', '']
        exports = sorted(self.export)
        externs = sorted(s for s in self.extern if s not in self.export and
                         not any(s == f.symbol for f in self.funcs.values() if f.name in self.defined))
        for s in exports:
            lines.append('.export %s' % s)
        lines.append('.import rt_read8, rt_read16, rt_read32, rt_write8, rt_write16, rt_write32')
        lines.append('.import rt_mul32, rt_muls16, rt_mulu16, rt_shl16, rt_shl32, rt_lsr16, rt_asr16, rt_lsr32, rt_asr32')
        lines.append('.import rt_divu16, rt_modu16, rt_divs32, rt_divu32, rt_mods32, rt_modu32')
        lines.append('.import rt_get_sr, rt_set_sr, rt_idle, rt_fault, rt_call_ptr')
        lines.append('.import rt_vdp_data16, rt_vdp_data32, rt_vdp_ctrl16, rt_vdp_ctrl32')
        if self.trace:
            lines.append('.import trace_ring, trace_pos')
        for s in externs:
            lines.append('.import %s' % s)
        lines.append('')
        lines.append('.segment "%s"' % self.segment)
        lines.append('.a16')
        lines.append('.i16')
        lines.extend(self.out)
        if self.init:
            lines.append('')
            lines.append('.export %s__init' % self.base)
            lines.append('.proc %s__init' % self.base)
            for sym, size, v in self.init:
                lines.append('    lda #$%04X' % (v & 0xFFFF))
                lines.append('    sta f:%s' % sym)
                if size == 4:
                    lines.append('    lda #$%04X' % ((v >> 16) & 0xFFFF))
                    lines.append('    sta f:%s + 2' % sym)
            lines.append('    rtl')
            lines.append('.endproc')
        if self.data:
            lines.append('')
            lines.append('.segment "CRODATA"')
            lines.extend(self.data)
        if self.bss:
            lines.append('')
            lines.append('.segment "CBSS"')
            lines.extend(self.bss)
        with open(outpath, 'w') as f:
            f.write('\n'.join(lines) + '\n')


INTRINSIC_TYPES = {
    'rd8': T('int', 1, False), 'rd16': T('int', 2, False), 'rd32': UINT,
    'wr8': VOID, 'wr16': VOID, 'wr32': VOID,
    'sx8': UINT, 'sx16': UINT, 'get_sr': T('int', 2, False), 'set_sr': VOID,
    'm68k_idle': VOID, 'm68k_fault': VOID,
}


def main():
    args = sys.argv[1:]
    skip, cpp_args = [], []
    trace = False
    rest = []
    k = 0
    while k < len(args):
        if args[k] == '--skip':
            skip += [s for s in args[k + 1].split(',') if s]
            k += 2
        elif args[k] == '--trace':
            trace = True
            k += 1
        elif args[k] == '--cpp-arg':
            cpp_args.append(args[k + 1])
            k += 2
        else:
            rest.append(args[k])
            k += 1
    if len(rest) != 3:
        sys.exit(__doc__)
    out, segment, path = rest
    c = Compiler(path, segment, skip, cpp_args, trace)
    try:
        c.compile()
    except CompileError as err:
        sys.exit('%s: %s' % (path, err))
    c.write(out)


if __name__ == '__main__':
    main()
