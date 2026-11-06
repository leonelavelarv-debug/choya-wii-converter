"""Decodificadores de texturas GX (Wii/GameCube) a RGBA8 con numpy."""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8 = 0, 1, 2, 3, 4, 5, 6
C4, C8, C14X2, CMPR = 8, 9, 10, 14

FORMAT_NAMES = {
    I4: "I4", I8: "I8", IA4: "IA4", IA8: "IA8", RGB565: "RGB565", RGB5A3: "RGB5A3",
    RGBA8: "RGBA8", C4: "C4", C8: "C8", C14X2: "C14X2", CMPR: "CMPR",
}

# (ancho de bloque, alto de bloque, bits por píxel)
BLOCK = {
    I4: (8, 8, 4), I8: (8, 4, 8), IA4: (8, 4, 8), IA8: (4, 4, 16), RGB565: (4, 4, 16),
    RGB5A3: (4, 4, 16), RGBA8: (4, 4, 32), C4: (8, 8, 4), C8: (8, 4, 8), C14X2: (4, 4, 16),
    CMPR: (8, 8, 4),
}

PAL_IA8, PAL_RGB565, PAL_RGB5A3 = 0, 1, 2
INTENSITY_FORMATS = {I4, I8, IA4, IA8}
PALETTE_FORMATS = {C4, C8, C14X2}


@dataclass
class GXTexture:
    name: str
    fmt: int
    width: int
    height: int
    mips: int
    data: bytes                     # todos los niveles tal como están en el disco
    palette: np.ndarray | None = None   # (N, 4) uint8 RGBA para C4/C8/C14X2
    palette_fmt: int | None = None
    source: str = ""
    notes: list[str] = field(default_factory=list)

    @property
    def level0(self) -> bytes:
        return self.data[:level_size(self.fmt, self.width, self.height)]


def level_size(fmt: int, w: int, h: int) -> int:
    bw, bh, bpp = BLOCK[fmt]
    bx = (w + bw - 1) // bw
    by = (h + bh - 1) // bh
    return bx * by * bw * bh * bpp // 8


def chain_size(fmt: int, w: int, h: int, mips: int) -> int:
    total = 0
    for i in range(mips):
        total += level_size(fmt, max(1, w >> i), max(1, h >> i))
    return total


def _untile(values: np.ndarray, w: int, h: int, bw: int, bh: int) -> np.ndarray:
    """values: (nbloques * bh * bw, ...) en orden de bloque -> imagen (h, w, ...)."""
    bx = (w + bw - 1) // bw
    by = (h + bh - 1) // bh
    rest = values.shape[1:]
    v = values.reshape((by, bx, bh, bw) + rest)
    v = v.transpose((0, 2, 1, 3) + tuple(range(4, 4 + len(rest))))
    v = v.reshape((by * bh, bx * bw) + rest)
    return v[:h, :w]


def _expand5(v):
    return ((v << 3) | (v >> 2)).astype(np.uint8)


def _expand6(v):
    return ((v << 2) | (v >> 4)).astype(np.uint8)


def _expand4(v):
    return (v * 17).astype(np.uint8)


def _expand3(v):
    return ((v << 5) | (v << 2) | (v >> 1)).astype(np.uint8)


def decode_rgb565(v: np.ndarray) -> np.ndarray:
    v = v.astype(np.uint32)
    out = np.empty(v.shape + (4,), np.uint8)
    out[..., 0] = _expand5((v >> 11) & 31)
    out[..., 1] = _expand6((v >> 5) & 63)
    out[..., 2] = _expand5(v & 31)
    out[..., 3] = 255
    return out


def decode_rgb5a3(v: np.ndarray) -> np.ndarray:
    v = v.astype(np.uint32)
    out = np.empty(v.shape + (4,), np.uint8)
    opaque = (v & 0x8000) != 0
    out[..., 0] = np.where(opaque, _expand5((v >> 10) & 31), _expand4((v >> 8) & 15))
    out[..., 1] = np.where(opaque, _expand5((v >> 5) & 31), _expand4((v >> 4) & 15))
    out[..., 2] = np.where(opaque, _expand5(v & 31), _expand4(v & 15))
    out[..., 3] = np.where(opaque, 255, _expand3((v >> 12) & 7))
    return out


def decode_ia8(v: np.ndarray) -> np.ndarray:
    v = v.astype(np.uint32)
    i = (v & 0xFF).astype(np.uint8)
    out = np.empty(v.shape + (4,), np.uint8)
    out[..., 0] = i
    out[..., 1] = i
    out[..., 2] = i
    out[..., 3] = (v >> 8).astype(np.uint8)
    return out


def decode_palette(raw: bytes, count: int, pal_fmt: int) -> np.ndarray:
    v = np.frombuffer(raw[: count * 2], dtype=">u2")
    if pal_fmt == PAL_IA8:
        return decode_ia8(v)
    if pal_fmt == PAL_RGB565:
        return decode_rgb565(v)
    if pal_fmt == PAL_RGB5A3:
        return decode_rgb5a3(v)
    raise ValueError(f"formato de paleta desconocido {pal_fmt}")


def _nibbles(buf: np.ndarray) -> np.ndarray:
    out = np.empty(buf.size * 2, np.uint8)
    out[0::2] = buf >> 4
    out[1::2] = buf & 0x0F
    return out


def _decode_cmpr(raw: np.ndarray, w: int, h: int) -> np.ndarray:
    nmacro = raw.size // 32
    sub = raw.reshape(nmacro, 4, 8)
    c0 = (sub[..., 0].astype(np.uint32) << 8) | sub[..., 1]
    c1 = (sub[..., 2].astype(np.uint32) << 8) | sub[..., 3]
    col0 = decode_rgb565(c0).astype(np.int32)
    col1 = decode_rgb565(c1).astype(np.int32)
    four = c0 > c1
    pal = np.empty((nmacro, 4, 4, 4), np.int32)
    pal[:, :, 0] = col0
    pal[:, :, 1] = col1
    c2_four = (2 * col0 + col1) // 3
    c3_four = (col0 + 2 * col1) // 3
    c2_three = (col0 + col1) // 2
    pal[:, :, 2] = np.where(four[..., None], c2_four, c2_three)
    pal[:, :, 3] = np.where(four[..., None], c3_four, 0)
    pal[:, :, 2, 3] = 255
    pal[:, :, 3, 3] = np.where(four, 255, 0)
    rows = sub[..., 4:8]  # (n, 4 sub, 4 filas)
    shifts = np.array([6, 4, 2, 0], np.uint8)
    idx = (rows[..., None] >> shifts) & 3  # (n, 4, 4, 4)
    n_i = np.arange(nmacro)[:, None, None, None]
    s_i = np.arange(4)[None, :, None, None]
    px = pal[n_i, s_i, idx]  # (n, 4 sub, 4 filas, 4 cols, 4 canales)
    px = px.reshape(nmacro, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(nmacro * 64, 4)
    return _untile(px.astype(np.uint8), w, h, 8, 8)


def decode_level(fmt: int, raw: bytes, w: int, h: int, palette: np.ndarray | None = None) -> np.ndarray:
    """Decodifica un nivel de mip a un arreglo (h, w, 4) uint8 RGBA."""
    bw, bh, _ = BLOCK[fmt]
    size = level_size(fmt, w, h)
    if len(raw) < size:
        raise ValueError(f"datos insuficientes para {FORMAT_NAMES.get(fmt, fmt)} {w}x{h}")
    buf = np.frombuffer(raw[:size], np.uint8)

    if fmt == CMPR:
        return _decode_cmpr(buf, w, h)
    if fmt in (I4, I8):
        vals = _nibbles(buf) * 17 if fmt == I4 else buf
        img = _untile(vals.astype(np.uint8), w, h, bw, bh)
        return np.repeat(img[..., None], 4, axis=2)
    if fmt == IA4:
        out = np.empty((buf.size, 4), np.uint8)
        i = _expand4(buf & 0x0F)
        out[:, 0] = out[:, 1] = out[:, 2] = i
        out[:, 3] = _expand4(buf >> 4)
        return _untile(out, w, h, bw, bh)
    if fmt == RGBA8:
        blocks = buf.reshape(-1, 2, 16, 2)
        out = np.empty((blocks.shape[0], 16, 4), np.uint8)
        out[..., 3] = blocks[:, 0, :, 0]
        out[..., 0] = blocks[:, 0, :, 1]
        out[..., 1] = blocks[:, 1, :, 0]
        out[..., 2] = blocks[:, 1, :, 1]
        return _untile(out.reshape(-1, 4), w, h, bw, bh)
    if fmt in (IA8, RGB565, RGB5A3, C14X2):
        v = np.frombuffer(raw[:size], dtype=">u2")
        if fmt == IA8:
            px = decode_ia8(v)
        elif fmt == RGB565:
            px = decode_rgb565(v)
        elif fmt == RGB5A3:
            px = decode_rgb5a3(v)
        else:
            px = _lookup(palette, (v & 0x3FFF).astype(np.int64))
        return _untile(px, w, h, bw, bh)
    if fmt in (C4, C8):
        idx = _nibbles(buf) if fmt == C4 else buf
        return _untile(_lookup(palette, idx.astype(np.int64)), w, h, bw, bh)
    raise ValueError(f"formato GX no soportado: {fmt}")


def _lookup(palette: np.ndarray | None, idx: np.ndarray) -> np.ndarray:
    if palette is None:
        # Sin paleta: escala de grises de los índices, para poder ver algo.
        g = (idx * 255 // max(1, int(idx.max()))).astype(np.uint8)
        out = np.repeat(g[:, None], 4, axis=1)
        out[:, 3] = 255
        return out
    idx = np.clip(idx, 0, len(palette) - 1)
    return palette[idx]


def decode_mips(tex: GXTexture) -> list[np.ndarray]:
    """Decodifica todos los niveles disponibles (corta si los datos no alcanzan)."""
    out = []
    off = 0
    for i in range(tex.mips):
        w, h = max(1, tex.width >> i), max(1, tex.height >> i)
        size = level_size(tex.fmt, w, h)
        if off + size > len(tex.data):
            break
        out.append(decode_level(tex.fmt, tex.data[off:off + size], w, h, tex.palette))
        off += size
    return out
