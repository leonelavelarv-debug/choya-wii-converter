"""Formato de paquete .wlpk (little-endian, para el runtime ARM64).

Cabecera (64 bytes):
  0  char[4] "WLPK"      4  u32 versión (1)     8  u32 número de entradas   12 u32 banderas
  16 u64 offset de la TOC    24 u64 offset de nombres   32 u64 tamaño de nombres
  40 u64 offset del manifiesto JSON   48 u64 tamaño del manifiesto   56 u64 reservado
Entrada (48 bytes), ordenadas por clave para búsqueda binaria:
  0 u8 tipo   1 u8 formato   2 u8 mips|canales   3 u8 banderas   4 u32 offset del nombre
  8 u64 clave   16 u64 offset de datos   24 u64 tamaño de datos
  32 u16 ancho   34 u16 alto   36 u32 extra0   40 u32 extra1   44 u32 extra2
  textura: extra0 = formato GX original, extra1 = crc32 del nivel 0 original, extra2 = tamaño del nivel 0
  audio:   extra0 = frecuencia, extra1 = inicio de bucle (0xFFFFFFFF = sin bucle), extra2 = cuadros
"""
from __future__ import annotations

import json
import struct
import zlib
from dataclasses import dataclass

MAGIC = b"WLPK"
VERSION = 1
HEADER = struct.Struct("<4sIII QQQQQQ")
ENTRY = struct.Struct("<BBBBIQQQHHIII")
assert HEADER.size == 64 and ENTRY.size == 48

KIND_TEXTURE = 1
KIND_AUDIO = 2

AUDIO_PCM16 = 1
AUDIO_DSPADPCM = 2   # ver audio.adpcm_payload; el runtime lo decodifica al vuelo

TEXFLAG_SWIZZLE_I = 1     # R -> RGBA (I4/I8)
TEXFLAG_SWIZZLE_IA = 2    # R -> RGB, G -> A (IA4/IA8)
AUDIOFLAG_LOOP = 1

FLAG_GLES3 = 1


def texture_key(level0: bytes, width: int, height: int, gx_fmt: int) -> int:
    """Clave que el runtime recalcula en GXInitTexObj: crc32 del nivel 0 + dimensiones + formato."""
    crc = zlib.crc32(level0) & 0xFFFFFFFF
    return (crc << 32) | (((width - 1) & 0x3FF) << 22) | (((height - 1) & 0x3FF) << 12) | ((gx_fmt & 0xF) << 8)


def path_key(path: str) -> int:
    """FNV-1a de 64 bits de la ruta del disco normalizada ('/carpeta/archivo', minúsculas)."""
    p = "/" + path.replace("\\", "/").lstrip("/").lower()
    h = 0xCBF29CE484222325
    for b in p.encode("utf-8"):
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


@dataclass
class Entry:
    kind: int
    fmt: int
    count: int
    flags: int
    key: int
    name: str
    data: bytes
    width: int = 0
    height: int = 0
    extra0: int = 0
    extra1: int = 0
    extra2: int = 0


class PackWriter:
    def __init__(self, flags: int = 0):
        self.flags = flags
        self.entries: dict[int, Entry] = {}
        self.duplicates = 0

    def add(self, e: Entry) -> bool:
        if e.key in self.entries:
            self.duplicates += 1
            return False
        self.entries[e.key] = e
        return True

    def write(self, path: str, manifest: dict) -> int:
        entries = sorted(self.entries.values(), key=lambda e: e.key)
        names = bytearray()
        name_offs = []
        for e in entries:
            name_offs.append(len(names))
            names.extend(e.name.encode("utf-8") + b"\x00")
        toc_off = HEADER.size
        names_off = toc_off + ENTRY.size * len(entries)
        man = json.dumps(manifest, ensure_ascii=False, indent=1).encode("utf-8")
        man_off = names_off + len(names)
        data_off = (man_off + len(man) + 15) & ~15
        offsets = []
        cur = data_off
        for e in entries:
            offsets.append(cur)
            cur = (cur + len(e.data) + 15) & ~15
        with open(path, "wb") as f:
            f.write(HEADER.pack(MAGIC, VERSION, len(entries), self.flags, toc_off, names_off, len(names),
                                man_off, len(man), 0))
            for e, no, do in zip(entries, name_offs, offsets):
                f.write(ENTRY.pack(e.kind, e.fmt, e.count, e.flags, no, e.key, do, len(e.data),
                                   e.width, e.height, e.extra0, e.extra1, e.extra2))
            f.write(names)
            f.write(man)
            for e, do in zip(entries, offsets):
                f.write(b"\x00" * (do - f.tell()))
                f.write(e.data)
            return f.tell()


def read_pack(path: str) -> tuple[dict, list[dict]]:
    """Lector de referencia (Python) para pruebas y para `choya info`."""
    with open(path, "rb") as f:
        blob = f.read()
    magic, ver, count, flags, toc, noff, nsize, moff, msize, _ = HEADER.unpack_from(blob, 0)
    if magic != MAGIC or ver != VERSION:
        raise ValueError("no es un paquete .wlpk compatible")
    manifest = json.loads(blob[moff:moff + msize].decode("utf-8"))
    entries = []
    for i in range(count):
        k, fmt, cnt, fl, no, key, do, ds, w, h, e0, e1, e2 = ENTRY.unpack_from(blob, toc + i * ENTRY.size)
        end = blob.index(b"\x00", noff + no)
        entries.append(dict(kind=k, fmt=fmt, count=cnt, flags=fl, key=key, offset=do, size=ds,
                            width=w, height=h, extra0=e0, extra1=e1, extra2=e2,
                            name=blob[noff + no:end].decode("utf-8"), data=blob[do:do + ds]))
    manifest["_flags"] = flags
    return manifest, entries
