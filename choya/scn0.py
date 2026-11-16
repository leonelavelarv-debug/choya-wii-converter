"""Escenas de NW4R G3D (SCN0): conjuntos de luces (LightSet), luces ambiente, luces y cámaras, horneadas por cuadro para
el runtime (entrada KIND_SCENE, bloque "CSCN").

Sigue a ogws (CC0): g3d_resanmscn.h/.cpp (cabecera y diccionarios "LightSet(NW4R)", "AmbLights(NW4R)", "Lights(NW4R)",
"Cameras(NW4R)"), g3d_reslightset.h/.cpp (cada conjunto nombra su luz ambiente y hasta 8 luces), g3d_resanmlight.cpp y
g3d_resanmamblight.cpp (GetAnmResult), g3d_resanmcamera.cpp, g3d_camera.cpp (UpdateCameraMtx para los tipos ROTATE y
AIM), g3d_light.cpp (LightObj) y revolution/GX/GXLight.c (GXInitLightSpot y GXInitLightDistAttn: los coeficientes de
atenuación que usa el hardware).

Lo que NO está descompilado en ogws es AnmScnRes::GetLight (de LightAnmResult a LightObj). Aquí se reconstruye así, y el
reporte lo dice: punto = posición + atenuación por distancia; direccional = dirección (pos -> aim) sin atenuación;
foco = posición + dirección + GXInitLightSpot(cutoff, spotFunc) + GXInitLightDistAttn; especular = una luz aparte en
specLightObjIdx con dirección (pos -> aim), color especular y LightObj::InitLightShininess. La niebla (Fogs) se cuenta
pero no se aplica.

Bloque "CSCN" versión 1, little-endian (los índices de luz y de cámara son los refNumber de NW4R: lo que usan
light_set_idx del material y ref_camera / ref_light de sus matrices de textura):
  0 char[4] "CSCN"  4 u32 versión  8 u32 cuadros  12 u32 banderas (bit0 bucle)  16 float cuadros por segundo
  20 u16 conjuntos  22 u16 ambientes  24 u16 luces  26 u16 cámaras  28 u16 nieblas (no se aplican)  30 libre
  32 u32 offset de conjuntos  36 u32 de ambientes  40 u32 de luces  44 u32 de cámaras  48..63 libre
  conjunto (32 bytes): u16 ref, i16 ambiente (índice en la tabla, -1 = blanco), u8 luces, 3 libres,
     i16 luz[8] (índice en la tabla de luces, -1 = ninguna)
  ambiente (12 bytes): u16 ref, u8 banderas (1 color, 2 alfa), u8 constante, u32 offset, u32 valores -> u8 RGBA
  luz (16 bytes): u16 ref, i16 especular (índice en la tabla), u8 tipo (0 punto, 1 direccional, 2 foco, 3 especular),
     u8 banderas (1 color, 2 alfa), u8 constante, u8 libre, u32 offset, u32 valores ->
     por cuadro 64 bytes: float pos[3], float dir[3] (mundo), u8 color[4], float a[3] (ángulo), float k[3]
     (distancia; en la especular, la atenuación de InitLightShininess), u32 encendida, float brillo, 4 libres
  cámara (16 bytes): u16 ref, u8 proyección (0 perspectiva, 1 ortográfica), u8 constante, u32 offset, u32 valores,
     4 libres -> por cuadro 64 bytes: float vista[12] (3x4 por filas, mundo -> cámara), float fovy (grados) o alto,
     float aspecto, float cerca, float lejos
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

import numpy as np

from .anim_mat import KeyTrack, MatAnimError, _cstr, _dic, _key_track, _Reader, clr_at

HEADER = struct.Struct("<4sIIIfHHHHHHIIII16x")
LIGHTSET = struct.Struct("<HhB3x8h8x")
AMB = struct.Struct("<HBBII")
LIGHT = struct.Struct("<HhBBBxII")
CAM = struct.Struct("<HBBII4x")
LIGHT_FRAME = struct.Struct("<3f3f4B3f3fIf4x")
CAM_FRAME = struct.Struct("<12f4f")
assert HEADER.size == 64 and LIGHTSET.size == 32 and AMB.size == 12 and LIGHT.size == 16 and CAM.size == 16
assert LIGHT_FRAME.size == 64 and CAM_FRAME.size == 64

POINT, DIRECTIONAL, SPOT, SPECULAR = 0, 1, 2, 3
GX_SP_OFF, GX_DA_OFF = 0, 0


class Scn0Error(MatAnimError):
    pass


# ------------------------------------------------------------------------- coeficientes de GX (GXLight.c de ogws)

def spot_coefs(angle: float, fn: int) -> tuple[float, float, float]:
    """GXInitLightSpot: (a0, a1, a2) de la atenuación por ángulo."""
    if angle <= 0.0 or angle > 90.0:
        fn = GX_SP_OFF
    c = math.cos(math.pi * angle / 180.0)
    if fn == 1:      # FLAT
        return -1000.0 * c, 1000.0, 0.0
    if fn == 2:      # COS
        b = 1.0 / (1.0 - c)
        return -c * b, b, 0.0
    if fn == 3:      # COS2
        cc = 1.0 / (1.0 - c)
        return 0.0, -c * cc, cc
    if fn in (4, 5, 6):
        d = 1.0 / ((1.0 - c) * (1.0 - c))
        if fn == 4:  # SHARP
            return (c * (c - 2.0)) * d, 2.0 * d, -d
        if fn == 5:  # RING1
            cc = -4.0 * d
            return cc * c, 4.0 * (1.0 + c) * d, cc
        return 1.0 - 2.0 * c * c * d, 4.0 * c * d, -2.0 * d   # RING2
    return 1.0, 0.0, 0.0


def dist_coefs(distance: float, brightness: float, fn: int) -> tuple[float, float, float]:
    """GXInitLightDistAttn: (k0, k1, k2) de la atenuación por distancia."""
    if distance < 0.0 or brightness <= 0.0 or brightness >= 1.0:
        fn = GX_DA_OFF
    if fn == 1:      # GENTLE
        return 1.0, (1.0 - brightness) / (brightness * distance), 0.0
    if fn == 2:      # MEDIUM
        return (1.0, 0.5 * (1.0 - brightness) / (brightness * distance),
                0.5 * (1.0 - brightness) / (distance * (brightness * distance)))
    if fn == 3:      # STEEP
        return 1.0, 0.0, (1.0 - brightness) / (distance * (brightness * distance))
    return 1.0, 0.0, 0.0


def shininess_coefs(shininess: float) -> tuple[tuple, tuple]:
    """LightObj::InitLightShininess: GXInitLightAttn(0, 0, 1, s/2, 0, 1 - s/2)."""
    return (0.0, 0.0, 1.0), (shininess / 2.0, 0.0, 1.0 - shininess / 2.0)


# ------------------------------------------------------------------------- cámara (g3d_camera.cpp)

def _sincos_deg(a: float):
    r = math.radians(a)
    return math.sin(r), math.cos(r)


def camera_rotate(pos, rot) -> np.ndarray:
    sx, cx = _sincos_deg(rot[0])
    sy, cy = _sincos_deg(rot[1])
    sz, cz = _sincos_deg(rot[2])
    right = (sx * sy * sz + cy * cz, cx * sz, sx * cy * sz - sy * cz)
    up = (sx * sy * cz - cy * sz, cx * cz, sx * cy * cz + sy * sz)
    back = (cx * sy, -sx, cx * cy)
    return _rows(pos, right, up, back)


def camera_aim(pos, target, twist: float) -> np.ndarray:
    back = np.subtract(pos, target).astype(float)
    if back[0] == 0.0 and back[2] == 0.0:
        m = np.zeros((3, 4))
        m[0] = (1, 0, 0, -pos[0])
        if back[1] <= 0.0:
            m[1] = (0, 0, 1, -pos[2])
            m[2] = (0, -1, 0, pos[1])
        else:
            m[1] = (0, 0, -1, pos[2])
            m[2] = (0, 1, 0, -pos[1])
        return m
    r = np.array([back[2], 0.0, -back[0]])
    back /= np.linalg.norm(back)
    r /= np.linalg.norm(r)
    u = np.cross(back, r)
    st, ct = _sincos_deg(twist)
    right = (st * u[0] + ct * r[0], st * u[1], st * u[2] + ct * r[2])
    up = (ct * u[0] - st * r[0], ct * u[1], ct * u[2] - st * r[2])
    return _rows(pos, right, up, back)


def _rows(pos, right, up, back) -> np.ndarray:
    m = np.zeros((3, 4))
    for i, v in enumerate((right, up, back)):
        m[i, :3] = v
        m[i, 3] = -float(np.dot(pos, v))
    return m


# ------------------------------------------------------------------------- lectura

@dataclass
class LightSet:
    name: str
    ref: int
    amb: str | None
    lights: list


@dataclass
class AmbLight:
    name: str
    ref: int
    flags: int
    colors: list          # u32 RGBA por cuadro (o uno)


@dataclass
class Light:
    name: str
    ref: int
    spec_ref: int
    flags: int
    enable: list | None   # bool por cuadro (None = constante según la bandera)
    tracks: dict          # pos, aim, refDistance, refBrightness, cutoff, shininess -> KeyTrack
    color: list
    spec_color: list
    dist_fn: int
    spot_fn: int


@dataclass
class Camera:
    name: str
    ref: int
    flags: int
    proj: int
    tracks: dict


@dataclass
class Scn0:
    name: str
    frames: int
    loop: bool
    lightsets: list = field(default_factory=list)
    ambs: list = field(default_factory=list)
    lights: list = field(default_factory=list)
    cameras: list = field(default_factory=list)
    fogs: int = 0


def _colors(r: _Reader, o: int, constant: bool, num_frame: int) -> list:
    if constant:
        return [r.u32(o)]
    fd = o + r.s32(o)
    return [r.u32(fd + 4 * k) for k in range(num_frame + 1)]


def read_scn0(d: bytes, off: int, name: str = "") -> Scn0:
    if d[off:off + 4] != b"SCN0":
        raise Scn0Error("no es SCN0")
    size, = struct.unpack_from(">I", d, off + 4)
    r = _Reader(d, off, min(len(d), off + size) if size >= 0x44 else len(d))
    rev = r.u32(off + 8)
    if rev != 4:
        raise Scn0Error(f"SCN0 versión {rev} no soportada")
    num_frame, _nspec, policy = r.unpack(">HHI", off + 0x30)
    out = Scn0(name, max(1, num_frame), policy == 1)
    top = {n: o for n, o in _dic(r, off + r.s32(off + 0x10), 16)}
    for lname, ld in _dic(r, top["LightSet(NW4R)"]) if "LightSet(NW4R)" in top else []:
        ref = r.u32(ld + 0x10)
        amb_rel = r.s32(ld + 0x14)
        nl = min(8, r.unpack(">B", ld + 0x1A)[0])
        names = []
        for i in range(nl):
            rel = r.s32(ld + 0x1C + 4 * i)
            names.append(_cstr(d, ld + 0x1C + rel) if rel else None)   # relativo al arreglo de nombres
        out.lightsets.append(LightSet(lname, ref, _cstr(d, ld + amb_rel) if amb_rel else None, names))
    for aname, ad in _dic(r, top["AmbLights(NW4R)"]) if "AmbLights(NW4R)" in top else []:
        flags = r.u32(ad + 0x14)
        out.ambs.append(AmbLight(aname, r.u32(ad + 0x10), flags, _colors(r, ad + 0x18, bool(flags & (1 << 31)),
                                                                         num_frame)))
    for lname, ld in _dic(r, top["Lights(NW4R)"]) if "Lights(NW4R)" in top else []:
        ref, spec, _ud, flags = r.unpack(">IIiI", ld + 0x10)
        enable = None
        if not flags & (1 << 23):
            bd = ld + 0x20 + r.s32(ld + 0x20)
            words = [r.u32(bd + 4 * k) for k in range(num_frame // 32 + 1)]
            enable = [bool(words[f // 32] & ((1 << 31) >> (f % 32))) for f in range(num_frame + 1)]
        tr = {}
        ltype, has_spec = flags & 3, bool(flags & 8)
        used = {"px", "py", "pz"}            # como GetAnmResult: solo se leen los campos de este tipo de luz
        if ltype != POINT or has_spec:
            used |= {"ax", "ay", "az"}
        if ltype in (POINT, SPOT):
            used |= {"dist", "bright"}
        if ltype == SPOT:
            used.add("cutoff")
        if has_spec:
            used.add("shin")
        for key, o, bit in (("px", 0x24, 19), ("py", 0x28, 20), ("pz", 0x2C, 21), ("ax", 0x34, 24), ("ay", 0x38, 25),
                            ("az", 0x3C, 26), ("dist", 0x44, 28), ("bright", 0x48, 29), ("cutoff", 0x50, 27),
                            ("shin", 0x58, 31)):
            tr[key] = _key_track(r, ld + o, bool(flags & (1 << bit))) if key in used else KeyTrack(const=0.0)
        dist_fn, = r.unpack(">I", ld + 0x40)
        spot_fn, = r.unpack(">I", ld + 0x4C)
        color = _colors(r, ld + 0x30, bool(flags & (1 << 22)), num_frame)
        spec_color = _colors(r, ld + 0x54, bool(flags & (1 << 30)), num_frame) if has_spec else [0xFFFFFFFF]
        out.lights.append(Light(lname, ref, spec, flags, enable, tr, color, spec_color, dist_fn, spot_fn))
    out.fogs = len(_dic(r, top["Fogs(NW4R)"])) if "Fogs(NW4R)" in top else 0
    for cname, cd in _dic(r, top["Cameras(NW4R)"]) if "Cameras(NW4R)" in top else []:
        ref, proj, flags = r.unpack(">III", cd + 0x10)
        tr = {}
        used = {"px", "py", "pz", "aspect", "near", "far"}     # como ResAnmCamera::GetAnmResult
        used |= {"ax", "ay", "az", "twist"} if flags & 1 else {"rx", "ry", "rz"}
        used.add("height" if proj == 1 else "fovy")
        for i, key in enumerate(("px", "py", "pz", "aspect", "near", "far", "rx", "ry", "rz", "ax", "ay", "az",
                                 "twist", "fovy", "height")):
            const_bit = {"px": 17, "py": 18, "pz": 19, "aspect": 20, "near": 21, "far": 22, "fovy": 23,
                         "height": 24, "ax": 25, "ay": 26, "az": 27, "twist": 28, "rx": 29, "ry": 30,
                         "rz": 31}[key]
            tr[key] = _key_track(r, cd + 0x20 + 4 * i, bool(flags & (1 << const_bit))) if key in used \
                else KeyTrack(const=0.0)
        out.cameras.append(Camera(cname, ref, flags, proj, tr))
    return out


def find(brres: bytes, sections: dict) -> list[tuple[str, int]]:
    return [(n, off) for n, off in sections.get("AnmScn(NW4R)", []) if brres[off:off + 4] == b"SCN0"]


# ------------------------------------------------------------------------- horneado

def _clip(frame: float, n: int) -> float:
    return 0.0 if frame <= 0 else min(frame, float(n))


def _rgba(c: int) -> tuple:
    return (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255


def _norm(v) -> tuple:
    v = np.asarray(v, float)
    n = float(np.linalg.norm(v))
    return tuple(v / n) if n > 0 else (0.0, -1.0, 0.0)


def light_frames(lt: Light, n: int, num_frame: int) -> tuple[list, list | None]:
    """(cuadros de la luz difusa, cuadros de su especular o None) como LIGHT_FRAME empaquetados."""
    ltype = lt.flags & 3
    diff, spec = [], []
    has_spec = bool(lt.flags & 8)
    for f in range(n):
        ff = float(f)
        cf = _clip(ff, num_frame)
        on = bool(lt.flags & 4)
        if lt.enable is not None:
            on = on or lt.enable[min(int(math.floor(cf)), len(lt.enable) - 1)]
        pos = (lt.tracks["px"].at(ff), lt.tracks["py"].at(ff), lt.tracks["pz"].at(ff))
        aim = (lt.tracks["ax"].at(ff), lt.tracks["ay"].at(ff), lt.tracks["az"].at(ff))
        color = _rgba(clr_at(lt.color, cf, num_frame))
        direction = _norm(np.subtract(aim, pos)) if (ltype != POINT or has_spec) else (0.0, -1.0, 0.0)
        a, k = (1.0, 0.0, 0.0), (1.0, 0.0, 0.0)
        if ltype in (POINT, SPOT):
            k = dist_coefs(lt.tracks["dist"].at(ff), lt.tracks["bright"].at(ff), lt.dist_fn)
        if ltype == SPOT:
            a = spot_coefs(lt.tracks["cutoff"].at(ff), lt.spot_fn)
        diff.append(LIGHT_FRAME.pack(*pos, *direction, *color, *a, *k, int(on), 0.0))
        if has_spec:
            shin = lt.tracks["shin"].at(ff)
            sa, sk = shininess_coefs(shin)
            spec.append(LIGHT_FRAME.pack(*pos, *direction, *_rgba(clr_at(lt.spec_color, cf, num_frame)), *sa, *sk,
                                         int(on), shin))
    return diff, (spec if has_spec else None)


def camera_frames(c: Camera, n: int) -> list:
    out = []
    for f in range(n):
        ff = float(f)
        t = {k: v.at(ff) for k, v in c.tracks.items()}
        pos = (t["px"], t["py"], t["pz"])
        if c.flags & 1:   # AIM
            view = camera_aim(pos, (t["ax"], t["ay"], t["az"]), t["twist"])
        else:
            view = camera_rotate(pos, (t["rx"], t["ry"], t["rz"]))
        out.append(CAM_FRAME.pack(*view.reshape(-1), t["height"] if c.proj == 1 else t["fovy"], t["aspect"],
                                  t["near"], t["far"]))
    return out


@dataclass
class BakedScene:
    frames: int
    loop: bool
    blob: bytes
    notes: dict = field(default_factory=dict)


def bake(s: Scn0, fps: float = 60.0) -> BakedScene:
    n = s.frames
    notes: dict = {}
    if s.fogs:
        notes["nieblas (no se aplican)"] = s.fogs
    notes["AnmScnRes::GetLight reconstruido (no está descompilado en ogws)"] = len(s.lights)
    body = bytearray()
    data_base = 64

    def add(raw: bytes) -> int:
        while (len(body)) % 4:
            body.extend(b"\0")
        o = len(body)
        body.extend(raw)
        return o

    # luces (con sus especulares como entradas aparte)
    light_recs = []        # (ref, spec_index, tipo, banderas, cuadros)
    by_name: dict[str, int] = {}
    for lt in s.lights:
        diff, spec = light_frames(lt, n, n)
        spec_idx = -1
        if spec is not None:
            spec_idx = len(light_recs) + 1
        by_name[lt.name] = len(light_recs)
        light_recs.append((lt.ref, spec_idx, lt.flags & 3, (lt.flags >> 4) & 3, diff))
        if spec is not None:
            light_recs.append((lt.spec_ref, -1, SPECULAR, (lt.flags >> 4) & 3, spec))
    amb_index = {a.name: i for i, a in enumerate(s.ambs)}
    sets = []
    missing = 0
    for ls in s.lightsets:
        idx = []
        for nm in ls.lights:
            if nm is None:
                continue
            if nm in by_name:
                idx.append(by_name[nm])
            else:
                missing += 1
        sets.append((ls.ref, amb_index.get(ls.amb, -1) if ls.amb else -1, idx[:8]))
    if missing:
        notes["luces nombradas en un conjunto que no están en la escena"] = missing
    # tablas de cabecera de 16/32 bytes, luego datos
    head_sets = bytearray()
    for ref, amb, idx in sets:
        head_sets += LIGHTSET.pack(ref, amb, len(idx), *(idx + [-1] * (8 - len(idx))))
    amb_heads, light_heads, cam_heads = [], [], []
    for a in s.ambs:
        vals = [_rgba(clr_at(a.colors, _clip(float(f), n), n)) for f in range(n)]
        const = all(v == vals[0] for v in vals)
        raw = b"".join(bytes(v) for v in (vals[:1] if const else vals))
        amb_heads.append((a.ref, a.flags & 3, int(const), raw, 1 if const else n))
    for ref, spec_idx, ltype, flags, frames in light_recs:
        const = all(fr == frames[0] for fr in frames)
        light_heads.append((ref, spec_idx, ltype, flags, int(const), b"".join(frames[:1] if const else frames),
                            1 if const else n))
    for c in s.cameras:
        frames = camera_frames(c, n)
        const = all(fr == frames[0] for fr in frames)
        cam_heads.append((c.ref, 1 if c.proj == 1 else 0, int(const), b"".join(frames[:1] if const else frames),
                          1 if const else n))
    off_sets = data_base
    off_amb = off_sets + len(head_sets)
    off_light = off_amb + AMB.size * len(amb_heads)
    off_cam = off_light + LIGHT.size * len(light_heads)
    data_start = off_cam + CAM.size * len(cam_heads)
    tables = bytearray(head_sets)
    for ref, fl, const, raw, count in amb_heads:
        tables += AMB.pack(ref, fl, const, data_start + add(raw), count)
    for ref, spec_idx, ltype, fl, const, raw, count in light_heads:
        tables += LIGHT.pack(ref, spec_idx, ltype, fl, const, data_start + add(raw), count)
    for ref, proj, const, raw, count in cam_heads:
        tables += CAM.pack(ref, proj, const, data_start + add(raw), count)
    head = HEADER.pack(b"CSCN", 1, n, 1 if s.loop else 0, fps, len(sets), len(amb_heads), len(light_heads),
                       len(cam_heads), s.fogs, 0, off_sets, off_amb, off_light, off_cam)
    return BakedScene(n, s.loop, head + bytes(tables) + bytes(body), notes)


def unpack(blob: bytes) -> dict:
    """Lector de referencia (pruebas)."""
    (_m, ver, frames, flags, fps, nsets, namb, nlight, ncam, nfog, _r, osets, oamb, olight,
     ocam) = HEADER.unpack_from(blob, 0)
    sets = [LIGHTSET.unpack_from(blob, osets + 32 * i) for i in range(nsets)]
    ambs = []
    for i in range(namb):
        ref, fl, const, off, count = AMB.unpack_from(blob, oamb + 12 * i)
        ambs.append(dict(ref=ref, flags=fl, const=bool(const), colors=[tuple(blob[off + 4 * k:off + 4 * k + 4])
                                                                        for k in range(count)]))
    lights = []
    for i in range(nlight):
        ref, spec, ltype, fl, const, off, count = LIGHT.unpack_from(blob, olight + 16 * i)
        fr = [LIGHT_FRAME.unpack_from(blob, off + 64 * k) for k in range(count)]
        lights.append(dict(ref=ref, spec=spec, type=ltype, flags=fl, const=bool(const), frames=fr))
    cams = []
    for i in range(ncam):
        ref, proj, const, off, count = CAM.unpack_from(blob, ocam + 16 * i)
        cams.append(dict(ref=ref, proj=proj, const=bool(const),
                         frames=[CAM_FRAME.unpack_from(blob, off + 64 * k) for k in range(count)]))
    return dict(version=ver, frames=frames, loop=bool(flags & 1), fps=fps, sets=sets, ambs=ambs, lights=lights,
                cameras=cams, fogs=nfog)
