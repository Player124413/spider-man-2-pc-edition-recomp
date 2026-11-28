"""Расшифровка лога падения Nintendo Switch (snx.log) в читаемый отчёт.

Извлекает смещения хоста `host pc/lr/frames` из вывода __libnx_exception_handler,
прогоняет их через addr2line (aarch64-none-elf-addr2line или llvm-addr2line) по
ELF-файлу snx_boot.elf, расшифровывает гостевые адреса (f_<hex>, ebp chain, stack scan,
last functions, регистры) по recomp/addrmap.json и каталогу экспортов (--exports),
а также выделяет текст фатальной ошибки движка GErrorHist.

Пример запуска (совместим с python3 -I):
    python3 -I tools/probe/crash.py snx.log \\
        --elf build/switch/host/boot/snx_boot.elf \\
        --exports build/kb/exports
"""
import argparse
import bisect
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]

RE_HOST_LINE = re.compile(
    r"\[boot\]\s+host pc \+(?:0x)?([0-9a-fA-F]+),\s*lr \+(?:0x)?([0-9a-fA-F]+).*?frames:(.*)$"
)
RE_HEX_OFF = re.compile(r"\+(?:0x)?([0-9a-fA-F]+)")
RE_TRAP = re.compile(r"\[rt\]\s+TRAP at ([0-9a-fA-F]+):\s*(.*)$")
RE_HOST_EXC = re.compile(r"\[boot\]\s+(host exception .*|guest access violation .*)$")
RE_CRITICAL = re.compile(r"\[boot\]\s+(Critical Error|.*Error.*):\s*(.*)$")
RE_GERROR = re.compile(r"\[boot\]\s+GErrorHist:\s*(.*)$")
RE_REGS = re.compile(
    r"eax=([0-9a-fA-F]{8})\s+ecx=([0-9a-fA-F]{8})\s+edx=([0-9a-fA-F]{8})\s+ebx=([0-9a-fA-F]{8})\s+"
    r"esp=([0-9a-fA-F]{8})\s+ebp=([0-9a-fA-F]{8})\s+esi=([0-9a-fA-F]{8})\s+edi=([0-9a-fA-F]{8})"
)
RE_STACK_LINE = re.compile(r"^\s+stack:\s*(.+)$")
RE_LAST_FUNCS = re.compile(r"^\s+last functions:\s*(.+)$")
RE_FRAME_ENTRY = re.compile(r"^\s+\[([0-9a-fA-F]{8})\]\s+([0-9a-fA-F]{8})(?:\s+(.*))?$")
RE_GUEST_FN = re.compile(r"\bf_([0-9a-fA-F]{8})\b")


def load_addrmap():
    path = ROOT / "recomp" / "addrmap.json"
    if not path.is_file():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    mods = [(int(base, 16), name) for name, base in data.get("modules", {}).items()]
    mods.sort()
    return mods


class GuestSymbols:
    """Расшифровка гостевых адресов по карте модулей и TSV-экспортам (tools/recon/exports.py)."""

    def __init__(self, exports_dir=None):
        self.mods = load_addrmap()
        self.mod_bases = [b for b, _ in self.mods]
        self.syms = []       # [(va, mod_name, sym_name), ...]
        self.sym_addrs = []
        if exports_dir:
            self._load_exports(Path(exports_dir))

    def _load_exports(self, exports_dir):
        if not exports_dir.is_dir():
            return
        stem_to_mod = {name.lower().replace(".", "_"): name for _, name in self.mods}
        for tsv in sorted(exports_dir.glob("*.tsv")):
            mod_name = stem_to_mod.get(tsv.stem.lower(), tsv.stem)
            with tsv.open(encoding="utf-8", errors="replace") as f:
                for ln in f:
                    parts = ln.rstrip("\r\n").split("\t")
                    if len(parts) < 3 or parts[0] == "addr":
                        continue
                    try:
                        va = int(parts[0], 16)
                    except ValueError:
                        continue
                    label = (parts[3] if len(parts) >= 4 and parts[3] else parts[2]).strip()
                    if label:
                        self.syms.append((va, mod_name, label))
        self.syms.sort(key=lambda x: x[0])
        self.sym_addrs = [x[0] for x in self.syms]

    def module_of(self, va):
        if va < 0x10000000 or va >= 0x20000000 or not self.mod_bases:
            return None
        idx = bisect.bisect_right(self.mod_bases, va) - 1
        if idx < 0:
            return None
        base, name = self.mods[idx]
        if va - base >= 0x01000000:
            return None
        return name, base

    def format_va(self, va):
        if self.sym_addrs:
            idx = bisect.bisect_right(self.sym_addrs, va) - 1
            if idx >= 0:
                s_va, mod, label = self.syms[idx]
                delta = va - s_va
                if delta == 0:
                    return f"{mod}!0x{va:08x} ({label})"
                if delta < 0x10000:
                    return f"{mod}!0x{va:08x} ({label}+0x{delta:x})"
        mod_info = self.module_of(va)
        if mod_info:
            mod, base = mod_info
            return f"{mod}!0x{va:08x} (+0x{va - base:x})"
        return f"0x{va:08x}"

    def annotate_symbol(self, text):
        def repl(m):
            va = int(m.group(1), 16)
            return f"{m.group(0)} [{self.format_va(va)}]"

        return RE_GUEST_FN.sub(repl, text)


def find_addr2line(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) or shutil.which(explicit) else None
    dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    candidates = [
        os.path.join(dkp, "devkitA64", "bin", "aarch64-none-elf-addr2line"),
        str(ROOT / "tools" / "llvm" / "bin" / "llvm-addr2line"),
        "aarch64-none-elf-addr2line",
        "llvm-addr2line",
        "addr2line",
    ]
    for cand in candidates:
        if os.path.sep in cand:
            if os.path.isfile(cand) and os.access(cand, os.X_OK):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None


def resolve_elf_addrs(tool, elf_path, offsets):
    """Прогнать список смещений ELF через addr2line (-f -C -e) -> dict[offset] = (func, fileline)."""
    if not tool or not elf_path or not os.path.isfile(elf_path) or not offsets:
        return {}
    uniq = sorted(set(offsets))
    cmd = [tool, "-f", "-C", "-e", str(elf_path), *[f"0x{a:x}" for a in uniq]]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", check=False)
    if proc.returncode != 0:
        return {}
    lines = proc.stdout.splitlines()
    res = {}
    for idx, addr in enumerate(uniq):
        if 2 * idx + 1 < len(lines):
            res[addr] = (lines[2 * idx].strip(), lines[2 * idx + 1].strip())
    return res


def parse_crashes(text):
    """Разбить лог на события падения (каждый блок — от host pc / TRAP до следующего)."""
    crashes = []
    cur = None
    section = None

    def ensure():
        nonlocal cur
        if cur is None:
            cur = {
                "host_pc": None,
                "host_lr": None,
                "host_frames": [],
                "trap_addr": None,
                "trap_what": None,
                "host_exc": None,
                "regs": None,
                "raw_stack": [],
                "last_funcs": [],
                "ebp_chain": [],
                "stack_scan": [],
                "gerror": [],
            }
            crashes.append(cur)
        return cur

    for line in text.splitlines():
        m_host = RE_HOST_LINE.search(line)
        if m_host:
            if cur is not None and (cur["host_pc"] is not None or cur["trap_what"] is not None):
                cur = None
            c = ensure()
            c["host_pc"] = int(m_host.group(1), 16)
            c["host_lr"] = int(m_host.group(2), 16)
            c["host_frames"] = [int(x, 16) for x in RE_HEX_OFF.findall(m_host.group(3))]
            section = None
            continue

        m_trap = RE_TRAP.search(line)
        if m_trap:
            if cur is not None and cur["trap_what"] is not None:
                cur = None
            c = ensure()
            c["trap_addr"] = int(m_trap.group(1), 16)
            c["trap_what"] = m_trap.group(2).strip()
            section = None
            continue

        m_exc = RE_HOST_EXC.search(line)
        if m_exc:
            c = ensure()
            c["host_exc"] = m_exc.group(1).strip()
            section = None
            continue

        m_gerr = RE_GERROR.search(line)
        if m_gerr:
            c = ensure()
            c["gerror"].append(m_gerr.group(1).strip())
            section = "gerror"
            continue

        m_crit = RE_CRITICAL.search(line)
        if m_crit and "GErrorHist" not in line and "host pc" not in line:
            c = ensure()
            if not c["trap_what"]:
                c["trap_what"] = f"{m_crit.group(1)}: {m_crit.group(2).strip()}"
            if m_crit.group(2).strip():
                c["gerror"].append(m_crit.group(2).strip())
            section = None
            continue

        if cur is None:
            continue

        m_regs = RE_REGS.search(line)
        if m_regs:
            names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
            cur["regs"] = {k: int(v, 16) for k, v in zip(names, m_regs.groups())}
            section = None
            continue

        m_st = RE_STACK_LINE.match(line)
        if m_st:
            cur["raw_stack"] = [int(x, 16) for x in m_st.group(1).split() if re.fullmatch(r"[0-9a-fA-F]{1,8}", x)]
            section = None
            continue

        m_lf = RE_LAST_FUNCS.match(line)
        if m_lf:
            cur["last_funcs"] = [int(x, 16) for x in m_lf.group(1).split() if re.fullmatch(r"[0-9a-fA-F]{1,8}", x)]
            section = None
            continue

        if line.strip() == "ebp chain:":
            section = "ebp"
            continue
        if line.strip() == "stack scan:":
            section = "scan"
            continue

        if section in ("ebp", "scan"):
            m_fe = RE_FRAME_ENTRY.match(line)
            if m_fe:
                entry = (int(m_fe.group(1), 16), int(m_fe.group(2), 16), (m_fe.group(3) or "").strip())
                if section == "ebp":
                    cur["ebp_chain"].append(entry)
                else:
                    cur["stack_scan"].append(entry)
                continue
            section = None
        elif section == "gerror":
            if line.startswith("[") or not line.strip():
                section = None
            else:
                cur["gerror"].append(line.strip())

    return crashes


def format_crash(idx, total, crash, syms, a2l_tool, elf_path):
    out = []
    out.append(f"=== Падение #{idx + 1} из {total} ===")
    if crash["trap_what"]:
        addr_s = syms.format_va(crash["trap_addr"]) if crash["trap_addr"] else "0x00000000"
        out.append(f"Причина: TRAP at {addr_s}: {crash['trap_what']}")
    elif crash["host_exc"]:
        out.append(f"Причина: {crash['host_exc']}")

    if crash["gerror"]:
        out.append("\n[GErrorHist]")
        full_err = "\n".join(crash["gerror"])
        out.append(full_err)
        if "<-" in full_err:
            parts = [p.strip() for p in full_err.split("<-") if p.strip()]
            if len(parts) > 1:
                out.append("Цепочка истории UE2:")
                for k, p in enumerate(parts):
                    out.append(f"  {k:2d}. {p}")

    # Стек хоста (Switch ELF)
    if crash["host_pc"] is not None:
        out.append("\n[Стек хоста (ELF)]")
        # Для pc адрес точный, для lr и возвратов из кадров — адрес инструкции вызова (ret - 4)
        entries = [("pc", crash["host_pc"], crash["host_pc"])]
        if crash["host_lr"] is not None:
            lr_q = crash["host_lr"] - 4 if crash["host_lr"] >= 4 else crash["host_lr"]
            entries.append(("lr", crash["host_lr"], lr_q))
        for n, fr in enumerate(crash["host_frames"]):
            fr_q = fr - 4 if fr >= 4 else fr
            entries.append((f"#{n}", fr, fr_q))

        resolved = resolve_elf_addrs(a2l_tool, elf_path, [q for _, _, q in entries])
        if not resolved and elf_path:
            if not os.path.isfile(elf_path):
                out.append(f"  (предупреждение: ELF не найден: {elf_path})")
            elif not a2l_tool:
                out.append("  (предупреждение: не найден aarch64-none-elf-addr2line / llvm-addr2line)")

        for tag, raw_off, query_off in entries:
            if query_off in resolved:
                func, loc = resolved[query_off]
                func_ann = syms.annotate_symbol(func)
                out.append(f"  {tag:4s} +0x{raw_off:08x}  {func_ann}  ({loc})")
            else:
                out.append(f"  {tag:4s} +0x{raw_off:08x}")

    # Регистры гостя
    if crash["regs"]:
        r = crash["regs"]
        out.append("\n[Регистры гостя]")
        out.append(
            f"  eax={r['eax']:08x} ecx={r['ecx']:08x} edx={r['edx']:08x} ebx={r['ebx']:08x}"
        )
        out.append(
            f"  esp={r['esp']:08x} ebp={r['ebp']:08x} esi={r['esi']:08x} edi={r['edi']:08x}"
        )
        for name, val in r.items():
            if syms.module_of(val):
                out.append(f"  {name} -> {syms.format_va(val)}")

    # Последние гостевые функции (SNX_TRACE)
    if crash["last_funcs"]:
        out.append("\n[Последние функции гостя]")
        for va in crash["last_funcs"]:
            out.append(f"  {syms.format_va(va)}")

    # Цепочка ebp и скан стека
    if crash["ebp_chain"]:
        out.append("\n[Гостевой стек (ebp chain)]")
        for fp, ret_va, orig_sym in crash["ebp_chain"]:
            sym = syms.format_va(ret_va)
            extra = f"  [{orig_sym}]" if orig_sym and orig_sym not in sym else ""
            out.append(f"  [{fp:08x}] {sym}{extra}")

    if crash["stack_scan"]:
        out.append("\n[Скан гостевого стека]")
        for sp, ret_va, orig_sym in crash["stack_scan"]:
            sym = syms.format_va(ret_va)
            extra = f"  [{orig_sym}]" if orig_sym and orig_sym not in sym else ""
            out.append(f"  [{sp:08x}] {sym}{extra}")

    if crash["raw_stack"]:
        hits = [va for va in crash["raw_stack"] if syms.module_of(va)]
        if hits:
            out.append("\n[Адреса модулей на вершине стека]")
            for va in hits:
                out.append(f"  {syms.format_va(va)}")

    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="путь к файлу лога падения (snx.log) или '-' для чтения из stdin")
    ap.add_argument("--elf", default="build/switch/host/boot/snx_boot.elf",
                    help="путь к snx_boot.elf с отладочной информацией")
    ap.add_argument("--exports", default=None,
                    help="каталог TSV-экспортов (build/kb/exports от tools/recon/exports.py)")
    ap.add_argument("--addr2line", default=None,
                    help="путь к aarch64-none-elf-addr2line или llvm-addr2line (автопоиск по умолчанию)")
    ap.add_argument("--all", action="store_true", help="показать все падения из лога (по умолчанию только последнее)")
    args = ap.parse_args(argv)

    if args.log == "-":
        text = sys.stdin.read()
    else:
        log_path = Path(args.log)
        if not log_path.is_file():
            print(f"Ошибка: файл лога не найден: {log_path}", file=sys.stderr)
            return 2
        text = log_path.read_text(encoding="utf-8", errors="replace")

    crashes = parse_crashes(text)
    if not crashes:
        print("В логе не найдено записей о падении (host pc / TRAP / GErrorHist).", file=sys.stderr)
        return 1

    syms = GuestSymbols(args.exports)
    a2l = find_addr2line(args.addr2line)
    selected = list(enumerate(crashes)) if args.all else [(len(crashes) - 1, crashes[-1])]
    reports = [format_crash(i, len(crashes), c, syms, a2l, args.elf) for i, c in selected]
    print("\n\n".join(reports))
    return 0


if __name__ == "__main__":
    sys.exit(main())
