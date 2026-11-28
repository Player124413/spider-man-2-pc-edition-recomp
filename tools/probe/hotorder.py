"""Порядок горячих функций для компоновщика по профилю SNX_PROF (мак или Switch): самые частые в выборках (лист)
функции идут подряд в начале .text — плотнее в кеше инструкций и TLB (A57: L1i 48 КБ, машинного кода ≈27 МБ, профиль
плоский). Потоки Core 1 (g — гость) и Core 2 (r — рендер) раскладываются в раздельные блоки, чтобы функции рендера
(render_main, Mesa) не вытесняли код гостя из 48 КБ L1i ядра 1. Вывод — фрагмент для `ld --section-ordering-file`
(GNU ld ≥ 2.43; `-ffunction-sections` уже включён):

    tools/.venv/bin/python -I tools/probe/hotorder.py --bin build/switch-prof/snx_boot_020_base.elf \
        build/switch-prof/prof_swamp_020.txt --out build/hot_order.ld
    tools/build.sh switch   # с -DSNX_SECTION_ORDER=build/hot_order.ld (см. CMakeLists.txt)

Имена функций — как в C (`f_<адрес>`, рантайм, HLE), поэтому профиль с мака годится для сборки Switch.
"""
import argparse
import bisect
import collections
import os
import re
import struct
import subprocess

NM_ELF = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin", "aarch64-none-elf-nm")

# Обязательные горячие точки входа Core 1 (трамплин rt_pin_enter, единая rt_icall_slow и шлюз rt_pin_call_native;
# всего ~292 Б — всегда в начале L1I ядра 1)
CORE1_PINNED = ("rt_pin_enter", "rt_icall_slow", "rt_pin_call_native")

# Якоря-маски для новых нативных оверрайдов Core 1 (D3DDrv / тени), которых ещё нет в прошлом профиле:
# GNU ld молча пропускает пустые маски, а появившиеся в сборке оверрайды сразу попадают в горячую зону Core 1
CORE1_ANCHOR_PATTERNS = ("ovr_d3ddrv_*", "ovr_engine_shadow_*")

# Секция пула заглушек pin_wrap_000..3ff (~16 КБ): сводим отдельные pin_wrap_* в одну секцию .text.pin_wrap,
# чтобы она лежала в горячей зоне Core 1 (<512 КБ), не вытесняя 16 КБ из первых 48 КБ L1I и не плодя 1024 строки
PIN_WRAP_SECTION = "pin_wrap"
_PIN_WRAP_RE = re.compile(r"^pin_wrap_[0-9a-fA-F]+$")
# Служебные суффиксы клонов GCC (.part.N, .constprop.N, .isra.N): между сборками без PGO и с PGO они могут
# появляться и исчезать, поэтому сводим к базовому имени и матчим .text.foo/.text.foo.*/.text.hot.foo/.text.hot.foo.*
# (холодные части GCC кладёт в .text.unlikely.<fn>, так что .text.foo.* их не захватывает)
_GCC_CLONE_RE = re.compile(r"(?:\.(?:part|constprop|isra)\.\d+)+$")

# Функции, принадлежащие исключительно потоку рендера Core 2 (Mesa/nouveau и цикл gl_cs)
RENDER_ONLY_EXACT = {"render_main", "plat_swap", "x_swap", "exec_draw"}
RENDER_ONLY_PREFIXES = (
    "_mesa_", "nvc0_", "nv50_", "gm107_", "gf100_", "nve4_", "nouveau_",
    "st_", "cso_", "cli_", "glsl_", "pushbuf_", "d3d8_r_",
)


def _norm_name(name):
    """Нормализовать имя символа к базовому имени секции (.cold пропускаем, клоны GCC сводим к базе)."""
    if name.endswith(".cold"):
        return None
    if name.endswith(".localalias"):
        name = name[:-11]
    name = _GCC_CLONE_RE.sub("", name)
    if _PIN_WRAP_RE.match(name):
        return PIN_WRAP_SECTION
    return name


def _section_pattern(name):
    """Паттерн секций ld для функции/маски: для C-функций матчим также клоны GCC (.part/.constprop/.isra)."""
    if name.endswith("*") or re.match(r"f[ic]?_[0-9a-f]{8}$", name) or name == PIN_WRAP_SECTION:
        return f"*(.text.{name} .text.hot.{name})"
    return f"*(.text.{name} .text.{name}.* .text.hot.{name} .text.hot.{name}.*)"


def _is_render_only(name):
    return name in RENDER_ONLY_EXACT or name.startswith(RENDER_ONLY_PREFIXES)


def _elf_symbols(path, slide):
    """Чтение таблицы символов ELF64 напрямую (работает без вызова внешнего aarch64-none-elf-nm)."""
    with open(path, "rb") as f:
        hdr = f.read(64)
        e_shoff = struct.unpack_from("<Q", hdr, 40)[0]
        e_shentsize, e_shnum = struct.unpack_from("<HH", hdr, 58)
        f.seek(e_shoff)
        shs = [struct.unpack_from("<IIQQQQIIQQ", f.read(e_shentsize), 0) for _ in range(e_shnum)]
        exec_shs = {i for i, sh in enumerate(shs) if sh[2] & 0x4}  # SHF_EXECINSTR
        symtab = next((sh for sh in shs if sh[1] == 2), None)      # SHT_SYMTAB
        if symtab is None:
            return []
        strtab = shs[symtab[6]]
        f.seek(strtab[4])
        strdata = f.read(strtab[5])
        f.seek(symtab[4])
        symdata = f.read(symtab[5])
        by_addr = {}
        entsz = symtab[9]
        for off in range(0, len(symdata), entsz):
            st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", symdata, off)
            if st_shndx not in exec_shs or (st_info & 0xF) not in (0, 2):
                continue
            end = strdata.find(b"\x00", st_name)
            nm = strdata[st_name:end].decode("utf-8", "replace")
            if not nm or nm.startswith(("$", ".L")):
                continue
            # На одном адресе может быть f_X и f_X.localalias — предпочитаем каноническое имя без .localalias
            prev = by_addr.get(st_value)
            score = (".localalias" not in nm, not nm.endswith(".cold"), st_size)
            if prev is None or score > prev[0]:
                by_addr[st_value] = (score, nm)
        return sorted((a + slide, nm) for a, (_, nm) in by_addr.items())


def symbols(path, slide):
    if open(path, "rb").read(4) == b"\x7fELF":
        return _elf_symbols(path, slide)
    out = subprocess.run(["nm", "-n", "--defined-only", path], capture_output=True, text=True).stdout
    by_addr = {}
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) == 3 and p[1] in "tTwW":
            addr = int(p[0], 16) + slide
            name = p[2][1:]  # Mach-O: ведущее подчёркивание
            prev = by_addr.get(addr)
            if prev is None or (".localalias" in prev and ".localalias" not in name):
                by_addr[addr] = name
    return sorted(by_addr.items())


def _select_cover(cnt, total, cover, exclude=()):
    """Выбрать функции по убыванию выборок до покрытия cover% (пропуская уже размещённые в exclude)."""
    if not total:
        return [], 0
    order, acc = [], 0
    for name, n in cnt.most_common():
        if 100 * acc / total >= cover:
            break
        acc += n
        if name not in exclude:
            order.append(name)
    return order, acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="бинарник, на котором снят профиль (для имён)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--threads", default="gr", help="чьи выборки считать: g — гость, r — рендер")
    ap.add_argument("--cover", type=float, default=99.5, help="процент выборок, который должны покрыть функции")
    ap.add_argument("profs", nargs="+")
    args = ap.parse_args()
    cnt_g, cnt_r = collections.Counter(), collections.Counter()
    tot_g = tot_r = 0
    for prof in args.profs:
        lines = open(prof).read().split("\n")
        m = re.match(r"base ([0-9a-f]+) slide ([0-9a-f]+)", lines[0])
        syms = symbols(args.bin, int(m.group(2), 16))
        starts = [a for a, _ in syms]
        for ln in lines[1:]:
            p = ln.split()
            if len(p) < 2 or p[0] not in args.threads:
                continue
            pc = int(p[1], 16)
            i = bisect.bisect_right(starts, pc) - 1
            if i >= 0 and pc - starts[i] < (1 << 20):  # дальше — системные библиотеки (мак), не наш образ
                nm = _norm_name(syms[i][1])
                if not nm:
                    continue
                if p[0] == "g":
                    cnt_g[nm] += 1
                    tot_g += 1
                elif p[0] == "r":
                    cnt_r[nm] += 1
                    tot_r += 1

    # Раздельные блоки: сначала Core 1 (g) с закреплёнными rt_pin_enter/rt_icall_slow, затем Core 2 (r)
    want_g = "g" in args.threads
    want_r = "r" in args.threads

    order_g, acc_g = [], 0
    seen = set()
    if want_g:
        # Исключаем из блока Core 1 чисто рендерные функции и функции, в >10 раз более частые на Core 2
        # (если не сгенерированный код гостя и не закреплённые точки входа)
        filt_g = collections.Counter()
        filt_tot_g = 0
        for nm, n in cnt_g.items():
            if _is_render_only(nm):
                continue
            if want_r and not re.match(r"f[ic]?_[0-9a-f]{8}$", nm) and nm not in CORE1_PINNED:
                if cnt_r.get(nm, 0) > 10 * n:
                    continue
            filt_g[nm] = n
            filt_tot_g += n
        for nm in CORE1_PINNED:
            order_g.append(nm)
            seen.add(nm)
        for pat in CORE1_ANCHOR_PATTERNS:
            order_g.append(pat)
            seen.add(pat)
        # Не дублируем функции, уже покрытые масками-якорями CORE1_ANCHOR_PATTERNS
        anchor_prefixes = tuple(p[:-1] for p in CORE1_ANCHOR_PATTERNS if p.endswith("*"))
        for nm in list(filt_g):
            if anchor_prefixes and nm.startswith(anchor_prefixes):
                seen.add(nm)
        picked_g, acc_g = _select_cover(filt_g, filt_tot_g, args.cover, exclude=seen)
        order_g.extend(picked_g)
        seen.update(picked_g)
        if PIN_WRAP_SECTION not in seen:
            order_g.append(PIN_WRAP_SECTION)
            seen.add(PIN_WRAP_SECTION)
        tot_g = filt_tot_g

    order_r, acc_r = [], 0
    if want_r:
        order_r, acc_r = _select_cover(cnt_r, tot_r, args.cover, exclude=seen)
        seen.update(order_r)

    with open(args.out, "w") as f:
        # фрагмент встаёт в начало .text раньше скрипта платформы: crt0 NRO обязан остаться по смещению 0 (там точка
        # входа и место под заголовок, который пишет elf2nro), иначе hbloader прыгает в чужую функцию
        f.write(".text : {\n  KEEP (*(.crt0))\n")
        if order_g:
            f.write("  /* Core 1 (поток гостя g): закреплённые трамплины и горячий код гостя */\n")
            for name in order_g:  # .text.hot.<имя> — та же функция, собранная по профилю (SNX_PGO=use)
                f.write(f"  {_section_pattern(name)}\n")
        if order_r:
            f.write("  /* Core 2 (поток рендера r): горячий код рендера и Mesa, не вошедший в Core 1 */\n")
            for name in order_r:
                f.write(f"  {_section_pattern(name)}\n")
        f.write("}\n")

    guest_g = sum(1 for n in order_g if re.match(r"f[ic]?_[0-9a-f]{8}$", n))
    cov_g = (100 * acc_g / tot_g) if tot_g else 0.0
    cov_r = (100 * acc_r / tot_r) if tot_r else 0.0
    print(f"Core 1 (g): {tot_g} выборок, {len(order_g)} функций (сгенерированных {guest_g}, покрытие {cov_g:.1f}%)")
    print(f"Core 2 (r): {tot_r} выборок, {len(order_r)} функций (покрытие {cov_r:.1f}%)")
    print(f"Итого: {len(order_g) + len(order_r)} функций -> {args.out}")


if __name__ == "__main__":
    main()
