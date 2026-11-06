"""Generadores de datos sintéticos con formato de Wii (no contienen datos de ningún juego)."""
from __future__ import annotations

import struct

import numpy as np

from choya import gxtex
from choya.u8 import build_u8


def tile(img: np.ndarray, bw: int, bh: int) -> np.ndarray:
    """Inverso de gxtex._untile: imagen -> valores en orden de bloque."""
    h, w = img.shape[:2]
    ph, pw = (-h) % bh, (-w) % bw
    img = np.pad(img, ((0, ph), (0, pw)) + ((0, 0),) * (img.ndim - 2))
    H, W = img.shape[:2]
    rest = img.shape[2:]
    v = img.reshape((H // bh, bh, W // bw, bw) + rest).transpose((0, 2, 1, 3) + tuple(range(4, 4 + len(rest))))
    return v.reshape((-1,) + rest)


def encode_gx(fmt: int, rgba: np.ndarray) -> bytes:
    bw, bh, _ = gxtex.BLOCK[fmt]
    r, g, b, a = (rgba[..., i].astype(np.uint32) for i in range(4))
    if fmt == gxtex.I4:
        v = tile((r >> 4).astype(np.uint8), bw, bh)
        return ((v[0::2] << 4) | v[1::2]).astype(np.uint8).tobytes()
    if fmt == gxtex.I8:
        return tile(r.astype(np.uint8), bw, bh).tobytes()
    if fmt == gxtex.IA4:
        return tile((((a >> 4) << 4) | (r >> 4)).astype(np.uint8), bw, bh).tobytes()
    if fmt == gxtex.IA8:
        return tile(((a << 8) | r).astype(">u2"), bw, bh).tobytes()
    if fmt == gxtex.RGB565:
        return tile((((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype(">u2"), bw, bh).tobytes()
    if fmt == gxtex.RGB5A3:
        opaque = 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
        trans = ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
        return tile(np.where(a == 255, opaque, trans).astype(">u2"), bw, bh).tobytes()
    if fmt == gxtex.RGBA8:
        px = tile(rgba, 4, 4).reshape(-1, 16, 4)
        out = np.empty((px.shape[0], 2, 16, 2), np.uint8)
        out[:, 0, :, 0] = px[..., 3]
        out[:, 0, :, 1] = px[..., 0]
        out[:, 1, :, 0] = px[..., 1]
        out[:, 1, :, 1] = px[..., 2]
        return out.tobytes()
    raise NotImplementedError(fmt)


def cmpr_block_solid(c0: int, c1: int, idx_rows: list[int]) -> bytes:
    """Un sub-bloque DXT1 de CMPR: 2 colores RGB565 y 4 filas de índices."""
    return struct.pack(">HH", c0, c1) + bytes(idx_rows)


def gradient(w: int, h: int, alpha: str = "none") -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0] = (x * 255 // max(1, w - 1))
    img[..., 1] = (y * 255 // max(1, h - 1))
    img[..., 2] = 128
    if alpha == "none":
        img[..., 3] = 255
    elif alpha == "binary":
        img[..., 3] = np.where((x // 8 + y // 8) % 2 == 0, 255, 0)
    else:
        img[..., 3] = (x * 255 // max(1, w - 1))
    return img


def yaz0_literal(data: bytes) -> bytes:
    """Yaz0 válido: literales y una referencia hacia atrás por cada grupo cuando hay repetición."""
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + b"\x00" * 8)
    i = 0
    while i < len(data):
        code_pos = len(out)
        out.append(0)
        code = 0
        for bit in range(7, -1, -1):
            if i >= len(data):
                break
            # referencia de 3+ bytes a distancia 1..16 si existe
            best = (0, 0)
            for dist in range(1, min(16, i) + 1):
                n = 0
                while i + n < len(data) and n < 17 and data[i + n] == data[i - dist + n]:
                    n += 1
                if n > best[0]:
                    best = (n, dist)
            if best[0] >= 3:
                n, dist = best
                out.extend(bytes([((n - 2) << 4) | ((dist - 1) >> 8), (dist - 1) & 0xFF]))
                i += n
            else:
                code |= 1 << bit
                out.append(data[i])
                i += 1
        out[code_pos] = code
    return bytes(out)


def build_brres(textures: list[tuple[str, int, int, int, bytes, int]]) -> bytes:
    """BRRES mínimo con 'Textures(NW4R)'. textures = [(nombre, fmt, w, h, datos, mips)]."""
    strings = bytearray()
    str_off = {}

    def s(name: str) -> int:
        if name not in str_off:
            strings.extend(struct.pack(">I", len(name)))
            str_off[name] = len(strings)
            strings.extend(name.encode() + b"\x00")
            strings.extend(b"\x00" * ((-len(strings)) % 4))
        return str_off[name]

    def group(entries: list[tuple[str, int]]) -> tuple[bytes, list[int]]:
        size = 8 + 16 * (len(entries) + 1)
        return size, entries

    root_grp_off = 0x18
    root_size = 8 + 16 * 2
    sub_grp_off = root_grp_off + root_size
    sub_size = 8 + 16 * (len(textures) + 1)
    tex_offs = []
    cur = (sub_grp_off + sub_size + 0x1F) & ~0x1F
    tex_blobs = []
    for name, fmt, w, h, data, mips in textures:
        tex_offs.append(cur)
        hdr = bytearray(b"TEX0" + struct.pack(">IIi", 0x40 + len(data), 1, -cur))
        hdr += struct.pack(">II", 0x40, 0)          # data offset, name offset (se fija después)
        hdr += struct.pack(">IHHII", 0, w, h, fmt, mips)
        hdr += struct.pack(">ff", 0.0, float(mips - 1))
        hdr += b"\x00" * (0x40 - len(hdr))
        tex_blobs.append(bytes(hdr) + data)
        cur = (cur + len(tex_blobs[-1]) + 0x1F) & ~0x1F
    str_base = cur
    s("Textures(NW4R)")
    for t in textures:
        s(t[0])

    out = bytearray(b"bres" + struct.pack(">HHIHH", 0xFEFF, 0, 0, 0x10, 1))
    out += b"root" + struct.pack(">I", root_size + 8)
    # grupo raíz
    out += struct.pack(">II", root_size, 1)
    out += struct.pack(">HHHHII", 0xFFFF, 0, 1, 0, 0, 0)
    out += struct.pack(">HHHHII", 0, 0, 0, 0, str_base + str_off["Textures(NW4R)"] - root_grp_off,
                       sub_grp_off - root_grp_off)
    # grupo de texturas
    out += struct.pack(">II", sub_size, len(textures))
    out += struct.pack(">HHHHII", 0xFFFF, 0, 1, 0, 0, 0)
    for (name, *_), off in zip(textures, tex_offs):
        out += struct.pack(">HHHHII", 0, 0, 0, 0, str_base + str_off[name] - sub_grp_off, off - sub_grp_off)
    for off, blob in zip(tex_offs, tex_blobs):
        out += b"\x00" * (off - len(out))
        out += blob
    out += b"\x00" * (str_base - len(out))
    out += strings
    struct.pack_into(">I", out, 8, len(out))
    for (name, *_), off in zip(textures, tex_offs):
        struct.pack_into(">I", out, off + 0x14, str_base + str_off[name] - off)
    return bytes(out)


COEFS = [0, 0, 2048, 0, 3840, -1920, 3600, -1680, 1024, 1024, 4000, -2000, 1800, 0, 3000, -1500]


def dsp_encode(pcm: np.ndarray, coefs=COEFS) -> tuple[bytes, np.ndarray]:
    """Codificador DSP-ADPCM simple (fuerza bruta por trama). Devuelve (datos, salida_simulada)."""
    pcm = pcm.astype(np.int64)
    out = bytearray()
    sim = []
    h1 = h2 = 0
    for f in range(0, len(pcm), 14):
        frame = list(pcm[f:f + 14]) + [0] * max(0, 14 - len(pcm[f:f + 14]))
        best = None
        for ci in range(8):
            c1, c2 = coefs[ci * 2], coefs[ci * 2 + 1]
            for sc in range(12):
                scale = 1 << sc
                a, b = h1, h2
                nibs, outs, err = [], [], 0
                for x in frame:
                    pred = 1024 + c1 * a + c2 * b
                    best_n, best_v, best_e = 0, 0, None
                    for n in range(-8, 8):
                        v = (((n * scale) << 11) + pred) >> 11
                        v = max(-32768, min(32767, v))
                        e = (v - x) ** 2
                        if best_e is None or e < best_e:
                            best_n, best_v, best_e = n, v, e
                    nibs.append(best_n & 15)
                    outs.append(best_v)
                    err += best_e
                    b, a = a, best_v
                if best is None or err < best[0]:
                    best = (err, (ci << 4) | sc, nibs, outs, a, b)
        _, ps, nibs, outs, h1, h2 = best
        out.append(ps)
        for i in range(0, 14, 2):
            out.append((nibs[i] << 4) | nibs[i + 1])
        sim.extend(outs)
    return bytes(out), np.array(sim[:len(pcm)], np.int16)


def build_dsp(pcm: np.ndarray, rate: int) -> tuple[bytes, np.ndarray]:
    data, sim = dsp_encode(pcm)
    hdr = struct.pack(">IIIHHIII", len(pcm), len(pcm) // 14 * 16 + 2, rate, 0, 0, 2, 0, 2)
    hdr += struct.pack(">16h", *COEFS) + struct.pack(">HHhhHhh", 0, data[0], 0, 0, 0, 0, 0)
    hdr += b"\x00" * (0x60 - len(hdr))
    return hdr + data, sim


def build_brstm(channels: list[np.ndarray], rate: int, block_size: int = 0x2000, loop: bool = False) -> tuple[bytes, list[np.ndarray]]:
    """BRSTM DSP-ADPCM con varios canales y bloques entrelazados."""
    enc = [dsp_encode(c) for c in channels]
    total = len(channels[0])
    nch = len(channels)
    streams = [e[0] for e in enc]
    ln = len(streams[0])
    blocks = (ln + block_size - 1) // block_size
    last = ln - (blocks - 1) * block_size
    last_pad = (last + 0x1F) & ~0x1F
    head = bytearray(b"HEAD" + b"\x00" * 4)
    base = 8
    # 3 referencias
    refs_pos = len(head)
    head += b"\x00" * 24
    si = len(head) - base
    head += struct.pack(">BBBBHHIIIIIIIIIII", 2, int(loop), nch, 0, rate, 0, 0, total, 0, blocks, block_size,
                        block_size // 8 * 14, last, last // 8 * 14, last_pad, 0, 0)
    ti = len(head) - base
    head += struct.pack(">BBH", 1, 0, 0) + b"\x00" * 8
    ci = len(head) - base
    head += struct.pack(">B3x", nch)
    entry_refs = len(head)
    head += b"\x00" * (8 * nch)
    for c in range(nch):
        entry = len(head) - base
        struct.pack_into(">II", head, entry_refs + c * 8, 0x01000000, entry)
        info = entry + 8
        head += struct.pack(">II", 0x01000000, info)
        head += struct.pack(">16h", *COEFS) + struct.pack(">HHhhHhh", 0, streams[c][0], 0, 0, 0, 0, 0)
        head += b"\x00" * 2
    for i, off in enumerate((si, ti, ci)):
        struct.pack_into(">II", head, refs_pos + i * 8, 0x01000000, off)
    head += b"\x00" * ((-len(head)) % 0x20)
    struct.pack_into(">I", head, 4, len(head))
    head_off = 0x40
    data_chunk_off = head_off + len(head)
    data_start = data_chunk_off + 0x20
    struct.pack_into(">I", head, base + si + 0x10, data_start)
    body = bytearray()
    for b in range(blocks):
        size = block_size if b < blocks - 1 else last
        stride = block_size if b < blocks - 1 else last_pad
        for c in range(nch):
            chunk = streams[c][b * block_size: b * block_size + size]
            body += chunk + b"\x00" * (stride - len(chunk))
    data_chunk = b"DATA" + struct.pack(">II", 0x20 + len(body), 0x18) + b"\x00" * 0x14 + bytes(body)
    hdr = bytearray(b"RSTM" + struct.pack(">HHIHH", 0xFEFF, 0x0100, 0, 0x40, 2))
    hdr += struct.pack(">IIIIII", head_off, len(head), 0, 0, data_chunk_off, len(data_chunk))
    hdr += b"\x00" * (0x40 - len(hdr))
    out = bytes(hdr) + bytes(head) + data_chunk
    out = out[:8] + struct.pack(">I", len(out)) + out[12:]
    return out, [e[1] for e in enc]


__all__ = ["encode_gx", "tile", "gradient", "yaz0_literal", "build_brres", "build_u8", "build_dsp",
           "build_brstm", "dsp_encode", "cmpr_block_solid"]
