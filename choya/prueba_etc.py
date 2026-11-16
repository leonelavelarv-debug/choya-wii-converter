"""Paquete de prueba ETC2/EAC (sin datos del juego) para confirmar el códec en la GPU de la consola.

`python3 -m choya prueba-etc carpeta` escribe carpeta/assets.wlpk y carpeta/LEEME-prueba-etc.txt. El paquete trae:
  - bloques al azar (semilla fija) de los cinco formatos que escribe el conversor (RGB8, RGB8A1, RGBA8_EAC, R11, RG11)
    en tamaños que no son múltiplo de 4 (1x1, 2x3, 5x7, 13x7, 33x17) y con la cadena de mipmaps completa. Cualquier
    secuencia de 64 bits es un bloque ETC2 válido, así que el azar cubre los modos individual, diferencial, T, H y
    planar (y en RGB8A1 el píxel transparente en los tres primeros); `modos()` los cuenta;
  - cuatro imágenes para mirar en wl_view: un damero con un círculo transparente (RGB8A1, borde nítido), un degradado de
    alfa (RGBA8_EAC), un degradado de intensidad (R11 con swizzle de intensidad) e intensidad + alfa (RG11).
`wl_view --verificar` (archivo "verificar" en el port) compara cada nivel de cada textura como la lee la GPU con el
decodificador de la CPU, por la ruta nativa y por la de respaldo, y cuenta el descarte por alfa de RGB8A1.
"""
from __future__ import annotations

import os

import numpy as np

from . import etc, optimize, pack

SIZES = [(1, 1), (2, 3), (5, 7), (13, 7), (33, 17), (64, 32)]
FORMATS = [(optimize.OUT_ETC2_RGB8, 8), (optimize.OUT_ETC2_RGB8A1, 8), (optimize.OUT_ETC2_RGBA8, 16),
           (optimize.OUT_R11_EAC, 8), (optimize.OUT_RG11_EAC, 16)]
NAMES = {optimize.OUT_ETC2_RGB8: "rgb8", optimize.OUT_ETC2_RGB8A1: "rgb8a1", optimize.OUT_ETC2_RGBA8: "rgba8",
         optimize.OUT_R11_EAC: "r11", optimize.OUT_RG11_EAC: "rg11"}


def _levels(w: int, h: int) -> list[tuple[int, int]]:
    out = [(w, h)]
    while out[-1] != (1, 1):
        pw, ph = out[-1]
        out.append((max(1, pw >> 1), max(1, ph >> 1)))
    return out


def mode_of(word: int, punch: bool = False) -> str:
    """Modo de un bloque de color ETC2 (64 bits, big-endian ya leído)."""
    hi = word >> 32
    if not punch and not (hi >> 1) & 1:
        return "individual"

    def over(shift):
        b = (hi >> shift) & 31
        d = (hi >> (shift - 3)) & 7
        d = d - 8 if d >= 4 else d
        return not 0 <= b + d <= 31

    if over(27):
        return "T"
    if over(19):
        return "H"
    if over(11):
        return "planar"
    return "diferencial"


def modos(data: bytes, fmt: int) -> dict:
    """Cuántos bloques de color hay de cada modo (para comprobar que la prueba los cubre)."""
    if fmt in (optimize.OUT_R11_EAC, optimize.OUT_RG11_EAC):
        return {}
    step = 16 if fmt == optimize.OUT_ETC2_RGBA8 else 8
    skip = 8 if fmt == optimize.OUT_ETC2_RGBA8 else 0
    out: dict = {}
    for o in range(0, len(data) - step + 1, step):
        word = int.from_bytes(data[o + skip:o + skip + 8], "big")
        m = mode_of(word, fmt == optimize.OUT_ETC2_RGB8A1)
        out[m] = out.get(m, 0) + 1
    return out


def _visual() -> list[tuple[str, int, int, bytes, int, int]]:
    """(nombre, formato, banderas, datos, ancho, alto) de las imágenes para mirar."""
    n = 64
    y, x = np.mgrid[0:n, 0:n]
    checker = (((x // 8) + (y // 8)) % 2).astype(np.uint8)
    rgba = np.zeros((n, n, 4), np.uint8)
    rgba[..., 0] = np.where(checker, 230, 40)
    rgba[..., 1] = (x * 4).astype(np.uint8)
    rgba[..., 2] = (y * 4).astype(np.uint8)
    rgba[..., 3] = 255
    circle = (x - 31.5) ** 2 + (y - 31.5) ** 2 < 20 ** 2
    rgba[circle] = 0
    grad = np.zeros((n, n, 4), np.uint8)
    grad[..., 0] = 255
    grad[..., 1] = 128
    grad[..., 2] = (y * 4).astype(np.uint8)
    grad[..., 3] = (x * 4 + 3).astype(np.uint8)
    inten = (x * 4).astype(np.uint8)
    alpha = (255 - y * 4).astype(np.uint8)
    return [
        ("visual/rgb8a1_circulo_transparente", optimize.OUT_ETC2_RGB8A1, 0, etc.encode_etc2_rgba1(rgba), n, n),
        ("visual/rgba8_degradado_alfa", optimize.OUT_ETC2_RGBA8, 0, etc.encode_etc2_rgba(grad), n, n),
        ("visual/r11_intensidad", optimize.OUT_R11_EAC, pack.TEXFLAG_SWIZZLE_I, etc.encode_eac(inten), n, n),
        ("visual/rg11_intensidad_alfa", optimize.OUT_RG11_EAC, pack.TEXFLAG_SWIZZLE_IA, etc.encode_rg11(inten, alpha),
         n, n),
    ]


def build(path: str, seed: int = 20261115) -> dict:
    """Escribe el paquete; devuelve un resumen (texturas por formato, modos cubiertos)."""
    rng = np.random.default_rng(seed)
    writer = pack.PackWriter(flags=pack.FLAG_GLES3)
    key = 1
    summary: dict = {"texturas": 0, "niveles": 0, "modos": {}, "formatos": {}}
    for fmt, bsize in FORMATS:
        for w, h in SIZES:
            levels = _levels(w, h)
            data = bytearray()
            for lw, lh in levels:
                nb = ((lw + 3) // 4) * ((lh + 3) // 4)
                data += rng.integers(0, 256, nb * bsize, dtype=np.uint8).tobytes()
            flags = {optimize.OUT_R11_EAC: pack.TEXFLAG_SWIZZLE_I,
                     optimize.OUT_RG11_EAC: pack.TEXFLAG_SWIZZLE_IA}.get(fmt, 0) if (w * h) % 2 else 0
            name = f"azar/{NAMES[fmt]}_{w}x{h}_m{len(levels)}"
            writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=fmt, count=len(levels), flags=flags, key=key, name=name,
                                  data=bytes(data), width=w, height=h))
            key += 1
            summary["texturas"] += 1
            summary["niveles"] += len(levels)
            summary["formatos"][NAMES[fmt]] = summary["formatos"].get(NAMES[fmt], 0) + 1
            for m, c in modos(bytes(data), fmt).items():
                mk = f"{NAMES[fmt]}:{m}"
                summary["modos"][mk] = summary["modos"].get(mk, 0) + c
    for name, fmt, flags, data, w, h in _visual():
        writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=fmt, count=1, flags=flags, key=key, name=name, data=data,
                              width=w, height=h))
        key += 1
        summary["texturas"] += 1
        summary["niveles"] += 1
    writer.write(path, {"tool": "choya prueba-etc", "seed": seed, "resumen": summary})
    return summary


LEEME = """Prueba ETC2/EAC de Choya Wii Converter (paquete sintético, sin datos del juego)

Contenido: {texturas} texturas, {niveles} niveles de mipmap. Bloques al azar de RGB8, RGB8A1, RGBA8_EAC, R11 y RG11
en tamaños que no son múltiplo de 4, más 4 imágenes para mirar.

En la consola (RG35XX H):
 1. Copia este assets.wlpk a ports/<carpeta del port de wl_view>/gamedata/ (guarda antes el tuyo).
 2. Crea en la carpeta del port un archivo vacío llamado verificar.
 3. Abre el port. wl_view escribe en log.txt y sale.
 4. Resultado esperado en log.txt:
      GL_RENDERER: Mali-G31 ...   GL_VERSION: OpenGL ES 3.x ...
      los cinco formatos "anunciado"
      ruta para ETC2/EAC: nativa
      cada formato con "0 con diferencias"; ruta de respaldo: 0 con diferencias
      RGB8A1: ... 0 con descarte distinto
      VERIFICACION OK
    R11/RG11 pueden quedar "a 1 de diferencia" (11 bits pasados a 8): eso cuenta como bien.
 5. Para probar el respaldo en la misma consola: en el lanzador exporta WL_SIN_ETC2=1 antes de ./wl_view.
    La verificación sigue revisando las dos rutas; el visor (sin "verificar") dice "ruta respaldo" en cada textura.
 6. Borra "verificar" y abre el port de nuevo para mirar. Al final de la lista:
      visual/rgb8a1_circulo_transparente: damero con un círculo por el que se ve el fondo gris; borde de escalera
        nítida (sin halo de color);
      visual/rgba8_degradado_alfa: de transparente (izquierda) a opaco (derecha);
      visual/r11_intensidad: de negro y transparente (izquierda) a blanco y opaco (derecha);
      visual/rg11_intensidad_alfa: de oscuro a claro (izquierda a derecha), opaco arriba y transparente abajo.
 7. Copia log.txt a la PC y mándalo. Hasta que no esté ese log, el códec en la Mali-G31 sigue sin confirmar.
"""


def main(a) -> int:
    os.makedirs(a.carpeta, exist_ok=True)
    path = os.path.join(a.carpeta, "assets.wlpk")
    s = build(path, a.semilla)
    with open(os.path.join(a.carpeta, "LEEME-prueba-etc.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(LEEME.format(**s))
    print(f"{path}: {s['texturas']} texturas, {s['niveles']} niveles")
    for k, v in sorted(s["modos"].items()):
        print(f"  {k}: {v} bloques")
    print(f"Instrucciones: {os.path.join(a.carpeta, 'LEEME-prueba-etc.txt')}")
    return 0
