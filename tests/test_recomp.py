"""Синтетические юнит-тесты рекомпилятора без файлов игры (python -I tests/test_recomp.py)."""
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

import discover  # noqa: E402
import emit  # noqa: E402
import flags  # noqa: E402
import gen  # noqa: E402
import image  # noqa: E402
import main as recomp_main  # noqa: E402
import overrides  # noqa: E402
import regs  # noqa: E402
import x86  # noqa: E402
import x87  # noqa: E402


def make_image(code, base=0x10100000, name="synth.dll", entry_rva=0x1000,
               exports=None, imports=None, relocs=None, extra_sections=None, path=""):
    """Собрать синтетический Image в памяти без PE на диске."""
    code_rva = 0x1000
    vsize = max(len(code), 1)
    size = ((code_rva + vsize + 0xFFF) & ~0xFFF)
    if extra_sections:
        for _, rva, raw, _ in extra_sections:
            size = max(size, ((rva + max(len(raw), 1) + 0xFFF) & ~0xFFF))
    data = bytearray(size)
    data[code_rva:code_rva + len(code)] = code
    sections = [image.Section(".text", base + code_rva, vsize, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE)]
    for sname, rva, raw, chars in (extra_sections or []):
        data[rva:rva + len(raw)] = raw
        sections.append(image.Section(sname, base + rva, len(raw), chars))
    exp = dict(exports or {})
    imps = list(imports or [])
    img = image.Image(
        name=name,
        path=path,
        base=base,
        preferred_base=base,
        size=size,
        entry=(base + entry_rva) if entry_rva else 0,
        data=data,
        sections=sections,
        exports=exp,
        export_ordinals={},
        forwarders={},
        imports=imps,
        relocs=set(relocs or ()),
        has_relocs=True,
        is_dll=name.lower().endswith(".dll"),
    )
    img.iat_by_va = {i.iat_va: i for i in imps}
    return img


def make_program(code, base=0x10100000, name="synth.dll", entry_rva=0x1000,
                 exports=None, imports=None, relocs=None, path=""):
    img = make_image(code, base=base, name=name, entry_rva=entry_rva,
                     exports=exports, imports=imports, relocs=relocs, path=path)
    prog = discover.Program({name.lower(): img})
    mod = discover.Module(img, prog)
    prog.modules[name.lower()] = mod
    return prog, mod


def make_minimal_pe(sections, image_base=0x10900000, entry_rva=0x1000,
                    machine=0x14C, timestamp=0x407C9FC8, is_dll=False):
    """Собрать валидный минимальный PE32-файл в байтах для тестирования main.py check."""
    file_align = 0x200
    sect_align = 0x1000
    nsect = len(sections)
    dos_hdr = bytearray(0x80)
    dos_hdr[0:2] = b"MZ"
    struct.pack_into("<I", dos_hdr, 0x3C, 0x80)

    # COFF Header (20 байт) + Optional Header PE32 (224 байт)
    chars = 0x0102 | (0x2000 if is_dll else 0)
    coff = struct.pack("<HHIIIHH", machine, nsect, timestamp, 0, 0, 0xE0, chars)

    raw_off = 0x200
    rva = sect_align
    sec_hdrs = bytearray()
    sec_blobs = bytearray()
    size_of_code = 0
    for sname, raw, schars in sections:
        vsz = max(len(raw), 1)
        rsz = ((len(raw) + file_align - 1) // file_align) * file_align if raw else 0
        padded = raw + b"\0" * (rsz - len(raw))
        if schars & (image.IMAGE_SCN_CNT_CODE | image.IMAGE_SCN_MEM_EXECUTE):
            size_of_code += rsz
        name_bytes = sname.encode("latin-1")[:8].ljust(8, b"\0")
        sec_hdrs += struct.pack("<8sIIIIIIHHI", name_bytes, vsz, rva, rsz,
                                raw_off if rsz else 0, 0, 0, 0, 0, schars)
        sec_blobs += padded
        raw_off += rsz
        rva += ((vsz + sect_align - 1) // sect_align) * sect_align

    size_of_image = rva
    opt = bytearray(0xE0)
    struct.pack_into("<HBB", opt, 0, 0x10B, 7, 0)  # PE32 magic
    struct.pack_into("<IIIII", opt, 4, size_of_code, 0, 0, entry_rva, sect_align)
    struct.pack_into("<III", opt, 24, sect_align, image_base, sect_align)
    struct.pack_into("<I", opt, 36, file_align)
    struct.pack_into("<HHHH", opt, 40, 4, 0, 0, 0)
    struct.pack_into("<HH", opt, 48, 4, 0)
    struct.pack_into("<II", opt, 56, size_of_image, 0x200)
    struct.pack_into("<HH", opt, 68, 3, 0)  # Subsystem CUI
    struct.pack_into("<IIII", opt, 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<II", opt, 88, 0, 16)  # NumberOfRvaAndSizes = 16

    headers = bytes(dos_hdr) + b"PE\0\0" + coff + bytes(opt) + bytes(sec_hdrs)
    assert len(headers) <= 0x200
    return headers.ljust(0x200, b"\0") + bytes(sec_blobs)


class TestX87Decode(unittest.TestCase):
    """Тесты декодирования опкодов x87 (x87.py) и анализа глубины стека."""

    def _dec(self, raw):
        ins = x86.decode(raw, 0x1000)
        self.assertIsNotNone(ins, f"не удалось декодировать {raw.hex()}")
        self.assertIsNotNone(ins.x87, f"не распознано как x87: {ins.text}")
        return x87.decode_x87(ins)

    def test_memory_loads_and_stores(self):
        cases = [
            ("d900", "ld_mem", "f32", 0, 1),    # fld dword ptr [eax]
            ("dd00", "ld_mem", "f64", 0, 1),    # fld qword ptr [eax]
            ("db28", "ld_mem", "f80", 0, 1),    # fld tbyte ptr [eax]
            ("db00", "ld_mem", "i32", 0, 1),    # fild dword ptr [eax]
            ("df00", "ld_mem", "i16", 0, 1),    # fild word ptr [eax]
            ("df28", "ld_mem", "i64", 0, 1),    # fild qword ptr [eax]
            ("d910", "st_mem", "f32", 1, 0),    # fst dword ptr [eax]
            ("d918", "st_mem", "f32", 1, -1),   # fstp dword ptr [eax]
            ("dd18", "st_mem", "f64", 1, -1),   # fstp qword ptr [eax]
            ("db18", "st_mem", "i32", 1, -1),   # fistp dword ptr [eax]
            ("db08", "st_mem", "i32t", 1, -1),  # fisttp dword ptr [eax]
            ("df38", "st_mem", "i64", 1, -1),   # fistp qword ptr [eax]
        ]
        for hx, kind, fmt, need, delta in cases:
            op = self._dec(bytes.fromhex(hx))
            self.assertEqual((op.kind, op.args[0], op.need, op.delta), (kind, fmt, need, delta), hx)

    def test_reversed_dc_de_sub_div_opcodes(self):
        # Критично для x87: DC/DE имеют «перевёрнутые» sub/subr и div/divr относительно D8.
        cases = [
            ("d8e1", "sub", 0, 1, False, 2, 0),    # fsub st(0), st(1)
            ("d8e9", "subr", 0, 1, False, 2, 0),   # fsubr st(0), st(1)
            ("dce1", "subr", 1, 0, False, 2, 0),   # fsubr st(1), st(0)
            ("dce9", "sub", 1, 0, False, 2, 0),    # fsub st(1), st(0)
            ("dee1", "subr", 1, 0, True, 2, -1),   # fsubrp st(1), st(0)
            ("dee9", "sub", 1, 0, True, 2, -1),    # fsubp st(1), st(0)
            ("dcf1", "divr", 1, 0, False, 2, 0),   # fdivr st(1), st(0)
            ("dcf9", "div", 1, 0, False, 2, 0),    # fdiv st(1), st(0)
            ("def1", "divr", 1, 0, True, 2, -1),   # fdivrp st(1), st(0)
            ("def9", "div", 1, 0, True, 2, -1),    # fdivp st(1), st(0)
        ]
        for hx, opn, dst, src, pop, need, delta in cases:
            op = self._dec(bytes.fromhex(hx))
            self.assertEqual(op.kind, "arith_st", hx)
            self.assertEqual(op.args, (opn, dst, src, pop), hx)
            self.assertEqual((op.need, op.delta), (need, delta), hx)

    def test_compare_consts_transcendentals_and_control(self):
        self.assertEqual(self._dec(bytes.fromhex("ded9")), x87.X87Op("cmp_st", (1, 2, False), 2, -2))  # fcompp
        self.assertEqual(self._dec(bytes.fromhex("dae9")), x87.X87Op("cmp_st", (1, 2, True), 2, -2))   # fucompp
        self.assertEqual(self._dec(bytes.fromhex("dbf1")), x87.X87Op("comi", (1, False), 2, 0))        # fcomi
        self.assertEqual(self._dec(bytes.fromhex("dfe9")), x87.X87Op("comi", (1, True), 2, -1))        # fucomip
        self.assertEqual(self._dec(bytes.fromhex("dfe0")), x87.X87Op("fnstsw_ax", (), 0, 0))           # fnstsw ax
        self.assertEqual(self._dec(bytes.fromhex("d9e8")), x87.X87Op("ld_const", ("1",), 0, 1))        # fld1
        self.assertEqual(self._dec(bytes.fromhex("d9ee")), x87.X87Op("ld_const", ("0",), 0, 1))        # fldz
        self.assertEqual(self._dec(bytes.fromhex("d9fa")), x87.X87Op("unary", ("sqrt",), 1, 0))        # fsqrt
        self.assertEqual(self._dec(bytes.fromhex("d9fb")), x87.X87Op("fsincos", (None,), 1, 1))        # fsincos
        self.assertEqual(self._dec(bytes.fromhex("d9f1")), x87.X87Op("binpop", ("fyl2x",), 2, -1))     # fyl2x
        self.assertEqual(self._dec(bytes.fromhex("d9ca")), x87.X87Op("fxch", (2,), 3, 0))              # fxch st(2)
        self.assertEqual(self._dec(bytes.fromhex("ddc0")), x87.X87Op("ffree", (0,), 0, 0))             # ffree st(0)
        self.assertEqual(self._dec(bytes.fromhex("dac1")), x87.X87Op("fcmov", ("b", 1), 2, 0))         # fcmovb

    def test_x87_stack_depth_analysis(self):
        # fld [eax]; fadd [ecx]; fstp [edx]; ret -> глубина на выходе 0, max_depth 1
        code = bytes.fromhex("d900 d801 d91a c3")
        prog, mod = make_program(code)
        mod.run()
        ctx = gen.Ctx(prog)
        fc = emit.FuncCompiler(mod, mod.functions[0x10101000], ctx)
        fc.compile()
        self.assertFalse(fc.x87_bad)
        self.assertEqual(fc.exit_depth, 0)
        self.assertEqual(fc.max_depth, 1)

        # Функция, возвращающая float: fld [eax]; ret -> exit_depth == 1
        prog2, mod2 = make_program(bytes.fromhex("d900 c3"))
        mod2.run()
        fc2 = emit.FuncCompiler(mod2, mod2.functions[0x10101000], gen.Ctx(prog2))
        fc2.compile()
        self.assertFalse(fc2.x87_bad)
        self.assertEqual(fc2.exit_depth, 1)

        # Серия ffree st(1); ffree st(0) очищает 2 живых значения на стеке
        prog3, mod3 = make_program(bytes.fromhex("d9e8 d9ee ddc1 ddc0 c3"))
        mod3.run()
        fc3 = emit.FuncCompiler(mod3, mod3.functions[0x10101000], gen.Ctx(prog3))
        fc3.compile()
        self.assertFalse(fc3.x87_bad)
        self.assertEqual(fc3.exit_depth, 0)

        # Underflow: faddp st(1), st(0) на пустом стеке -> x87_bad == True
        prog4, mod4 = make_program(bytes.fromhex("dec1 c3"))
        mod4.run()
        fc4 = emit.FuncCompiler(mod4, mod4.functions[0x10101000], gen.Ctx(prog4))
        fc4.compile()
        self.assertTrue(fc4.x87_bad)


class TestFlags(unittest.TestCase):
    """Тесты анализа живости и слияния флагов (flags.py)."""

    def _run_plan(self, code):
        prog, mod = make_program(code)
        mod.run()
        f = mod.functions[0x10101000]
        fc = emit.FuncCompiler(mod, f, gen.Ctx(prog))
        fc.analyze()
        return fc, f.insns

    def test_flag_info_basics(self):
        i_cmp = x86.decode(bytes.fromhex("39d8"), 0x1000)      # cmp eax, ebx
        self.assertEqual(flags.flag_info(i_cmp), (0, flags.ALL, "sub"))
        i_inc = x86.decode(bytes.fromhex("40"), 0x1000)        # inc eax (CF не трогает)
        self.assertEqual(flags.flag_info(i_inc), (0, flags.P | flags.A | flags.Z | flags.S | flags.O, "inc"))
        i_adc = x86.decode(bytes.fromhex("11d8"), 0x1000)      # adc eax, ebx (читает CF)
        self.assertEqual(flags.flag_info(i_adc), (flags.C, flags.ALL, "adc"))
        i_shl0 = x86.decode(bytes.fromhex("c1e000"), 0x1000)   # shl eax, 0
        self.assertEqual(flags.flag_info(i_shl0), (0, 0, None))
        i_shlcl = x86.decode(bytes.fromhex("d3e0"), 0x1000)    # shl eax, cl
        self.assertEqual(flags.flag_info(i_shlcl), (flags.ALL, flags.ALL, "shift"))

    def test_fuse_cmp_jl_without_materialization(self):
        # cmp eax, ebx; jl +2; xor eax, eax; ret
        fc, addrs = self._run_plan(bytes.fromhex("39d8 7c02 31c0 c3"))
        cmp_a, jl_a = addrs[0], addrs[1]
        self.assertEqual(fc.fused.get(jl_a), cmp_a)
        self.assertNotIn(cmp_a, fc.mat)
        self.assertIn(cmp_a, fc.fused_used)

    def test_materialize_cf_across_inc_and_for_adc(self):
        # add eax, ebx; adc ecx, edx; ret -> add обязан материализовать CF
        fc, addrs = self._run_plan(bytes.fromhex("01d8 11d1 c3"))
        self.assertEqual(fc.mat.get(addrs[0]), flags.C)
        self.assertEqual(fc.fused, {})

        # cmp eax, ebx; inc ecx; jb +2; xor eax, eax; ret
        # inc портит Z/S/O, но сохраняет CF -> cmp материализует CF, jb не сливается
        fc2, addrs2 = self._run_plan(bytes.fromhex("39d8 41 7202 31c0 c3"))
        self.assertEqual(fc2.mat.get(addrs2[0]), flags.C)
        self.assertNotIn(addrs2[2], fc2.fused)

    def test_merge_point_forces_materialization(self):
        # test eax, eax; jmp L; (ветка 2: test ecx, ecx) L: je exit; mov eax, 1; exit: ret
        # На стыке путей перед je слияние невозможно — оба сеттера материализуют ZF.
        code = bytes.fromhex(
            "85c0 "        # 1000: test eax, eax
            "7404 "        # 1002: je 1008 (ветка 1)
            "85c9 "        # 1004: test ecx, ecx
            "eb02 "        # 1006: jmp 100a
            "85d2 "        # 1008: test edx, edx
            "7405 "        # 100a: je 1011 (два предшественника: 1006 и 1008!)
            "b801000000 "  # 100c: mov eax, 1
            "c3"           # 1011: ret
        )
        fc, _ = self._run_plan(code)
        self.assertEqual(fc.mat.get(0x10101004), flags.Z)
        self.assertEqual(fc.mat.get(0x10101008), flags.Z)
        self.assertNotIn(0x1010100a, fc.fused)


class TestRegs(unittest.TestCase):
    """Тесты effects, сохранения callee-saved и __SEH_prolog/__SEH_epilog (regs.py)."""

    def test_effects(self):
        # nop-подобный lea eax, [eax] -> (0, 0)
        self.assertEqual(regs.effects(x86.decode(bytes.fromhex("8d00"), 0x1000)), (0, 0))
        # mov eax, [ebx + ecx*4 + 8] -> читает ebx, ecx; пишет eax
        self.assertEqual(
            regs.effects(x86.decode(bytes.fromhex("8b448b08"), 0x1000)),
            (regs.BIT["ebx"] | regs.BIT["ecx"], regs.EAX),
        )
        # rep movsd -> читает и пишет esi, edi, ecx
        si_di_cx = regs.BIT["esi"] | regs.BIT["edi"] | regs.ECX
        self.assertEqual(regs.effects(x86.decode(bytes.fromhex("f3a5"), 0x1000)), (si_di_cx, si_di_cx))
        # fnstsw ax -> читает и пишет eax
        self.assertEqual(regs.effects(x86.decode(bytes.fromhex("dfe0"), 0x1000)), (regs.EAX, regs.EAX))
        # rdtsc -> пишет eax, edx
        self.assertEqual(regs.effects(x86.decode(bytes.fromhex("0f31"), 0x1000)), (0, regs.EAX | regs.EDX))

    def test_merge_states(self):
        b = regs.BIT["ebx"]
        self.assertEqual(regs.merge((b, 0, 0), (b, 0, 0)), (b, 0, 0))    # E & E -> E
        self.assertEqual(regs.merge((0, b, 0), (0, 0, b)), (0, 0, b))    # Sc & Sd -> Sd
        self.assertEqual(regs.merge((b, 0, 0), (0, b, 0)), (0, 0, 0))    # E & Sc -> X

    def test_callee_saved_analysis(self):
        # 1) push ebx; push esi; mov ebx, eax; mov esi, ecx; add eax, ebx; pop esi; pop ebx; ret
        # ebx и esi сохранены и восстановлены в эпилоге -> не попадают в out.
        prog, mod = make_program(bytes.fromhex("53 56 89c3 89ce 01d8 5e 5b c3"))
        mod.run()
        ctx = gen.Ctx(prog)
        ra = regs.compute(prog, ctx, log=lambda *_: None)
        e = 0x10101000
        self.assertEqual(ra.preserved[e], regs.CALLEE_SAVED)
        self.assertEqual(ctx.reg_out[e], regs.EAX | regs.ESP)

        # 2) MSVC /O1 идиома push 2; pop edi; ret -> edi ИЗМЕНЁН (не сохранён!)
        prog2, mod2 = make_program(bytes.fromhex("6a02 5f c3"))
        mod2.run()
        ctx2 = gen.Ctx(prog2)
        ra2 = regs.compute(prog2, ctx2, log=lambda *_: None)
        self.assertFalse(ra2.preserved[e] & regs.BIT["edi"])
        self.assertTrue(ctx2.reg_out[e] & regs.BIT["edi"])

        # 3) Порча ebx без push -> ebx в out
        prog3, mod3 = make_program(bytes.fromhex("bb01000000 c3"))
        mod3.run()
        ctx3 = gen.Ctx(prog3)
        ra3 = regs.compute(prog3, ctx3, log=lambda *_: None)
        self.assertFalse(ra3.preserved[e] & regs.BIT["ebx"])
        self.assertTrue(ctx3.reg_out[e] & regs.BIT["ebx"])

    def test_seh_prolog_epilog_recognition_and_preservation(self):
        # Собираем в одном модуле:
        #   0x10101000: функция-клиент: push 8; push 0; call __SEH_prolog; mov ebx,1; mov esi,2; mov edi,3; call __SEH_epilog; ret
        #   0x10101040: __SEH_prolog (push 0x10102000 + стандартная последовательность SEH_PROLOG + ret)
        #   0x10101080: __SEH_epilog (стандартная последовательность SEH_EPILOG)
        # Важно: call __SEH_prolog -> rel32 = 0x1040 - (0x1004 + 5) = 0x37;
        # call __SEH_epilog после 3x mov (15 байт, адрес 0x1018) -> rel32 = 0x1080 - (0x1018 + 5) = 0x63.
        fn = bytearray()
        fn += bytes.fromhex("6a08 6a00")                           # 1000: push 8; push 0
        fn += b"\xe8" + struct.pack("<i", 0x40 - (len(fn) + 5))    # 1004: call 1040 (__SEH_prolog)
        fn += bytes.fromhex("bb01000000 be02000000 bf03000000")    # 1009: mov ebx,1; mov esi,2; mov edi,3
        fn += b"\xe8" + struct.pack("<i", 0x80 - (len(fn) + 5))    # 1018: call 1080 (__SEH_epilog)
        fn += b"\xc3"                                              # 101d: ret
        fn = fn.ljust(0x40, b"\xcc")

        sehp = bytes.fromhex(
            "6800201010 "          # push 0x10102000 (адрес _except_handler3)
            "64a100000000 "        # mov eax, dword ptr fs:[0]
            "50 "                  # push eax
            "8b442410 "            # mov eax, dword ptr [esp + 0x10]
            "896c2410 "            # mov dword ptr [esp + 0x10], ebp
            "8d6c2410 "            # lea ebp, [esp + 0x10]
            "2be0 "                # sub esp, eax
            "53 56 57 "            # push ebx; push esi; push edi
            "c3"                   # ret
        ).ljust(0x40, b"\xcc")

        sehe = bytes.fromhex(
            "8b4df0 "              # mov ecx, dword ptr [ebp - 0x10]
            "64890d00000000 "      # mov dword ptr fs:[0], ecx
            "59 5f 5e 5b "         # pop ecx; pop edi; pop esi; pop ebx
            "c9 "                  # leave
            "51 "                  # push ecx
            "c3"                   # ret
        )

        code = bytes(fn) + sehp + sehe
        prog, mod = make_program(code)
        mod.run()
        ctx = gen.Ctx(prog)
        ra = regs.compute(prog, ctx, log=lambda *_: None)
        self.assertEqual(ra.helper(0x10101040), "sehp")
        self.assertEqual(ra.helper(0x10101080), "sehe")
        # Благодаря __SEH_prolog/__SEH_epilog все callee-saved регистры сохранены!
        self.assertEqual(ra.preserved[0x10101000], regs.CALLEE_SAVED)
        self.assertEqual(ctx.reg_out[0x10101000] & regs.CALLEE_SAVED, 0)


class TestEmit(unittest.TestCase):
    """Тесты генератора C (emit.py): ha без заворота, SNX_FPU_SINGLE, запрет &eax..&edi, детерминизм."""

    def test_ha_wrapping_rules(self):
        prog, mod = make_program(b"\xc3")
        mod.run()
        fc = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog))
        # [eax + 0x10] -> без заворота
        self.assertEqual(fc.ha(x86.Op("m", 4, base="eax", disp=0x10)), "m + eax + 0x10u")
        # [ebp - 0x20] -> без заворота
        self.assertEqual(fc.ha(x86.Op("m", 4, base="ebp", disp=0xFFFFFFE0)), "m + ebp - 0x20u")
        # [eax] -> без заворота
        self.assertEqual(fc.ha(x86.Op("m", 4, base="eax", disp=0)), "m + eax")
        # [eax + 0x20000] (>= SMALL_DISP) -> с заворотом (u32)
        self.assertEqual(fc.ha(x86.Op("m", 4, base="eax", disp=0x20000)), "m + (u32)(eax + 0x20000u)")
        # [eax + ecx*4 + 8] (с индексом) -> с заворотом
        self.assertEqual(fc.ha(x86.Op("m", 4, base="eax", index="ecx", scale=4, disp=8)),
                         "m + (u32)(eax + ecx*4u + 0x8u)")
        # fs:[0] -> с заворотом и c->fs_base
        self.assertEqual(fc.ha(x86.Op("m", 4, seg="fs", disp=0)), "m + (u32)(c->fs_base + 0x0u)")

    def test_snx_fpu_single_option(self):
        # fld dword [eax]; fadd dword [ecx]; fmul st(0), st(0); fsqrt; fchs; fstp dword [edx]; ret
        code = bytes.fromhex("d900 d801 d8c8 d9fa d9e0 d91a c3")
        prog, mod = make_program(code)
        mod.run()
        f = mod.functions[0x10101000]
        ctx = gen.Ctx(prog)

        old = os.environ.pop("SNX_FPU_SINGLE", None)
        old_f32 = os.environ.pop("SNX_FPU_FLOAT32", None)
        try:
            code_double = emit.FuncCompiler(mod, f, ctx).compile()
            self.assertNotIn("(double)(float)", code_double)
            self.assertIn("f0 = f0 + (double)ldf32(m + ecx);", code_double)
            self.assertIn("f0 = sqrt(f0);", code_double)

            os.environ["SNX_FPU_SINGLE"] = "1"
            code_single = emit.FuncCompiler(mod, f, ctx).compile()
            self.assertIn("f0 = (double)(float)(f0 + (double)ldf32(m + ecx));", code_single)
            self.assertIn("f0 = (double)(float)(f0 * f0);", code_single)
            self.assertIn("f0 = (double)(float)sqrt(f0);", code_single)
            # fchs и fld не округляются к float по спецификации x87 PC
            self.assertIn("f0 = -f0;", code_single)
            self.assertIn("f0 = (double)ldf32(m + eax);", code_single)
        finally:
            if old is None:
                os.environ.pop("SNX_FPU_SINGLE", None)
            else:
                os.environ["SNX_FPU_SINGLE"] = old
            if old_f32 is None:
                os.environ.pop("SNX_FPU_FLOAT32", None)
            else:
                os.environ["SNX_FPU_FLOAT32"] = old_f32

    def test_snx_fpu_float32_option(self):
        # Чистая f32-функция: fld dword [eax]; fadd dword [ecx]; fsqrt; fstp dword [edx]; ret
        code = bytes.fromhex("d900 d801 d9fa d91a c3")
        prog, mod = make_program(code)
        mod.run()
        f = mod.functions[0x10101000]
        ctx = gen.Ctx(prog)

        old_s = os.environ.pop("SNX_FPU_SINGLE", None)
        old_f = os.environ.pop("SNX_FPU_FLOAT32", None)
        try:
            # По умолчанию выключено (совместимость с difftest PC=53)
            c_def = emit.FuncCompiler(mod, f, ctx).compile()
            self.assertIn("double f0 = 0;", c_def)
            self.assertIn("sqrt(f0)", c_def)

            # При SNX_FPU_FLOAT32=1 — нативный float без (double)/(float) кастов
            os.environ["SNX_FPU_FLOAT32"] = "1"
            c_f32 = emit.FuncCompiler(mod, f, ctx).compile()
            self.assertIn("float f0 = 0;", c_f32)
            self.assertIn("f0 = ldf32(m + eax);", c_f32)
            self.assertIn("f0 = f0 + ldf32(m + ecx);", c_f32)
            self.assertIn("f0 = sqrtf(f0);", c_f32)
            self.assertIn("stf32(m + edx, f0);", c_f32)
            self.assertNotIn("(double)ldf32", c_f32)

            # SNX_FPU_SINGLE=1 перекрывает SNX_FPU_FLOAT32=1
            os.environ["SNX_FPU_SINGLE"] = "1"
            c_sing = emit.FuncCompiler(mod, f, ctx).compile()
            self.assertIn("double f0 = 0;", c_sing)
            os.environ.pop("SNX_FPU_SINGLE", None)

            # Наличие f64 (fld qword [eax]) -> остаётся double
            prog_d, mod_d = make_program(bytes.fromhex("dd00 d91a c3"))
            mod_d.run()
            c_d = emit.FuncCompiler(mod_d, mod_d.functions[0x10101000], gen.Ctx(prog_d)).compile()
            self.assertIn("double f0 = 0;", c_d)

            # Наличие libm (fsin) -> остаётся double
            prog_t, mod_t = make_program(bytes.fromhex("d900 d9fe d91a c3"))
            mod_t.run()
            c_t = emit.FuncCompiler(mod_t, mod_t.functions[0x10101000], gen.Ctx(prog_t)).compile()
            self.assertIn("double f0 = 0;", c_t)

            # Модуль msvcr70.dll -> всегда double
            prog_crt, mod_crt = make_program(code, base=0x13000000, name="msvcr70.dll")
            mod_crt.run()
            c_crt = emit.FuncCompiler(mod_crt, mod_crt.functions[0x13001000], gen.Ctx(prog_crt)).compile()
            self.assertIn("double f0 = 0;", c_crt)

            # Граница вызовов: callee возвращает float в ST0 -> (double)f0; caller читает -> (float)c->st_ret
            # 0x10101000: call +5 (0x1010100a); fadd dword [ecx]; fstp dword [edx]; ret
            # 0x1010100a: fld dword [eax]; ret
            pair = bytes.fromhex("e805000000 d801 d91a c3 d900 c3")
            prog_p, mod_p = make_program(pair)
            mod_p.run()
            ctx_p = gen.Ctx(prog_p)
            gen.compute_summaries(ctx_p)
            c_caller = emit.FuncCompiler(mod_p, mod_p.functions[0x10101000], ctx_p).compile()
            c_callee = emit.FuncCompiler(mod_p, mod_p.functions[0x1010100a], ctx_p).compile()
            self.assertIn("float f0 = 0;", c_caller)
            self.assertIn("f0 = (float)c->st_ret;", c_caller)
            self.assertIn("float f0 = 0;", c_callee)
            self.assertIn("c->st_ret = (double)f0;", c_callee)
        finally:
            if old_s is None:
                os.environ.pop("SNX_FPU_SINGLE", None)
            else:
                os.environ["SNX_FPU_SINGLE"] = old_s
            if old_f is None:
                os.environ.pop("SNX_FPU_FLOAT32", None)
            else:
                os.environ["SNX_FPU_FLOAT32"] = old_f

    def test_x87_compare_fusion(self):
        # 1) Математическая проверка всех 10 реальных (test ah, imm, cc) + sahf + fcomi на IEEE-754 (включая NaN и -0.0)
        def eval_c_expr(expr, fx, fy):
            env = {"fx": fx, "fy": fy, "__builtin_isunordered": lambda a, b: (a != a) or (b != b)}
            py = expr.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
            py = py.replace(" not =", "!=")
            return bool(eval(py, {"__builtins__": {}}, env))

        def x87_fcom_ah(x, y):
            if x != x or y != y:
                return 0x45
            if x < y:
                return 0x01
            if x == y:
                return 0x40
            return 0x00

        samples = [(1.0, 2.0), (2.0, 1.0), (1.0, 1.0), (-0.0, 0.0), (float("nan"), 1.0), (1.0, float("nan")),
                   (float("nan"), float("nan"))]
        for mask, cc in [(0x05, "p"), (0x41, "ne"), (0x41, "p"), (0x44, "p"), (0x01, "ne"),
                         (0x05, "np"), (0x44, "np"), (0x41, "e"), (0x01, "e"), (0x41, "np")]:
            expr = emit._x87_test_cond(mask, cc)
            self.assertIsNotNone(expr)
            for fx, fy in samples:
                v = x87_fcom_ah(fx, fy) & mask
                want = emit._eval_cc(cc, 0, int(bin(v).count("1") % 2 == 0), int(v == 0), 0, 0)
                self.assertEqual(eval_c_expr(expr, fx, fy), want, f"test ah,{mask:#x}/{cc} @ ({fx},{fy}): {expr}")

        for cc in ("p", "np", "e", "ne", "b", "ae", "a", "be"):
            expr = emit._x87_sahf_cond(cc, allow_of=True)
            self.assertIsNotNone(expr)
            for fx, fy in samples:
                ah = x87_fcom_ah(fx, fy)
                want = emit._eval_cc(cc, ah & 1, (ah >> 2) & 1, (ah >> 6) & 1, 0, 0)
                self.assertEqual(eval_c_expr(expr, fx, fy), want, f"sahf/fcomi {cc} @ ({fx},{fy}): {expr}")

        # 2) ax мёртв на обеих ветках (jne перепрыгивает nop прямо на xor eax, eax; ret) -> убраны fnstsw ax, FCOM, PARITY
        # fld dword [eax]; fcomp dword [ecx]; fnstsw ax; test ah, 0x41; jne +1; nop; xor eax, eax; ret
        c_dead = bytes.fromhex("d900 d819 dfe0 f6c441 7501 90 31c0 c3")
        prog, mod = make_program(c_dead)
        mod.run()
        out = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertIn("fx = f0; fy = (double)ldf32(m + ecx);", out)
        self.assertIn("if ((!(fx > fy))) goto L_1010100c;", out)
        self.assertNotIn("FCOM(", out)
        self.assertNotIn("PARITY(", out)
        self.assertNotIn("fsw", out)

        # 3) ax жив на ветке jne -> ret -> fnstsw ax и FCOM сохранены, но test ah/PARITY убраны
        # fld dword [eax]; fcomp dword [ecx]; fnstsw ax; test ah, 0x41; jne +2; xor eax, eax; ret
        c_live = bytes.fromhex("d900 d819 dfe0 f6c441 7502 31c0 c3")
        prog, mod = make_program(c_live)
        mod.run()
        out = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertIn("fsw = FCOM(fx, fy);", out)
        self.assertIn("if ((!(fx > fy))) goto L_1010100d;", out)
        self.assertNotIn("PARITY(", out)

        # 4) Промежуточный fstp st(0) между fnstsw ax и test ah допускается и сливается (ftst)
        # fld dword [eax]; ftst; fnstsw ax; fstp st(0); test ah, 0x44; jnp +1; nop; xor eax, eax; ret
        c_ftst = bytes.fromhex("d900 d9e4 dfe0 ddd8 f6c444 7b01 90 31c0 c3")
        prog, mod = make_program(c_ftst)
        mod.run()
        out = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertIn("fx = f0; fy = 0.0;", out)
        self.assertIn("if ((fx == fy)) goto L_1010100e;", out)
        self.assertNotIn("FCOM(", out)
        self.assertNotIn("PARITY(", out)

        # 5) Промежуточный второй fcomp между fnstsw ax и test ah ЗАПРЕЩАЕТ слияние
        # fld dword [eax]; fld dword [eax]; fcomp dword [ecx]; fnstsw ax; fcomp dword [edx]; test ah, 0x41; jne +1; nop; xor eax, eax; ret
        c_inter = bytes.fromhex("d900 d900 d819 dfe0 d81a f6c441 7501 90 31c0 c3")
        prog, mod = make_program(c_inter)
        mod.run()
        fc_inter = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog))
        out_inter = fc_inter.compile()
        self.assertEqual(fc_inter.x87_fused_cmp, {})
        self.assertIn("FCOM(", out_inter)

        # 6) sahf и fcomip сливаются в прямые сравнения без материализации флагов
        # fld dword [eax]; fcomp dword [ecx]; fnstsw ax; sahf; jae +1; nop; xor eax, eax; ret
        c_sahf = bytes.fromhex("d900 d819 dfe0 9e 7301 90 31c0 c3")
        prog, mod = make_program(c_sahf)
        mod.run()
        out_s = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertIn("if ((fx >= fy)) goto L_1010100a;", out_s)
        self.assertNotIn("FCOM(", out_s)

        # fld dword [eax]; fld dword [ecx]; fcomip st(0), st(1); fstp st(0); jb +1; nop; xor eax, eax; ret
        c_comi = bytes.fromhex("d900 d901 dff1 ddd8 7201 90 31c0 c3")
        prog, mod = make_program(c_comi)
        mod.run()
        out_ci = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertIn("fx = f1; fy = f0;", out_ci)
        self.assertIn("if ((!(fx >= fy))) goto L_1010100b;", out_ci)
        self.assertNotIn("cf =", out_ci)

        # 7) Цепочка из двух подряд fcomp; fnstsw ax; test ah; jcc: второй fnstsw ax убивает ax первого!
        # fld [ebx]; fcomp [ecx]; fnstsw ax; test ah, 0x41; jne +1; nop;
        # fld [ebx]; fcomp [edx]; fnstsw ax; test ah, 0x41; jne +1; nop; xor eax, eax; ret
        c_chain = bytes.fromhex("d903 d819 dfe0 f6c441 7501 90 d903 d81a dfe0 f6c441 7501 90 31c0 c3")
        prog, mod = make_program(c_chain)
        mod.run()
        out_ch = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertNotIn("FCOM(", out_ch)
        self.assertNotIn("fsw", out_ch)

    def test_gnatives_call_dispatch(self):
        # Проверяем динамический поиск GNatives из Core.dll (как EN 0x1021b780, так и RUS 0x1021b7d0)
        for gn_base in (0x1021b780, 0x1021b7d0):
            # call dword ptr [esi*4 + gn_base]
            # call dword ptr [eax*4 + gn_base + 0x400] (расширенная страница +256)
            # call dword ptr [esi*4 + 0x10200000] (не GNatives -> обычный RT_ICALL)
            # ret
            code = (
                b"\xff\x14\xb5" + struct.pack("<I", gn_base)
                + b"\xff\x14\x85" + struct.pack("<I", gn_base + 0x400)
                + b"\xff\x14\xb5" + struct.pack("<I", 0x10200000)
                + b"\xc3"
            )
            prog, mod = make_program(
                code, base=0x10100000, name="Core.dll",
                exports={"?GNatives@@3PAP8UObject@@AEXAAUFFrame@@QAX@ZA": gn_base},
            )
            mod.run()
            ctx = gen.Ctx(prog)
            self.assertEqual(ctx.gnatives_base, gn_base)
            out = emit.FuncCompiler(mod, mod.functions[0x10101000], ctx).compile()
            self.assertIn("u32 gi_ = esi;", out)
            self.assertIn("u32 gi_ = (u32)(eax + 256u);", out)
            self.assertEqual(out.count("RT_GNATIVE_CALL(c, gi_, t);"), 2)
            self.assertEqual(out.count("RT_ICALL(c, t);"), 1)

    def test_pin_c_param_and_inline_rdtsc(self):
        # Проверяем: SNX_C_PARAM / SNX_C_ARG / P_(CPU *restrict c = g_pin_c;), вызов гостя (SNX_C_ARG) vs
        # оверрайда (c), встраивание fi_*(SNX_C_PARAM), таблицу SNX_GUEST_FN и RT_RDTSC().
        # 0x10101000: rdtsc; call 0x1010100e (листовая -> fi_); call 0x10101012 (оверрайд -> (c)); ret
        # 0x1010100e: add eax, 1; ret
        # 0x10101012: xor eax, eax; ret (заменяется оверрайдом)
        # 0x10101015: jmp 0x1010101a (хвостовой переход на _setjmp3 -> f_1010101a(SNX_C_ARG))
        # 0x1010101a: xor eax, eax; ret (экспорт _setjmp3)
        code = bytes.fromhex(
            "0f31 "                # 1000: rdtsc
            "e807000000 "          # 1002: call 100e (5 байт, next=1007 -> +7 = 100e)
            "e806000000 "          # 1007: call 1012 (next=100c -> +6 = 1012)
            "c3 "                  # 100c: ret
            "90 "                  # 100d: nop
            "83c001 c3 "           # 100e: add eax, 1; ret
            "31c0 c3 "             # 1012: xor eax, eax; ret
            "e900000000 "          # 1015: jmp 101a (хвостовой переход на _setjmp3)
            + "90" * 33 + "31c0 c3"  # 101a: > INLINE_MAX инструкций (_setjmp3, не инлайнится)
        )
        pe_bytes = make_minimal_pe([(".text", code, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE)],
                                   image_base=0x10100000, entry_rva=0x1000, is_dll=True)
        with tempfile.TemporaryDirectory() as td:
            pe_path = os.path.join(td, "synth.dll")
            with open(pe_path, "wb") as f:
                f.write(pe_bytes)
            img = image.load_image(pe_path, 0x10100000)
            img.exports["_setjmp3"] = 0x1010101a
            prog = discover.Program({"synth.dll": img})
            mod = discover.Module(img, prog)
            prog.modules["synth.dll"] = mod
            mod.strong.update({0x10101000, 0x1010100e, 0x10101012, 0x10101015, 0x1010101a})
            mod.run()
            ctx = gen.Ctx(prog, overrides={0x10101012: ("ovr_synth_test", 0)}, wraps={0x10101012: "ovr_synth_test"})
            outdir = os.path.join(td, "out")
            gen.write_modules(ctx, [mod], outdir, jobs=1, log=lambda *_: None)
            with open(os.path.join(outdir, "synth", "synth_0000.c")) as f:
                chunk = f.read()
            with open(os.path.join(outdir, "synth", "synth_table.c")) as f:
                table = f.read()
            self.assertIn("void f_10101000(SNX_C_PARAM)", chunk)
            self.assertIn("P_(CPU *restrict c = g_pin_c;)", chunk)
            self.assertIn("u64 t = RT_RDTSC();", chunk)
            self.assertIn("fi_1010100e(SNX_C_ARG);", chunk)
            self.assertIn("f_1010101a(SNX_C_ARG); return;", chunk)
            self.assertNotIn("f_1010101a(c)", chunk)
            self.assertIn("ovr_synth_test(c);", chunk)
            self.assertIn("void ovr_synth_test(CPU *c);", chunk)
            self.assertIn("fi_1010100e(SNX_C_PARAM)", chunk)
            self.assertIn("void (*const ovr_orig_ovr_synth_test)(CPU *c) = SNX_GUEST_FN(f_10101012);", table)
            self.assertIn("{0x10101000u, SNX_GUEST_FN(f_10101000)},", table)
            self.assertIn("{0x10101012u, ovr_synth_test, 1},", table)
            # Синтаксическая проверка сгенерированного чанка и таблицы реальным хостовым cc (без SNX_PIN)
            res = subprocess.run(
                ["cc", "-std=c11", "-fsyntax-only", "-Wall", "-Wextra", "-Wno-unused-label",
                 "-Wno-unused-variable", "-Wno-unused-function", "-I", ROOT,
                 os.path.join(outdir, "synth", "synth_0000.c"),
                 os.path.join(outdir, "synth", "synth_table.c")],
                capture_output=True, text=True,
            )
            self.assertEqual(res.returncode, 0, f"ошибка синтаксиса C:\n{res.stderr}")

    def test_no_address_of_guest_regs(self):
        # Набор инструкций, ранее/потенциально передающих регистры в хелперы:
        # cpuid, rdtsc, rep movsd, rep stosd, xchg, cmpxchg, div, mul, shld, bsf, x87
        code = bytes.fromhex(
            "0fa2 "          # cpuid
            "0f31 "          # rdtsc
            "f3a5 "          # rep movsd
            "f3ab "          # rep stosd
            "87c3 "          # xchg eax, ebx
            "0fb119 "        # cmpxchg dword ptr [ecx], ebx
            "f7e3 "          # mul ebx
            "f7f3 "          # div ebx
            "0fa4c304 "      # shld ebx, eax, 4
            "0fbcc3 "        # bsf eax, ebx
            "d900 d801 d91a "  # fld; fadd; fstp
            "c3"
        )
        prog, mod = make_program(code)
        mod.run()
        code_c = emit.FuncCompiler(mod, mod.functions[0x10101000], gen.Ctx(prog)).compile()
        self.assertIsNone(re.search(r"&(eax|ecx|edx|ebx|ebp|esi|edi)\b", code_c),
                          f"найдён запрещённый адрес регистра гостя в C:\n{code_c}")

    def test_parallel_write_modules_deterministic(self):
        # Создаём синтетический PE-модуль с > FUNCS_PER_FILE функциями, чтобы проверить несколько чанков.
        orig_fpf = gen.FUNCS_PER_FILE
        try:
            gen.FUNCS_PER_FILE = 2  # форсируем разбиение на 4 чанка
            # 8 функций: простая арифметика + ret (по 4 байта)
            code = b"".join(bytes.fromhex("83c001 c3") for _ in range(8))
            pe_bytes = make_minimal_pe([(".text", code, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE)],
                                       image_base=0x10100000, entry_rva=0x1000, is_dll=True)
            with tempfile.TemporaryDirectory() as td:
                pe_path = os.path.join(td, "synth.dll")
                with open(pe_path, "wb") as f:
                    f.write(pe_bytes)
                img = image.load_image(pe_path, 0x10100000)
                prog = discover.Program({"synth.dll": img})
                mod = discover.Module(img, prog)
                prog.modules["synth.dll"] = mod
                for k in range(8):
                    mod.strong.add(0x10101000 + 4 * k)
                mod.run()

                out1 = os.path.join(td, "out1")
                out2 = os.path.join(td, "out2")
                ctx1 = gen.Ctx(prog)
                gen.write_modules(ctx1, [mod], out1, jobs=1, log=lambda *_: None)
                gen.write_hle_stubs(ctx1, out1)

                ctx2 = gen.Ctx(prog)
                gen.write_modules(ctx2, [mod], out2, jobs=2, log=lambda *_: None)
                gen.write_hle_stubs(ctx2, out2)

                for dirpath, _, fnames in os.walk(out1):
                    rel = os.path.relpath(dirpath, out1)
                    for fn in sorted(fnames):
                        p1 = os.path.join(out1, rel, fn)
                        p2 = os.path.join(out2, rel, fn)
                        self.assertTrue(os.path.exists(p2), f"отсутствует {p2}")
                        with open(p1, "rb") as f1, open(p2, "rb") as f2:
                            self.assertEqual(f1.read(), f2.read(), f"расхождение в {fn}")
        finally:
            gen.FUNCS_PER_FILE = orig_fpf


class TestCheckAndOverrides(unittest.TestCase):
    """Тесты main.py check и overrides.verify на синтетических каталогах и PE."""

    def test_check_missing_modules_and_broken_pe(self):
        with tempfile.TemporaryDirectory() as td:
            sys_dir = os.path.join(td, "System")
            os.makedirs(sys_dir)
            # Кладём битый PE вместо Core.dll; остальные 11 модулей отсутствуют
            with open(os.path.join(sys_dir, "Core.dll"), "wb") as f:
                f.write(b"NOT_A_VALID_PE_HEADER_0123456789")
            buf = io.StringIO()
            errs, warns = recomp_main.check_game(sys_dir, stream=buf)
            text = buf.getvalue()
            self.assertGreaterEqual(len(errs), 12)
            self.assertEqual(len(warns), 5)  # нет Maps/Textures/Sounds/Music/Movies
            self.assertIn("битый PE", text)
            self.assertIn("ОТСУТСТВУЕТ", text)
            self.assertIn("Maps/", text)

    def test_check_cli_nonzero_on_invalid_dir(self):
        with tempfile.TemporaryDirectory() as td:
            res = subprocess.run(
                [sys.executable, "-I", os.path.join(ROOT, "recomp", "main.py"), "check", "--game", td],
                capture_output=True, text=True,
            )
            self.assertEqual(res.returncode, 1)
            self.assertIn("ОТСУТСТВУЕТ", res.stdout)

    def test_check_safedisc_detection_sections_and_entropy(self):
        with tempfile.TemporaryDirectory() as td:
            sys_dir = os.path.join(td, "System")
            os.makedirs(sys_dir)
            for d in recomp_main.ASSET_DIRS:
                os.makedirs(os.path.join(td, d))
                with open(os.path.join(td, d, "dummy.dat"), "wb") as f:
                    f.write(b"x")
            # 1) SafeDisc с секциями stxt774/stxt371 и высокой энтропией .text (~8.0)
            high_ent = bytes(range(256)) * 16
            sd_pe = make_minimal_pe(
                [
                    (".text", high_ent, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE),
                    ("stxt774", b"\x90" * 64, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE),
                    ("stxt371", b"\xc3" * 64, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE),
                ],
                image_base=0x10900000,
                entry_rva=0x3000,  # точка входа в stxt371
            )
            with open(os.path.join(sys_dir, "Game.exe"), "wb") as f:
                f.write(sd_pe)

            buf = io.StringIO()
            errs, warns = recomp_main.check_game(sys_dir, stream=buf)
            text = buf.getvalue()
            self.assertEqual(warns, [])  # все 5 каталогов ассетов на месте!
            self.assertTrue(any("SafeDisc" in e for e in errs), errs)
            self.assertIn("ОБНАРУЖЕН ЗАЩИЩЁННЫЙ (SafeDisc)", text)
            self.assertIn("stxt774", text)

            # 2) SafeDisc с секцией .icd
            icd_pe = make_minimal_pe(
                [
                    (".text", b"\x90\xc3" * 32, image.IMAGE_SCN_MEM_EXECUTE | image.IMAGE_SCN_CNT_CODE),
                    (".icd", b"\x90" * 32, 0x40000040),
                ],
                image_base=0x10900000,
                entry_rva=0x1000,
            )
            with open(os.path.join(sys_dir, "Game.exe"), "wb") as f:
                f.write(icd_pe)
            buf2 = io.StringIO()
            errs2, _ = recomp_main.check_game(sys_dir, stream=buf2)
            self.assertTrue(any("SafeDisc" in e for e in errs2), errs2)
            self.assertIn(".icd", buf2.getvalue())

    def test_overrides_verify_unique_missing_and_duplicate(self):
        sig_bytes = bytes(int(p, 16) if p != "?" else 0x00 for p in overrides.SIGS["file_read"].split())
        # Ровно одно совпадение -> ok
        prog1, _ = make_program(b"\x90" * 16 + sig_bytes + b"\xc3", base=0x10900000, name="game.exe")
        res1 = {k: (st, det) for k, _, st, det in overrides.verify(prog1)}
        self.assertEqual(res1["game.exe!=file_read"][0], "ok")
        self.assertEqual(res1["game.exe!=file_read"][1], 0x10901010)
        # Другая сигнатура game.exe отсутствует -> missing_sig
        self.assertEqual(res1["game.exe!=file_write"][0], "missing_sig")
        # Дубликат сигнатуры -> dup_sig
        prog2, _ = make_program(sig_bytes + b"\x90" * 8 + sig_bytes, base=0x10900000, name="game.exe")
        res2 = {k: (st, det) for k, _, st, det in overrides.verify(prog2)}
        self.assertEqual(res2["game.exe!=file_read"][0], "dup_sig")
        self.assertEqual(len(res2["game.exe!=file_read"][1]), 2)
        # Проверка экспортов msvcr70.dll
        prog3, _ = make_program(b"\xc3", base=0x13000000, name="msvcr70.dll", exports={"rand": 0x13001000})
        res3 = {k: (st, det) for k, _, st, det in overrides.verify(prog3)}
        self.assertEqual(res3["msvcr70.dll!rand"][0], "ok")
        self.assertEqual(res3["msvcr70.dll!srand"][0], "missing_export")


if __name__ == "__main__":
    unittest.main()
