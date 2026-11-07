"""Lector de modelos MDL0 (NW4R G3D) dentro de BRRES.

Produce mallas listas para GPU: posiciones ya en espacio del modelo (pose de reposo),
normales, color de vértice, UV0 y submallas por material con su textura principal.

El formato se lee de forma defensiva: cada estructura se valida con su puntero de regreso
al MDL0 (campo +0x04) y los desplazamientos dudosos se prueban con varias bases. Lo que no
cuadra se anota en `warnings` en vez de detener la conversión.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

import numpy as np

SECTIONS_V8 = ["defs", "bones", "verts", "norms", "colors", "uvs", "mats", "shaders", "objs", "texs", "pals"]
SECTIONS_V10 = ["defs", "bones", "verts", "norms", "colors", "uvs", "furvec", "furpos", "mats", "shaders",
                "objs", "texs", "pals"]

ATTR_NONE, ATTR_DIRECT, ATTR_IDX8, ATTR_IDX16 = 0, 1, 2, 3


class MDL0Error(ValueError):
    pass


@dataclass
class Bone:
    name: str
    index: int
    node_id: int
    world: np.ndarray          # 4x4


@dataclass
class Layer:
    texture: str
    wrap_s: int = 0            # 0 clamp, 1 repeat, 2 mirror
    wrap_t: int = 0
    texmap: int = 0            # unidad GX (GX_TEXMAP0..7) que usan las etapas TEV


@dataclass
class Material:
    name: str
    index: int
    offset: int
    cull: int = 2              # 0 ninguna, 1 frente, 2 atrás, 3 todo
    xlu: bool = False
    layers: list[Layer] = field(default_factory=list)
    tev: object = None         # tev.TevRaw


@dataclass
class SubMesh:
    material: int              # índice en Model.materials
    first: int                 # primer índice
    count: int                 # número de índices
    xlu: bool


@dataclass
class Model:
    name: str
    version: int
    positions: np.ndarray      # (n, 3) float32
    normals: np.ndarray        # (n, 3) float32 (ceros si no hay)
    colors: np.ndarray         # (n, 4) uint8 (blanco si no hay)
    uvs: np.ndarray            # (n, 2) float32
    indices: np.ndarray        # (m,) uint32
    submeshes: list[SubMesh]
    materials: list[Material]
    has_normals: bool
    has_colors: bool
    stats: dict = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


# ------------------------------------------------------------------------- utilidades

def _s32(d: bytes, o: int) -> int:
    return struct.unpack_from(">i", d, o)[0]


def _u32(d: bytes, o: int) -> int:
    return struct.unpack_from(">I", d, o)[0]


def _u16(d: bytes, o: int) -> int:
    return struct.unpack_from(">H", d, o)[0]


def _cstr(d: bytes, o: int) -> str:
    if o <= 0 or o >= len(d):
        return ""
    end = d.find(b"\x00", o)
    return d[o:end if end >= 0 else len(d)].decode("shift_jis", errors="replace")


def _group(d: bytes, off: int) -> list[tuple[str, int]]:
    """Grupo de índice NW4R: [(nombre, offset_absoluto)] sin la entrada raíz."""
    if off <= 0 or off + 8 > len(d):
        return []
    _size, count = struct.unpack_from(">II", d, off)
    if count > 100000:
        raise MDL0Error("grupo de índice dañado")
    out = []
    for i in range(1, count + 1):
        e = off + 8 + i * 16
        name_ptr, data_ptr = struct.unpack_from(">ii", d, e + 8)
        out.append((_cstr(d, off + name_ptr), off + data_ptr))
    return out


def _back_ok(d: bytes, abs_off: int, mdl0: int, field_off: int = 4) -> bool:
    """El campo de regreso (offset negativo al MDL0) apunta al inicio del modelo."""
    try:
        return abs_off + _s32(d, abs_off + field_off) == mdl0
    except struct.error:
        return False


def _euler_matrix(s, r, t) -> np.ndarray:
    rx, ry, rz = (math.radians(a) for a in r)
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    rot_x = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    rot_y = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rot_z = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    m = np.eye(4)
    m[:3, :3] = rot_z @ rot_y @ rot_x @ np.diag(s)
    m[:3, 3] = t
    return m


# ------------------------------------------------------------------------- arreglos de vértices

_FMT = {0: ("B", 1), 1: ("b", 1), 2: (">H", 2), 3: (">h", 2), 4: (">f", 4)}


def _read_array(d: bytes, blk: int, comps: int, fmt: int, divisor: int, stride: int, count: int) -> np.ndarray:
    data_off = blk + _s32(d, blk + 0x08)
    if fmt not in _FMT:
        raise MDL0Error(f"formato de componente desconocido {fmt}")
    dt, size = _FMT[fmt]
    stride = stride or comps * size
    out = np.zeros((count, comps), np.float32)
    raw = np.frombuffer(d, np.uint8, count=min(len(d) - data_off, stride * count), offset=data_off)
    if raw.size < stride * count:
        raise MDL0Error("arreglo de vértices truncado")
    rows = raw.reshape(count, stride)[:, :comps * size]
    vals = np.frombuffer(rows.tobytes(), np.dtype(dt)).reshape(count, comps).astype(np.float32)
    if fmt != 4 and divisor:
        vals = vals / float(1 << divisor)
    out[:] = vals
    return out


def _read_positions(d, blk):
    comp, fmt = _u32(d, blk + 0x14), _u32(d, blk + 0x18)
    div, stride, count = d[blk + 0x1C], d[blk + 0x1D], _u16(d, blk + 0x1E)
    comps = 3 if comp == 1 else 2
    arr = _read_array(d, blk, comps, fmt, div, stride, count)
    if comps == 2:
        arr = np.concatenate([arr, np.zeros((count, 1), np.float32)], 1)
    return arr


def _read_normals(d, blk):
    comp, fmt = _u32(d, blk + 0x14), _u32(d, blk + 0x18)
    div, stride, count = d[blk + 0x1C], d[blk + 0x1D], _u16(d, blk + 0x1E)
    comps = 3 if comp == 0 else 9
    arr = _read_array(d, blk, comps, fmt, div, stride, count)
    return arr[:, :3]


def _read_uvs(d, blk):
    comp, fmt = _u32(d, blk + 0x14), _u32(d, blk + 0x18)
    div, stride, count = d[blk + 0x1C], d[blk + 0x1D], _u16(d, blk + 0x1E)
    comps = 2 if comp == 1 else 1
    arr = _read_array(d, blk, comps, fmt, div, stride, count)
    if comps == 1:
        arr = np.concatenate([arr, np.zeros((count, 1), np.float32)], 1)
    return arr


def _read_colors(d, blk):
    _comp, fmt = _u32(d, blk + 0x14), _u32(d, blk + 0x18)
    stride, count = d[blk + 0x1C], _u16(d, blk + 0x1E)
    base = blk + _s32(d, blk + 0x08)
    sizes = {0: 2, 1: 3, 2: 4, 3: 2, 4: 3, 5: 4}
    if fmt not in sizes:
        raise MDL0Error(f"formato de color desconocido {fmt}")
    stride = stride or sizes[fmt]
    out = np.full((count, 4), 255, np.uint8)
    for i in range(count):
        p = base + i * stride
        if fmt == 0:
            v = _u16(d, p)
            out[i, :3] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
        elif fmt in (1, 2):
            out[i, :3] = tuple(d[p:p + 3])
        elif fmt == 3:
            v = _u16(d, p)
            out[i] = ((v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17)
        elif fmt == 4:
            v = (d[p] << 16) | (d[p + 1] << 8) | d[p + 2]
            out[i] = tuple(((v >> s) & 63) * 255 // 63 for s in (18, 12, 6, 0))
        else:
            out[i] = tuple(d[p:p + 4])
    return out


# ------------------------------------------------------------------------- objetos

def _vcd_fields(lo: int, hi: int) -> list[tuple[str, int]]:
    """Orden de atributos de un vértice GX y su tipo (directo / índice 8 / índice 16)."""
    fields = []
    if lo & 1:
        fields.append(("pnmtx", ATTR_DIRECT))
    for i in range(8):
        if lo & (1 << (1 + i)):
            fields.append((f"tmtx{i}", ATTR_DIRECT))
    names = [("pos", 9), ("nrm", 11), ("clr0", 13), ("clr1", 15)]
    for n, sh in names:
        t = (lo >> sh) & 3
        if t:
            fields.append((n, t))
    for i in range(8):
        t = (hi >> (2 * i)) & 3
        if t:
            fields.append((f"tex{i}", t))
    return fields


def _parse_display_list(d: bytes, start: int, size: int, fields, max_pos: int):
    """Decodifica primitivas GX. Devuelve (triángulos [(v0,v1,v2)], cargas de matriz {slot: id})."""
    end = min(len(d), start + size)
    p = start
    tris = []
    loads = {}
    vsize = sum(1 if t in (ATTR_DIRECT, ATTR_IDX8) else 2 for _, t in fields)
    snapshots = []
    while p < end:
        op = d[p]
        if op == 0x00:
            p += 1
            continue
        if op in (0x20, 0x28, 0x30, 0x38):           # LOAD_INDX_A..D
            idx, addr = _u16(d, p + 1), _u16(d, p + 3)
            if op == 0x20:
                loads[(addr & 0x0FFF) // 12] = idx
            p += 5
            continue
        if op == 0x08:                                # LOAD_CP
            p += 6
            continue
        if op == 0x10:                                # LOAD_XF
            n = (_u16(d, p + 1) + 1) * 4
            p += 5 + n
            continue
        if op == 0x61:                                # LOAD_BP
            p += 5
            continue
        prim = op & 0xF8
        if prim not in (0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8):
            raise MDL0Error(f"opcode GX desconocido 0x{op:02X}")
        n = _u16(d, p + 1)
        p += 3
        verts = []
        for _ in range(n):
            v = {}
            for name, t in fields:
                if t in (ATTR_DIRECT, ATTR_IDX8):
                    v[name] = d[p]
                    p += 1
                else:
                    v[name] = _u16(d, p)
                    p += 2
            verts.append(v)
        if p > end:
            raise MDL0Error("lista de dibujo truncada")
        mat = dict(loads)
        snapshots.append(mat)
        tagged = [(v, len(snapshots) - 1) for v in verts]
        if prim == 0x90:
            tris += [tuple(tagged[i:i + 3]) for i in range(0, n - 2, 3)]
        elif prim == 0x98:
            for i in range(n - 2):
                a, b, c = tagged[i], tagged[i + 1], tagged[i + 2]
                tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
        elif prim == 0xA0:
            tris += [(tagged[0], tagged[i], tagged[i + 1]) for i in range(1, n - 1)]
        elif prim == 0x80:
            for i in range(0, n - 3, 4):
                q = tagged[i:i + 4]
                tris += [(q[0], q[1], q[2]), (q[0], q[2], q[3])]
        # líneas y puntos se ignoran en el visor
    return tris, snapshots, vsize


def _check_dl(d: bytes, start: int, size: int) -> bool:
    """¿Parece una lista de dibujo válida? (primer opcode no nulo reconocible)."""
    if start <= 0 or start >= len(d) or size <= 0:
        return False
    for i in range(start, min(len(d), start + min(size, 64))):
        op = d[i]
        if op == 0:
            continue
        return op in (0x20, 0x28, 0x30, 0x38, 0x08, 0x10, 0x61) or (op & 0xF8) in (0x80, 0x90, 0x98, 0xA0)
    return False


# ------------------------------------------------------------------------- lector principal

def read_mdl0(d: bytes, mdl0: int, name: str) -> Model:
    if d[mdl0:mdl0 + 4] != b"MDL0":
        raise MDL0Error("no es MDL0")
    version = _u32(d, mdl0 + 8)
    warnings: list[str] = []

    # Detectar cuántas secciones tiene esta versión: el nombre del modelo va justo después.
    layout = None
    for names in (SECTIONS_V8, SECTIONS_V10, SECTIONS_V10 + ["extra"]):
        n_off = mdl0 + 0x10 + 4 * len(names)
        if _cstr(d, mdl0 + _s32(d, n_off)) == name:
            layout = names
            break
    if layout is None:
        layout = SECTIONS_V8 if version <= 9 else SECTIONS_V10
        warnings.append(f"no se pudo confirmar la cantidad de secciones (versión {version})")
    sec = {}
    for i, sname in enumerate(layout):
        off = _s32(d, mdl0 + 0x10 + 4 * i)
        sec[sname] = _group(d, mdl0 + off) if off else []

    # Huesos
    bones: list[Bone] = []
    by_node: dict[int, Bone] = {}
    mismatch = 0
    composed: dict[int, np.ndarray] = {}
    for bname, b in sec["bones"]:
        if not _back_ok(d, b, mdl0):
            warnings.append(f"hueso {bname}: estructura inesperada")
            continue
        idx, node = _s32(d, b + 0x0C), _s32(d, b + 0x10)
        s = struct.unpack_from(">3f", d, b + 0x20)
        r = struct.unpack_from(">3f", d, b + 0x2C)
        t = struct.unpack_from(">3f", d, b + 0x38)
        parent_rel = _s32(d, b + 0x5C)
        local = _euler_matrix(s, r, t)
        stored = np.eye(4)
        stored[:3, :] = np.array(struct.unpack_from(">12f", d, b + 0x70)).reshape(3, 4)
        if parent_rel and (b + parent_rel) not in composed:
            comp = None                      # el padre aún no se leyó: no se puede comparar
        else:
            parent = composed.get(b + parent_rel) if parent_rel else None
            comp = (parent @ local) if parent is not None else local
        world = stored
        if comp is not None and (not np.all(np.isfinite(stored))
                                 or np.abs(stored - comp).max() > 1e-2 * max(1.0, np.abs(comp).max())):
            mismatch += 1
            world = comp
        composed[b] = world
        bone = Bone(bname, idx, node, world)
        bones.append(bone)
        by_node[node] = bone
    if mismatch:
        warnings.append(f"{mismatch} huesos con matriz guardada distinta a la calculada; se usó la calculada")

    def arrays(key, reader):
        out = {}
        for aname, blk in sec.get(key, []):
            if not _back_ok(d, blk, mdl0):
                warnings.append(f"arreglo {aname}: estructura inesperada")
                continue
            try:
                out[_s32(d, blk + 0x10)] = reader(d, blk)
            except (MDL0Error, struct.error, ValueError) as e:
                warnings.append(f"arreglo {aname}: {e}")
        return out

    verts = arrays("verts", _read_positions)
    norms = arrays("norms", _read_normals)
    colors = arrays("colors", _read_colors)
    uvs = arrays("uvs", _read_uvs)

    # Materiales y sus capas de textura (desde la sección de enlaces de texturas).
    materials: list[Material] = []
    mat_by_off: dict[int, Material] = {}
    for mname, m in sec["mats"]:
        if not _back_ok(d, m, mdl0):
            warnings.append(f"material {mname}: estructura inesperada")
            continue
        flags = _u32(d, m + 0x10)
        cull = _u32(d, m + 0x18)
        mat = Material(mname, _s32(d, m + 0x0C), m, cull if cull <= 3 else 2, bool(flags & 0x80000000))
        materials.append(mat)
        mat_by_off[m] = mat
    layer_refs: dict[int, list[tuple[int, str]]] = {}
    for tname, t in sec["texs"]:
        n = _u32(d, t)
        for i in range(min(n, 256)):
            e = t + 4 + i * 8
            mo, ro = _s32(d, e), _s32(d, e + 4)
            for base in (t, e):
                if base + mo in mat_by_off:
                    layer_refs.setdefault(base + mo, []).append((base + ro, tname))
                    break
    for moff, refs in layer_refs.items():
        mat = mat_by_off[moff]
        for li, (ref, tname) in enumerate(sorted(set(refs))):
            ws = _u32(d, ref + 0x18) if ref + 0x1C <= len(d) else 1
            wt = _u32(d, ref + 0x1C) if ref + 0x20 <= len(d) else 1
            tm = _u32(d, ref + 0x10) if ref + 0x14 <= len(d) else li
            mat.layers.append(Layer(tname, ws if ws <= 2 else 1, wt if wt <= 2 else 1, tm if tm < 8 else li))

    # TEV: lista de comandos del shader (etapas) + la del material (registros, alfa, mezcla, texgens)
    from . import tev as _tev
    shader_offs = {off for _, off in sec["shaders"]}
    for mat in materials:
        m = mat.offset
        size = _u32(d, m)
        cmds = []
        sh = m + _s32(d, m + 0x28)
        if sh in shader_offs:
            cmds += _tev.scan_commands(d, sh + 0x10, sh + _u32(d, sh))
        dl = _s32(d, m + 0x38)
        start = m + dl if 0x40 <= dl < size else m + 0x40
        cmds += _tev.scan_commands(d, start, m + size)
        mat.tev = _tev.raw_from_commands(cmds, d[m + 0x16], d[m + 0x14])
        if not mat.tev.found:
            warnings.append(f"material {mat.name}: no se encontraron comandos TEV")

    # Lista de dibujo: qué objeto se dibuja con qué material.
    draws: list[tuple[int, int, bool]] = []
    for dname, doff in sec["defs"]:
        if dname not in ("DrawOpa", "DrawXlu"):
            continue
        p = doff
        while p < len(d) and d[p] == 0x04:
            mat_i, obj_i = _u16(d, p + 1), _u16(d, p + 3)
            draws.append((obj_i, mat_i, dname == "DrawXlu"))
            p += 8
    mat_index = {m.index: i for i, m in enumerate(materials)}

    # Objetos
    pos_l, nrm_l, col_l, uv_l, idx_l = [], [], [], [], []
    obj_meshes: dict[int, tuple[int, int]] = {}
    total = 0
    stats = {"objetos": 0, "triangulos": 0, "omitidos": 0}
    has_n = has_c = False
    for oname, o in sec["objs"]:
        if not _back_ok(d, o, mdl0):
            warnings.append(f"objeto {oname}: estructura inesperada")
            stats["omitidos"] += 1
            continue
        try:
            node = _s32(d, o + 0x08)
            lo, hi = _u32(d, o + 0x0C), _u32(d, o + 0x10)
            psize, poff_rel = _u32(d, o + 0x24), _s32(d, o + 0x2C)
            oidx = _s32(d, o + 0x3C)
            ids = struct.unpack_from(">hhhh8h", d, o + 0x48)
            vid, nid, cid = ids[0], ids[1], ids[2]
            tid = ids[4]
            start = None
            for base in (o + 0x24, o + 0x2C, o):
                if _check_dl(d, base + poff_rel, psize):
                    start = base + poff_rel
                    break
            if start is None:
                raise MDL0Error("no se encontró la lista de primitivas")
            fields = _vcd_fields(lo, hi)
            if any(t == ATTR_DIRECT for n, t in fields if n in ("pos", "nrm", "clr0", "tex0")):
                raise MDL0Error("atributos directos no soportados")
            tris, snaps, _ = _parse_display_list(d, start, psize, fields, 0)
            P = verts.get(vid)
            if P is None:
                raise MDL0Error("sin arreglo de posiciones")
            N, C, T = norms.get(nid), colors.get(cid), uvs.get(tid)
            single = by_node.get(node).world if node >= 0 and node in by_node else None
            n_tri = len(tris)
            pos = np.zeros((n_tri * 3, 3), np.float32)
            nrm = np.zeros((n_tri * 3, 3), np.float32)
            col = np.full((n_tri * 3, 4), 255, np.uint8)
            uv = np.zeros((n_tri * 3, 2), np.float32)
            k = 0
            for tri in tris:
                for v, snap_i in tri:
                    m = single
                    if "pnmtx" in v:
                        mid = snaps[snap_i].get(v["pnmtx"] // 3)
                        b = by_node.get(mid) if mid is not None else None
                        m = b.world if b is not None else None
                    p3 = P[v["pos"]] if v["pos"] < len(P) else np.zeros(3, np.float32)
                    if m is not None:
                        p3 = m[:3, :3] @ p3 + m[:3, 3]
                    pos[k] = p3
                    if N is not None and "nrm" in v and v["nrm"] < len(N):
                        n3 = N[v["nrm"]]
                        if m is not None:
                            n3 = m[:3, :3] @ n3
                        ln = float(np.linalg.norm(n3))
                        nrm[k] = n3 / ln if ln > 0 else n3
                    if C is not None and "clr0" in v and v["clr0"] < len(C):
                        col[k] = C[v["clr0"]]
                    if T is not None and "tex0" in v and v["tex0"] < len(T):
                        uv[k] = T[v["tex0"]]
                    k += 1
            has_n |= N is not None and "nrm" in dict(fields)
            has_c |= C is not None and "clr0" in dict(fields)
            pos_l.append(pos)
            nrm_l.append(nrm)
            col_l.append(col)
            uv_l.append(uv)
            idx_l.append(np.arange(total, total + n_tri * 3, dtype=np.uint32))
            obj_meshes[oidx] = (total, n_tri * 3)
            total += n_tri * 3
            stats["objetos"] += 1
            stats["triangulos"] += n_tri
        except (MDL0Error, struct.error, IndexError, KeyError) as e:
            warnings.append(f"objeto {oname}: {e}")
            stats["omitidos"] += 1

    submeshes = []
    used = set()
    for obj_i, mat_i, xlu in draws:
        if obj_i in obj_meshes and (obj_i, mat_i) not in used:
            used.add((obj_i, mat_i))
            first, count = obj_meshes[obj_i]
            submeshes.append(SubMesh(mat_index.get(mat_i, 0), first, count, xlu))
    for obj_i, (first, count) in obj_meshes.items():   # objetos sin comando de dibujo
        if not any(s.first == first for s in submeshes):
            submeshes.append(SubMesh(0, first, count, False))
    if not draws and obj_meshes:
        warnings.append("sin lista DrawOpa/DrawXlu: materiales asignados por defecto")

    cat = (lambda lst, shape, dt: np.concatenate(lst) if lst else np.zeros(shape, dt))
    model = Model(
        name=name, version=version,
        positions=cat(pos_l, (0, 3), np.float32), normals=cat(nrm_l, (0, 3), np.float32),
        colors=cat(col_l, (0, 4), np.uint8), uvs=cat(uv_l, (0, 2), np.float32),
        indices=cat(idx_l, (0,), np.uint32), submeshes=submeshes, materials=materials,
        has_normals=has_n, has_colors=has_c, stats=stats, warnings=warnings)
    model.stats.update(huesos=len(bones), materiales=len(materials), version=version)
    _weld(model)
    return model


def _weld(m: Model):
    """Une vértices idénticos para reducir el búfer (los triángulos se generan sin compartir)."""
    if m.positions.shape[0] == 0:
        return
    key = np.concatenate([m.positions.view(np.uint32), m.normals.view(np.uint32),
                          m.colors.view(np.uint8).reshape(-1, 4).astype(np.uint32), m.uvs.view(np.uint32)], 1)
    _uniq, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.reshape(-1)
    m.positions, m.normals, m.colors, m.uvs = m.positions[first], m.normals[first], m.colors[first], m.uvs[first]
    m.indices = inverse[m.indices].astype(np.uint32)


def find_models(brres: bytes, brres_sections: dict) -> list[tuple[str, int]]:
    return [(n, off) for n, off in brres_sections.get("3DModels(NW4R)", []) if brres[off:off + 4] == b"MDL0"]


def describe(d: bytes, mdl0: int, name: str, max_items: int = 12) -> str:
    """Volcado de diagnóstico en texto: campos crudos de huesos, objetos y listas de dibujo."""
    out = []
    ver = _u32(d, mdl0 + 8)
    out.append(f"MDL0 '{name}' versión {ver}, tamaño {_u32(d, mdl0 + 4)}")
    for names in (SECTIONS_V8, SECTIONS_V10):
        n_off = mdl0 + 0x10 + 4 * len(names)
        out.append(f"  ¿{len(names)} secciones? nombre en cabecera = '{_cstr(d, mdl0 + _s32(d, n_off))}'")
    layout = SECTIONS_V8 if ver <= 9 else SECTIONS_V10
    for names in (SECTIONS_V8, SECTIONS_V10):
        if _cstr(d, mdl0 + _s32(d, mdl0 + 0x10 + 4 * len(names))) == name:
            layout = names
    sec = {}
    for i, sname in enumerate(layout):
        off = _s32(d, mdl0 + 0x10 + 4 * i)
        sec[sname] = _group(d, mdl0 + off) if off else []
        out.append(f"  sección {i:2d} {sname:8s} offset 0x{off:X}  entradas {len(sec[sname])}")
    hexs = lambda a, n: d[a:a + n].hex(" ")
    for dname, doff in sec["defs"]:
        out.append(f"  def {dname}: {hexs(doff, 48)}")
    for bname, b in sec["bones"][:max_items]:
        t = struct.unpack_from(">3f", d, b + 0x38)
        mt = struct.unpack_from(">12f", d, b + 0x70)
        out.append(f"  hueso {bname}: back_ok={_back_ok(d, b, mdl0)} idx={_s32(d, b + 0x0C)} node={_s32(d, b + 0x10)} "
                   f"flags=0x{_u32(d, b + 0x14):X} T=({t[0]:.2f},{t[1]:.2f},{t[2]:.2f}) "
                   f"M.T=({mt[3]:.2f},{mt[7]:.2f},{mt[11]:.2f}) padre={_s32(d, b + 0x5C)}")
    if len(sec["bones"]) > max_items:
        out.append(f"  ... {len(sec['bones']) - max_items} huesos más")
    for oname, o in sec["objs"][:max_items]:
        lo, hi = _u32(d, o + 0x0C), _u32(d, o + 0x10)
        out.append(f"  objeto {oname}: back_ok={_back_ok(d, o, mdl0)} node={_s32(d, o + 0x08)} "
                   f"VCD={lo:08X}/{hi:08X} campos={[n for n, _ in _vcd_fields(lo, hi)]}")
        out.append(f"    cabecera: {hexs(o, 0x68)}")
        psize, poff = _u32(d, o + 0x24), _s32(d, o + 0x2C)
        for base, lbl in ((o + 0x24, "+0x24"), (o + 0x2C, "+0x2C"), (o, "+0")):
            if _check_dl(d, base + poff, psize):
                out.append(f"    primitivas (base {lbl}): {hexs(base + poff, 40)}")
                break
        else:
            out.append("    primitivas: NO encontradas")
    if len(sec["objs"]) > max_items:
        out.append(f"  ... {len(sec['objs']) - max_items} objetos más")

    # Materiales y shaders (TEV)
    from . import tev
    try:
        model = read_mdl0(d, mdl0, name)
        mats = {m.offset: m for m in model.materials}
    except Exception:  # noqa: BLE001
        mats = {}
    shader_offs = {off for _, off in sec["shaders"]}
    for mname, m in sec["mats"][:max_items]:
        size = _u32(d, m)
        mat = mats.get(m)
        sh_rel = _s32(d, m + 0x28)
        out.append(f"  material {mname}: tamaño 0x{size:X} idx={_s32(d, m + 0x0C)} flags=0x{_u32(d, m + 0x10):X} "
                   f"texgens={d[m + 0x14]} canales={d[m + 0x15]} etapas={d[m + 0x16]} ind={d[m + 0x17]} "
                   f"cull={_u32(d, m + 0x18)} shader(+0x28)->{'shader OK' if m + sh_rel in shader_offs else hex(sh_rel)}")
        out.append(f"    cabecera: {hexs(m, 0x48)}")
        if mat:
            for i, lay in enumerate(mat.layers):
                out.append(f"    capa {i}: textura '{lay.texture}' wrap {lay.wrap_s}/{lay.wrap_t}")
        cfg = tev.decode(tev.scan_commands(d, m + 0x40, m + size))
        out += tev.format_config(cfg)
    for sname, sh in sec["shaders"][:max_items]:
        size = _u32(d, sh)
        users = [n for n, m in sec["mats"] if m + _s32(d, m + 0x28) == sh]
        out.append(f"  shader {sname}: tamaño 0x{size:X} etapas={d[sh + 0x0C]} usado por {users[:4]}")
        out.append(f"    cabecera: {hexs(sh, 0x20)}")
        cfg = tev.decode(tev.scan_commands(d, sh + 0x10, sh + size))
        out += tev.format_config(cfg, active=d[sh + 0x0C] or None)
    return "\n".join(out)
