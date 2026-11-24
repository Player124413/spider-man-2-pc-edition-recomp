"""Нативные замены гостевых функций для Spider-Man 2 (Webhead/UE2).

Формат унаследован от shrekNX: "модуль!экспорт" -> (C-символ, глубина x87 на выходе),
"модуль!=имя" —	function по сигнатуре из SIGS (?), WRAPS оставляет оригинал доступным
как ovr_orig_<символ>. Реализации — runtime/overrides_*.c.

На старте переносим только ABI-независимые замены: математика MSVC 7 (msvcr71) и
статические копии memcpy. Специфичные для Shrek 2 obеррайты (правки скриптов, HUD,
 D3D-состояния и т. п.) сюда НЕ входят: у Webhead другая версия движка и другая игра.
"""
import re

OVERRIDES = {}
WRAPS = {}
SIGS = {}

# memcpy из статического CRT: своя копия почти в каждом модуле, хвосты — переходами
# по вычисленному адресу (signature проверена по DLL Spider-Man 2).
SIGS["memcpy"] = "55 8B EC 53 56 57 A1 ? ? ? ? 83 38 00 0F 84 ? ? ? ? 8B 4D 10 8B 7D 08 8B 75 0C 8B D9 FC"
for _m in ["Engine.dll", "D3DDrv.dll", "ALAudio.dll", "Fire.dll", "Webhead.dll", "Gameplay.dll",
           "Window.dll", "webhead.exe"]:
    OVERRIDES[f"{_m}!=memcpy"] = ("ovr_memcpy", 0)

# msvcr71: математика (внутри — x87-трюки с нестандартной передачей стека,
# статически не переводится). Экспорты сверены с msvcr71.dll из этой сборки игры.
for _n in ["acos", "asin", "atan", "atan2", "ceil", "cos", "cosh", "exp", "fabs", "floor", "fmod", "frexp",
           "ldexp", "log", "log10", "modf", "pow", "sin", "sinh", "sqrt", "tan", "tanh", "_logb", "_nextafter",
           "_y0", "_y1", "_yn", "_wtof", "wcstod", "atof", "strtod", "_atodbl"]:
    OVERRIDES[f"msvcr71.dll!{_n}"] = (f"ovr_crt_{_n.lstrip('_')}", 0 if _n == "_atodbl" else 1)
# msvcr71: rand/srand без _getptd (три вызова HLE на каждое число; runtime/overrides_crt.c)
OVERRIDES["msvcr71.dll!rand"] = ("ovr_crt_rand", 0)
OVERRIDES["msvcr71.dll!srand"] = ("ovr_crt_srand", 0)
# msvcr71: нативный qsort (runtime/overrides_crt.c)
WRAPS["msvcr71.dll!qsort"] = ("ovr_crt_qsort", 0)


def _match_sig(img, sig):
    rx = re.compile(b"".join(b"." if p == "?" else re.escape(bytes([int(p, 16)])) for p in sig.split()), re.S)
    hits = []
    for s in img.code_sections():
        o = s.va - img.base
        hits += [s.va + m.start() for m in rx.finditer(bytes(img.data[o:o + s.vsize]))]
    return hits


def _find_sig(img, sig):
    hits = _match_sig(img, sig)
    if len(hits) != 1:
        raise SystemExit(f"overrides: signature {sig!r} in {img.name}: {len(hits)} matches")
    return hits[0]


def verify(prog):
    """Проверить все сигнатуры и экспорты без аварийного выхода.

    -> список (key, sym, status, detail), status: 'ok' | 'missing_module' |
       'missing_sig' | 'dup_sig' | 'missing_export' | 'unknown_sig'.
    """
    results = []
    entries = list(OVERRIDES.items()) + list(WRAPS.items())
    for key, (sym, _) in entries:
        mod, name = key.split("!", 1)
        img = prog.images.get(mod.lower())
        if img is None:
            results.append((key, sym, "missing_module", mod))
            continue
        if name.startswith("="):
            sig_name = name[1:]
            sig = SIGS.get(sig_name)
            if sig is None:
                results.append((key, sym, "unknown_sig", sig_name))
                continue
            hits = _match_sig(img, sig)
            if len(hits) == 1:
                results.append((key, sym, "ok", hits[0]))
            elif len(hits) == 0:
                results.append((key, sym, "missing_sig", 0))
            else:
                results.append((key, sym, "dup_sig", hits))
        else:
            va = img.exports.get(name)
            if va is not None:
                results.append((key, sym, "ok", va))
            else:
                results.append((key, sym, "missing_export", name))
    return results


def _addr(prog, key):
    mod, name = key.split("!")
    img = prog.images.get(mod.lower())
    if img is None:
        return None
    if name.startswith("="):
        return _find_sig(img, SIGS[name[1:]])
    return img.exports.get(name)


def resolve(prog):
    """-> {guest addr: (sym, ret_depth)} для загруженных модулей."""
    out = {}
    for key, val in list(OVERRIDES.items()) + list(WRAPS.items()):
        a = _addr(prog, key)
        if a is not None:
            out[a] = val
    return out


def wrapped(prog):
    """-> {guest addr: sym}: оригинал обёртки доступен как ovr_orig_<символ>."""
    out = {}
    for key, (sym, _) in WRAPS.items():
        a = _addr(prog, key)
        if a is not None:
            out[a] = sym
    return out
