"""Загрузка PE-образа в гостевую раскладку с ребейзом на базу из addrmap.json."""
import json
import os
import struct
from dataclasses import dataclass, field

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_CNT_CODE = 0x00000020


@dataclass
class Section:
    name: str
    va: int
    vsize: int
    chars: int

    @property
    def end(self):
        return self.va + self.vsize

    @property
    def executable(self):
        return bool(self.chars & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE))


@dataclass
class Import:
    iat_va: int  # адрес слота IAT в гостевой памяти
    dll: str  # имя DLL в нижнем регистре, как в таблице импорта
    name: str | None
    ordinal: int | None

    @property
    def key(self):
        return self.name if self.name is not None else f"#{self.ordinal}"


@dataclass
class Image:
    name: str
    path: str
    base: int
    preferred_base: int
    size: int
    entry: int  # VA точки входа (0, если нет)
    data: bytearray  # весь образ по RVA, релокации уже применены под base
    sections: list[Section]
    exports: dict[str, int]  # имя -> VA
    export_ordinals: dict[int, int]  # ординал -> VA
    forwarders: dict[str, str]
    imports: list[Import]
    relocs: set[int]  # VA 4-байтовых полей с абсолютными адресами (после ребейза)
    has_relocs: bool
    is_dll: bool
    iat_by_va: dict[int, Import] = field(default_factory=dict)

    def contains(self, va):
        return self.base <= va < self.base + self.size

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.va + max(s.vsize, 1):
                return s
        return None

    def is_code(self, va):
        s = self.section_of(va)
        return s is not None and s.executable

    def u8(self, va):
        return self.data[va - self.base]

    def u32(self, va):
        o = va - self.base
        return struct.unpack_from("<I", self.data, o)[0]

    def bytes_at(self, va, n):
        o = va - self.base
        return bytes(self.data[o:o + n])

    def code_sections(self):
        return [s for s in self.sections if s.executable]


def load_addrmap(path=None):
    path = path or os.path.join(HERE, "addrmap.json")
    with open(path) as f:
        m = json.load(f)
    return {k: int(v, 16) for k, v in m["modules"].items()}


def load_image(path, base=None):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"],
    ])
    oh = pe.OPTIONAL_HEADER
    pref = oh.ImageBase
    base = pref if base is None else base
    size = oh.SizeOfImage
    data = bytearray(size)
    hdr = pe.__data__[:oh.SizeOfHeaders]
    data[:len(hdr)] = hdr
    sections = []
    for s in pe.sections:
        raw = s.get_data()
        n = min(len(raw), s.Misc_VirtualSize or len(raw), size - s.VirtualAddress)
        data[s.VirtualAddress:s.VirtualAddress + n] = raw[:n]
        sections.append(Section(s.Name.rstrip(b"\0").decode("latin-1"), base + s.VirtualAddress,
                                s.Misc_VirtualSize, s.Characteristics))

    relocs = set()
    has_relocs = hasattr(pe, "DIRECTORY_ENTRY_BASERELOC")
    delta = (base - pref) & 0xFFFFFFFF
    if has_relocs:
        for blk in pe.DIRECTORY_ENTRY_BASERELOC:
            for e in blk.entries:
                if e.type == 0:  # ABSOLUTE — выравнивание
                    continue
                if e.type != 3:
                    raise ValueError(f"{path}: unsupported reloc type {e.type} at {e.rva:#x}")
                v = struct.unpack_from("<I", data, e.rva)[0]
                struct.pack_into("<I", data, e.rva, (v + delta) & 0xFFFFFFFF)
                relocs.add(base + e.rva)
    elif base != pref:
        raise ValueError(f"{path}: no relocations, cannot rebase {pref:#x} -> {base:#x}")

    exports, export_ordinals, forwarders = {}, {}, {}
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if e.forwarder:
                if e.name:
                    forwarders[e.name.decode("latin-1")] = e.forwarder.decode("latin-1")
                continue
            va = base + e.address
            export_ordinals[e.ordinal] = va
            if e.name:
                exports[e.name.decode("latin-1")] = va

    imports = []
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            dll = d.dll.decode("latin-1").lower()
            for i in d.imports:
                imports.append(Import(base + (i.address - pref), dll,
                                      i.name.decode("latin-1") if i.name else None,
                                      i.ordinal if i.import_by_ordinal else None))

    img = Image(
        name=os.path.basename(path), path=path, base=base, preferred_base=pref, size=size,
        entry=base + oh.AddressOfEntryPoint if oh.AddressOfEntryPoint else 0,
        data=data, sections=sections, exports=exports, export_ordinals=export_ordinals,
        forwarders=forwarders, imports=imports, relocs=relocs, has_relocs=has_relocs,
        is_dll=bool(pe.FILE_HEADER.Characteristics & 0x2000),
    )
    img.iat_by_va = {i.iat_va: i for i in imports}
    pe.close()
    return img


def load_all(game_system_dir, addrmap=None):
    addrmap = addrmap or load_addrmap()
    out = {}
    for name, base in addrmap.items():
        p = os.path.join(game_system_dir, name)
        out[name.lower()] = load_image(p, base)
    return out
