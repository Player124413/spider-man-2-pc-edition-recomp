"""Минимальный kernel32 для эталона в unicorn: трамплины импортов исполняются на Python.

Нужен, чтобы оригинальный CRT (msvcr70) можно было инициализировать (DllMain) и гонять функции,
которые ходят в кучу, TLS и блокировки. Это не HLE рантайма (тот — на C), а подставка для тестов;
список функций — подсказка для Ф5.
"""
import struct

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

HEAP_BASE, HEAP_SIZE = 0x30000000, 0x400000
STR_BASE = HEAP_BASE  # первые 4 КБ кучи — строки окружения/командной строки
FAKE_HEAP = 0x00C00000
MODULE_PATH = b"C:\\Shrek2\\System\\game.exe"


class Kernel32:
    def __init__(self, emu, images):
        self.emu = emu
        self.uc = emu.uc
        self.hit = None  # имя нереализованного импорта, на котором остановились
        self.brk = HEAP_BASE + 0x1000
        self.tls = {}
        self.names = {}
        for img in images:
            for imp in img.imports:
                t, = struct.unpack("<I", self.uc.mem_read(imp.iat_va, 4))
                self.names[t] = imp.name
        self.cmdline_a, self.env_a, self.cmdline_w, self.env_w = STR_BASE, STR_BASE + 0x20, STR_BASE + 0x40, STR_BASE + 0x80
        self.uc.mem_write(self.cmdline_a, b"game.exe\0")
        self.uc.mem_write(self.env_a, b"\0\0")  # пустое окружение
        self.uc.mem_write(self.cmdline_w, "game.exe\0".encode("utf-16-le"))
        self.uc.mem_write(self.env_w, b"\0" * 4)
        self.uc.hook_add(UC_HOOK_CODE, self._on_code, begin=0x00100000, end=0x0010FFFF)

    # --- память
    def rd(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def wr(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def alloc(self, size, zero=True):
        size = max(size, 1)
        p = (self.brk + 8 + 15) & ~15
        if p + size > HEAP_BASE + HEAP_SIZE:
            return 0
        self.wr(p - 4, size)
        if zero:
            self.uc.mem_write(p, b"\0" * size)
        self.brk = p + size
        return p

    def realloc(self, p, size):
        q = self.alloc(size)
        if q and p:
            old = self.rd(p - 4)
            self.uc.mem_write(q, bytes(self.uc.mem_read(p, min(old, size))))
        return q

    # --- реализация: имя -> (число аргументов stdcall, функция(args) -> eax)
    def impl(self, name):
        k = self
        return {
            "GetVersion": (0, lambda a: 0x0A280105),  # Windows XP 5.1.2600
            "GetVersionExA": (1, lambda a: k._version(a[0])),
            "HeapCreate": (3, lambda a: FAKE_HEAP),
            "HeapDestroy": (1, lambda a: 1),
            "GetProcessHeap": (0, lambda a: FAKE_HEAP),
            "HeapAlloc": (3, lambda a: k.alloc(a[2])),
            "HeapReAlloc": (4, lambda a: k.realloc(a[2], a[3])),
            "HeapFree": (3, lambda a: 1),
            "HeapSize": (3, lambda a: k.rd(a[2] - 4)),
            "HeapValidate": (3, lambda a: 1),
            "TlsAlloc": (0, lambda a: 1 + len(k.tls)),
            "TlsSetValue": (2, lambda a: k.tls.__setitem__(a[0], a[1]) or 1),
            "TlsGetValue": (1, lambda a: k.tls.get(a[0], 0)),
            "TlsFree": (1, lambda a: 1),
            "GetCurrentThreadId": (0, lambda a: 0x104),
            "GetCurrentProcessId": (0, lambda a: 0x100),
            "GetCurrentThread": (0, lambda a: 0xFFFFFFFE),
            "GetCurrentProcess": (0, lambda a: 0xFFFFFFFF),
            "GetLastError": (0, lambda a: 0),
            "SetLastError": (1, lambda a: 0),
            "InitializeCriticalSection": (1, lambda a: 0),
            "InitializeCriticalSectionAndSpinCount": (2, lambda a: 1),
            "DeleteCriticalSection": (1, lambda a: 0),
            "EnterCriticalSection": (1, lambda a: 0),
            "LeaveCriticalSection": (1, lambda a: 0),
            "InterlockedIncrement": (1, lambda a: k._xadd(a[0], 1)),
            "InterlockedDecrement": (1, lambda a: k._xadd(a[0], -1)),
            "InterlockedExchange": (2, lambda a: k._xchg(a[0], a[1])),
            "InterlockedCompareExchange": (3, lambda a: k._cmpxchg(a[0], a[1], a[2])),
            "GetCommandLineA": (0, lambda a: k.cmdline_a),
            "GetCommandLineW": (0, lambda a: k.cmdline_w),
            "GetEnvironmentStrings": (0, lambda a: k.env_a),
            "GetEnvironmentStringsA": (0, lambda a: k.env_a),
            "GetEnvironmentStringsW": (0, lambda a: k.env_w),
            "FreeEnvironmentStringsA": (1, lambda a: 1),
            "FreeEnvironmentStringsW": (1, lambda a: 1),
            "GetEnvironmentVariableA": (3, lambda a: 0),
            "GetStartupInfoA": (1, lambda a: k._startup(a[0])),
            "GetStdHandle": (1, lambda a: {0xFFFFFFF6: 3, 0xFFFFFFF5: 7, 0xFFFFFFF4: 11}.get(a[0], 0xFFFFFFFF)),
            "GetFileType": (1, lambda a: 2),  # FILE_TYPE_CHAR
            "SetHandleCount": (1, lambda a: a[0]),
            "GetACP": (0, lambda a: 1252),
            "GetOEMCP": (0, lambda a: 437),
            "GetCPInfo": (2, lambda a: k._cpinfo(a[1])),
            "GetModuleHandleA": (1, lambda a: 0),
            "GetModuleHandleW": (1, lambda a: 0),
            "GetProcAddress": (2, lambda a: 0),
            "GetModuleFileNameA": (3, lambda a: k._modname(a[1], a[2])),
            "DisableThreadLibraryCalls": (1, lambda a: 1),
            "GetTickCount": (0, lambda a: 1000),
            "QueryPerformanceCounter": (1, lambda a: k._qpc(a[0])),
            "GetSystemTimeAsFileTime": (1, lambda a: k._qpc(a[0])),
            "VirtualAlloc": (4, lambda a: k.alloc(a[1]) if a[0] == 0 else a[0]),
            "VirtualFree": (3, lambda a: 1),
            "WideCharToMultiByte": (8, lambda a: k._w2a(a[2], a[3], a[4], a[5])),
            "MultiByteToWideChar": (6, lambda a: k._a2w(a[2], a[3], a[4], a[5])),
            "GetStringTypeW": (4, lambda a: k._ctype(self._str(a[1], a[2], True), a[3])),
            "GetStringTypeA": (5, lambda a: k._ctype(self._str(a[2], a[3], False), a[4])),
            "LCMapStringW": (6, lambda a: k._lcmap(a[1], self._str(a[2], a[3], True), a[4], a[5], True)),
            "LCMapStringA": (6, lambda a: k._lcmap(a[1], self._str(a[2], a[3], False), a[4], a[5], False)),
            "SetUnhandledExceptionFilter": (1, lambda a: 0),
            "IsBadReadPtr": (2, lambda a: 0),
            "IsBadWritePtr": (2, lambda a: 0),
            "IsBadCodePtr": (1, lambda a: 0),
            "LoadLibraryA": (1, lambda a: 0),
            "FreeLibrary": (1, lambda a: 1),
            "CloseHandle": (1, lambda a: 1),
            "SetConsoleCtrlHandler": (2, lambda a: 1),
            "GetTimeZoneInformation": (1, lambda a: k.uc.mem_write(a[0], b"\0" * 172) or 0),
            "GetUserDefaultLCID": (0, lambda a: 0x409),
            "IsValidCodePage": (1, lambda a: 1),
            "IsDebuggerPresent": (0, lambda a: 0),
            "IsProcessorFeaturePresent": (1, lambda a: 0),
            "OutputDebugStringA": (1, lambda a: 0),
            "Sleep": (1, lambda a: 0),
        }.get(name)

    def _ctype(self, chars, out):  # CT_CTYPE1 для latin-1
        res = []
        for c in chars:
            ch, f = chr(c), 0
            f |= 0x001 if ch.isupper() else 0
            f |= 0x002 if ch.islower() else 0
            f |= 0x004 if "0" <= ch <= "9" else 0
            f |= 0x008 if ch in " \t\n\v\f\r" else 0
            f |= 0x010 if 0x21 <= c < 0x7F and not ch.isalnum() else 0
            f |= 0x020 if c < 0x20 or c == 0x7F else 0
            f |= 0x040 if ch in " \t" else 0
            f |= 0x080 if ch in "0123456789abcdefABCDEF" else 0
            f |= 0x100 if ch.isalpha() else 0
            res.append(f)
        self.uc.mem_write(out, struct.pack(f"<{len(res)}H", *res))
        return 1

    def _lcmap(self, flags, chars, dst, cap, w):
        conv = {0x100: str.lower, 0x200: str.upper}.get(flags & 0x300, lambda x: x)
        res = [ord(conv(chr(c))) if len(conv(chr(c))) == 1 and ord(conv(chr(c))) < 256 else c for c in chars]
        if cap:
            res = res[:cap]
            self.uc.mem_write(dst, struct.pack(f"<{len(res)}H", *res) if w else bytes(res))
        return len(res)

    def _str(self, p, n, w):
        """Строка гостя: n символов или до нуля включительно (n = -1)."""
        out = []
        while n == 0xFFFFFFFF or len(out) < n:
            c = struct.unpack("<H", self.uc.mem_read(p + 2 * len(out), 2))[0] if w else self.uc.mem_read(p + len(out), 1)[0]
            out.append(c)
            if c == 0 and n == 0xFFFFFFFF:
                break
        return out

    def _w2a(self, src, n, dst, cap):  # кодовая страница — как latin-1, прочее -> '?'
        s = bytes(c if c < 256 else 0x3F for c in self._str(src, n, True))
        if cap:
            s = s[:cap]
            self.uc.mem_write(dst, s)
        return len(s)

    def _a2w(self, src, n, dst, cap):
        s = self._str(src, n, False)
        if cap:
            s = s[:cap]
            self.uc.mem_write(dst, struct.pack(f"<{len(s)}H", *s))
        return len(s)

    def _version(self, p):
        self.uc.mem_write(p + 4, struct.pack("<IIII", 5, 1, 2600, 2))
        return 1

    def _startup(self, p):
        self.uc.mem_write(p, struct.pack("<I", 68) + b"\0" * 64)
        return 0

    def _cpinfo(self, p):
        self.uc.mem_write(p, struct.pack("<I", 1) + b"?\0" + b"\0" * 12)
        return 1

    def _modname(self, buf, n):
        s = MODULE_PATH[:max(n - 1, 0)]
        self.uc.mem_write(buf, s + b"\0")
        return len(s)

    def _qpc(self, p):
        self.uc.mem_write(p, struct.pack("<Q", 123456789))
        return 1

    def _xadd(self, p, d):
        v = (self.rd(p) + d) & 0xFFFFFFFF
        self.wr(p, v)
        return v

    def _xchg(self, p, v):
        old = self.rd(p)
        self.wr(p, v)
        return old

    def _cmpxchg(self, p, v, cmp):
        old = self.rd(p)
        if old == cmp:
            self.wr(p, v)
        return old

    def _on_code(self, uc, addr, size, data):
        name = self.names.get(addr, f"thunk {addr:08x}")
        f = self.impl(name)
        if f is None:
            self.hit = f"HLE {name}"
            uc.emu_stop()
            return
        n, fn = f
        esp = uc.reg_read(UC_X86_REG_ESP)
        a = struct.unpack(f"<{n + 1}I", uc.mem_read(esp, 4 * (n + 1)))
        uc.reg_write(UC_X86_REG_EAX, (fn(list(a[1:])) or 0) & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 * (n + 1))
        uc.reg_write(UC_X86_REG_EIP, a[0])
