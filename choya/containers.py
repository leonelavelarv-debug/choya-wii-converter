"""Lectores de TPL y BRRES (TEX0/PLT0) que devuelven GXTexture."""
from __future__ import annotations

import struct

from .gxtex import BLOCK, PALETTE_FORMATS, GXTexture, chain_size, decode_palette, level_size

TPL_MAGIC = b"\x00\x20\xAF\x30"
BRRES_MAGIC = b"bres"


def is_tpl(data: bytes) -> bool:
    return data[:4] == TPL_MAGIC


def is_brres(data: bytes) -> bool:
    return data[:4] == BRRES_MAGIC


def _fit_mips(fmt: int, w: int, h: int, wanted: int, available: int) -> int:
    """Número de mips que caben realmente en los datos disponibles."""
    mips = max(1, wanted)
    while mips > 1 and chain_size(fmt, w, h, mips) > available:
        mips -= 1
    return mips


def read_tpl(data: bytes, source: str = "") -> list[GXTexture]:
    if not is_tpl(data):
        raise ValueError("no es TPL")
    count, table = struct.unpack_from(">II", data, 4)
    out = []
    for i in range(count):
        img_off, pal_off = struct.unpack_from(">II", data, table + i * 8)
        h, w, fmt, data_off = struct.unpack_from(">HHII", data, img_off)
        min_filter = struct.unpack_from(">I", data, img_off + 0x14)[0]
        max_lod = data[img_off + 0x22]
        if fmt not in BLOCK:
            continue
        wanted = (max_lod + 1) if min_filter >= 2 else 1
        avail = len(data) - data_off
        mips = _fit_mips(fmt, w, h, wanted, avail)
        size = chain_size(fmt, w, h, mips)
        if size > avail:
            continue
        tex = GXTexture(name=f"{source}#{i}", fmt=fmt, width=w, height=h, mips=mips,
                        data=bytes(data[data_off:data_off + size]), source=source)
        if fmt in PALETTE_FORMATS and pal_off:
            n, _unpacked, _pad, pfmt, pdata = struct.unpack_from(">HBBII", data, pal_off)
            tex.palette = decode_palette(data[pdata:pdata + n * 2], n, pfmt)
            tex.palette_fmt = pfmt
            tex.palette_raw = bytes(data[pdata:pdata + n * 2])
        out.append(tex)
    return out


def _read_name(data: bytes, base: int, ptr: int) -> str:
    if ptr == 0:
        return ""
    off = base + ptr
    end = data.index(b"\x00", off)
    return data[off:end].decode("shift_jis", errors="replace")


def _index_group(data: bytes, off: int) -> list[tuple[str, int]]:
    """Lee un grupo de índice de NW4R. Devuelve [(nombre, offset_absoluto)] sin la entrada raíz."""
    _size, count = struct.unpack_from(">II", data, off)
    out = []
    for i in range(1, count + 1):
        e = off + 8 + i * 16
        _id, _unk, _l, _r, name_ptr, data_ptr = struct.unpack_from(">HHHHII", data, e)
        out.append((_read_name(data, off, name_ptr), off + data_ptr))
    return out


def brres_sections(data: bytes) -> dict[str, list[tuple[str, int]]]:
    """Devuelve {carpeta: [(nombre, offset)]}, p. ej. 'Textures(NW4R)'."""
    if not is_brres(data):
        raise ValueError("no es BRRES")
    bom = struct.unpack_from(">H", data, 4)[0]
    if bom != 0xFEFF:
        raise ValueError("BRRES con orden de bytes no soportado")
    root_off = struct.unpack_from(">H", data, 0x0C)[0]
    if data[root_off:root_off + 4] != b"root":
        raise ValueError("BRRES sin nodo raíz")
    sections = {}
    for folder, grp in _index_group(data, root_off + 8):
        try:
            sections[folder] = _index_group(data, grp)
        except (struct.error, ValueError):
            sections[folder] = []
    return sections


def read_brres(data: bytes, source: str = "") -> tuple[list[GXTexture], dict[str, int]]:
    """Extrae texturas (TEX0 + PLT0) y cuenta el resto de subarchivos por tipo."""
    sections = brres_sections(data)
    palettes = {}
    for name, off in sections.get("Palettes(NW4R)", []):
        if data[off:off + 4] != b"PLT0":
            continue
        data_off, = struct.unpack_from(">I", data, off + 0x10)
        pfmt, = struct.unpack_from(">I", data, off + 0x18)
        n, = struct.unpack_from(">H", data, off + 0x1C)
        raw = bytes(data[off + data_off: off + data_off + n * 2])
        palettes[name] = (decode_palette(raw, n, pfmt), pfmt, raw)

    textures = []
    for name, off in sections.get("Textures(NW4R)", []):
        if data[off:off + 4] != b"TEX0":
            continue
        data_off, = struct.unpack_from(">I", data, off + 0x10)
        w, h, fmt, nimg = struct.unpack_from(">HHII", data, off + 0x1C)
        if fmt not in BLOCK or w == 0 or h == 0:
            continue
        start = off + data_off
        tex_size, = struct.unpack_from(">I", data, off + 4)
        avail = min(len(data) - start, max(0, tex_size - data_off)) or (len(data) - start)
        mips = _fit_mips(fmt, w, h, nimg, avail)
        size = chain_size(fmt, w, h, mips)
        if size > len(data) - start:
            continue
        tex = GXTexture(name=f"{source}/{name}", fmt=fmt, width=w, height=h, mips=mips,
                        data=bytes(data[start:start + size]), source=source)
        if fmt in PALETTE_FORMATS:
            if name in palettes:
                tex.palette, tex.palette_fmt, tex.palette_raw = palettes[name]
            else:
                tex.notes.append("paleta no encontrada")
        textures.append(tex)

    counts = {folder: len(items) for folder, items in sections.items()}
    return textures, counts


def build_tpl(images: list[tuple[int, int, int, bytes, int]]) -> bytes:
    """Construye un TPL (para pruebas). images = [(fmt, w, h, datos, mips)], sin paletas."""
    header = bytearray(TPL_MAGIC + struct.pack(">II", len(images), 0x0C))
    table_size = 8 * len(images)
    img_hdrs_off = 0x0C + table_size
    data_off = (img_hdrs_off + 0x24 * len(images) + 0x1F) & ~0x1F
    table = bytearray()
    hdrs = bytearray()
    blobs = bytearray()
    for i, (fmt, w, h, blob, mips) in enumerate(images):
        table.extend(struct.pack(">II", img_hdrs_off + i * 0x24, 0))
        off = data_off + len(blobs)
        min_filter = 5 if mips > 1 else 1
        hdrs.extend(struct.pack(">HHIIIIIIfBBBB", h, w, fmt, off, 0, 0, min_filter, 1, 0.0,
                                0, 0, mips - 1, 0))
        blobs.extend(blob)
        blobs.extend(b"\x00" * ((-len(blobs)) % 0x20))
    out = header + table + hdrs
    out.extend(b"\x00" * (data_off - len(out)))
    return bytes(out + blobs)


__all__ = ["read_tpl", "read_brres", "brres_sections", "is_tpl", "is_brres", "build_tpl",
           "level_size"]
