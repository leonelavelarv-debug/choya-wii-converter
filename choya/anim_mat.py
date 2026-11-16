"""Animaciones de material y visibilidad de NW4R G3D: SRT0 (matrices de textura), CLR0 (colores), PAT0 (cambio de
textura) y VIS0 (visibilidad de nodos), horneadas para un modelo (entrada KIND_MATANIM, bloque "CMAN").

La lectura sigue a ogws (dominio público, CC0): g3d_resanm.h/.cpp (cuadros clave de Hermite, colores con interpolación
lineal de 15 bits, bits de visibilidad), g3d_resanmtexsrt/clr/texpat/vis.h/.cpp y la aplicación al material de
g3d_anmtexsrt.cpp (ApplyTexSrtAnmResult), g3d_anmclr.cpp (ApplyClrAnmResult: (color & máscara) | animado),
g3d_anmtexpat.cpp (ApplyTexPatAnmResult: la textura de la unidad GX_TEXMAP i, sin tocar el wrap) y g3d_draw.cpp (un
objeto se dibuja si su nodo de visibilidad está visible).

Todo se resuelve aquí, en la PC: los nombres de material, de nodo y de textura pasan a índices del modelo y a claves de
textura del paquete; la matriz de cada coordenada se recalcula como LoadResTexSrt con la matriz de efecto del material;
los colores salen ya con la máscara aplicada sobre los del material. El runtime solo copia valores por cuadro.

Bloque "CMAN" versión 1, little-endian:
  0 char[4] "CMAN"   4 u32 versión (1)   8 u32 cuadros   12 u32 tipo (1 SRT0, 2 CLR0, 3 PAT0, 4 VIS0)
  16 u64 clave del modelo   24 u32 banderas (bit0 en bucle)   28 float cuadros por segundo (60)
  32 u32 pistas   36 u32 claves de textura (PAT0)   40 u32 offset de las pistas   44 u32 offset de las claves
  48..63 libre
  pistas de 16 bytes: u16 destino (material; en VIS0, submalla), u8 ranura (coordenada, color o unidad de textura),
     u8 banderas (bit0 constante: un solo valor), u32 offset de los valores, u32 valores, u32 libre
  valores por cuadro:
     SRT0: float[8] (filas s y t de la matriz de textura, como el material del CMDL)
     CLR0: u8[4] RGBA (ranura 0 CLR0, 1 CLR1, 2 AMB0, 3 AMB1, 4-6 TEV0-2 = registros C0-C2, 7-10 TEVK0-3)
     PAT0: u16 índice en la tabla de claves (0xFFFF = sin textura en el paquete: se deja la del material)
     VIS0: u8 0/1
  claves: u64 por textura (PAT0)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from . import mdl0 as md
from .anim import _hermite

TYPE_SRT, TYPE_CLR, TYPE_PAT, TYPE_VIS = 1, 2, 3, 4
TYPE_NAMES = {TYPE_SRT: "srt0", TYPE_CLR: "clr0", TYPE_PAT: "pat0", TYPE_VIS: "vis0"}
MAGIC = {TYPE_SRT: b"SRT0", TYPE_CLR: b"CLR0", TYPE_PAT: b"PAT0", TYPE_VIS: b"VIS0"}
FOLDERS = {TYPE_SRT: "AnmTexSrt(NW4R)", TYPE_CLR: "AnmClr(NW4R)", TYPE_PAT: "AnmTexPat(NW4R)",
           TYPE_VIS: "AnmVis(NW4R)"}
REVISIONS = {TYPE_SRT: (4,), TYPE_CLR: (3,), TYPE_PAT: (3,), TYPE_VIS: (3,)}
HEADER = struct.Struct("<4sIIIQIfIIII16x")
TRACK = struct.Struct("<HBBIII")
assert HEADER.size == 64 and TRACK.size == 16
NO_KEY = 0xFFFF
MAX_FRAMES = 1 << 16
CLR_TARGETS = ("CLR0", "CLR1", "AMB0", "AMB1", "TEV0", "TEV1", "TEV2", "TEVK0", "TEVK1", "TEVK2", "TEVK3")


class MatAnimError(ValueError):
    pass


def _cstr(d: bytes, o: int) -> str:
    if not 0 <= o < len(d):
        raise MatAnimError("nombre fuera del archivo")
    end = d.find(b"\x00", o)
    return d[o:end if end >= 0 else len(d)].decode("shift_jis", errors="replace")


class _Reader:
    """Lecturas con límites: un archivo cortado o con offsets malos da MatAnimError, nunca lee fuera."""

    def __init__(self, d: bytes, start: int, end: int):
        self.d, self.start, self.end = d, start, end

    def unpack(self, fmt: str, o: int):
        size = struct.calcsize(fmt)
        if o < self.start or o + size > self.end:
            raise MatAnimError(f"lectura fuera del bloque en 0x{o:X}")
        return struct.unpack_from(fmt, self.d, o)

    def u32(self, o):
        return self.unpack(">I", o)[0]

    def s32(self, o):
        return self.unpack(">i", o)[0]


def _dic(r: _Reader, dic: int, limit: int = 4096) -> list[tuple[str, int]]:
    """ResDic de NW4R: [(nombre, dirección de los datos)] sin la entrada raíz; offsets relativos al diccionario."""
    _size, count = r.unpack(">II", dic)
    if count > limit:
        raise MatAnimError("diccionario dañado")
    out = []
    for i in range(1, count + 1):
        name_rel, data_rel = r.unpack(">ii", dic + 8 + 16 * i + 8)
        out.append((_cstr(r.d, dic + name_rel), dic + data_rel))
    return out


@dataclass
class KeyTrack:
    """Un valor animado (ResAnmData): constante o cuadros clave de Hermite (frame, value, slope)."""
    const: float | None = None
    keys: list = field(default_factory=list)

    def at(self, frame: float) -> float:
        if self.const is not None:
            return self.const
        k = self.keys
        if frame <= k[0][0]:
            return k[0][1]
        if frame >= k[-1][0]:
            return k[-1][1]
        lo = 0
        while lo + 1 < len(k) and k[lo + 1][0] <= frame:   # el último cuadro clave con frame <= cuadro
            lo += 1
        f0, v0, t0 = k[lo]
        if frame == f0:
            return v0
        f1, v1, t1 = k[lo + 1]
        p = frame - f0
        return _hermite(v0, t0, v1, t1, p / (f1 - f0), p)


def _key_track(r: _Reader, o: int, constant: bool) -> KeyTrack:
    if constant:
        return KeyTrack(const=r.unpack(">f", o)[0])
    kd = o + r.s32(o)                       # relativo al propio ResAnmData
    n, = r.unpack(">H", kd)
    if n == 0:
        raise MatAnimError("pista sin cuadros clave")
    keys = [r.unpack(">fff", kd + 8 + 12 * i) for i in range(n)]
    if any(keys[i + 1][0] < keys[i][0] for i in range(n - 1)):
        raise MatAnimError("cuadros clave fuera de orden")
    return KeyTrack(keys=keys)


def _header(d: bytes, off: int, kind: int) -> _Reader:
    if d[off:off + 4] != MAGIC[kind]:
        raise MatAnimError(f"no es {MAGIC[kind].decode()}")
    size, = struct.unpack_from(">I", d, off + 4)
    end = min(len(d), off + size) if size >= 0x20 else len(d)
    r = _Reader(d, off, end)
    rev = r.u32(off + 8)
    if rev not in REVISIONS[kind]:
        raise MatAnimError(f"{MAGIC[kind].decode()} versión {rev} no soportada")
    return r


# ------------------------------------------------------------------------------------------------ lectores

@dataclass
class Srt0:
    name: str
    frames: int
    loop: bool
    mode: int                                   # TexMatrixMode (0 Maya, 1 XSI, 2 3ds Max)
    mats: dict = field(default_factory=dict)    # material -> {coordenada: (banderas 4 bits, [5 KeyTrack])}
    ind: int = 0                                # matrices indirectas animadas (no se aplican en el visor)


def read_srt0(d: bytes, off: int, name: str = "") -> Srt0:
    r = _header(d, off, TYPE_SRT)
    num_frame, num_mat, mode, policy = r.unpack(">HHII", off + 0x1C)
    if mode > 2:
        raise MatAnimError(f"modo de matriz {mode} desconocido")
    out = Srt0(name, max(1, num_frame), policy == 1, mode)
    for mname, md_ in _dic(r, off + r.s32(off + 0x10)):
        flags, ind_flags = r.unpack(">II", md_ + 4)
        ptr = md_ + 0xC
        slots: dict = {}
        for group, gflags in ((0, flags), (1, ind_flags)):
            for i in range(8 if group == 0 else 3):
                if not (gflags >> (4 * i)) & 1:
                    continue
                td = md_ + r.s32(ptr)
                ptr += 4
                tf = r.u32(td)
                a = td + 4
                tracks = []

                def take(const_bit):
                    nonlocal a
                    t = _key_track(r, a, bool(tf & const_bit))
                    a += 4
                    return t
                if not tf & 2:              # SCALE_ONE no: Su y (si no es uniforme) Sv
                    su = take(1 << 5)
                    sv = su if tf & (1 << 4) else take(1 << 6)
                else:
                    su = sv = KeyTrack(const=1.0)
                rot = take(1 << 7) if not tf & 4 else KeyTrack(const=0.0)
                if not tf & 8:
                    tu, tv = take(1 << 8), take(1 << 9)
                else:
                    tu = tv = KeyTrack(const=0.0)
                tracks = [su, sv, rot, tu, tv]
                if group == 0:
                    slots[i] = (tf & 0xF, tracks)
                else:
                    out.ind += 1
        out.mats[mname] = slots
    return out


@dataclass
class Clr0:
    name: str
    frames: int
    loop: bool
    mats: dict = field(default_factory=dict)    # material -> {objetivo: (máscara, [colores u32 por cuadro] o constante)}


def read_clr0(d: bytes, off: int, name: str = "") -> Clr0:
    r = _header(d, off, TYPE_CLR)
    num_frame, num_mat, policy = r.unpack(">HHI", off + 0x1C)
    out = Clr0(name, max(1, num_frame), policy == 1)
    for mname, md_ in _dic(r, off + r.s32(off + 0x10)):
        flags = r.u32(md_ + 4)
        a = md_ + 8
        targets = {}
        for i in range(len(CLR_TARGETS)):
            f = (flags >> (2 * i)) & 3
            if not f & 1:
                continue
            mask, = r.unpack(">I", a)
            if f & 2:
                colors = [r.u32(a + 4)]
            else:
                fd = a + 4 + r.s32(a + 4)        # relativo al campo del color
                colors = [r.u32(fd + 4 * k) for k in range(num_frame + 1)]
            targets[i] = (mask, colors)
            a += 8
        out.mats[mname] = targets
    return out


def clr_at(colors: list, frame: float, num_frame: int) -> int:
    """GetResColorAnmResult con ClipFrame: interpolación lineal de 15 bits entre cuadros enteros."""
    if len(colors) == 1:
        return colors[0]
    frame = 0.0 if frame <= 0 else min(frame, float(num_frame))
    i = int(frame)
    frac = frame - i
    if frac == 0 or i + 1 >= len(colors):
        return colors[min(i, len(colors) - 1)]
    ratio = int(32768 * frac)
    a, b = colors[i], colors[i + 1]
    out = 0
    for sh in (24, 16, 8, 0):
        x, y = (a >> sh) & 255, (b >> sh) & 255
        out |= ((x + (((y - x) * ratio) >> 15)) & 255) << sh
    return out


@dataclass
class Pat0:
    name: str
    frames: int
    loop: bool
    textures: list = field(default_factory=list)   # nombres de textura por índice
    palettes: list = field(default_factory=list)
    mats: dict = field(default_factory=dict)       # material -> {unidad: (banderas 4 bits, [(cuadro, tex, pltt)])}


def read_pat0(d: bytes, off: int, name: str = "") -> Pat0:
    r = _header(d, off, TYPE_PAT)
    num_frame, num_mat, num_tex, num_pltt, policy = r.unpack(">HHHHI", off + 0x2C)
    out = Pat0(name, max(1, num_frame), policy == 1)

    def names(arr_rel, n):
        if not arr_rel or n == 0:
            return []
        arr = off + arr_rel
        return [_cstr(d, arr + r.s32(arr + 4 * i)) for i in range(n)]   # relativos al principio del arreglo
    out.textures = names(r.s32(off + 0x14), num_tex)
    out.palettes = names(r.s32(off + 0x18), num_pltt)
    for mname, md_ in _dic(r, off + r.s32(off + 0x10)):
        flags = r.u32(md_ + 4)
        a = md_ + 8
        units = {}
        for i in range(8):
            f = (flags >> (4 * i)) & 0xF
            if not f & 1:
                continue
            if f & 2:
                tex, pltt = r.unpack(">HH", a)
                frames = [(0.0, tex, pltt)]
            else:
                ad = md_ + r.s32(a)               # relativo a los datos del material
                n, = r.unpack(">H", ad)
                if n == 0:
                    raise MatAnimError("PAT0 sin cuadros")
                frames = [r.unpack(">fHH", ad + 8 + 8 * k) for k in range(n)]
            units[i] = (f, frames)
            a += 4
        out.mats[mname] = units
    return out


def pat_at(frames: list, frame: float) -> tuple[int, int]:
    """SearchFrame: el último cuadro con frame <= cuadro (cambio discreto, sin interpolar)."""
    if frame <= frames[0][0]:
        return frames[0][1], frames[0][2]
    best = frames[0]
    for fr in frames:
        if fr[0] <= frame:
            best = fr
        else:
            break
    return best[1], best[2]


@dataclass
class Vis0:
    name: str
    frames: int
    loop: bool
    nodes: dict = field(default_factory=dict)   # nodo -> bool constante o [bool por cuadro]


def read_vis0(d: bytes, off: int, name: str = "") -> Vis0:
    r = _header(d, off, TYPE_VIS)
    num_frame, num_node, policy = r.unpack(">HHI", off + 0x1C)
    out = Vis0(name, max(1, num_frame), policy == 1)
    for nname, nd in _dic(r, off + r.s32(off + 0x10)):
        flags = r.u32(nd + 4)
        if flags & 2:
            out.nodes[nname] = bool(flags & 1)
        else:
            words = [r.u32(nd + 8 + 4 * k) for k in range(num_frame // 32 + 1)]
            out.nodes[nname] = [bool(words[f // 32] & ((1 << 31) >> (f % 32))) for f in range(num_frame + 1)]
    return out


READERS = {TYPE_SRT: read_srt0, TYPE_CLR: read_clr0, TYPE_PAT: read_pat0, TYPE_VIS: read_vis0}


def find(brres: bytes, sections: dict) -> list[tuple[int, str, int]]:
    """[(tipo, nombre, offset)] de las animaciones de material y visibilidad de un BRRES."""
    out = []
    for kind, folder in FOLDERS.items():
        for n, off in sections.get(folder, []):
            if brres[off:off + 4] == MAGIC[kind]:
                out.append((kind, n, off))
    return out


def targets(anim) -> set[str]:
    return set(anim.nodes) if isinstance(anim, Vis0) else set(anim.mats)


# ------------------------------------------------------------------------------------------------ horneado

@dataclass
class Baked:
    kind: int
    frames: int
    loop: bool
    tracks: list = field(default_factory=list)    # (destino, ranura, np.ndarray de valores por cuadro)
    keys: list = field(default_factory=list)      # PAT0: claves de textura
    notes: dict = field(default_factory=dict)     # avisos contados (p. ej. paletas animadas)

    def note(self, what: str, n: int = 1):
        self.notes[what] = self.notes.get(what, 0) + n


def _mat_index(model: md.Model) -> dict[str, list[int]]:
    out: dict[str, list[int]] = {}
    for i, m in enumerate(model.materials):
        out.setdefault(m.name, []).append(i)
    return out


def _color_u32(rgba) -> int:
    r, g, b, a = (max(0, min(255, int(v))) for v in rgba)
    return (r << 24) | (g << 16) | (b << 8) | a


def _base_color(mat: md.Material, target: int) -> int:
    """El color del material antes de la animación (lo que leen GXGetTevColor / ResMatChan)."""
    if target in (0, 1):
        return _color_u32(mat.chans[target].mat)
    if target in (2, 3):
        return _color_u32(mat.chans[target - 2].amb)
    t = mat.tev
    if t is None:
        return 0x000000FF
    if 4 <= target <= 6:
        return _color_u32(t.cregs[target - 3])       # cregs[0] es PREV: TEV0..2 = C0..C2
    return _color_u32(t.kregs[target - 7])


def bake(model: md.Model, anim, key_of_texture=None) -> Baked:
    """Valores por cuadro (0..cuadros-1) para este modelo. key_of_texture(nombre) -> clave del paquete o 0."""
    if isinstance(anim, Srt0):
        kind = TYPE_SRT
    elif isinstance(anim, Clr0):
        kind = TYPE_CLR
    elif isinstance(anim, Pat0):
        kind = TYPE_PAT
    else:
        kind = TYPE_VIS
    n = anim.frames
    if n > MAX_FRAMES:
        raise MatAnimError(f"{n} cuadros: demasiados")
    out = Baked(kind, n, anim.loop)
    mats = _mat_index(model)
    frames = np.arange(n, dtype=np.float64)
    if kind == TYPE_SRT:
        if anim.ind:
            out.note("matrices indirectas (no se aplican en el visor)", anim.ind)
        for mname, slots in anim.mats.items():
            for mi in mats.get(mname, []):
                mat = model.materials[mi]
                for slot, (flag, tracks) in slots.items():
                    eff = mat.tex_effects[slot] if slot < len(mat.tex_effects) else None
                    if eff is None:
                        eff = md.TexEffect(0, (1.0, 1.0, 0.0, 0.0, 0.0), 0, 1, np.eye(4))
                    vals = np.zeros((n, 8), np.float32)
                    for f in range(n):
                        srt = tuple(t.at(float(f)) for t in tracks)
                        tm = md.tex_matrix(eff, anim.mode, flag=flag, srt=srt)
                        vals[f] = tm.rows if tm is not None else (1, 0, 0, 0, 0, 1, 0, 0)
                    if slot >= (mat.tev.num_texgens if mat.tev else 0):
                        out.note("coordenadas animadas sin texgen")
                    out.tracks.append((mi, slot, vals))
    elif kind == TYPE_CLR:
        for mname, tg in anim.mats.items():
            for mi in mats.get(mname, []):
                mat = model.materials[mi]
                for target, (mask, colors) in tg.items():
                    base = _base_color(mat, target)
                    vals = np.zeros((n, 4), np.uint8)
                    for f in range(n):
                        c = (base & mask) | clr_at(colors, float(f), anim.frames)
                        vals[f] = ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255)
                    out.tracks.append((mi, target, vals))
    elif kind == TYPE_PAT:
        key_index: dict[int, int] = {}
        for mname, units in anim.mats.items():
            for mi in mats.get(mname, []):
                for unit, (flag, keyframes) in units.items():
                    vals = np.full(n, NO_KEY, np.uint16)
                    if flag & 8:
                        out.note("paletas animadas (se usa la de la textura)")
                    if not flag & 4:
                        out.tracks.append((mi, unit, vals))
                        continue
                    for f in range(n):
                        ti, _pi = pat_at(keyframes, float(f))
                        tname = anim.textures[ti] if ti < len(anim.textures) else None
                        key = key_of_texture(tname) if (tname and key_of_texture) else 0
                        if not key:
                            out.note("texturas de PAT0 que no están en el paquete")
                            continue
                        if key not in key_index:
                            key_index[key] = len(out.keys)
                            out.keys.append(key)
                        vals[f] = key_index[key]
                    out.tracks.append((mi, unit, vals))
    else:
        by_name: dict[str, list[int]] = {}
        for b in model.bones:
            by_name.setdefault(b.name, []).append(b.index)
        for nname, v in anim.nodes.items():
            node_ids = set(by_name.get(nname, []))
            for si, sm in enumerate(model.submeshes):
                if sm.vis_node not in node_ids:
                    continue
                if isinstance(v, bool):
                    vals = np.full(n, int(v), np.uint8)
                else:
                    vals = np.array([int(v[min(int(np.floor(f)), len(v) - 1)]) for f in frames], np.uint8)
                out.tracks.append((si, 0, vals))
    return out


def pack(model_key: int, b: Baked, fps: float = 60.0) -> bytes:
    """Bloque CMAN; las pistas que no cambian se guardan con un solo valor."""
    body = bytearray()
    tracks = bytearray()
    data_start = HEADER.size + TRACK.size * len(b.tracks)
    for target, slot, vals in b.tracks:
        const = bool(len(vals)) and bool(np.all(vals == vals[0]))
        stored = vals[:1] if const else vals
        if b.kind == TYPE_SRT:
            raw = np.asarray(stored, "<f4").tobytes()
        elif b.kind == TYPE_CLR:
            raw = np.asarray(stored, np.uint8).tobytes()
        elif b.kind == TYPE_PAT:
            raw = np.asarray(stored, "<u2").tobytes()
        else:
            raw = np.asarray(stored, np.uint8).tobytes()
        while (data_start + len(body)) % 4:
            body += b"\x00"
        tracks += TRACK.pack(target, slot, 1 if const else 0, data_start + len(body), len(stored), 0)
        body += raw
    while (data_start + len(body)) % 8:
        body += b"\x00"
    keys_off = data_start + len(body)
    body += b"".join(struct.pack("<Q", k) for k in b.keys)
    head = HEADER.pack(b"CMAN", 1, b.frames, b.kind, model_key, 1 if b.loop else 0, fps, len(b.tracks),
                       len(b.keys), HEADER.size, keys_off if b.keys else 0)
    return head + bytes(tracks) + bytes(body)


def unpack(blob: bytes) -> dict:
    """Lector de referencia (pruebas)."""
    if blob[:4] != b"CMAN":
        raise ValueError("no es CMAN")
    _m, ver, frames, kind, key, flags, fps, nt, nk, toff, koff = HEADER.unpack_from(blob, 0)
    width = {TYPE_SRT: (8, "<f4"), TYPE_CLR: (4, np.uint8), TYPE_PAT: (1, "<u2"), TYPE_VIS: (1, np.uint8)}[kind]
    tracks = []
    for i in range(nt):
        target, slot, tflags, voff, count, _ = TRACK.unpack_from(blob, toff + TRACK.size * i)
        vals = np.frombuffer(blob, width[1], count * width[0], voff).reshape(count, width[0]) \
            if width[0] > 1 else np.frombuffer(blob, width[1], count, voff)
        tracks.append(dict(target=target, slot=slot, const=bool(tflags & 1), values=vals))
    keys = list(struct.unpack_from(f"<{nk}Q", blob, koff)) if nk else []
    return dict(version=ver, frames=frames, kind=kind, model_key=key, loop=bool(flags & 1), fps=fps, tracks=tracks,
                keys=keys)
