"""Descompresores usados por juegos de Wii: Yaz0, Yay0 y LZ77 de Nintendo (0x10 / 0x11)."""
from __future__ import annotations

import struct


class CompressionError(ValueError):
    pass


def detect(data: bytes) -> str | None:
    """Devuelve el nombre de la compresión o None si los datos no están comprimidos."""
    if data[:4] == b"Yaz0":
        return "yaz0"
    if data[:4] == b"Yay0":
        return "yay0"
    if data[:4] == b"LZ77" and len(data) > 8 and data[4] in (0x10, 0x11):
        return "lz77-header"
    if len(data) >= 4 and data[0] in (0x10, 0x11):
        size = data[1] | (data[2] << 8) | (data[3] << 16)
        # Heurística: tamaño plausible y al menos tan grande como la entrada comprimida.
        if 0 < size < 64 * 1024 * 1024 and size >= len(data) // 2:
            return "lz77"
    return None


def yaz0(data: bytes) -> bytes:
    if data[:4] != b"Yaz0":
        raise CompressionError("no es Yaz0")
    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(size)
    src, dst = 16, 0
    n = len(data)
    while dst < size:
        if src >= n:
            raise CompressionError("Yaz0 truncado")
        code = data[src]
        src += 1
        for bit in range(7, -1, -1):
            if dst >= size:
                break
            if code & (1 << bit):
                out[dst] = data[src]
                dst += 1
                src += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                length = b1 >> 4
                if length == 0:
                    length = data[src] + 0x12
                    src += 1
                else:
                    length += 2
                start = dst - dist
                if start < 0:
                    raise CompressionError("Yaz0: referencia fuera de rango")
                for i in range(length):
                    if dst >= size:
                        break
                    out[dst] = out[start + i]
                    dst += 1
    return bytes(out)


def yay0(data: bytes) -> bytes:
    if data[:4] != b"Yay0":
        raise CompressionError("no es Yay0")
    size, link_off, chunk_off = struct.unpack_from(">III", data, 4)
    out = bytearray(size)
    mask_pos, link_pos, chunk_pos = 16, link_off, chunk_off
    dst = 0
    mask = 0
    bits = 0
    while dst < size:
        if bits == 0:
            mask = struct.unpack_from(">I", data, mask_pos)[0]
            mask_pos += 4
            bits = 32
        if mask & 0x80000000:
            out[dst] = data[chunk_pos]
            chunk_pos += 1
            dst += 1
        else:
            link = struct.unpack_from(">H", data, link_pos)[0]
            link_pos += 2
            dist = (link & 0x0FFF) + 1
            length = link >> 12
            if length == 0:
                length = data[chunk_pos] + 0x12
                chunk_pos += 1
            else:
                length += 2
            start = dst - dist
            for i in range(length):
                if dst >= size:
                    break
                out[dst] = out[start + i]
                dst += 1
        mask = (mask << 1) & 0xFFFFFFFF
        bits -= 1
    return bytes(out)


def lz77(data: bytes) -> bytes:
    """LZ77 de Nintendo: tipo 0x10 (LZ10) y 0x11 (LZ11). Acepta la cabecera opcional 'LZ77'."""
    if data[:4] == b"LZ77":
        data = data[4:]
    kind = data[0]
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    src = 4
    if size == 0:
        size = struct.unpack_from("<I", data, 4)[0]
        src = 8
    out = bytearray(size)
    dst = 0
    while dst < size:
        flags = data[src]
        src += 1
        for bit in range(7, -1, -1):
            if dst >= size:
                break
            if not flags & (1 << bit):
                out[dst] = data[src]
                src += 1
                dst += 1
                continue
            if kind == 0x10:
                b1, b2 = data[src], data[src + 1]
                src += 2
                length = (b1 >> 4) + 3
                dist = ((b1 & 0x0F) << 8 | b2) + 1
            elif kind == 0x11:
                b1 = data[src]
                ind = b1 >> 4
                if ind == 0:
                    b2, b3 = data[src + 1], data[src + 2]
                    src += 3
                    length = ((b1 & 0x0F) << 4 | (b2 >> 4)) + 0x11
                    dist = ((b2 & 0x0F) << 8 | b3) + 1
                elif ind == 1:
                    b2, b3, b4 = data[src + 1], data[src + 2], data[src + 3]
                    src += 4
                    length = ((b1 & 0x0F) << 12 | b2 << 4 | (b3 >> 4)) + 0x111
                    dist = ((b3 & 0x0F) << 8 | b4) + 1
                else:
                    b2 = data[src + 1]
                    src += 2
                    length = ind + 1
                    dist = ((b1 & 0x0F) << 8 | b2) + 1
            else:
                raise CompressionError(f"LZ77 tipo desconocido 0x{kind:02X}")
            start = dst - dist
            if start < 0:
                raise CompressionError("LZ77: referencia fuera de rango")
            for i in range(length):
                if dst >= size:
                    break
                out[dst] = out[start + i]
                dst += 1
    return bytes(out)


def decompress(data: bytes) -> tuple[bytes, str | None]:
    """Descomprime si reconoce el formato. Devuelve (datos, nombre_del_formato)."""
    kind = detect(data)
    try:
        if kind == "yaz0":
            return yaz0(data), kind
        if kind == "yay0":
            return yay0(data), kind
        if kind in ("lz77", "lz77-header"):
            return lz77(data), "lz77"
    except (IndexError, CompressionError, struct.error):
        if kind == "lz77":  # la heurística LZ77 puede equivocarse: tratar como no comprimido
            return data, None
        raise
    return data, None
