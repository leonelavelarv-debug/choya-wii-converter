"""Formato de malla del paquete (entrada KIND_MODEL) y exportación OBJ para revisar en la PC.

Bloque "CMDL" versión 2, little-endian:
  0  char[4] "CMDL"   4 u32 versión (2)   8 u32 vértices   12 u32 índices   16 u32 submallas
  20 u32 banderas (bit0 normales, bit1 color de vértice)   24 float min[3]   36 float max[3]
  48 u32 materiales   52..63 reservado
  64 vértices: float pos[3], float normal[3], u8 color[4], float uv[2]  (36 bytes)
  luego índices u32, luego submallas de 32 bytes:
     u32 primer índice, u32 cantidad, u64 clave de la textura principal (0 = sin textura),
     u8 cull, u8 xlu, u8 wrap_s, u8 wrap_t, u32 material, u32[2] reservado
  luego materiales de 512 bytes (MATERIAL, ver abajo): registros TEV crudos, como en el hardware GX.
"""
from __future__ import annotations

import os
import struct

import numpy as np

from .mdl0 import Model
from .tev import TevRaw

VERTEX = np.dtype([("pos", "<f4", 3), ("nrm", "<f4", 3), ("col", "u1", 4), ("uv", "<f4", 2)])
assert VERTEX.itemsize == 36
SUBMESH = struct.Struct("<IIQBBBBI8x")
assert SUBMESH.size == 32
HEADER_SIZE = 64

# Registro de material (512 bytes):
#   0 u8 etapas, u8 texgens, u8 xlu, u8 cull        4 u32 alpha_compare   8 u32 blend   12 u32 zmode
#  16 s16 cregs[4][4] (PREV, C0, C1, C2; RGBA)       48 u8 kregs[4][4] (K0..K3)
#  64 u32 ksel[8]   96 u32 texgen[8]   128 u32 dualtex[8]
# 160 etapas[16] de 12 bytes: u32 color_env, u32 alpha_env, u8 texmap, u8 texcoord, u8 tex_on, u8 ras
# 352 mapas[8] de 16 bytes: u64 clave de textura, u8 wrap_s, u8 wrap_t, u8 presente, 5 bytes libres
MATERIAL_SIZE = 512


def pack_material(t: TevRaw | None, xlu: bool, cull: int, maps: list[tuple[int, int, int] | None]) -> bytes:
    t = t or TevRaw()
    out = bytearray(MATERIAL_SIZE)
    struct.pack_into("<BBBBIII", out, 0, t.num_stages, t.num_texgens, int(xlu), cull,
                     t.alpha_compare, t.blend, t.zmode)
    struct.pack_into("<16h", out, 16, *[max(-1024, min(1023, v)) for row in t.cregs for v in row])
    struct.pack_into("<16B", out, 48, *[v for row in t.kregs for v in row])
    struct.pack_into("<8I", out, 64, *t.ksel)
    struct.pack_into("<8I", out, 96, *t.texgen)
    struct.pack_into("<8I", out, 128, *t.dualtex)
    for i in range(16):
        tm, tc, en, ras = t.tref[i]
        struct.pack_into("<IIBBBB", out, 160 + 12 * i, t.color_env[i], t.alpha_env[i], tm, tc, en, ras)
    for i, mp in enumerate(maps[:8]):
        if mp:
            key, ws, wt = mp
            struct.pack_into("<QBBB", out, 352 + 16 * i, key, ws, wt, 1)
    return bytes(out)


def pack_model(m: Model, texture_keys: list[int], map_keys: list[list[tuple[int, int, int] | None]] | None = None
               ) -> bytes:
    """texture_keys[i] = clave de la textura principal del material i; map_keys[i][texmap] = (clave, ws, wt)."""
    n = m.positions.shape[0]
    v = np.zeros(n, VERTEX)
    v["pos"], v["nrm"], v["col"], v["uv"] = m.positions, m.normals, m.colors, m.uvs
    lo = m.positions.min(0) if n else np.zeros(3)
    hi = m.positions.max(0) if n else np.zeros(3)
    flags = (1 if m.has_normals else 0) | (2 if m.has_colors else 0)
    out = bytearray(b"CMDL" + struct.pack("<IIIII", 2, n, len(m.indices), len(m.submeshes), flags))
    out += struct.pack("<6f", *lo, *hi)
    out += struct.pack("<I12x", len(m.materials))
    assert len(out) == HEADER_SIZE
    out += v.tobytes()
    out += m.indices.astype("<u4").tobytes()
    for s in m.submeshes:
        mat = m.materials[s.material] if s.material < len(m.materials) else None
        key = texture_keys[s.material] if s.material < len(texture_keys) else 0
        lay = mat.layers[0] if mat and mat.layers else None
        out += SUBMESH.pack(s.first, s.count, key, mat.cull if mat else 2, int(s.xlu or (mat.xlu if mat else 0)),
                            lay.wrap_s if lay else 1, lay.wrap_t if lay else 1, s.material)
    for i, mat in enumerate(m.materials):
        maps = map_keys[i] if map_keys and i < len(map_keys) else [None] * 8
        xlu = mat.xlu or any(s.xlu for s in m.submeshes if s.material == i)
        out += pack_material(mat.tev, xlu, mat.cull, maps)
    return bytes(out)


def unpack_model(blob: bytes) -> dict:
    """Lector de referencia (pruebas)."""
    if blob[:4] != b"CMDL":
        raise ValueError("no es CMDL")
    ver, nv, ni, ns, flags = struct.unpack_from("<IIIII", blob, 4)
    bbox = struct.unpack_from("<6f", blob, 24)
    base = HEADER_SIZE if ver >= 2 else 48
    nm = struct.unpack_from("<I", blob, 48)[0] if ver >= 2 else 0
    v = np.frombuffer(blob, VERTEX, nv, base)
    io = base + nv * 36
    idx = np.frombuffer(blob, "<u4", ni, io)
    so = io + ni * 4
    subs = [SUBMESH.unpack_from(blob, so + i * 32) for i in range(ns)]
    mo = so + ns * 32
    mats = [blob[mo + i * MATERIAL_SIZE: mo + (i + 1) * MATERIAL_SIZE] for i in range(nm)]
    return dict(version=ver, vertices=v, indices=idx, submeshes=subs, flags=flags, bbox=bbox, materials=mats)


def write_obj(m: Model, path: str, texture_files: list[str | None]):
    """OBJ + MTL para abrir el modelo en el Visor 3D de Windows o en Blender."""
    base = os.path.splitext(path)[0]
    mtl_name = os.path.basename(base) + ".mtl"
    with open(base + ".mtl", "w", encoding="utf-8") as f:
        for i, mat in enumerate(m.materials):
            f.write(f"newmtl m{i}_{mat.name}\nKd 1 1 1\n")
            if i < len(texture_files) and texture_files[i]:
                f.write(f"map_Kd {texture_files[i]}\n")
            f.write("\n")
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# {m.name} — Choya Wii Converter\nmtllib {mtl_name}\n")
        for p in m.positions:
            f.write(f"v {p[0]:.5f} {p[1]:.5f} {p[2]:.5f}\n")
        for t in m.uvs:
            f.write(f"vt {t[0]:.5f} {1 - t[1]:.5f}\n")
        for n in m.normals:
            f.write(f"vn {n[0]:.4f} {n[1]:.4f} {n[2]:.4f}\n")
        for s in m.submeshes:
            mat = m.materials[s.material] if s.material < len(m.materials) else None
            f.write(f"usemtl m{s.material}_{mat.name if mat else 'x'}\n")
            idx = m.indices[s.first:s.first + s.count].reshape(-1, 3) + 1
            for a, b, c in idx:   # GX usa sentido horario como cara frontal; OBJ, antihorario
                f.write(f"f {a}/{a}/{a} {c}/{c}/{c} {b}/{b}/{b}\n")
