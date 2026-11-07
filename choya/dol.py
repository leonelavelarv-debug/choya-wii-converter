"""Lectura de main.dol (GameCube/Wii) y de las bases SDA que instala el arranque.

El DOL tiene 7 secciones de código y 11 de datos. Cada una dice dónde está en el archivo,
en qué dirección se carga y cuánto mide. Aquí solo se lee: nunca se modifica el DOL.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field


@dataclass
class Section:
    kind: str          # "text" o "data"
    index: int
    offset: int
    address: int
    size: int

    @property
    def end(self) -> int:
        return self.address + self.size


@dataclass
class Dol:
    data: bytes
    sections: list[Section] = field(default_factory=list)
    bss_address: int = 0
    bss_size: int = 0
    entry: int = 0

    def section_at(self, address: int) -> Section | None:
        for s in self.sections:
            if s.address <= address < s.end:
                return s
        return None

    def u32(self, address: int) -> int:
        s = self.section_at(address)
        if s is None or address + 4 > s.end:
            raise KeyError(f"la dirección {address:08X} no está en ninguna sección del DOL")
        return struct.unpack_from(">I", self.data, s.offset + address - s.address)[0]

    @property
    def text_end(self) -> int:
        return max((s.end for s in self.sections if s.kind == "text"), default=0)

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()

    @property
    def sha1(self) -> str:
        return hashlib.sha1(self.data).hexdigest()


def read_dol(data: bytes) -> Dol:
    if len(data) < 0x100:
        raise ValueError("main.dol demasiado pequeño")
    offs = struct.unpack_from(">18I", data, 0x00)
    addrs = struct.unpack_from(">18I", data, 0x48)
    sizes = struct.unpack_from(">18I", data, 0x90)
    bss_addr, bss_size, entry = struct.unpack_from(">3I", data, 0xD8)
    dol = Dol(data=data, bss_address=bss_addr, bss_size=bss_size, entry=entry)
    for i in range(18):
        if sizes[i] == 0:
            continue
        if offs[i] + sizes[i] > len(data):
            raise ValueError(f"sección {i} fuera del archivo")
        kind = "text" if i < 7 else "data"
        dol.sections.append(Section(kind, i if i < 7 else i - 7, offs[i], addrs[i], sizes[i]))
    if dol.section_at(entry) is None:
        raise ValueError(f"el punto de entrada {entry:08X} no está en ninguna sección")
    return dol


# --- desensamblado mínimo para el arranque de CodeWarrior -------------------------------

def _bl_target(pc: int, ins: int) -> int | None:
    if ins >> 26 != 18 or ins & 3 != 1:       # b con LK=1 y AA=0
        return None
    li = ins & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (pc + li) & 0xFFFFFFFF


def register_constants(dol: Dol, start: int, limit: int = 96) -> dict[int, int]:
    """Sigue lis/ori/addi/addis desde `start` hasta blr y devuelve el valor final de cada registro."""
    regs: dict[int, int] = {}
    pc = start
    for _ in range(limit):
        ins = dol.u32(pc)
        op, rd, ra, imm = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, ins & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if ins == 0x4E800020:                  # blr
            break
        if op == 15:                           # addis (lis si rA=0)
            base = 0 if ra == 0 else regs.get(ra)
            if base is not None:
                regs[rd] = (base + (simm << 16)) & 0xFFFFFFFF
        elif op == 14:                         # addi (li si rA=0)
            base = 0 if ra == 0 else regs.get(ra)
            if base is not None:
                regs[rd] = (base + simm) & 0xFFFFFFFF
        elif op == 24:                         # ori rA, rS, imm (aquí rd es rS)
            if rd in regs:
                regs[ra] = regs[rd] | imm
        elif op == 18 and ins & 1:             # otra llamada: lo que haya después ya no es el arranque
            break
        pc += 4
    return regs


@dataclass
class BootInfo:
    entry: int
    init_registers: int
    sda_base: int        # r13
    sda2_base: int       # r2
    stack: int | None    # r1


def boot_info(dol: Dol, init_registers: int | None = None) -> BootInfo:
    """Busca __init_registers (el primer `bl` desde la entrada) y lee r13, r2 y r1."""
    if init_registers is None:
        pc = dol.entry
        for _ in range(16):
            t = _bl_target(pc, dol.u32(pc))
            if t is not None:
                init_registers = t
                break
            pc += 4
        if init_registers is None:
            raise ValueError("no encontré la llamada a __init_registers al inicio del arranque")
    regs = register_constants(dol, init_registers)
    if 13 not in regs or 2 not in regs:
        raise ValueError(f"__init_registers en {init_registers:08X} no fija r13 y r2 con constantes")
    return BootInfo(dol.entry, init_registers, regs[13], regs[2], regs.get(1))
