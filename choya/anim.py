"""Animaciones de huesos CHR0 (NW4R G3D) y su horneado para el paquete (entrada KIND_ANIM, bloque "CANM").

La lectura sigue a ogws (g3d_resanmchr.h/.cpp): por nodo, escala, giro (grados XYZ) y traslación, cada componente
constante o con cuadros clave (FVS32/48/96, interpolación de Hermite) o, el giro, con un valor por cuadro (CV8/16/32,
interpolación lineal). La pose del modelo sigue a g3d_calcworld.cpp (CalcWorldMtx_Basic y la compensación de escala
de Maya, SSC): los nodos sin animación se quedan en su pose de reposo.

Bloque "CANM" versión 1, little-endian:
  0 char[4] "CANM"   4 u32 versión (1)   8 u32 cuadros   12 u32 huesos   16 u64 clave del modelo
  24 u32 banderas (bit0 en bucle)   28 float cuadros por segundo (60)
  32 por cuadro y por hueso del modelo (en su orden), la matriz de piel 3x4 por filas (pose animada por la inversa de
     la de reposo): lo que el vértice multiplica.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

import numpy as np

from .mdl0 import Model, _euler_matrix

# ResAnmChrNodeData::Flag
F_ANM_EXISTS, F_SCALE_UNIFORM = 1 << 0, 1 << 4
F_SSC_APPLY = 1 << 10
F_HAS_S, F_HAS_R, F_HAS_T = 1 << 22, 1 << 23, 1 << 24
F_S_CONST = (1 << 13, 1 << 14, 1 << 15)
F_R_CONST = (1 << 16, 1 << 17, 1 << 18)
F_T_CONST = (1 << 19, 1 << 20, 1 << 21)
SCALE_FMT = (1 << 25) | (1 << 26)
ROT_FMT = (1 << 27) | (1 << 28) | (1 << 29)
TRANS_FMT = (1 << 30) | (1 << 31)
NODE_SSC_APPLY = 1 << 5          # ResNodeData::FLAG_SSC_APPLY


class CHR0Error(ValueError):
    pass


def _hermite(v0, t0, v1, t1, s, d):
    s1 = s - 1.0
    return v0 + s * (s * ((2.0 * s - 3.0) * (v0 - v1))) + d * s1 * (s1 * t0 + s * t1)


def _s10_5(v: int) -> float:
    return v / 32.0


def _s7_8(v: int) -> float:
    return v / 256.0


@dataclass
class Track:
    """Un componente animado: constante (const) o cuadros clave (keys: [(cuadro, valor, pendiente)]) o un valor por
    cuadro (values, interpolación lineal)."""
    const: float | None = None
    keys: list = field(default_factory=list)
    values: list = field(default_factory=list)

    def at(self, frame: float) -> float:
        if self.const is not None:
            return self.const
        if self.values:
            fi = int(frame)
            if frame <= 0:
                return self.values[0]
            if fi >= len(self.values) - 1:
                return self.values[-1]
            r = frame - fi
            v0 = self.values[fi]
            return v0 if r == 0 else v0 + r * (self.values[fi + 1] - v0)
        k = self.keys
        if not k:
            return 0.0
        if frame <= k[0][0]:
            return k[0][1]
        if frame >= k[-1][0]:
            return k[-1][1]
        lo, hi = 0, len(k) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if k[mid][0] <= frame:
                lo = mid
            else:
                hi = mid
        f0, v0, t0 = k[lo]
        f1, v1, t1 = k[lo + 1]
        if frame == f0:
            return v0
        d = frame - f0
        return _hermite(v0, t0, v1, t1, d / (f1 - f0), d)


@dataclass
class NodeAnim:
    name: str
    flags: int
    s: list | None = None      # 3 Track (o None si no anima la escala)
    r: list | None = None
    t: list | None = None

    def srt(self, frame: float):
        s = tuple(tr.at(frame) for tr in self.s) if self.s else (1.0, 1.0, 1.0)
        r = tuple(tr.at(frame) for tr in self.r) if self.r else (0.0, 0.0, 0.0)
        t = tuple(tr.at(frame) for tr in self.t) if self.t else (0.0, 0.0, 0.0)
        return s, r, t


@dataclass
class Chr0:
    name: str
    frames: int
    loop: bool
    nodes: dict[str, NodeAnim]


def _cstr(d: bytes, o: int) -> str:
    end = d.find(b"\x00", o)
    return d[o:end if end >= 0 else len(d)].decode("shift_jis", errors="replace")


def _fvs(d: bytes, o: int, kind: int) -> list:
    n, = struct.unpack_from(">H", d, o)
    if n == 0 or n > 65535:
        raise CHR0Error("cuadros clave dañados")
    keys = []
    if kind == 1:      # FVS32: escala y desplazamiento, cuadro u8, valor de 12 bits, pendiente s7.5 de 12 bits
        scale, offset = struct.unpack_from(">ff", d, o + 8)
        for i in range(n):
            w, = struct.unpack_from(">I", d, o + 16 + 4 * i)
            slope = ((w & 0xFFF) ^ 0x800) - 0x800
            keys.append((float(w >> 24), scale * ((w >> 12) & 0xFFF) + offset, slope / 32.0))
    elif kind == 2:    # FVS48: cuadro s10.5, valor u16, pendiente s7.8
        scale, offset = struct.unpack_from(">ff", d, o + 8)
        for i in range(n):
            fr, val, slope = struct.unpack_from(">hHh", d, o + 16 + 6 * i)
            keys.append((_s10_5(fr), scale * val + offset, _s7_8(slope)))
    else:              # FVS96: flotantes
        for i in range(n):
            keys.append(struct.unpack_from(">fff", d, o + 8 + 12 * i))
    return keys


def _cv(d: bytes, o: int, kind: int, num_frame: int) -> list:
    n = num_frame + 1
    if kind == 1:
        scale, offset = struct.unpack_from(">ff", d, o)
        return [v * scale + offset for v in d[o + 8:o + 8 + n]]
    if kind == 2:
        scale, offset = struct.unpack_from(">ff", d, o)
        return [v * scale + offset for v in struct.unpack_from(f">{n}H", d, o + 8)]
    return list(struct.unpack_from(f">{n}f", d, o))


def read_chr0(d: bytes, off: int, name: str = "") -> Chr0:
    if d[off:off + 4] != b"CHR0":
        raise CHR0Error("no es CHR0")
    revision, = struct.unpack_from(">I", d, off + 8)
    dic_rel, = struct.unpack_from(">i", d, off + 0x10)
    if revision >= 5:  # versiones posteriores (MKWii) tienen otra cabecera; Wii Sports usa la 3 y la 4
        raise CHR0Error(f"CHR0 versión {revision} no soportada")
    num_frame, num_node, policy = struct.unpack_from(">HHI", d, off + 0x1C)
    dic = off + dic_rel
    _size, count = struct.unpack_from(">II", d, dic)
    if count > 4096:
        raise CHR0Error("diccionario dañado")
    nodes: dict[str, NodeAnim] = {}
    for i in range(1, count + 1):
        e = dic + 8 + 16 * i
        name_rel, data_rel = struct.unpack_from(">ii", d, e + 8)
        nd = dic + data_rel
        nname = _cstr(d, dic + name_rel)
        flags, = struct.unpack_from(">I", d, nd + 4)
        p = nd + 8

        def comp(fmt_bits, const_bits, kind_of, cv=False):
            nonlocal p
            out = []
            for cb in const_bits:
                if fmt_bits == 0 or (flags & cb):
                    out.append(Track(const=struct.unpack_from(">f", d, p)[0]))
                else:
                    rel, = struct.unpack_from(">i", d, p)
                    if cv:
                        out.append(Track(values=_cv(d, nd + rel, kind_of, num_frame)))
                    else:
                        out.append(Track(keys=_fvs(d, nd + rel, kind_of)))
                p += 4
            return out

        na = NodeAnim(nname, flags)
        if flags & F_HAS_S:
            fmt = (flags & SCALE_FMT) >> 25        # 0 constante, 1 FVS32, 2 FVS48, 3 FVS96
            if flags & F_SCALE_UNIFORM:
                x = comp(fmt, F_S_CONST[:1], fmt)[0]
                na.s = [x, x, x]
            else:
                na.s = comp(fmt, F_S_CONST, fmt)
        if flags & F_HAS_R:
            fmt = (flags & ROT_FMT) >> 27          # 0 const, 1 FVS32, 2 FVS48, 3 FVS96, 4 CV8, 5 CV16, 6 CV32
            if fmt >= 4:
                na.r = comp(fmt, F_R_CONST, fmt - 3, cv=True)
            else:
                na.r = comp(fmt, F_R_CONST, fmt)
        if flags & F_HAS_T:
            fmt = (flags & TRANS_FMT) >> 30
            na.t = comp(fmt, F_T_CONST, fmt)
        nodes[nname] = na
    return Chr0(name, max(1, num_frame), policy == 1, nodes)


def _pose(model: Model, chr0: Chr0 | None, frame: float) -> list[np.ndarray]:
    """Matriz final de cada hueso (espacio del modelo) como CalcWorld de NW4R."""
    bones = model.bones
    world: list = [None] * len(bones)    # W sin la escala propia
    scale: list = [None] * len(bones)
    final: list = [None] * len(bones)

    def calc(i):
        if final[i] is not None:
            return
        b = bones[i]
        if b.parent >= 0 and b.parent != i:
            calc(b.parent)
            w1, s1 = world[b.parent], scale[b.parent]
        else:
            w1, s1 = np.eye(4), np.ones(3)
        na = chr0.nodes.get(b.name) if chr0 else None
        if na is not None and na.flags & F_ANM_EXISTS:
            s, r, t = na.srt(frame)   # lo que no trae: escala 1, sin giro, sin traslación (GetAnmResult_*)
            # Con las banderas PATCH, el nodo pone su escala, giro o traslación de reposo (PatchChrAnmResult).
            s = b.srt[0] if na.flags & (1 << 7) else s
            r = b.srt[1] if na.flags & (1 << 8) else r
            t = b.srt[2] if na.flags & (1 << 9) else t
            ssc = bool(na.flags & F_SSC_APPLY)
        else:
            s, r, t = b.srt
            ssc = bool(b.flags & NODE_SSC_APPLY)
        rt = _euler_matrix((1.0, 1.0, 1.0), r, t)
        if ssc:      # Maya: la escala del padre solo mueve la traslación
            rt = rt.copy()
            rt[:3, 3] *= s1
            w = w1 @ rt
        else:        # W = W1 · S1 · RT
            w = w1 @ np.diag([*s1, 1.0]) @ rt
        world[i] = w
        scale[i] = np.array(s, dtype=float)
        final[i] = w @ np.diag([*s, 1.0])

    for i in range(len(bones)):
        calc(i)
    return final


def matches(model: Model, chr0: Chr0) -> bool:
    """La animación es de este modelo: todos sus nodos son huesos del modelo (y al menos uno se mueve)."""
    names = {b.name for b in model.bones}
    return bool(chr0.nodes) and all(n in names for n in chr0.nodes)


def bake(model: Model, chr0: Chr0) -> np.ndarray:
    """(cuadros, huesos, 3, 4) float32: la matriz de piel de cada hueso en cada cuadro."""
    rest = _pose(model, None, 0.0)
    inv = [np.linalg.inv(m) if abs(np.linalg.det(m)) > 1e-12 else np.eye(4) for m in rest]
    out = np.zeros((chr0.frames, len(model.bones), 3, 4), np.float32)
    for f in range(chr0.frames):
        pose = _pose(model, chr0, float(f))
        for i, m in enumerate(pose):
            out[f, i] = (m @ inv[i])[:3, :]
    return out


def pack_anim(model_key: int, baked: np.ndarray, loop: bool, fps: float = 60.0) -> bytes:
    frames, bones = baked.shape[:2]
    head = b"CANM" + struct.pack("<IIIQIf", 1, frames, bones, model_key, 1 if loop else 0, fps)
    return head + baked.astype("<f4").tobytes()


def unpack_anim(blob: bytes) -> dict:
    if blob[:4] != b"CANM":
        raise ValueError("no es CANM")
    ver, frames, bones, key, flags, fps = struct.unpack_from("<IIIQIf", blob, 4)
    m = np.frombuffer(blob, "<f4", frames * bones * 12, 32).reshape(frames, bones, 3, 4)
    return dict(version=ver, frames=frames, bones=bones, model_key=key, loop=bool(flags & 1), fps=fps, matrices=m)


def find_chr0(brres: bytes, sections: dict) -> list[tuple[str, int]]:
    return [(n, off) for n, off in sections.get("AnmChr(NW4R)", []) if brres[off:off + 4] == b"CHR0"]


def rest_matches_stored(model: Model) -> float:
    """Diferencia mayor entre la pose de reposo calculada y la guardada (para el reporte)."""
    pose = _pose(model, None, 0.0)
    return max((float(np.abs(p - b.world).max()) for p, b in zip(pose, model.bones)), default=0.0)


__all__ = ["Chr0", "CHR0Error", "read_chr0", "bake", "matches", "pack_anim", "unpack_anim", "find_chr0",
           "rest_matches_stored", "math"]
