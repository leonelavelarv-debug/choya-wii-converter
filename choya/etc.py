"""Codificador ETC2 (modos compatibles con ETC1) y EAC, vectorizado con numpy.

- encode_etc2_rgb: bloques de 8 bytes, válidos para GL_COMPRESSED_RGB8_ETC2.
  Solo usa los modos individual y diferencial, y nunca desborda el diferencial,
  así que un decodificador ETC2 no entra por error a los modos T/H/planar.
- encode_eac: bloques de 8 bytes de un canal (alfa de RGBA8_EAC, o R11/RG11).
- encode_etc2_rgba1: ETC2 con alfa de 1 bit (GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2), 8 bytes por bloque:
  para CMPR con transparencia (4 bpp en vez de los 8 de RGBA8_EAC). Solo el modo diferencial: el bit que en ETC1
  elige individual/diferencial dice si el bloque es opaco; si no lo es, el índice 2 es un píxel transparente
  (RGBA 0) y los índices 0, 1, 3 dan +0, +b, -b de la tabla.
También incluye decodificadores para medir la calidad (PSNR) en las pruebas.
"""
from __future__ import annotations

import numpy as np

ETC1_TABLES = np.array([[2, 8], [5, 17], [9, 29], [13, 42], [18, 60], [24, 80], [33, 106], [47, 183]],
                       np.int32)
# índice de píxel 0..3 -> +a, +b, -a, -b
ETC1_MODS = np.stack([ETC1_TABLES[:, 0], ETC1_TABLES[:, 1], -ETC1_TABLES[:, 0], -ETC1_TABLES[:, 1]], 1)

EAC_TABLES = np.array([
    [-3, -6, -9, -15, 2, 5, 8, 14], [-3, -7, -10, -13, 2, 6, 9, 12],
    [-2, -5, -8, -13, 1, 4, 7, 12], [-2, -4, -6, -13, 1, 3, 5, 12],
    [-3, -6, -8, -12, 2, 5, 7, 11], [-3, -7, -9, -11, 2, 6, 8, 10],
    [-4, -7, -8, -11, 3, 6, 7, 10], [-3, -5, -8, -11, 2, 4, 7, 10],
    [-2, -6, -8, -10, 1, 5, 7, 9], [-2, -5, -8, -10, 1, 4, 7, 9],
    [-2, -4, -8, -10, 1, 3, 7, 9], [-2, -5, -7, -10, 1, 4, 6, 9],
    [-3, -4, -7, -10, 2, 3, 6, 9], [-1, -2, -3, -10, 0, 1, 2, 9],
    [-4, -6, -8, -9, 3, 5, 7, 8], [-3, -5, -7, -9, 2, 4, 6, 8],
], np.int32)

# Para cada píxel del bloque en orden ETC (columna mayor: j = x*4 + y), su (y, x).
_ORDER_Y = np.array([j % 4 for j in range(16)])
_ORDER_X = np.array([j // 4 for j in range(16)])

CHUNK = 4096


def _blocks(img: np.ndarray) -> tuple[np.ndarray, int, int]:
    """(h, w, c) -> (nbloques, 16, c) en orden ETC por bloque. Rellena repitiendo el borde."""
    h, w = img.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        img = np.pad(img, ((0, ph), (0, pw)) + ((0, 0),) * (img.ndim - 2), mode="edge")
    H, W = img.shape[:2]
    by, bx = H // 4, W // 4
    b = img.reshape(by, 4, bx, 4, -1).transpose(0, 2, 1, 3, 4).reshape(by * bx, 4, 4, -1)
    return b[:, _ORDER_Y, _ORDER_X], by, bx


# Máscaras de subbloque por flip, sobre el orden ETC (j = x*4 + y).
_SUB_NOFLIP = (_ORDER_X >= 2).astype(np.int64)   # flip=0: columnas 0-1 | 2-3
_SUB_FLIP = (_ORDER_Y >= 2).astype(np.int64)     # flip=1: filas 0-1 | 2-3


def _best_table(px: np.ndarray, base: np.ndarray):
    """px: (n, 8, 3) int32; base: (n, 3). Devuelve (error, tabla, índices(n,8))."""
    # colores candidatos: (n, 8 tablas, 4 mods, 3)
    cand = np.clip(base[:, None, None, :] + ETC1_MODS[None, :, :, None], 0, 255)
    diff = px[:, None, None, :, :] - cand[:, :, :, None, :]       # (n, 8, 4, 8px, 3)
    err = (diff * diff).sum(-1)                                   # (n, 8, 4, 8px)
    best_idx = err.argmin(2)                                      # (n, 8, 8px)
    best_err = err.min(2).sum(-1)                                 # (n, 8)
    table = best_err.argmin(1)                                    # (n,)
    n = np.arange(px.shape[0])
    return best_err[n, table], table, best_idx[n, table]


def _encode_rgb_chunk(blk: np.ndarray) -> np.ndarray:
    n = blk.shape[0]
    blk = blk.astype(np.int32)
    best_err = np.full(n, np.iinfo(np.int64).max, np.int64)
    best_words = np.zeros(n, np.uint64)

    for flip, sub in ((0, _SUB_NOFLIP), (1, _SUB_FLIP)):
        order = np.argsort(sub, kind="stable")       # 8 píxeles del sub 0 y luego 8 del sub 1
        pix = blk[:, order]                          # (n, 16, 3)
        p0, p1 = pix[:, :8], pix[:, 8:]
        avg0, avg1 = p0.mean(1), p1.mean(1)

        # Modo individual: 4 bits por canal.
        q0 = np.clip(np.rint(avg0 * 15 / 255), 0, 15).astype(np.int32)
        q1 = np.clip(np.rint(avg1 * 15 / 255), 0, 15).astype(np.int32)
        e0, t0, i0 = _best_table(p0, q0 * 17)
        e1, t1, i1 = _best_table(p1, q1 * 17)
        ind_err = e0.astype(np.int64) + e1
        ind = (q0, q1, t0, t1, i0, i1)

        # Modo diferencial: 5 bits + delta de 3 bits, sin desbordar.
        d0 = np.clip(np.rint(avg0 * 31 / 255), 0, 31).astype(np.int32)
        d1t = np.clip(np.rint(avg1 * 31 / 255), 0, 31).astype(np.int32)
        d1 = np.clip(d0 + np.clip(d1t - d0, -4, 3), 0, 31)
        b0 = (d0 << 3) | (d0 >> 2)
        b1 = (d1 << 3) | (d1 >> 2)
        f0, u0, j0 = _best_table(p0, b0)
        f1, u1, j1 = _best_table(p1, b1)
        dif_err = f0.astype(np.int64) + f1

        for diffbit, err, params in ((0, ind_err, ind), (1, dif_err, (d0, d1, u0, u1, j0, j1))):
            better = err < best_err
            if not better.any():
                continue
            words = _pack_rgb(flip, diffbit, params, order)
            best_err = np.where(better, err, best_err)
            best_words = np.where(better, words, best_words)
    return best_words


def _pack_rgb(flip: int, diffbit: int, params, order: np.ndarray) -> np.ndarray:
    c0, c1, t0, t1, i0, i1 = params
    c0 = c0.astype(np.uint64)
    c1 = c1.astype(np.uint64)
    if diffbit == 0:
        hi = ((c0[:, 0] << 28) | (c1[:, 0] << 24) | (c0[:, 1] << 20) | (c1[:, 1] << 16)
              | (c0[:, 2] << 12) | (c1[:, 2] << 8))
    else:
        d = (c1.astype(np.int64) - c0.astype(np.int64)) & 7
        d = d.astype(np.uint64)
        hi = ((c0[:, 0] << 27) | (d[:, 0] << 24) | (c0[:, 1] << 19) | (d[:, 1] << 16)
              | (c0[:, 2] << 11) | (d[:, 2] << 8))
    hi = hi | (t0.astype(np.uint64) << 5) | (t1.astype(np.uint64) << 2) | np.uint64(diffbit << 1) | np.uint64(flip)
    # índices por píxel en orden ETC
    idx = np.empty((c0.shape[0], 16), np.uint64)
    idx[:, order[:8]] = i0
    idx[:, order[8:]] = i1
    msb = (idx >> np.uint64(1)) & np.uint64(1)
    lsb = idx & np.uint64(1)
    shifts = np.arange(16, dtype=np.uint64)
    lo = ((msb << (shifts + np.uint64(16))) | (lsb << shifts)).sum(1, dtype=np.uint64)
    return (hi << np.uint64(32)) | lo


def encode_etc2_rgb(rgb: np.ndarray) -> bytes:
    """rgb: (h, w, 3|4) uint8 -> bloques ETC2 RGB8 (8 bytes cada uno, big-endian)."""
    blk, _, _ = _blocks(np.ascontiguousarray(rgb[..., :3]))
    out = np.empty(blk.shape[0], np.uint64)
    for s in range(0, blk.shape[0], CHUNK):
        out[s:s + CHUNK] = _encode_rgb_chunk(blk[s:s + CHUNK])
    return out.astype(">u8").tobytes()


# Punchthrough: modificadores por índice (msb lsb) cuando el bloque no es opaco; el índice 2 es transparente.
PT_MODS = np.stack([np.zeros(8, np.int32), ETC1_TABLES[:, 1], np.zeros(8, np.int32), -ETC1_TABLES[:, 1]], 1)


def _best_table_masked(px: np.ndarray, base: np.ndarray, opaque_px: np.ndarray, mods: np.ndarray, allowed):
    """Como _best_table, solo con los píxeles opacos (los transparentes van al índice 2). allowed: índices válidos."""
    cand = np.clip(base[:, None, None, :] + mods[None, :, :, None], 0, 255)
    diff = px[:, None, None, :, :] - cand[:, :, :, None, :]       # (n, 8, 4, 8px, 3)
    err = (diff * diff).sum(-1).astype(np.int64)
    big = np.int64(1) << np.int64(40)
    for k in range(4):
        if k not in allowed:
            err[:, :, k, :] = big
    best_idx = err.argmin(2)
    best_err = (err.min(2) * opaque_px[:, None, :]).sum(-1)       # (n, 8)
    table = best_err.argmin(1)
    n = np.arange(px.shape[0])
    idx = np.where(opaque_px == 1, best_idx[n, table], 2)
    return best_err[n, table], table, idx


def _encode_rgba1_chunk(blk: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    n = blk.shape[0]
    blk = blk.astype(np.int32)
    opaque = (alpha >= 128).astype(np.int64)                      # (n, 16)
    all_opaque = opaque.all(1)
    best_err = np.full(n, np.iinfo(np.int64).max, np.int64)
    best_words = np.zeros(n, np.uint64)
    for flip, sub in ((0, _SUB_NOFLIP), (1, _SUB_FLIP)):
        order = np.argsort(sub, kind="stable")
        pix, op = blk[:, order], opaque[:, order]
        p0, p1, o0, o1 = pix[:, :8], pix[:, 8:], op[:, :8], op[:, 8:]
        # Promedio de los píxeles opacos de cada mitad (si no hay, el de todos: no importa).
        avg0 = np.where(o0.sum(1, keepdims=True) > 0, (p0 * o0[..., None]).sum(1) / np.maximum(o0.sum(1), 1)[:, None],
                        p0.mean(1))
        avg1 = np.where(o1.sum(1, keepdims=True) > 0, (p1 * o1[..., None]).sum(1) / np.maximum(o1.sum(1), 1)[:, None],
                        p1.mean(1))
        d0 = np.clip(np.rint(avg0 * 31 / 255), 0, 31).astype(np.int32)
        d1t = np.clip(np.rint(avg1 * 31 / 255), 0, 31).astype(np.int32)
        d1 = np.clip(d0 + np.clip(d1t - d0, -4, 3), 0, 31)
        b0 = (d0 << 3) | (d0 >> 2)
        b1 = (d1 << 3) | (d1 >> 2)
        # Bloque opaco: la tabla de ETC1 completa. Con transparentes: +0, +b, -b y el índice 2 para el hueco.
        fo0, uo0, jo0 = _best_table_masked(p0, b0, np.ones_like(o0), ETC1_MODS, (0, 1, 2, 3))
        fo1, uo1, jo1 = _best_table_masked(p1, b1, np.ones_like(o1), ETC1_MODS, (0, 1, 2, 3))
        ft0, ut0, jt0 = _best_table_masked(p0, b0, o0, PT_MODS, (0, 1, 3))
        ft1, ut1, jt1 = _best_table_masked(p1, b1, o1, PT_MODS, (0, 1, 3))
        ao = all_opaque
        err = np.where(ao, fo0 + fo1, ft0 + ft1)
        t0 = np.where(ao, uo0, ut0)
        t1 = np.where(ao, uo1, ut1)
        i0 = np.where(ao[:, None], jo0, jt0)
        i1 = np.where(ao[:, None], jo1, jt1)
        words = _pack_rgb(flip, 1, (d0, d1, t0, t1, i0, i1), order)
        # El bit "diferencial" (bit 33) es el de opaco: 1 si no hay transparentes.
        words = np.where(ao, words, words & ~np.uint64(1 << 33))
        better = err < best_err
        best_err = np.where(better, err, best_err)
        best_words = np.where(better, words, best_words)
    return best_words


def encode_etc2_rgba1(rgba: np.ndarray) -> bytes:
    """rgba: (h, w, 4) uint8 -> bloques ETC2 RGB8A1 (alfa < 128 = transparente)."""
    blk, _, _ = _blocks(np.ascontiguousarray(rgba))
    out = np.empty(blk.shape[0], np.uint64)
    for s in range(0, blk.shape[0], CHUNK):
        out[s:s + CHUNK] = _encode_rgba1_chunk(blk[s:s + CHUNK, :, :3], blk[s:s + CHUNK, :, 3])
    return out.astype(">u8").tobytes()


def _encode_eac_chunk(a: np.ndarray) -> np.ndarray:
    """a: (n, 16) int32 en orden ETC -> palabras de 64 bits."""
    n = a.shape[0]
    amin, amax = a.min(1), a.max(1)
    best_err = np.full(n, np.iinfo(np.int64).max, np.int64)
    best = np.zeros(n, np.uint64)
    span = (amax - amin).astype(np.float64)
    for t in range(16):
        mods = EAC_TABLES[t]
        lo, hi = int(mods.min()), int(mods.max())
        m0 = np.clip(np.ceil(span / (hi - lo)), 1, 15).astype(np.int32)
        for dm in (0, 1):
            m = np.clip(m0 + dm, 1, 15)
            base = np.clip(np.rint((amin + amax) / 2 - (lo + hi) * m / 2), 0, 255).astype(np.int32)
            for db in (-1, 0, 1):
                b = np.clip(base + db, 0, 255)
                cand = np.clip(b[:, None] + mods[None, :] * m[:, None], 0, 255)   # (n, 8)
                diff = a[:, :, None] - cand[:, None, :]                            # (n, 16, 8)
                err = diff * diff
                idx = err.argmin(2)
                e = err.min(2).sum(1).astype(np.int64)
                better = e < best_err
                if not better.any():
                    continue
                shifts = (np.uint64(45) - np.arange(16, dtype=np.uint64) * np.uint64(3))
                bits = (idx.astype(np.uint64) << shifts).sum(1, dtype=np.uint64)
                word = ((b.astype(np.uint64) << np.uint64(56)) | (m.astype(np.uint64) << np.uint64(52))
                        | np.uint64(t << 48) | bits)
                best_err = np.where(better, e, best_err)
                best = np.where(better, word, best)
    return best


def encode_eac(channel: np.ndarray) -> bytes:
    """channel: (h, w) uint8 -> bloques EAC de 8 bytes."""
    blk, _, _ = _blocks(np.ascontiguousarray(channel)[..., None])
    a = blk[..., 0].astype(np.int32)
    out = np.empty(a.shape[0], np.uint64)
    for s in range(0, a.shape[0], CHUNK):
        out[s:s + CHUNK] = _encode_eac_chunk(a[s:s + CHUNK])
    return out.astype(">u8").tobytes()


def encode_etc2_rgba(rgba: np.ndarray) -> bytes:
    """RGBA8 ETC2 + EAC: por bloque, 8 bytes de alfa y luego 8 bytes de color."""
    alpha = np.frombuffer(encode_eac(rgba[..., 3]), ">u8")
    color = np.frombuffer(encode_etc2_rgb(rgba[..., :3]), ">u8")
    out = np.empty(alpha.size * 2, ">u8")
    out[0::2] = alpha
    out[1::2] = color
    return out.tobytes()


def encode_rg11(r: np.ndarray, g: np.ndarray) -> bytes:
    a = np.frombuffer(encode_eac(r), ">u8")
    b = np.frombuffer(encode_eac(g), ">u8")
    out = np.empty(a.size * 2, ">u8")
    out[0::2] = a
    out[1::2] = b
    return out.tobytes()


# ---------------------------------------------------------------- decodificadores (verificación)

def _deblock(blocks: np.ndarray, w: int, h: int) -> np.ndarray:
    """(n, 16, c) en orden ETC -> (h, w, c)."""
    bx, by = (w + 3) // 4, (h + 3) // 4
    c = blocks.shape[-1]
    grid = np.empty((blocks.shape[0], 4, 4, c), blocks.dtype)
    grid[:, _ORDER_Y, _ORDER_X] = blocks
    img = grid.reshape(by, bx, 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(by * 4, bx * 4, c)
    return img[:h, :w]


def decode_etc2_rgb(data: bytes, w: int, h: int) -> np.ndarray:
    words = np.frombuffer(data, ">u8").astype(np.uint64)
    hi = (words >> np.uint64(32)).astype(np.int64)
    lo = (words & np.uint64(0xFFFFFFFF)).astype(np.int64)
    diff = (hi >> 1) & 1
    flip = hi & 1
    t0, t1 = (hi >> 5) & 7, (hi >> 2) & 7
    n = words.size
    base0 = np.empty((n, 3), np.int64)
    base1 = np.empty((n, 3), np.int64)
    for c, (s4a, s4b, s5) in enumerate(((28, 24, 27), (20, 16, 19), (12, 8, 11))):
        r4a, r4b = (hi >> s4a) & 15, (hi >> s4b) & 15
        r5 = (hi >> s5) & 31
        dd = (hi >> (s5 - 3)) & 7
        dd = np.where(dd >= 4, dd - 8, dd)
        r5b = r5 + dd
        if (diff == 1).any() and ((r5b < 0) | (r5b > 31))[diff == 1].any():
            raise ValueError("bloque diferencial desbordado (modo T/H/planar no soportado)")
        base0[:, c] = np.where(diff == 1, (r5 << 3) | (r5 >> 2), r4a * 17)
        base1[:, c] = np.where(diff == 1, (r5b << 3) | (r5b >> 2), r4b * 17)
    j = np.arange(16)
    idx = (((lo[:, None] >> (j + 16)) & 1) << 1) | ((lo[:, None] >> j) & 1)
    sub = np.where(flip[:, None] == 1, _SUB_FLIP[None, :], _SUB_NOFLIP[None, :])
    table = np.where(sub == 1, t1[:, None], t0[:, None])
    mod = ETC1_MODS[table, idx]
    base = np.where(sub[..., None] == 1, base1[:, None, :], base0[:, None, :])
    px = np.clip(base + mod[..., None], 0, 255).astype(np.uint8)
    return _deblock(px, w, h)


def decode_etc2_rgba1(data: bytes, w: int, h: int) -> np.ndarray:
    """ETC2 RGB8A1 en el subconjunto que produce encode_etc2_rgba1 (solo modo diferencial, sin desbordes)."""
    words = np.frombuffer(data, ">u8").astype(np.uint64)
    hi = (words >> np.uint64(32)).astype(np.int64)
    lo = (words & np.uint64(0xFFFFFFFF)).astype(np.int64)
    opaque = (hi >> 1) & 1
    flip = hi & 1
    t0, t1 = (hi >> 5) & 7, (hi >> 2) & 7
    n = words.size
    base0 = np.empty((n, 3), np.int64)
    base1 = np.empty((n, 3), np.int64)
    for c, s5 in enumerate((27, 19, 11)):
        r5 = (hi >> s5) & 31
        dd = (hi >> (s5 - 3)) & 7
        dd = np.where(dd >= 4, dd - 8, dd)
        r5b = r5 + dd
        if ((r5b < 0) | (r5b > 31)).any():
            raise ValueError("bloque desbordado (modo T/H/planar no soportado)")
        base0[:, c] = (r5 << 3) | (r5 >> 2)
        base1[:, c] = (r5b << 3) | (r5b >> 2)
    j = np.arange(16)
    idx = (((lo[:, None] >> (j + 16)) & 1) << 1) | ((lo[:, None] >> j) & 1)
    sub = np.where(flip[:, None] == 1, _SUB_FLIP[None, :], _SUB_NOFLIP[None, :])
    table = np.where(sub == 1, t1[:, None], t0[:, None])
    mod = np.where(opaque[:, None] == 1, ETC1_MODS[table, idx], PT_MODS[table, idx])
    base = np.where(sub[..., None] == 1, base1[:, None, :], base0[:, None, :])
    rgb = np.clip(base + mod[..., None], 0, 255)
    transparent = (opaque[:, None] == 0) & (idx == 2)
    rgb = np.where(transparent[..., None], 0, rgb)
    a = np.where(transparent, 0, 255)
    px = np.concatenate([rgb, a[..., None]], -1).astype(np.uint8)
    return _deblock(px, w, h)


def decode_eac(data: bytes, w: int, h: int, stride_words: int = 1, offset_words: int = 0) -> np.ndarray:
    words = np.frombuffer(data, ">u8").astype(np.uint64)[offset_words::stride_words]
    base = (words >> np.uint64(56)).astype(np.int64)
    mult = ((words >> np.uint64(52)) & np.uint64(15)).astype(np.int64)
    table = ((words >> np.uint64(48)) & np.uint64(15)).astype(np.int64)
    j = np.arange(16, dtype=np.uint64)
    idx = ((words[:, None] >> (np.uint64(45) - j * np.uint64(3))) & np.uint64(7)).astype(np.int64)
    val = np.clip(base[:, None] + EAC_TABLES[table[:, None], idx] * mult[:, None], 0, 255).astype(np.uint8)
    return _deblock(val[..., None], w, h)[..., 0]


def decode_etc2_rgba(data: bytes, w: int, h: int) -> np.ndarray:
    words = np.frombuffer(data, ">u8")
    rgb = decode_etc2_rgb(words[1::2].tobytes(), w, h)
    a = decode_eac(data, w, h, stride_words=2, offset_words=0)
    return np.concatenate([rgb, a[..., None]], axis=2)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return float("inf") if mse == 0 else 10 * np.log10(255 * 255 / mse)
