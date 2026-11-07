"""Formato de malla del paquete (entrada KIND_MODEL) y exportación OBJ para revisar en la PC.

Bloque "CMDL" little-endian:
  0  char[4] "CMDL"   4 u32 versión (1)   8 u32 vértices   12 u32 índices   16 u32 submallas
  20 u32 banderas (bit0 normales, bit1 color de vértice)   24 float min[3]   36 float max[3]
  48 vértices: float pos[3], float normal[3], u8 color[4], float uv[2]  (36 bytes)
  luego índices u32, luego submallas de 32 bytes:
     u32 primer índice, u32 cantidad, u64 clave de textura (0 = sin textura),
     u8 cull (0 ninguno, 1 frente, 2 atrás, 3 todo), u8 xlu, u8 wrap_s, u8 wrap_t, u32[3] reservado
"""
from __future__ import annotations

import os
import struct

import numpy as np

from .mdl0 import Model

VERTEX = np.dtype([("pos", "<f4", 3), ("nrm", "<f4", 3), ("col", "u1", 4), ("uv", "<f4", 2)])
assert VERTEX.itemsize == 36
SUBMESH = struct.Struct("<IIQBBBB12x")
assert SUBMESH.size == 32


def pack_model(m: Model, texture_keys: list[int]) -> bytes:
    """texture_keys[i] = clave de la textura principal del material i (0 si no hay)."""
    n = m.positions.shape[0]
    v = np.zeros(n, VERTEX)
    v["pos"], v["nrm"], v["col"], v["uv"] = m.positions, m.normals, m.colors, m.uvs
    lo = m.positions.min(0) if n else np.zeros(3)
    hi = m.positions.max(0) if n else np.zeros(3)
    flags = (1 if m.has_normals else 0) | (2 if m.has_colors else 0)
    out = bytearray(b"CMDL" + struct.pack("<IIIII", 1, n, len(m.indices), len(m.submeshes), flags))
    out += struct.pack("<6f", *lo, *hi)
    out += v.tobytes()
    out += m.indices.astype("<u4").tobytes()
    for s in m.submeshes:
        mat = m.materials[s.material] if s.material < len(m.materials) else None
        key = texture_keys[s.material] if s.material < len(texture_keys) else 0
        lay = mat.layers[0] if mat and mat.layers else None
        out += SUBMESH.pack(s.first, s.count, key, mat.cull if mat else 2, int(s.xlu or (mat.xlu if mat else 0)),
                            lay.wrap_s if lay else 1, lay.wrap_t if lay else 1)
    return bytes(out)


def unpack_model(blob: bytes) -> dict:
    """Lector de referencia (pruebas)."""
    if blob[:4] != b"CMDL":
        raise ValueError("no es CMDL")
    _ver, nv, ni, ns, flags = struct.unpack_from("<IIIII", blob, 4)
    bbox = struct.unpack_from("<6f", blob, 24)
    v = np.frombuffer(blob, VERTEX, nv, 48)
    io = 48 + nv * 36
    idx = np.frombuffer(blob, "<u4", ni, io)
    so = io + ni * 4
    subs = [SUBMESH.unpack_from(blob, so + i * 32) for i in range(ns)]
    return dict(vertices=v, indices=idx, submeshes=subs, flags=flags, bbox=bbox)


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
