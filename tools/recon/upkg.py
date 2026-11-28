#!/usr/bin/env python3
"""Пакеты UE2 (.u, версия 129): экспорты и дизассемблер байткода UnrealScript.

  upkg.py exports <pkg.u> [подстрока]          — экспорты: индекс, класс, полное имя, смещение, размер
  upkg.py at <pkg.u> <смещение>                — экспорт, в данные которого попадает смещение в файле
  upkg.py dis <pkg.u> <Класс.Функция|индекс>   — байткод функции (или State) по токенам
  upkg.py grep <pkg.u> <подстрока>              — строки байткода всех функций с подстрокой

Запускать с python3 -I. Байткод — по токенам UE2 (UT2003/2004): нативные вызовы до EX_EndFunctionParms.
"""
import struct
import sys


class Pkg:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        tag, ver, lic, flags, nn, no, ne, eo, ni, io = struct.unpack_from('<IHHIIIIIII', self.b, 0)
        assert tag == 0x9E2A83C1, 'не пакет UE'
        self.ver = ver
        p = no
        self.names = []
        for _ in range(nn):
            n, p = self.ci(p)
            s = self.b[p:p + n - 1].decode('latin-1') if n > 0 else ''
            p += n + 4
            self.names.append(s)
        self.imports = []
        p = io
        for _ in range(ni):
            cpkg, p = self.ci(p)
            cname, p = self.ci(p)
            outer = struct.unpack_from('<i', self.b, p)[0]
            p += 4
            name, p = self.ci(p)
            self.imports.append((self.names[cname], outer, self.names[name]))
        self.exports = []
        p = eo
        for _ in range(ne):
            cls, p = self.ci(p)
            sup, p = self.ci(p)
            outer = struct.unpack_from('<i', self.b, p)[0]
            p += 4
            name, p = self.ci(p)
            oflags = struct.unpack_from('<I', self.b, p)[0]
            p += 4
            size, p = self.ci(p)
            off = 0
            if size > 0:
                off, p = self.ci(p)
            self.exports.append(dict(cls=cls, sup=sup, outer=outer, name=self.names[name], flags=oflags,
                                     size=size, off=off))

    def ci(self, p):
        """компактный индекс: знак и 6 бит, далее по 7 бит"""
        b0 = self.b[p]
        p += 1
        v = b0 & 0x3F
        if b0 & 0x40:
            sh = 6
            while True:
                bx = self.b[p]
                p += 1
                v |= (bx & 0x7F) << sh
                sh += 7
                if not bx & 0x80:
                    break
        return (-v if b0 & 0x80 else v), p

    def obj(self, i):
        """полное имя объекта по ссылке (>0 — экспорт i-1, <0 — импорт -i-1)"""
        if i == 0:
            return 'None'
        parts = []
        while i:
            if i > 0:
                e = self.exports[i - 1]
                parts.append(e['name'])
                i = e['outer']
            else:
                im = self.imports[-i - 1]
                parts.append(im[2])
                i = im[1]
        return '.'.join(reversed(parts))

    def cls_name(self, e):
        return self.obj(e['cls']) if e['cls'] else 'Class'


# токены выражений UE2
class Dis:
    def __init__(self, pkg, start, end):
        self.k, self.p, self.end = pkg, start, end
        self.base = start

    def u8(self):
        v = self.k.b[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.k.b, self.p)[0]
        self.p += 2
        return v

    def i32(self):
        v = struct.unpack_from('<i', self.k.b, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from('<f', self.k.b, self.p)[0]
        self.p += 4
        return v

    def ref(self):
        v, self.p = self.k.ci(self.p)
        return self.k.obj(v).split('.')[-1]

    def name(self):
        v, self.p = self.k.ci(self.p)
        return self.k.names[v]

    def args(self):
        out = []
        while self.k.b[self.p] != 0x16:
            out.append(self.expr())
        self.p += 1
        return '(' + ', '.join(out) + ')'

    def expr(self):
        t = self.u8()
        if t in (0x00, 0x01, 0x02):
            return ('', '', 'Default.')[t] + self.ref()
        if t == 0x04:
            return 'return ' + self.expr()
        if t == 0x05:
            self.u8()
            return 'switch (' + self.expr() + ')'
        if t == 0x06:
            return 'goto @%04x' % self.u16()
        if t == 0x07:
            j = self.u16()
            return 'if !(%s) goto @%04x' % (self.expr(), j)
        if t == 0x08:
            return 'stop'
        if t == 0x09:
            self.u16()
            return 'assert(' + self.expr() + ')'
        if t == 0x0A:
            j = self.u16()
            return 'default: (next @ffff)' if j == 0xFFFF else 'case %s: (next @%04x)' % (self.expr(), j)
        if t == 0x0B:
            return 'nothing'
        if t == 0x0C:
            out = []
            while True:
                n = self.name()
                off = self.i32()
                if n == 'None':
                    break
                out.append('%s=@%04x' % (n, off))
            return 'labels ' + ' '.join(out)
        if t == 0x0D:
            return 'goto ' + self.expr()
        if t == 0x0E:
            return 'eat ' + self.expr()
        if t in (0x0F, 0x14, 0x44):
            a = self.expr()
            return '%s = %s' % (a, self.expr())
        if t in (0x10, 0x1A):
            i = self.expr()
            return '%s[%s]' % (self.expr(), i)
        if t == 0x11:
            a = [self.expr() for _ in range(4)]
            return 'new(%s)' % ', '.join(a)
        if t in (0x12, 0x19):
            a = self.expr()
            self.u16()
            self.u8()
            return '%s%s.%s' % ('class:' if t == 0x12 else '', a, self.expr())
        if t in (0x13, 0x2E):
            c = self.ref()
            return '%s(%s)' % (c, self.expr())
        if t == 0x16:
            return ')'
        if t == 0x17:
            return 'self'
        if t == 0x18:
            self.u16()
            return self.expr()
        if t == 0x1B:
            n = self.name()
            return n + self.args()
        if t == 0x1C:
            n = self.ref()
            return n + self.args()
        if t == 0x1D:
            return str(self.i32())
        if t == 0x1E:
            return '%g' % self.f32()
        if t == 0x1F:
            e = self.k.b.index(b'\0', self.p)
            s = self.k.b[self.p:e].decode('latin-1')
            self.p = e + 1
            return '"%s"' % s
        if t == 0x20:
            v, self.p = self.k.ci(self.p)
            return "obj'%s'" % self.k.obj(v)
        if t == 0x21:
            return "'%s'" % self.name()
        if t == 0x22:
            return 'rot(%d,%d,%d)' % (self.i32(), self.i32(), self.i32())
        if t == 0x23:
            return 'vect(%g,%g,%g)' % (self.f32(), self.f32(), self.f32())
        if t == 0x24:
            return str(self.u8())
        if t == 0x25:
            return '0'
        if t == 0x26:
            return '1'
        if t == 0x27:
            return 'true'
        if t == 0x28:
            return 'false'
        if t == 0x29:
            return 'nativeparm ' + self.ref()
        if t == 0x2A:
            return 'none'
        if t == 0x2C:
            return str(self.u8())
        if t == 0x2D:
            return self.expr()
        if t == 0x2F:
            a = self.expr()
            j = self.u16()
            return 'foreach %s (end @%04x)' % (a, j)
        if t == 0x30:
            return 'iterpop'
        if t == 0x31:
            return 'iternext'
        if t in (0x32, 0x33):
            self.ref()
            a = self.expr()
            return '(%s %s %s)' % (a, '==' if t == 0x32 else '!=', self.expr())
        if t == 0x34:
            s = bytearray()
            while True:
                c = struct.unpack_from('<H', self.k.b, self.p)[0]
                self.p += 2
                if not c:
                    break
                s += chr(c).encode()
            return '"%s"' % s.decode()
        if t == 0x36:
            m = self.ref()
            return '%s.%s' % (self.expr(), m)
        if t == 0x37:
            return self.expr() + '.Length'
        if t == 0x38:
            return 'global.' + self.name() + self.args()
        if t == 0x39:
            c = self.u8()
            return 'cast%02x(%s)' % (c, self.expr())
        if t in (0x40, 0x41):
            a = self.expr()
            i = self.expr()
            n = self.expr()
            return '%s.%s(%s, %s)' % (a, 'Insert' if t == 0x40 else 'Remove', i, n)
        if t == 0x42:
            d = self.ref()
            n = self.name()
            return '%s<%s>%s' % (d, n, self.args())
        if t == 0x43:
            return 'delegate ' + self.name()
        if t >= 0x60:
            idx = ((t - 0x60) << 8 | self.u8()) if t < 0x70 else t
            return 'native%d%s' % (idx, self.args())
        raise ValueError('токен %02x на %x' % (t, self.p - 1))

    def lines(self):
        while self.p < self.end:
            at = self.p - self.base
            try:
                s = self.expr()
            except (ValueError, IndexError, struct.error) as e:
                yield at, '?? %s' % e
                return
            yield at, s
            if s.startswith('return ') and self.end - self.p < 8:
                return  # дальше — хвост UFunction (флаги, iNative)


def struct_script(k, e):
    """начало и конец байткода UFunction/UState: тег None, UField, UStruct (v129: с CppText)"""
    p = e['off']
    if e['flags'] & 0x02000000:  # RF_HasStack
        _, p = k.ci(p)
        _, p = k.ci(p)
        p += 8 + 4
        _, p = k.ci(p)
    _, p = k.ci(p)  # тег None
    for _ in range(2):  # SuperField, Next
        _, p = k.ci(p)
    for _ in range(3 + (k.ver >= 120)):  # ScriptText, Children, FriendlyName, CppText
        _, p = k.ci(p)
    line, pos, size = struct.unpack_from('<iii', k.b, p)
    p += 12
    return p, size


def find(k, spec):
    if spec.lstrip('-').isdigit():
        return int(spec)
    cls, _, fn = spec.rpartition('.')
    for i, e in enumerate(k.exports):
        full = k.obj(i + 1)
        if full.lower().endswith((cls + '.' + fn).lower()) and k.cls_name(e).split('.')[-1] in ('Function', 'State'):
            return i
    raise SystemExit('нет функции ' + spec)


def dis_script(k, i):
    e = k.exports[i]
    p, size = struct_script(k, e)
    # ScriptSize — размер в памяти (ссылки там 4 байта); в файле байткод короче — конец ищем по токену 0x53
    end = e['off'] + e['size']
    print('%s %s: data %x..%x, script %x, size(mem) %d' % (k.cls_name(e), k.obj(i + 1), e['off'], end, p, size))
    for at, s in Dis(k, p, end).lines():
        print('  @%04x %s' % (at, s))


def is_code(k, e):
    return k.cls_name(e).split('.')[-1] in ('Function', 'State') and e['size'] > 0


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, k = sys.argv[1], Pkg(sys.argv[2])
    if cmd == 'exports':
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for i, e in enumerate(k.exports):
            full = k.obj(i + 1)
            if sub in full.lower():
                print('%5d %-14s %-60s %8x %6d' % (i, k.cls_name(e), full, e['off'], e['size']))
    elif cmd == 'at':
        off = int(sys.argv[3], 0)
        for i, e in enumerate(k.exports):
            if e['off'] <= off < e['off'] + e['size']:
                print('%5d %-14s %s %x+%d' % (i, k.cls_name(e), k.obj(i + 1), e['off'], e['size']))
    elif cmd == 'grep':
        sub = sys.argv[3].lower()
        for i, e in enumerate(k.exports):
            if not is_code(k, e):
                continue
            p, _ = struct_script(k, e)
            for at, s in Dis(k, p, e['off'] + e['size']).lines():
                if sub in s.lower():
                    print('%s @%04x %s' % (k.obj(i + 1), at, s))
    elif cmd == 'dis':
        dis_script(k, find(k, sys.argv[3]))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
