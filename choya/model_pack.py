"""Formato de malla del paquete (entrada KIND_MODEL) y exportación OBJ para revisar en la PC.

Bloque "CMDL" versión 3, little-endian (el runtime lee también la 2 y la 1):
  0  char[4] "CMDL"   4 u32 versión (3)   8 u32 vértices   12 u32 índices   16 u32 submallas
  20 u32 banderas (bit0 normales, bit1 color de vértice, bit2 con huesos)   24 float min[3]   36 float max[3]
  48 u32 materiales   52 u32 huesos   56..63 reservado
  64 vértices: float pos[3], float normal[3], u8 color[4], float uv[2], u8 hueso[4], u8 peso[4]  (44 bytes;
     36 en la versión 2, sin huesos). Peso 0 en los cuatro = vértice fijo.
  luego índices u32, luego submallas de 32 bytes:
     u32 primer índice, u32 cantidad, u64 clave de la textura principal (0 = sin textura),
     u8 cull, u8 xlu, u8 wrap_s, u8 wrap_t, u32 material, u32[2] reservado
  luego materiales de 1024 bytes (512 en la versión 2; MATERIAL, ver abajo): registros TEV crudos, como en GX.
  luego huesos de 64 bytes: i32 padre, u32 banderas, float inversa de la pose de reposo[12] (3x4 por filas),
     u32 FNV-1a del nombre, u32 reservado.
"""
from __future__ import annotations

import os
import struct

import numpy as np

from .mdl0 import Model
from .tev import TevRaw

VERTEX_V2 = np.dtype([("pos", "<f4", 3), ("nrm", "<f4", 3), ("col", "u1", 4), ("uv", "<f4", 2)])
VERTEX = np.dtype([("pos", "<f4", 3), ("nrm", "<f4", 3), ("col", "u1", 4), ("uv", "<f4", 2),
                   ("bone", "u1", 4), ("weight", "u1", 4)])
assert VERTEX_V2.itemsize == 36 and VERTEX.itemsize == 44
BONE = struct.Struct("<iI12fII")
assert BONE.size == 64
VERSION = 3
SUBMESH = struct.Struct("<IIQBBBBI8x")
assert SUBMESH.size == 32
HEADER_SIZE = 64

# Registro de material (512 bytes):
#   0 u8 etapas, u8 texgens, u8 xlu, u8 cull        4 u32 alpha_compare   8 u32 blend   12 u32 zmode
#  16 s16 cregs[4][4] (PREV, C0, C1, C2; RGBA)       48 u8 kregs[4][4] (K0..K3)
#  64 u32 ksel[8]   96 u32 texgen[8]   128 u32 dualtex[8]
# 160 etapas[16] de 12 bytes: u32 color_env, u32 alpha_env, u8 texmap, u8 texcoord, u8 tex_on, u8 ras
# 352 mapas[8] de 16 bytes: u64 clave de textura, u8 wrap_s, u8 wrap_t, u8 presente, 5 bytes libres
# 512 matrices de textura[8]: float[2][4] (filas s y t sobre (s, t, 1, 1)); ResTexSrtData por la matriz de efecto
# 768 u8 modo de cada matriz[8]: 0 sin matriz, 1 + modo de mapeo de NW4R (0 coordenadas, 1/3 entorno, 2 proyección)
# 776 u8 canales de luz, 3 libres
# 780 canales[2] de 20 bytes: u32 banderas (ResChan), u8 color de material[4], u8 color ambiente[4],
#     u32 control del color, u32 control del alfa (registro XF 0x100E de GX)
MATERIAL_SIZE = 1024
MATERIAL_SIZE_V2 = 512


def pack_material(t: TevRaw | None, xlu: bool, cull: int, maps: list[tuple[int, int, int] | None],
                  mat=None) -> bytes:
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
    if mat is not None:
        for i, tm in enumerate(mat.tex_mtx[:8]):
            if tm is not None:
                struct.pack_into("<8f", out, 512 + 32 * i, *tm.rows)
                out[768 + i] = 1 + tm.map_mode
        out[776] = mat.num_chans
        for c, ch in enumerate(mat.chans[:2]):
            struct.pack_into("<I4B4BII", out, 780 + 20 * c, ch.flags, *ch.mat, *ch.amb, ch.ctrl_color,
                             ch.ctrl_alpha)
    return bytes(out)


def fnv1a32(text: str) -> int:
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def pack_model(m: Model, texture_keys: list[int], map_keys: list[list[tuple[int, int, int] | None]] | None = None
               ) -> bytes:
    """texture_keys[i] = clave de la textura principal del material i; map_keys[i][texmap] = (clave, ws, wt)."""
    n = m.positions.shape[0]
    v = np.zeros(n, VERTEX)
    v["pos"], v["nrm"], v["col"], v["uv"] = m.positions, m.normals, m.colors, m.uvs
    skinned = m.skinned
    if skinned:
        v["bone"], v["weight"] = m.skin_idx, m.skin_w
    lo = m.positions.min(0) if n else np.zeros(3)
    hi = m.positions.max(0) if n else np.zeros(3)
    flags = (1 if m.has_normals else 0) | (2 if m.has_colors else 0) | (4 if skinned else 0)
    bones = m.bones if skinned else []
    out = bytearray(b"CMDL" + struct.pack("<IIIII", VERSION, n, len(m.indices), len(m.submeshes), flags))
    out += struct.pack("<6f", *lo, *hi)
    out += struct.pack("<II8x", len(m.materials), len(bones))
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
        out += pack_material(mat.tev, xlu, mat.cull, maps, mat)
    for b in bones:
        inv = np.linalg.inv(b.world) if abs(np.linalg.det(b.world)) > 1e-12 else np.eye(4)
        out += BONE.pack(b.parent, b.flags, *inv[:3, :].reshape(-1), fnv1a32(b.name), 0)
    return bytes(out)


def unpack_model(blob: bytes) -> dict:
    """Lector de referencia (pruebas)."""
    if blob[:4] != b"CMDL":
        raise ValueError("no es CMDL")
    ver, nv, ni, ns, flags = struct.unpack_from("<IIIII", blob, 4)
    bbox = struct.unpack_from("<6f", blob, 24)
    base = HEADER_SIZE if ver >= 2 else 48
    nm = struct.unpack_from("<I", blob, 48)[0] if ver >= 2 else 0
    nb = struct.unpack_from("<I", blob, 52)[0] if ver >= 3 else 0
    vt = VERTEX if ver >= 3 else VERTEX_V2
    v = np.frombuffer(blob, vt, nv, base)
    io = base + nv * vt.itemsize
    idx = np.frombuffer(blob, "<u4", ni, io)
    so = io + ni * 4
    subs = [SUBMESH.unpack_from(blob, so + i * 32) for i in range(ns)]
    mo = so + ns * 32
    msz = MATERIAL_SIZE if ver >= 3 else MATERIAL_SIZE_V2
    mats = [blob[mo + i * msz: mo + (i + 1) * msz] for i in range(nm)]
    bo = mo + nm * msz
    bones = [BONE.unpack_from(blob, bo + i * BONE.size) for i in range(nb)]
    return dict(version=ver, vertices=v, indices=idx, submeshes=subs, flags=flags, bbox=bbox, materials=mats,
                bones=bones)


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
