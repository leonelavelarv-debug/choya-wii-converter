"""Optimizador de texturas: reduce, elige formato de salida y codifica la cadena de mips."""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image

from . import etc
from .gxtex import INTENSITY_FORMATS, FORMAT_NAMES, GXTexture, decode_mips, CMPR, I4, I8, IA4, IA8
from .profiles import Profile

# Formatos de salida (deben coincidir con runtime/include/wl/format.h)
OUT_ETC2_RGB8 = 1
OUT_ETC2_RGBA8 = 2
OUT_RGB565 = 3
OUT_RGBA4444 = 4
OUT_RGBA5551 = 5
OUT_R11_EAC = 6      # intensidad (I4/I8): el runtime aplica swizzle RRRR
OUT_RG11_EAC = 7     # intensidad + alfa (IA4/IA8): swizzle RRRG
OUT_L8 = 8           # GLES 2: luminancia
OUT_LA8 = 9          # GLES 2: luminancia + alfa
OUT_RGBA8 = 10
OUT_ETC2_RGB8A1 = 11  # ETC2 con alfa de 1 bit: CMPR con transparencia (4 bpp en vez de 8)

OUT_NAMES = {
    OUT_ETC2_RGB8: "ETC2_RGB8", OUT_ETC2_RGBA8: "ETC2_RGBA8_EAC", OUT_RGB565: "RGB565",
    OUT_RGBA4444: "RGBA4444", OUT_RGBA5551: "RGBA5551", OUT_R11_EAC: "R11_EAC",
    OUT_RG11_EAC: "RG11_EAC", OUT_L8: "L8", OUT_LA8: "LA8", OUT_RGBA8: "RGBA8", OUT_ETC2_RGB8A1: "ETC2_RGB8A1",
}

# bits por píxel (los formatos de bloque usan bloques de 4x4)
OUT_BPP = {
    OUT_ETC2_RGB8: 4, OUT_ETC2_RGBA8: 8, OUT_R11_EAC: 4, OUT_RG11_EAC: 8,
    OUT_RGB565: 16, OUT_RGBA4444: 16, OUT_RGBA5551: 16, OUT_L8: 8, OUT_LA8: 16, OUT_RGBA8: 32, OUT_ETC2_RGB8A1: 4,
}
BLOCK_FORMATS = {OUT_ETC2_RGB8, OUT_ETC2_RGBA8, OUT_R11_EAC, OUT_RG11_EAC, OUT_ETC2_RGB8A1}


def out_level_size(fmt: int, w: int, h: int) -> int:
    if fmt in BLOCK_FORMATS:
        return ((w + 3) // 4) * ((h + 3) // 4) * (16 if OUT_BPP[fmt] == 8 else 8)
    return w * h * OUT_BPP[fmt] // 8


def gx_vram_bytes(tex: GXTexture) -> int:
    """Lo que ocuparía la textura sin convertir, expandida a RGBA8 (lo que haría un port ingenuo)."""
    total = 0
    for i in range(tex.mips):
        total += max(1, tex.width >> i) * max(1, tex.height >> i) * 4
    return total


@dataclass
class ConvertedTexture:
    fmt: int
    width: int
    height: int
    mips: int
    data: bytes
    alpha: str          # "none" | "binary" | "full"
    psnr: float | None
    fallback: str | None = None   # por qué no se usó el formato comprimido


def classify_alpha(a: np.ndarray) -> str:
    if a.min() == 255:
        return "none"
    if np.all((a == 0) | (a == 255)):
        return "binary"
    return "full"


def target_size(w: int, h: int, max_side: int) -> tuple[int, int]:
    if max(w, h) <= max_side:
        return w, h
    scale = max_side / max(w, h)
    nw = max(1, round(w * scale))
    nh = max(1, round(h * scale))
    # Mantener potencias de dos si el original lo era (repetición de UV en GLES 2).
    if w & (w - 1) == 0:
        nw = 1 << (nw.bit_length() - 1) if nw & (nw - 1) else nw
    if h & (h - 1) == 0:
        nh = 1 << (nh.bit_length() - 1) if nh & (nh - 1) else nh
    return nw, nh


def _resize(img: np.ndarray, w: int, h: int) -> np.ndarray:
    if img.shape[1] == w and img.shape[0] == h:
        return img
    pil = Image.fromarray(img, "RGBA").convert("RGBa")  # premultiplicado: evita halos en el alfa
    pil = pil.resize((w, h), Image.Resampling.LANCZOS if (w < img.shape[1] / 2) else Image.Resampling.BOX)
    return np.asarray(pil.convert("RGBA"))


def _mip_chain(img: np.ndarray, levels: int) -> list[np.ndarray]:
    out = [img]
    for _ in range(1, levels):
        h, w = out[-1].shape[:2]
        if w == 1 and h == 1:
            break
        out.append(_resize(out[-1], max(1, w // 2), max(1, h // 2)))
    return out


def choose_format(src_fmt: int, alpha: str, profile: Profile) -> int:
    intensity = src_fmt in INTENSITY_FORMATS
    if profile.gles3:
        if intensity:
            return OUT_R11_EAC if src_fmt in (I4, I8) else OUT_RG11_EAC
        if alpha == "binary" and src_fmt == CMPR:
            return OUT_ETC2_RGB8A1   # CMPR solo tiene alfa de 1 bit (y sus huecos son negros, como en ETC2)
        return OUT_ETC2_RGB8 if alpha == "none" else OUT_ETC2_RGBA8
    if intensity:
        # GLES 2 no tiene swizzle: LA8 conserva el alfa = intensidad de I4/I8 y el alfa real de IA4/IA8.
        return OUT_LA8
    return {"none": OUT_RGB565, "binary": OUT_RGBA5551}.get(alpha, OUT_RGBA4444)


def _pack16(img: np.ndarray, fmt: int) -> bytes:
    r, g, b, a = (img[..., i].astype(np.uint16) for i in range(4))
    if fmt == OUT_RGB565:
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    elif fmt == OUT_RGBA4444:
        v = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)
    else:  # RGBA5551
        v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)
    return v.astype("<u2").tobytes()  # GL lee shorts en el orden nativo (little-endian en ARM64)


def encode_level(img: np.ndarray, fmt: int) -> bytes:
    if fmt == OUT_ETC2_RGB8:
        return etc.encode_etc2_rgb(img)
    if fmt == OUT_ETC2_RGBA8:
        return etc.encode_etc2_rgba(img)
    if fmt == OUT_ETC2_RGB8A1:
        return etc.encode_etc2_rgba1(img)
    if fmt == OUT_R11_EAC:
        return etc.encode_eac(img[..., 0])
    if fmt == OUT_RG11_EAC:
        return etc.encode_rg11(img[..., 0], img[..., 3])
    if fmt in (OUT_RGB565, OUT_RGBA4444, OUT_RGBA5551):
        return _pack16(img, fmt)
    if fmt == OUT_L8:
        return img[..., 0].tobytes()
    if fmt == OUT_LA8:
        return np.stack([img[..., 0], img[..., 3]], -1).tobytes()
    if fmt == OUT_RGBA8:
        return img.tobytes()
    raise ValueError(fmt)


def decode_out(fmt: int, data: bytes, w: int, h: int) -> np.ndarray:
    """Decodifica un nivel de salida a RGBA8 (h, w, 4), como lo verá la GPU."""
    if fmt == OUT_ETC2_RGB8:
        rgb = etc.decode_etc2_rgb(data, w, h)
        return np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], 2)
    if fmt == OUT_ETC2_RGBA8:
        return etc.decode_etc2_rgba(data, w, h)
    if fmt == OUT_ETC2_RGB8A1:
        return etc.decode_etc2_rgba1(data, w, h)
    if fmt in (OUT_R11_EAC, OUT_RG11_EAC):
        if fmt == OUT_R11_EAC:
            i = etc.decode_eac(data, w, h)
            a = i
        else:
            i = etc.decode_eac(data, w, h, 2, 0)
            a = etc.decode_eac(data, w, h, 2, 1)
        return np.stack([i, i, i, a], -1)
    if fmt == OUT_L8:
        i = np.frombuffer(data, np.uint8).reshape(h, w)
        return np.stack([i, i, i, np.full_like(i, 255)], -1)
    if fmt == OUT_LA8:
        la = np.frombuffer(data, np.uint8).reshape(h, w, 2)
        return np.stack([la[..., 0]] * 3 + [la[..., 1]], -1)
    if fmt == OUT_RGBA8:
        return np.frombuffer(data, np.uint8).reshape(h, w, 4).copy()
    v = np.frombuffer(data, "<u2").reshape(h, w).astype(np.uint32)
    if fmt == OUT_RGB565:
        return np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31,
                         np.full_like(v, 255)], -1).astype(np.uint8)
    if fmt == OUT_RGBA4444:
        return np.stack([(v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17],
                        -1).astype(np.uint8)
    if fmt == OUT_RGBA5551:
        return np.stack([((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31,
                         (v & 1) * 255], -1).astype(np.uint8)
    raise ValueError(fmt)


def _measure(img: np.ndarray, fmt: int, data: bytes, alpha: str) -> float:
    h, w = img.shape[:2]
    dec = decode_out(fmt, data, w, h)
    ch = 4 if alpha != "none" else 3
    if alpha == "binary":
        # El color de un píxel transparente no se ve (ETC2 RGB8A1 y CMPR lo dejan en negro): se compara premultiplicado.
        img = img.copy()
        img[img[..., 3] < 128, :3] = 0
        dec = dec.copy()
        dec[dec[..., 3] < 128, :3] = 0
    return etc.psnr(dec[..., :ch], img[..., :ch])


def is_ui_texture(path: str) -> bool:
    p = path.lower().replace("\\", "/")
    return any(k in p for k in ("/timg/", "/layout", "/lyt", ".brlyt", "/blyt/", "/menu", "/ui/"))


def convert(tex: GXTexture, profile: Profile, measure: bool = True) -> ConvertedTexture:
    levels = decode_mips(tex)
    if not levels:
        raise ValueError("textura sin datos decodificables")
    base = levels[0]
    if tex.fmt in INTENSITY_FORMATS:
        # GX replica la intensidad en RGB; el canal A es el alfa real (IA) o la intensidad (I).
        alpha = "none" if tex.fmt in (I4, I8) else classify_alpha(base[..., 3])
    else:
        alpha = classify_alpha(base[..., 3])
    fmt = choose_format(tex.fmt, alpha, profile)
    max_side = profile.max_size_ui if is_ui_texture(tex.source) else profile.max_size_3d
    w, h = target_size(tex.width, tex.height, max_side)
    img = _resize(base, w, h)
    n_mips = 1
    if profile.keep_mips and tex.mips > 1:
        n_mips = max(1, min(tex.mips, max(w, h).bit_length()))
    chain = _mip_chain(img, n_mips)
    ui = is_ui_texture(tex.source)
    threshold = profile.min_psnr_ui if ui else profile.min_psnr_3d

    fallback = None
    q = None
    if w * h <= profile.small_lossless:
        # Texturas diminutas (tablas de color, puntos de UI): comprimir no ahorra casi nada y las arruina.
        fmt, fallback = OUT_RGBA8, f"diminuta ({w}x{h})"
    else:
        lvl0 = encode_level(img, fmt)
        if measure:
            q = _measure(img, fmt, lvl0, alpha)
            if q < threshold:
                bad = round(q, 1)
                # I4/I8 llevan la intensidad también en el alfa: RGB565 la perdería.
                lossy_ok = alpha == "none" and tex.fmt not in INTENSITY_FORMATS and profile.gles3
                options = [OUT_RGB565, OUT_RGBA8] if lossy_ok else [OUT_RGBA8]
                if fmt == OUT_ETC2_RGB8A1:
                    options = [OUT_ETC2_RGBA8, OUT_RGBA8]   # antes de 32 bpp, el de 8 bpp con alfa EAC
                for alt in options:
                    q2 = _measure(img, alt, encode_level(img, alt), alpha)
                    if q2 >= threshold or alt == OUT_RGBA8:
                        fmt, q = alt, q2
                        fallback = f"{OUT_NAMES[choose_format(tex.fmt, alpha, profile)]} daba {bad} dB"
                        break
    data = b"".join(encode_level(m, fmt) for m in chain)
    if measure and q is None:
        q = _measure(img, fmt, data[:out_level_size(fmt, w, h)], alpha)
    return ConvertedTexture(fmt=fmt, width=w, height=h, mips=len(chain), data=data, alpha=alpha, psnr=q,
                            fallback=fallback)


def describe(tex: GXTexture) -> str:
    return f"{FORMAT_NAMES.get(tex.fmt, tex.fmt)} {tex.width}x{tex.height} mips={tex.mips}"
