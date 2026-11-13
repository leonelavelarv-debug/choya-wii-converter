"""Copia el runtime de WiiCompiled y lo mueve a las direcciones de Wii Sports (RSPE01 Rev 1).

El runtime de WiiCompiled reemplaza funciones del juego por código nativo (HLE) y lee variables
del SDK en direcciones fijas. Esas direcciones son de otros juegos (Brawl, NSMBW, MKW, SPM; el
runtime mezcla los cuatro). Este módulo:

  1. copia runtime/ a runtime-wiisports/ en tu copia local de WiiCompiled (nada se sube);
  2. mueve cada función nativa a su dirección en Wii Sports, buscándola por su nombre;
  3. desactiva con `#if 0` las que Wii Sports no tiene y los registros repetidos;
  4. mueve las variables del SDK cuyo comentario las nombra (`// FifoObj`, `// HorVer+0x1C`);
  5. anota en retarget.md lo que no pudo resolver con seguridad, para revisarlo a mano.

Regla: ante la duda no se cambia nada y se anota. Una dirección mal movida en silencio es peor
que una marcada para revisar. Choya no incluye código de WiiCompiled: solo lee tu copia.
"""
from __future__ import annotations

import gzip
import os
import re
import shutil
from collections import Counter, defaultdict
from dataclasses import dataclass, field

from . import recomp

BUNDLED_SYMBOLS = os.path.join(recomp.DATA, "RSPE01_01.symbols.gz")
MARKER = ".choya-generado"
OUT_DIR = "runtime-wiisports"
MISSING_PREFIX = "choya_ausente_"
MISSING_FILE = "src/choya_ausentes.cpp"
HLE_FILE = "src/choya_wiisports_hle.cpp"
PERFIL_FILE = "src/choya_perfil.cpp"  # perfilador por muestreo (solo con CHOYA_PERFIL)
GLES_FILE = "src/choya_gles.cpp"  # pantalla GLES propia de Choya (solo con CHOYA_GLES=1)

# Funciones nativas propias de Choya para lo que Wii Sports usa y los juegos de WiiCompiled no.
# Cada una: (función de Wii Sports, cuerpo en C++ con {addr} para la dirección).
CHOYA_NATIVES = [
    # RPSysCoreController::initScene enciende la barra de sensores. En la consola es un bit de GPIO de
    # Hollywood (0xCD0000C0); aquí no hay hardware: se guarda el estado y se devuelve el anterior.
    ("WPADSetSensorBarPower", """namespace {{ bool g_choyaSensorBar = false; }}
extern "C" uint32_t WPADSetSensorBarPower_choya(uint32_t enable)
{{
    const bool previous = g_choyaSensorBar;
    g_choyaSensorBar = enable != 0;
    return previous ? 1u : 0u;
}}
PPC_NATIVE_OVERRIDE({addr}, WPADSetSensorBarPower_choya, uint32_t, (uint32_t enable), (enable));
"""),
]

LOW_MEM_END = 0x80004000      # 0x80000000-0x80003FFF: globales del sistema, iguales en todos los juegos
GUEST_END = 0x81800000

_STATEMENTS = re.compile(
    r"\b(?P<macro>PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_FUNCTION(?:_AS)?|REGISTER_TRANSLATED_FUNCTION"
    r"|MKW_KNOWN_TYPED_NATIVE|GX_FATAL_STUB)\s*\(\s*(?:0x)?(?P<addr>[0-9A-Fa-f]{8})[uU]?"
    r"\s*(?:,\s*(?P<sym>[A-Za-z_]\w*))?")
_FUNC_REF = re.compile(r"\bfunc_(?P<addr>8[0-9A-Fa-f]{7})\b")
_HEX = re.compile(r"\b0x(?P<addr>8[01][0-9A-Fa-f]{6})(?P<suf>[uU]?)\b")
# Referencia en un comentario: "Nombre", "Nombre + 0x14", "Nombre.Campo" o "Nombre's" (dentro de una función)
_CREF = re.compile(r"^\s*(?P<name>[A-Za-z_]\w*)(?:(?P<poss>'s)|\s*\+\s*0x(?P<off>[0-9A-Fa-f]+)|\.(?P<field>[A-Za-z_]\w*))?")
# Etiquetas de juego que preceden al nombre en los comentarios: "// SPM AlarmQueue", "// (NSMBW) FifoObj"
_TAGS = re.compile(r"^(?:\s*(?:(?:SPM|NSMBW|MKW|Brawl|BRAWL|RSBE01|SMNE01|R8PE01|RMCP01)\b|[(),:]))*")
# Desplazamientos desde las bases SDA: "r13 - 0x3d00u", "ctx->gpr[13] - 0x6F24u", "RuntimeConfig::SDA1_BASE - k"
_SDA_EXPR = re.compile(r"(?P<reg>\br13\b|gpr\[13\]|SDA1_BASE|\br2\b|gpr\[2\]|SDA2_BASE)\s*-\s*0x(?P<off>[0-9A-Fa-f]+)(?P<suf>[uU]?)")
_SDA_CONST = re.compile(r"\b(?P<name>k\w*FromR13)\s*=\s*0x(?P<off>[0-9A-Fa-f]+)(?P<suf>[uU]?)")
_INLINE_COMMENT = re.compile(r"^[)\s]*/\*\s*([A-Za-z_]\w*)\s*\*/")
_GENERIC = re.compile(r"^(?:fn|func|sub|lbl|FUN|zz|gap|jumptable|switch)_[0-9A-Fa-f]")
_SOURCE_EXT = (".cpp", ".h", ".hpp", ".inc", ".c")
_CATALOG = "MKW_KNOWN_TYPED_NATIVE"   # catálogo de llamadas tipadas: se mueve pero no registra


@dataclass
class Symbols:
    funcs: dict[str, int]
    data: dict[str, int]
    size: dict[int, int]
    ambiguous: set[str]

    def get(self, name: str) -> int | None:
        if name in self.ambiguous:
            return None
        return self.funcs.get(name, self.data.get(name))


def load_wiisports_symbols(path: str | None = None) -> Symbols:
    """Símbolos de ogws (funciones y datos), de config/RSPE01_01/symbols.txt (CC0)."""
    rows = []
    if path:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]+);\s*//\s*type:(\w+)(?:.*\bsize:0x([0-9A-Fa-f]+))?",
                             line)
                if m:
                    rows.append((int(m.group(3), 16), m.group(4)[0], int(m.group(5) or "0", 16), m.group(1)))
    else:
        with gzip.open(BUNDLED_SYMBOLS, "rt", encoding="utf-8") as f:
            for line in f:
                a, kind, size, _sec, name = line.split()
                rows.append((int(a, 16), kind, int(size, 16), name))
    funcs, data, size, seen = {}, {}, {}, Counter()
    for a, kind, sz, name in rows:
        if recomp.is_placeholder(name):
            continue
        seen[name] += 1
        (funcs if kind == "f" else data).setdefault(name, a)
        size.setdefault(a, sz)
    return Symbols(funcs, data, size, {n for n, c in seen.items() if c > 1})


def base_names(symbol: str) -> list[str]:
    """Nombres del SDK que puede representar un símbolo del runtime:
    GX__SetTevOrder_8017214c → GXSetTevOrder · __GX__SaveFifo_8016cdbc → __GXSaveFifo ·
    OS____MaskInterrupts → __OSMaskInterrupts · __OSUnmaskInterrupts_801a69bc_hle → __OSUnmaskInterrupts ·
    WPADControlLed_HLE → WPADControlLed · GX__SetDirtyState → __GXSetDirtyState ·
    SPM_GXSetDrawSync → GXSetDrawSync · NAND_IOS_Open_HLE → IOS_Open · OS__Panic_801A2660_Cpu → OSPanic."""
    s = symbol
    for _ in range(3):
        s = re.sub(r"_(?:hle|HLE|Hle|Cpu|SPM|Entry)$", "", s)
        s = re.sub(r"_[0-9a-fA-F]{8}$", "", s)
    s = re.sub(r"^(?:Brawl|SPM|NSMBW|MKW|HLE)_", "", s)
    s = re.sub(r"^[A-Za-z]+_(?=IOS_)", "", s)
    joined = re.sub(r"(?<=[A-Za-z0-9])__", "", s, count=1)
    cands = [s, joined]
    m = re.match(r"^([A-Z][A-Za-z0-9]*)____(\w+)$", s)
    if m:
        cands.append(f"__{m.group(1)}{m.group(2)}")
    if not joined.startswith("_"):
        cands.append("__" + joined)
    out = []
    for c in cands:
        if c and c not in out:
            out.append(c)
    return out


def load_game_maps(wiicompiled: str) -> dict[str, dict[int, str]]:
    maps = {}
    proj = os.path.join(wiicompiled, "projects")
    for game in sorted(os.listdir(proj)) if os.path.isdir(proj) else []:
        p = os.path.join(proj, game, "MAP.txt")
        if game != "wiisports" and os.path.isfile(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                maps[game] = {a: n for a, n in recomp.parse_map(f.read()) if n and not n.startswith("0x") and not _GENERIC.match(n)}
    return maps


def source_files(root: str):
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("build"))
        for fn in sorted(files):
            if fn.endswith(_SOURCE_EXT):
                yield os.path.join(dirpath, fn)


def disabled_lines(text: str) -> set[int]:
    """Líneas apagadas por un `#if 0` (hasta su #else/#endif), igual que las ignora el traductor."""
    out, frames = set(), []          # cada marco: [activo, es_if0]
    for i, raw in enumerate(text.split("\n"), 1):
        s = raw.strip()
        d = s[1:].lstrip() if s.startswith("#") else ""
        if d.startswith("if"):
            zero = re.match(r"if\s+0\b", d) is not None
            frames.append([not zero, zero])
            out.add(i)
            continue
        if d.startswith(("else", "elif")) and frames:
            if frames[-1][1]:
                frames[-1] = [True, False]
            out.add(i)
            continue
        if d.startswith("endif") and frames:
            frames.pop()
            out.add(i)
            continue
        if not all(f[0] for f in frames):
            out.add(i)
    return out


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _statement_end(text: str, open_paren: int) -> int:
    """Índice justo después del `;` (o del `)`) que cierra la instrucción."""
    depth, i, n = 0, open_paren, len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                j = i + 1
                while j < n and text[j] in " \t":
                    j += 1
                return j + 1 if j < n and text[j] == ";" else i + 1
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        i += 1
    return n


def _fmt(old_text: str, value: int) -> str:
    return f"{value:08X}" if any(c in "ABCDEF" for c in old_text) else f"{value:08x}"


# --- resultados ---------------------------------------------------------------------

@dataclass
class Change:
    file: str
    line: int
    kind: str           # registro | desactivado | duplicado | funcion | dato | sin_resolver
    old: int
    new: int | None
    name: str
    text: str


@dataclass
class RetargetResult:
    out_dir: str
    source_game: str | None
    changes: list[Change] = field(default_factory=list)
    game: "GameInfo | None" = None
    structs: list = field(default_factory=list)

    def count(self, kind: str) -> int:
        return sum(1 for c in self.changes if c.kind == kind)


class _Resolver:
    def __init__(self, ws: Symbols, maps: dict[str, dict[int, str]]):
        self.ws = ws
        self.maps = list(maps.values())
        self.moved: dict[int, tuple[str, int]] = {}    # dirección vieja → (nombre, nueva), de los registros
        self._pairs: list[tuple[int, int]] = []
        self.func_at: dict[int, str] = {}
        self.sda: dict[str, int | None] = {"r13": None, "r2": None}
        self.sda_seen: dict[tuple, str] = {}
        self.missing: set[int] = set()
        self.anchors: dict[str, int] = {}               # símbolo → dirección vieja, de los comentarios de todo el runtime

    def neighbors(self, addr: int) -> tuple[str, int] | None:
        """Dirección sin nombre entre dos funciones nativas ya movidas con el mismo desfase: si al
        aplicar ese desfase cae justo en el inicio de una función de Wii Sports, es esa función
        (el SDK se enlaza en el mismo orden en todos los juegos)."""
        import bisect
        pairs = self._pairs
        i = bisect.bisect_left(pairs, (addr, 0))
        if i == 0 or i >= len(pairs) or pairs[i][0] == addr:
            return None
        (lo_old, lo_new), (hi_old, hi_new) = pairs[i - 1], pairs[i]
        delta = lo_old - lo_new
        if delta != hi_old - hi_new:
            return None
        name = self.func_at.get(addr - delta)
        return (name, addr - delta) if name else None

    def set_moved(self, moved: dict[int, tuple[str, int]]) -> None:
        self.moved = moved
        self._pairs = sorted((old, new) for old, (_n, new) in moved.items())
        self.func_at = {a: n for n, a in self.ws.funcs.items() if n not in self.ws.ambiguous}

    def function(self, addr: int, symbol: str = "", comment: str = "") -> tuple[str, int] | None:
        """El nombre del símbolo manda (lo puso quien escribió la función nativa); después el
        comentario de la línea; al final los mapas de los otros juegos."""
        cands = base_names(symbol) if symbol else []
        m = _CREF.match(_strip_tags(comment))
        if m and not (m.group("poss") or m.group("off") or m.group("field")):
            cands.append(m.group("name"))
        cands += [mp[addr] for mp in self.maps if addr in mp]
        for c in cands:
            a = self.ws.funcs.get(c) if c not in self.ws.ambiguous else None
            if a is not None:
                return c, a
        return None


def _strip_tags(comment: str) -> str:
    return comment[_TAGS.match(comment).end():] if comment else comment


def _comment_ref(comment: str):
    m = _CREF.match(_strip_tags(comment))
    if not m:
        return None
    if m.group("poss"):
        return m.group("name"), "dentro", None
    if m.group("off"):
        return m.group("name"), "off", int(m.group("off"), 16)
    if m.group("field"):
        return m.group("name"), "campo", m.group("field")
    return m.group("name"), "off", 0


def _literals(code: str) -> list[re.Match]:
    return [m for m in _HEX.finditer(code) if LOW_MEM_END <= int(m.group("addr"), 16) < GUEST_END]


def _anchors(lines: list[str], dead: set[int]) -> dict[str, int]:
    """Base vieja de cada símbolo, sacada de las líneas `0x... // Nombre` o `// Nombre + 0x14` del archivo."""
    out = {}
    for i, raw in enumerate(lines, 1):
        code, sep, comment = raw.partition("//")
        lits = _literals(code)
        if i in dead or not sep or len(lits) != 1:
            continue
        ref = _comment_ref(comment)
        if ref and ref[1] == "off":
            out.setdefault(ref[0], int(lits[0].group("addr"), 16) - ref[2])
    return out


def _resolve_literal(old: int, comment: str, single: bool, anchors: dict[str, int], res: _Resolver):
    """→ (kind, nombre, nueva) o None. Solo con prueba: el comentario, un registro movido, o una
    variable que otro comentario del runtime ya identificó (misma dirección o dentro de ella)."""
    if old in res.moved:
        name, new = res.moved[old]
        return "funcion", name, new
    ref = _comment_ref(comment) if single else None
    if ref and ref[1] != "dentro":
        name, how, extra = ref
        base = res.ws.get(name)
        if base is not None:
            if how == "off":
                off = extra
            else:                           # campo: la base vieja sale de otra línea
                ob = anchors.get(name, res.anchors.get(name))
                off = None if ob is None else old - ob
            size = res.ws.size.get(base, 0)
            if off is not None and off >= 0 and not (off and size and off >= size):
                label = name if how == "off" and off == 0 else (
                    f"{name}.{extra}" if how == "campo" else f"{name}+0x{off:X}")
                return ("funcion" if name in res.ws.funcs else "dato"), label, base + off
    if ref:
        return None
    # sin comentario: ¿cae dentro de una variable ya identificada en otro lado?
    hits = []
    for name, ob in res.anchors.items():
        base = res.ws.get(name)
        size = res.ws.size.get(base, 0) if base is not None else 0
        if base is not None and name in res.ws.data and ob <= old < ob + max(size, 1):
            hits.append((name, base + old - ob, old - ob))
    if len(hits) == 1:
        name, new, off = hits[0]
        return "dato", name if off == 0 else f"{name}+0x{off:X}", new
    near = res.neighbors(old)
    if near:
        return "vecinos", near[0], near[1]
    return None


# Motivos con los que WiiCompiled apagó un registro porque *otro* juego no tenía la función.
_ABSENT = re.compile(r"no matching function|name not in|not linked|no \w+ symbol at this address|"
                     r"has no |not in the \w+ dol|is not present|does not exist", re.I)


def _live_symbol(text: str, sym: str, dead: set[int]) -> bool:
    """¿La función nativa `sym` existe fuera de un `#if 0`? (definida aquí o declarada para otro archivo).
    Si su cuerpo también quedó apagado (p. ej. porque leía direcciones de MKW), no se reactiva."""
    for m in re.finditer(rf"\b{re.escape(sym)}\s*\(", text):
        line = _line_of(text, m.start())
        if line in dead:
            continue
        before = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
        if _STATEMENTS.search(before) or before.strip().startswith("//"):
            continue                     # es el propio registro o un comentario
        return True
    return False


def reactivate(text: str, rel: str, res: "_Resolver", changes: list[Change]) -> str:
    """WiiCompiled apagó con `#if 0  // BRAWL: no matching function...` funciones nativas que Brawl, SPM o
    NSMBW no tienen. Si Wii Sports sí las tiene (por el nombre del símbolo) y el bloque contiene solo ese
    registro, se vuelve a encender: `#if 0` → `#if 1`. Así OSInitFont, por ejemplo, vuelve a ser nativa
    en vez de leer un registro de hardware que no existe."""
    lines = text.split("\n")
    dead_before = disabled_lines(text)
    stack = []                     # (línea, es_if0, motivo_de_ausencia)
    blocks = []                    # (inicio, fin) de cada #if 0
    for i, raw in enumerate(lines):
        s = raw.strip()
        d = s[1:].lstrip() if s.startswith("#") else ""
        if d.startswith("if"):
            stack.append((i, re.match(r"if\s+0\b", d) is not None, bool(_ABSENT.search(s))))
        elif d.startswith(("else", "elif")) and stack:
            stack[-1] = (stack[-1][0], False, False)
        elif d.startswith("endif") and stack:
            start, zero, absent = stack.pop()
            if zero:
                blocks.append((start, i, absent))
    for start, end, absent in sorted(blocks):
        if not absent or "choya:" in lines[start]:
            continue                     # (un bloque interno ya encendido junto con el de afuera)
        body = "\n".join(lines[start + 1:end])
        content = [l for l in lines[start + 1:end]
                   if l.strip() and not l.strip().startswith(("#", "//"))]
        m = _STATEMENTS.search(body)
        if not m or not content or m.group("macro") == _CATALOG:
            continue
        stmt_end = _statement_end(body, body.index("(", m.start()))
        stmt_lines = [l for l in body[m.start():stmt_end].split("\n") if l.strip()]
        if len(content) != len(stmt_lines) or len(_STATEMENTS.findall(body)) != 1:
            continue
        inner = [lines[j].strip() for j in range(start + 1, end) if lines[j].strip().startswith("#if")]
        if any(not (re.match(r"#\s*if\s+0\b", x) and _ABSENT.search(x)) for x in inner):
            continue
        sym = m.group("sym") or ""
        name = next((c for c in base_names(sym) if c in res.ws.funcs and c not in res.ws.ambiguous), None)
        if not name or not _live_symbol(text, sym, dead_before):
            continue
        for j in [start] + [j for j in range(start + 1, end) if lines[j].strip().startswith("#if")]:
            lines[j] = re.sub(r"#(\s*)if\s+0\b", r"#\1if 1", lines[j], count=1) + \
                f"  // choya: Wii Sports sí tiene {name}"
        changes.append(Change(rel, start + 1, "reactivado", int(m.group("addr"), 16), None, name,
                              " ".join(body[m.start():stmt_end].split())[:140]))
    return "\n".join(lines)



def retarget_text(text: str, rel: str, res: _Resolver, registered: dict[int, str],
                  changes: list[Change]) -> str:
    # 1) instrucciones de registro
    edits = []
    dead = disabled_lines(text)
    for m in _STATEMENTS.finditer(text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        line = _line_of(text, m.start())
        if text[line_start:m.start()].strip().startswith(("//", "#define")) or line in dead:
            continue
        if edits and line_start < edits[-1][1]:
            continue
        old = int(m.group("addr"), 16)
        end = _statement_end(text, text.index("(", m.start()))
        eol = text.find("\n", end)
        eol = len(text) if eol < 0 else eol
        trailing = text[end:eol].strip()
        sym = m.group("sym") or ""
        snippet = " ".join(text[m.start():end].split())[:140]
        registers = m.group("macro") != _CATALOG
        hit = res.function(old, sym, trailing[2:] if trailing.startswith("//") else "")
        here = f"{rel}:{line}"
        reason = "Wii Sports no tiene esta funcion"
        if hit and registers and registered.get(hit[1], here) != here:
            changes.append(Change(rel, line, "duplicado", old, hit[1], hit[0], snippet))
            reason, hit = f"repetido; ya registrado en {registered[hit[1]]}", None
        elif hit is None:
            changes.append(Change(rel, line, "desactivado", old, None, sym, snippet))
        if hit:
            if registers:
                registered[hit[1]] = here
            a0, a1 = m.span("addr")
            edits.append((a0, a1, _fmt(m.group("addr"), hit[1])))
            changes.append(Change(rel, line, "registro", old, hit[1], hit[0], snippet))
        else:
            edits.append((line_start, eol,
                          f"#if 0  // choya: {reason} ({old:08X} {sym})\n{text[line_start:eol]}\n#endif"))
    for a0, a1, rep in sorted(edits, key=lambda e: e[0], reverse=True):
        text = text[:a0] + rep + text[a1:]

    # 2) func_XXXXXXXX y constantes 0x80xxxxxx fuera de los registros
    lines = text.split("\n")
    dead = disabled_lines(text)
    anchors = _anchors(lines, dead)
    # func_XXXXXXXX definidas en este mismo archivo (p. ej. `static void func_801A6D30(CpuContext*) {}`):
    # son del runtime, no del juego, y no se tocan.
    local_defs = {int(a, 16) for a in re.findall(r"\bfunc_(8[0-9A-Fa-f]{7})\s*\([^;{)]*\)\s*(?:const\s*)?\{", text)}
    for i, raw in enumerate(lines, 1):
        s = raw.strip()
        if i in dead or s.startswith(("//", "*", "/*", "#")) or _STATEMENTS.search(raw) or "choya:" in raw:
            continue
        code, sep, comment = raw.partition("//")
        code = _sda_line(code, comment, rel, i, s, res, changes)
        lits = _literals(code)

        def sub_func(m, i=i, s=s):
            old = int(m.group("addr"), 16)
            if old in local_defs:
                return m.group(0)
            hit = res.moved.get(old)
            kind = "funcion"
            if hit is None:
                hit, kind = res.neighbors(old), "vecinos"
            if hit:
                name, new = hit
                changes.append(Change(rel, i, kind, old, new, name, s[:140]))
                return "func_" + _fmt(m.group("addr"), new)
            # Función de otro juego sin equivalente: se renombra para que nunca se enlace con una función
            # de Wii Sports que caiga en la misma dirección; el stub de choya_ausentes.cpp avisa si se llama.
            changes.append(Change(rel, i, "ausente", old, None, _suggest(old, res), s[:140]))
            res.missing.add(old)
            return f"{MISSING_PREFIX}{old:08X}"

        def sub_hex(m, i=i, s=s, comment=comment, single=len(lits) == 1):
            old = int(m.group("addr"), 16)
            if not (LOW_MEM_END <= old < GUEST_END):
                return m.group(0)
            r = _resolve_literal(old, comment, single, anchors, res)
            if r:
                kind, name, new = r
                changes.append(Change(rel, i, kind, old, new, name, s[:140]))
                return "0x" + _fmt(m.group("addr"), new) + m.group("suf")
            changes.append(Change(rel, i, "sin_resolver", old, None, _suggest(old, res), s[:140]))
            return m.group(0)

        new_code = _HEX.sub(sub_hex, _FUNC_REF.sub(sub_func, code))
        lines[i - 1] = new_code + sep + comment
    return "\n".join(lines)


def _sda_line(code: str, comment: str, rel: str, i: int, s: str, res: _Resolver, changes: list[Change]) -> str:
    """Reescribe `r13 - 0xNNNN` (y r2) con el desfase de Wii Sports para la variable que nombra
    el comentario. Sin nombre seguro se deja igual y se anota."""
    def name_for(after: str) -> str | None:
        m = _INLINE_COMMENT.match(after)
        if m:
            return m.group(1)
        ref = _comment_ref(comment)
        return ref[0] if ref and ref[1] == "off" and ref[2] == 0 else None

    def rewrite(m, reg_key: str, single: bool):
        old_off = int(m.group("off"), 16)
        base = res.sda[reg_key]
        name = name_for(code[m.end():]) if single or _INLINE_COMMENT.match(code[m.end():]) else None
        if name is None:
            name = res.sda_seen.get((rel, reg_key, old_off))     # mismo desfase ya nombrado antes en el archivo
        addr = res.ws.get(name) if name else None
        if base is None or addr is None or not (0 < base - addr < 0x10000):
            changes.append(Change(rel, i, "sin_resolver", old_off, None,
                                  f"SDA {reg_key}-0x{old_off:X} {name or ''}".strip(), s[:140]))
            return m.group(0)
        new_off = base - addr
        res.sda_seen[(rel, reg_key, old_off)] = name
        changes.append(Change(rel, i, "sda", old_off, new_off, f"{name} ({reg_key}-0x{new_off:X})", s[:140]))
        hexed = f"{new_off:X}" if any(c in "ABCDEF" for c in m.group("off")) else f"{new_off:x}"
        return m.group(0)[:m.start("off") - m.start()] + hexed + m.group("suf")

    exprs = list(_SDA_EXPR.finditer(code))
    consts = list(_SDA_CONST.finditer(code))
    single = len(exprs) + len(consts) == 1
    out, pos = [], 0
    for m in sorted(exprs + consts, key=lambda m: m.start()):
        reg = m.groupdict().get("reg") or "r13"
        key = "r2" if ("2]" in reg or reg in ("r2", "SDA2_BASE")) else "r13"
        out.append(code[pos:m.start()])
        out.append(rewrite(m, key, single))
        pos = m.end()
    out.append(code[pos:])
    return "".join(out)


def _suggest(old: int, res: _Resolver) -> str:
    """Pista para la revisión manual: qué función es esa dirección en otros juegos."""
    names = sorted({mp[old] for mp in res.maps if old in mp})
    hits = [f"{n}→{res.ws.funcs[n]:08X}" for n in names if n in res.ws.funcs and n not in res.ws.ambiguous]
    return ", ".join(hits or names)[:80]


def _collect_moves(root: str, res: _Resolver) -> None:
    """Primera pasada: qué dirección vieja pasa a cuál, según los registros. Si dos registros
    no se ponen de acuerdo sobre una dirección vieja, esa dirección no se mueve en el resto del código."""
    seen: dict[int, set] = defaultdict(set)
    bases: dict[str, set] = defaultdict(set)
    for sub in ("src", "include"):
        for path in source_files(os.path.join(root, sub)):
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            dead = disabled_lines(text)
            for name, ob in _anchors(text.split("\n"), dead).items():
                bases[name].add(ob)
            for m in _STATEMENTS.finditer(text):
                if _line_of(text, m.start()) in dead:
                    continue
                old = int(m.group("addr"), 16)
                end = _statement_end(text, text.index("(", m.start()))
                eol = text.find("\n", end)
                trailing = text[end:eol if eol >= 0 else len(text)].strip()
                hit = res.function(old, m.group("sym") or "", trailing[2:] if trailing.startswith("//") else "")
                if hit:
                    seen[old].add(hit)
    res.set_moved({old: next(iter(h)) for old, h in seen.items() if len(h) == 1})
    res.anchors = {n: next(iter(b)) for n, b in bases.items() if len(b) == 1 and res.ws.get(n) is not None}


def detect_source_games(runtime: str, maps: dict[str, dict[int, str]]) -> list[str]:
    votes = Counter()
    for path in source_files(os.path.join(runtime, "src")):
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in _STATEMENTS.finditer(f.read()):
                a = int(m.group("addr"), 16)
                votes.update(g for g, mp in maps.items() if a in mp)
    return [g for g, _ in votes.most_common()]


# --- datos del juego y parches revisados a mano ----------------------------------------

@dataclass
class GameInfo:
    """Lo que Choya sabe de tu disco: sale de recomp.yml, main.dol, boot.bin y fst.bin."""
    dol_path: str | None = None
    entry: int | None = None
    stack: int | None = None
    sda: int | None = None
    sda2: int | None = None
    game_code: int | None = None      # boot.bin[0:4], p. ej. "RSPE"
    maker_word: int | None = None     # boot.bin[4:8]: fabricante, número de disco, revisión
    fst_size: int | None = None
    dol: object = None                # choya.dol.Dol, para buscar tablas en los datos del juego

    def find(self, pattern: bytes) -> int | None:
        """Dirección de `pattern` en las secciones de datos del main.dol, solo si aparece una vez."""
        if self.dol is None:
            return None
        hits = []
        for s in self.dol.sections:
            if s.kind != "data":
                continue
            blob = self.dol.data[s.offset:s.offset + s.size]
            i = blob.find(pattern)
            while i >= 0:
                hits.append(s.address + i)
                i = blob.find(pattern, i + 1)
        return hits[0] if len(hits) == 1 else None

    @property
    def arena_lo(self) -> int | None:
        # Consola retail sin depurador: OSInit usa el fin de la pila de arranque (_db_stack_end = _stack_addr),
        # redondeado a 32. Es lo mismo que WiiCompiled hace para Brawl (0x805B5150 → 0x805B5160).
        return None if self.stack is None else (self.stack + 31) & ~31

    @property
    def arena_hi(self) -> int | None:
        # El apploader deja la FST al final de MEM1 y el arena termina justo debajo.
        return None if self.fst_size is None else 0x81800000 - ((self.fst_size + 31) & ~31)


def read_game_info(wiicompiled: str) -> GameInfo:
    import struct
    from . import dol as doltools
    g = GameInfo()
    yml = os.path.join(wiicompiled, "projects", "wiisports", "recomp.yml")
    if not os.path.isfile(yml):
        return g
    with open(yml, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"^\s*sda_base:\s*0x([0-9A-Fa-f]+)", text, re.M)
    g.sda = int(m.group(1), 16) if m else None
    m = re.search(r"^\s*sda2_base:\s*0x([0-9A-Fa-f]+)", text, re.M)
    g.sda2 = int(m.group(1), 16) if m else None
    m = re.search(r"^\s*dol:\s*\n\s*path:\s*\"?([^\"\n]+)\"?", text, re.M)
    if m and os.path.isfile(m.group(1).strip()):
        g.dol_path = m.group(1).strip()
        with open(g.dol_path, "rb") as f:
            d = doltools.read_dol(f.read())
        b = doltools.boot_info(d)
        g.entry, g.stack, g.dol = b.entry, b.stack, d
        sysd = os.path.dirname(g.dol_path)
        boot = os.path.join(sysd, "boot.bin")
        if os.path.isfile(boot):
            with open(boot, "rb") as f:
                g.game_code, g.maker_word = struct.unpack(">II", f.read(8))
        fst = os.path.join(sysd, "fst.bin")
        if os.path.isfile(fst):
            g.fst_size = os.path.getsize(fst)
    return g


@dataclass
class Patch:
    file: str
    pattern: str          # el grupo "v" es el número hexadecimal que se cambia
    value: object         # función (Symbols, GameInfo) -> int | None
    label: str
    optional: bool = False  # si no está en Wii Sports no importa (la función que lo usa quedó desactivada)


def _sym(name: str, off: int = 0):
    return lambda ws, g: None if ws.get(name) is None else ws.get(name) + off


_H = r"(?P<v>[0-9A-Fa-f]{8})"

# Revisados uno por uno contra ogws. Si WiiCompiled cambia y un patrón ya no aparece, se anota
# como "parche no aplicado" en vez de adivinar.
PATCHES = [
    Patch("include/system_bridge.h", r"kDefaultEntryAddress\s*=\s*0x" + _H, lambda ws, g: g.entry,
          "entrada (__start) de Wii Sports"),
    Patch("src/system_bridge.cpp", r"kMem1ArenaLoDefault\s*=\s*0x" + _H, lambda ws, g: g.arena_lo,
          "MEM1 arena lo: fin de la pila de arranque"),
    Patch("src/system_bridge.cpp", r"mem1ArenaHiDefault\s*=\s*0x" + _H, lambda ws, g: g.arena_hi,
          "MEM1 arena hi: fin de MEM1 menos la FST"),
    Patch("src/system_bridge.cpp", r"\{0x80000000u,\s*0x" + _H + r"u,\s*\"Disc game code\"",
          lambda ws, g: g.game_code, "ID del disco (boot.bin)"),
    Patch("src/system_bridge.cpp", r"\{0x80000004u,\s*0x" + _H + r"u,\s*\"Disc maker",
          lambda ws, g: g.maker_word, "fabricante y revision (boot.bin)"),
    Patch("src/system_bridge.cpp", r"\{0x80003194u,\s*0x" + _H + r"u,\s*\"OS app game code\"",
          lambda ws, g: g.game_code, "ID del juego para OSGetAppGamename"),
    Patch("src/system_bridge.cpp", r"\{0x80003194u,\s*0x" + _H + r"u,\s*\"OS app gamename\"",
          lambda ws, g: g.game_code, "ID del juego para OSGetAppGamename"),
    Patch("src/hle/gx/gx_init.cpp", r"kCpInterruptHandlerAddr\s*=\s*0x" + _H, _sym("GXCPInterruptHandler"),
          "GXCPInterruptHandler"),
    Patch("src/hle/gx/gx_init.cpp", r"kPeTokenHandlerAddr\s*=\s*0x" + _H, _sym("GXTokenInterruptHandler"),
          "GXTokenInterruptHandler"),
    Patch("src/hle/gx/gx_stubs.cpp", r"kGxDrawDoneCallbackAddr\s*=\s*0x" + _H, _sym("DrawDoneCB"), "DrawDoneCB"),
    Patch("src/hle/audio/ax_internal.h", r"kAxDspTaskAddr\s*=\s*0x" + _H, _sym("__AXDSPTask"), "__AXDSPTask"),
    Patch("src/hle/audio/ax_internal.h", r"kAxInitCallback\s*=\s*0x" + _H, _sym("__AXDSPInitCallback"),
          "__AXDSPInitCallback"),
    Patch("src/hle/audio/ax_internal.h", r"kAxResumeCallback\s*=\s*0x" + _H, _sym("__AXDSPResumeCallback"),
          "__AXDSPResumeCallback"),
    Patch("src/hle/os/os_interrupt.cpp", r"kDefaultCallbackAddr\s*=\s*0x" + _H, _sym("__OSDefaultPowerCallback"),
          "__OSDefaultPowerCallback"),
    Patch("src/hle/os/os_interrupt.cpp", r"InvokeIndirectCpu\(0x(?P<v>80211A2C)u", _sym("IPCInit"), "IPCInit"),
    Patch("src/hle/os/os_thread.cpp", r"kSchedulerInitFlagAddr\s*=\s*0x" + _H, _sym("__OSErrorTable", 0x40),
          "__OSErrorTable[OS_ERROR_FPE]"),
    Patch("src/hle/storage/dvd.cpp", r"kDvdFsInitAddress\s*=\s*0x" + _H, _sym("__DVDFSInit"), "__DVDFSInit"),
    Patch("src/hle/vi.cpp", r"kViTimingGuardAddr\s*=\s*0x" + _H, _sym("__VIDimming_All_Clear"),
          "__VIDimming_All_Clear"),
    Patch("src/hle/storage/nand_api.cpp", r"WriteGuestNandPath\(0x" + _H, _sym("s_homeDir"), "s_homeDir"),
    Patch("src/hle/storage/nand_api.cpp", r"Memory::Write32\(0x(?P<v>805A0BA8), 2\)", _sym("s_libState"),
          "s_libState"),
    Patch("src/hle/os/os_alarm.cpp", r"func_(?P<v>801D66A8)\b", _sym("InsertAlarm"), "InsertAlarm"),
    Patch("src/hle/os/os_init.cpp", r"func_(?P<v>801E0480)\b", _sym("__OSInitAlarm"), "__OSInitAlarm"),
    # Copia de gxData y FIFO guardada durante una display list (GXDisplayList.c); mismo orden que NSMBW:
    # DisplayListFifo, __savedGXdata (+0x80), OldCPUFifo (+0x680).
    Patch("src/hle/gx/gx_init.cpp", r"0x(?P<v>804DF0B0)\b", _sym("__savedGXdata"), "__savedGXdata"),
    Patch("src/hle/gx/gx_init.cpp", r"0x(?P<v>804DF6B0)\b", _sym("OldCPUFifo"), "OldCPUFifo"),
    # Tablas de SC {código, "JPN"...}: se buscan en los datos de tu main.dol.
    Patch("src/hle/sc.cpp", r"LookupProductRegion\(0x(?P<v>8047C4E0)u, 5, 13",
          lambda ws, g: g.find(b"\x00JPN\x00\x01USA\x00\x02EUR\x00"), "tabla de áreas de SC"),
    Patch("src/hle/sc.cpp", r"LookupProductRegion\(0x(?P<v>8047FAB0)u, 4, 4",
          lambda ws, g: g.find(b"\x00JP\x00\x01US\x00\x02EU\x00"), "tabla de regiones de SC",
          optional=True),   # SCGetProductGameRegion no existe en el SDK de Wii Sports
    # Wii Sports no tiene btm_cb con ese nombre; con 0 la escritura cae fuera de la memoria y se ignora.
    Patch("src/hle/os/os_init.cpp", r"kBtmCbBase\s*=\s*0x" + _H, lambda ws, g: 0, "sin btm_cb: escritura omitida"),
]


# Tamaños que el runtime da por hechos, comparados con los objetos de Wii Sports en ogws.
STRUCT_CHECKS = [
    ("DefaultThread", 0x318, "OSThread (estado en +0x2C8, cola en +0x2DC)"),
    ("IdleContext", 0x2C8, "OSContext"),
    ("gxData", 0x600, "GXData (copia de 0x600 bytes)"),
    ("__savedGXdata", 0x600, "GXData guardado en display lists"),
    ("FifoObj", 0x80, "GXFifoObj"),
    ("DisplayListFifo", 0x80, "GXFifoObj de display lists"),
    ("CPUFifo", 0x24, "kFifoObjSize = 0x24"),
    ("GPFifo", 0x24, "kFifoObjSize = 0x24"),
    ("__OSErrorTable", 0x44, "17 manejadores; FPE en +0x40"),
]


def struct_checks(ws: Symbols) -> list[tuple[str, int, int | None, str]]:
    out = []
    for name, size, what in STRUCT_CHECKS:
        a = ws.get(name)
        out.append((name, size, None if a is None else ws.size.get(a), what))
    return out


# Cambios de código (no de direcciones) para Wii Sports: (archivo, texto exacto, reemplazo, etiqueta).
# Cada texto debe aparecer exactamente una vez; si no, se anota y no se toca nada.
_B_NOTE = "  // choya: botón B, Wii Sports lo pide en casi todos los menús"

# Controles del jugador 1 al estilo de Nebula (leonelavelarv-debug/nebula, platform/knulli/game/host_window_knulli.cpp):
# el mando de la consola como game controller de SDL con el mapeo de PortMaster, los botones de cara por posición
# (o por letra si el firmware los nombra así), un perfil que da el lanzador según el firmware y la combinación de
# salida del firmware con Start, que cierra el juego limpio (gptokeyb mata con la misma si no contesta).
# Nebula usa SDL2 y los mapeos de PortMaster numeran los botones como SDL2; el runtime usa SDL3, que los numera
# distinto (A salía en X y B en L1), así que el mando de la consola se lee de /dev/input con la numeración de SDL2
# (choya_mando.cpp). Los demás mandos siguen por SDL3.
# Wii Remote vertical, como se juega Wii Sports: A = derecha (A), B = abajo (B), 1 = arriba (X), 2 = izquierda (Y),
# R1 = agitar, Start = +, Select/L1/L2 = −, cruceta y stick izquierdo = cruceta del Remote.
_NEBULA_PAD = r"""// choya: controles al estilo de Nebula. CHOYA_FACE_LAYOUT=positions|labels (labels: el firmware nombra A al botón
// de la derecha); CHOYA_QUIT_COMBO=menu|select|both (KNULLI y muOS: MENU + Start; ArkOS: SELECT + Start).
struct ChoyaPadProfile {
    bool labels = false;
    bool quitMenu = true;
    bool quitSelect = false;
};
const ChoyaPadProfile& ChoyaProfile() {
    static const ChoyaPadProfile profile = [] {
        ChoyaPadProfile p;
        const char* face = std::getenv("CHOYA_FACE_LAYOUT");
        p.labels = face != nullptr && std::strcmp(face, "labels") == 0;
        if (const char* quit = std::getenv("CHOYA_QUIT_COMBO")) {
            p.quitMenu = std::strcmp(quit, "select") != 0;
            p.quitSelect = std::strcmp(quit, "select") == 0 || std::strcmp(quit, "both") == 0;
        }
        RT_LOGF(RT_TAG_CONFIG, "[input] perfil: botones por %s, salir con %s + Start\n", p.labels ? "letra" : "posicion",
                p.quitMenu && p.quitSelect ? "MENU o SELECT" : (p.quitSelect ? "SELECT" : "MENU"));
        return p;
    }();
    return profile;
}

// Los botones de un mando (nombres de game controller de SDL) al Wii Remote vertical, como se juega Wii Sports.
void ChoyaApply(const ChoyaMando& m, const ChoyaPadProfile& profile, Intent& intent) {
    // Caras por posición (south abajo, east derecha, west izquierda, north arriba).
    bool south = m.south, east = m.east, west = m.west, north = m.north;
    if (profile.labels) {  // el firmware las nombra por letra: a posiciones
        std::swap(south, east);
        std::swap(west, north);
    }
    uint32_t bits = 0;
    if (east) bits |= kWpadA;
    if (south) bits |= kWpadB;
    if (north) bits |= kWpadOne;
    if (west) bits |= kWpadTwo;
    if (m.start) bits |= kWpadPlus;
    if (m.back || m.leftshoulder || m.lefttrigger > 0.5f) bits |= kWpadMinus;
    // Cruceta del Remote vertical: la cruceta y el stick izquierdo en ocho direcciones.
    if (m.dpup) bits |= kWpadUp;
    if (m.dpdown) bits |= kWpadDown;
    if (m.dpleft) bits |= kWpadLeft;
    if (m.dpright) bits |= kWpadRight;
    const float sx = m.leftx, sy = -m.lefty;  // +y arriba
    constexpr float kStickMin = kStickThreshold / 127.0f;
    if (sx * sx + sy * sy >= kStickMin * kStickMin) {
        if (std::fabs(sx) >= kDiagonalSlope * std::fabs(sy)) bits |= sx < 0 ? kWpadLeft : kWpadRight;
        if (std::fabs(sy) >= kDiagonalSlope * std::fabs(sx)) bits |= sy < 0 ? kWpadDown : kWpadUp;
    }
    intent.buttons |= bits;
    intent.shake = intent.shake || m.rightshoulder;
    // La combinación de salida del firmware: MENU (guide; KNULLI, muOS) o SELECT (ArkOS) + Start cierra el juego
    // limpio, como cerrar la ventana (el gptokeyb del lanzador mata con los mismos botones si esto no contesta).
    const bool quit = m.start && ((profile.quitMenu && m.guide) || (profile.quitSelect && m.back));
    static bool s_quitSent = false;
    if (quit && !s_quitSent) {
        s_quitSent = true;
        RT_LOGF(RT_TAG_CONFIG, "[input] %s + Start: salir\n", m.guide ? "MENU" : "SELECT");
        SDL_Event ev{};
        ev.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&ev);
    }
}

Intent GamepadIntent() {
    Intent intent;
    const ChoyaPadProfile& profile = ChoyaProfile();
    // El mando de la consola: de /dev/input con el mapeo de PortMaster numerado como SDL2 (choya_mando.cpp).
    ChoyaMando m{};
    const bool console = choya_mando_leer(&m);
    if (console) ChoyaApply(m, profile, intent);
    const char* consoleName = choya_mando_nombre();
    // Otros mandos (USB, Bluetooth) por SDL3.
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(ids[i]);
        if (gamepad == nullptr) gamepad = SDL_OpenGamepad(ids[i]);
        if (gamepad == nullptr) continue;
        const char* name = SDL_GetGamepadName(gamepad);
        if (console && name != nullptr && std::strcmp(name, consoleName) == 0) continue;  // ya leído arriba
        {  // una vez por mando: cuál es y con qué mapeo
            static std::vector<SDL_JoystickID> s_seen;
            if (std::find(s_seen.begin(), s_seen.end(), ids[i]) == s_seen.end()) {
                s_seen.push_back(ids[i]);
                char* mapping = SDL_GetGamepadMapping(gamepad);
                RT_LOGF(RT_TAG_CONFIG, "[input] mando \"%s\" por SDL3 (%s)\n", name ? name : "?",
                        mapping != nullptr ? mapping : "sin mapeo");
                if (mapping != nullptr) SDL_free(mapping);
            }
        }
        const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForName(name);
        if (kind != WiiRemoteInput::Kind::NotWii && kind != WiiRemoteInput::Kind::WiiUPro) {
            continue;  // un Wii Remote de verdad: el juego lo lee por KPAD
        }
        const auto button = [gamepad](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad, b); };
        ChoyaMando p{};
        p.south = button(SDL_GAMEPAD_BUTTON_SOUTH);
        p.east = button(SDL_GAMEPAD_BUTTON_EAST);
        p.west = button(SDL_GAMEPAD_BUTTON_WEST);
        p.north = button(SDL_GAMEPAD_BUTTON_NORTH);
        p.back = button(SDL_GAMEPAD_BUTTON_BACK);
        p.guide = button(SDL_GAMEPAD_BUTTON_GUIDE);
        p.start = button(SDL_GAMEPAD_BUTTON_START);
        p.leftshoulder = button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        p.rightshoulder = button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        p.dpup = button(SDL_GAMEPAD_BUTTON_DPAD_UP);
        p.dpdown = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        p.dpleft = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        p.dpright = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        p.leftx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        p.lefty = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        p.lefttrigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
        p.righttrigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
        ChoyaApply(p, profile, intent);
    }
    SDL_free(ids);
    return intent;
}

"""
EDITS = [
    # Botón B en el teclado (S o B) y en el mando (gatillo derecho). Los juegos de WiiCompiled se juegan con
    # el Wii Remote de lado y no lo usan; Wii Sports pide "A y B" desde la pantalla de título.
    # Medición por tramos (choya_perfil.cpp): cada llamada entre funciones del juego pasa por aquí; mientras se mide
    # se cuenta cuántas veces se llama cada una. Sin medir es una lectura y un salto que no se toma.
    ("include/abi_bridge.h", "inline void ApplyRuntimeCallOptions(uint32_t target, CpuContext* ctx) {\n",
     'extern "C" volatile bool g_choyaMidiendo;  // choya: medición por tramos (choya_perfil.cpp)\n'
     'extern "C" void choya_medir_llamada(uint32_t target, const void* ctx);\n'
     'extern "C" uint32_t g_choyaSinEfectos;  // choya: CHOYA_SIN_REVERB (choya_perfil.cpp)\n'
     'extern "C" bool choya_sin_efectos(void* ctx);\n'
     "inline void ApplyRuntimeCallOptions(uint32_t target, CpuContext* ctx) {\n"
     "    if (__builtin_expect(g_choyaMidiendo, 0)) choya_medir_llamada(target, ctx);  // choya\n",
     "medición por tramos (llamadas)"),
    # CHOYA_SIN_REVERB=1: la llamada indirecta a los efectos de audio de nw4r (AuxCallbackFunc) no se hace; los búferes
    # del bus auxiliar quedan en cero (sin reverberación). Sin la variable, la comparación nunca es cierta.
    ("include/abi_bridge.h", "    ApplyRuntimeCallOptions(target, cpu);\n"
     "    if (TryDispatchRawCpuTarget(TranslatedFunctionRegistry::FindRawByAddressPtr(target), cpu)) {\n"
     "        return;\n    }\n    if (TryDispatchResolvedCpuTarget(TranslatedFunctionRegistry::FindByAddressPtr(target), cpu)) {\n",
     "    ApplyRuntimeCallOptions(target, cpu);\n"
     "    if (__builtin_expect(target == g_choyaSinEfectos, 0) && choya_sin_efectos(cpu)) return;  // choya\n"
     "    if (TryDispatchRawCpuTarget(TranslatedFunctionRegistry::FindRawByAddressPtr(target), cpu)) {\n"
     "        return;\n    }\n    if (TryDispatchResolvedCpuTarget(TranslatedFunctionRegistry::FindByAddressPtr(target), cpu)) {\n",
     "sin efectos de audio (CHOYA_SIN_REVERB)"),
    ("include/keybinds.h", "    kMinus,\n    kActionCount,",
     "    kMinus,\n    kButtonB," + _B_NOTE + "\n    kActionCount,", "tecla para el botón B"),
    ("src/keybinds.cpp", '{"minus", "- (Minus)", {SDL_SCANCODE_MINUS, SDL_SCANCODE_TAB}},',
     '{"minus", "- (Minus)", {SDL_SCANCODE_MINUS, SDL_SCANCODE_TAB}},\n'
     '        {"b", "B", {SDL_SCANCODE_S, SDL_SCANCODE_B}},' + _B_NOTE, "tecla para el botón B"),
    ("src/virtual_remote.cpp", "kWpadMinus = 0x1000;",
     "kWpadMinus = 0x1000, kWpadB = 0x0400;" + _B_NOTE, "tecla para el botón B"),
    ("src/virtual_remote.cpp", "    if (Held(kMinus)) intent.buttons |= kWpadMinus;\n",
     "    if (Held(kMinus)) intent.buttons |= kWpadMinus;\n"
     "    if (Held(kButtonB)) intent.buttons |= kWpadB;" + _B_NOTE + "\n", "tecla para el botón B"),
    ("src/virtual_remote.cpp", "        if (held(kMinus)) intent.buttons |= kWpadMinus;\n",
     "        if (held(kMinus)) intent.buttons |= kWpadMinus;\n"
     "        if (held(kButtonB)) intent.buttons |= kWpadB;" + _B_NOTE + "\n", "botón B en el mando"),
    ("src/virtual_remote.cpp",
     '    {"back,leftshoulder,lefttrigger", "back,touchpad,leftshoulder,lefttrigger", "back,leftshoulder,lefttrigger"},\n};',
     '    {"back,leftshoulder,lefttrigger", "back,touchpad,leftshoulder,lefttrigger", "back,leftshoulder,lefttrigger"},\n'
     '    {"a", "a", "a"},' + _B_NOTE + '\n};', "botón B en el mando"),
    # Wii Sports se juega con el Wii Remote: el Remote 1 lo hacen el teclado y los mandos. BrawlRecompiled los deja
    # solo como mandos de GameCube (Present() siempre falso) y entonces ningún botón llegaba al juego.
    ("src/virtual_remote.cpp", "    (void)chan;\n    return false;\n}",
     "    return chan == 0;  // choya: Wii Sports solo lee el Wii Remote; teclado y mandos hacen de Remote 1\n}",
     "mandos como Wii Remote 1"),
    # Controles al estilo de Nebula (ver _NEBULA_PAD): reemplazan el mapeo por texto de BrawlRecompiled.
    ("src/virtual_remote.cpp", "#include <SDL3/SDL_timer.h>\n",
     "#include <SDL3/SDL_timer.h>\n#include <SDL3/SDL_events.h>  // choya: salir con MENU + Start\n#include <cstring>\n"
     "#include \"choya_mando.h\"  // choya: el mando de la consola como en Nebula\n",
     "controles de Nebula"),
    ("src/virtual_remote.cpp", "Intent GamepadIntent() {\n",
     _NEBULA_PAD + "[[maybe_unused]] Intent GamepadIntentBrawl() {  // choya: el de BrawlRecompiled, ya sin uso\n",
     "controles de Nebula"),
    ("src/virtual_remote.cpp", '"plus", "minus",\n};', '"plus", "minus", "b",' + _B_NOTE + '\n};',
     "botón B en el mando"),
    # Wii Sports registra su función de apagado en cada cuadro: anotarlo solo cuando cambia.
    ("src/hle/os/os_interrupt.cpp",
     '    RT_LOG(RT_TAG_OS) << "OSSetPowerCallback_801ab75c called: newCB=0x"\n'
     '              << std::hex << newCallback << std::dec << std::endl;',
     '    static uint32_t choyaLastPowerCallback = 0xFFFFFFFFu;  // choya: Wii Sports la llama en cada cuadro\n'
     '    if (newCallback != choyaLastPowerCallback) {\n'
     '        choyaLastPowerCallback = newCallback;\n'
     '        RT_LOG(RT_TAG_OS) << "OSSetPowerCallback_801ab75c called: newCB=0x"\n'
     '                  << std::hex << newCallback << std::dec << std::endl;\n'
     '    }', "log de OSSetPowerCallback solo al cambiar"),
    # Pantalla de inicio: dice qué juego es y conserva el crédito del runtime.
    ("src/settings_overlay.cpp", 'constexpr const char* kTitle = "Made by JosiahG";',
     'constexpr const char* kTitle = "Wii Sports - Choya Wii Converter";  // choya', "pantalla de inicio"),
    ("src/settings_overlay.cpp", '"A Super Smash Bros. Brawl PC recomp. Built on WiiCompiled\'s code.";',
     '"Runtime: BrawlRecompiled by JosiahG, built on WiiCompiled. Press A.";  // choya', "pantalla de inicio"),
    ("src/settings_overlay.cpp", 'Outlined("SUPER SMASH BROS. BRAWL",', 'Outlined("WII SPORTS",', "pantalla de inicio"),
    ("src/settings_overlay.cpp", 'Outlined("Made by JosiahG",', 'Outlined("Choya Wii Converter",',
     "pantalla de inicio"),
    # Modo de gráficos "null" en Linux: el juego corre completo sin dibujar. Sirve para medir en la consola
    # cuánto rinde el procesador antes de tener gráficos (el driver de la Mali solo da Vulkan 1.0).
    ("src/main.cpp",
     'std::array<GraphicsBackendEntry, 2> kGraphicsBackends{{\n'
     '            {"auto", BACKEND_AUTO}, {"vulkan", BACKEND_VULKAN},\n        }};',
     'std::array<GraphicsBackendEntry, 3> kGraphicsBackends{{\n'
     '            {"auto", BACKEND_AUTO}, {"vulkan", BACKEND_VULKAN}, {"null", BACKEND_NULL},  // choya\n        }};',
     "gráficos null en Linux"),
    ("include/runtime_config.h",
     'static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};',
     'static constexpr std::array<std::string_view, 3> values{"auto", "vulkan", "null"};  // choya',
     "gráficos null en Linux"),
    # Compilado en la PC para la consola: -mcpu=native describiría la PC. La RG35XX H es Cortex-A53 (ARMv8.0).
    # Motor de Choya, paso 1: después de cada cuadro de aurora, la pantalla GLES propia presenta el suyo
    # (choya_gles.cpp; solo hace algo con CHOYA_GLES=1).
    ("src/hle/vi.cpp", '#include "guest_code_patch.h"\n',
     '#include "guest_code_patch.h"\nextern "C" void choya_gles_frame();  // choya: pantalla GLES propia\n',
     "pantalla GLES de Choya (declaración)"),
    ("src/hle/vi.cpp", "    aurora_end_frame();\n    const Clock::time_point paceStart = Clock::now();\n",
     "    aurora_end_frame();\n    choya_gles_frame();  // choya: pantalla GLES propia\n"
     "    const Clock::time_point paceStart = Clock::now();\n",
     "pantalla GLES de Choya (cada cuadro)"),
    # Clang no tiene __builtin_setjmp/__builtin_longjmp en aarch64. Solo los usan los save states: en ARM el cambio de
    # hilo va directo y los save states quedan apagados. En la PC (x86-64) todo sigue igual.
    ("src/save_states.cpp", "    __builtin_longjmp(slot.jmp, 1);\n",
     "#if defined(__x86_64__)\n    __builtin_longjmp(slot.jmp, 1);\n#else\n    std::abort();  // choya: sin save states en ARM\n#endif\n",
     "save states solo en x86-64 (salto)"),
    ("src/save_states.cpp", "    void** jmp = g_fibers[HostContext::Current()].jmp;\n    if (__builtin_setjmp(jmp) == 0) {",
     "#if !defined(__x86_64__)\n    HostContext::Switch(target);  // choya: ARM, cambio de hilo directo\n#else\n"
     "    void** jmp = g_fibers[HostContext::Current()].jmp;\n    if (__builtin_setjmp(jmp) == 0) {",
     "save states solo en x86-64 (cambio de hilo)"),
    ("src/save_states.cpp", "        SwitchBelow(target);\n    }\n}",
     "        SwitchBelow(target);\n    }\n#endif\n}", "save states solo en x86-64 (cierre)"),
    ("src/save_states.cpp", "bool Available() { return !DetClock::Enabled(); }",
     "bool Available() {\n#if defined(__x86_64__)\n    return !DetClock::Enabled();\n#else\n    return false;  // choya: sin save states en ARM\n#endif\n}",
     "save states solo en x86-64 (menú)"),
    ("cmake/PublicProducts.cmake", "    set(MKW_BASELINE_ARCH_FLAG -mcpu=native)",
     "    set(MKW_BASELINE_ARCH_FLAG -mcpu=cortex-a53)  # choya: RG35XX H (Allwinner H700, Cortex-A53)",
     "procesador de la consola"),
]


# Cambios en aurora-main (fuera del runtime): se aplican en su lugar y solo una vez (llevan "choya:").
# Sin X11 ni Wayland en KNULLI, SDL abre la ventana "offscreen", que Dawn no sabe usar. Para el backend Null basta una
# superficie Wayland ficticia: Dawn solo revisa que los punteros no sean nulos y el backend Null nunca los toca.
# En la PC (x11/wayland) este camino no se usa.
AURORA_EDITS = [
    ("lib/dawn/BackendBinding.cpp",
     "    desc->window = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);\n"
     "    return std::move(desc);\n  }\n",
     "    desc->window = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);\n"
     "    return std::move(desc);\n  }\n"
     "  // choya: ventana offscreen (KNULLI) + backend Null: superficie Wayland ficticia que Dawn Null no usa.\n"
     "  if (SDL_strcmp(driver, \"offscreen\") == 0 &&\n"
     "      (SDL_GetWindowFlags(window) & (SDL_WINDOW_VULKAN | SDL_WINDOW_OPENGL)) == 0) {\n"
     "    static char choyaDisplay, choyaSurface;\n"
     "    std::shared_ptr<wgpu::SurfaceSourceWaylandSurface> desc = std::make_shared<wgpu::SurfaceSourceWaylandSurface>();\n"
     "    desc->display = &choyaDisplay;\n"
     "    desc->surface = &choyaSurface;\n"
     "    return std::move(desc);\n"
     "  }\n",
     "superficie ficticia para Dawn Null (ventana offscreen)"),
    # La superficie del backend Null solo admite RenderAttachment: con CopySrc la configuración falla y aurora
    # intenta "recuperar" la superficie en cada cuadro.
    ("lib/webgpu/gpu.cpp",
     "              .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,\n",
     "              .usage = g_backendType == wgpu::BackendType::Null  // choya: Null solo admite RenderAttachment\n"
     "                           ? wgpu::TextureUsage::RenderAttachment\n"
     "                           : wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,\n",
     "superficie Null sin CopySrc"),
    # Con el backend Null, ImGui (portada, menús) se dibuja con el renderizador por software de SDL en la ventana
    # offscreen que nadie ve: en la consola eso se comía ~100 ms por cuadro (SDL_BlitTriangle_Slow).
    ("lib/imgui.cpp",
     "    SDL_Renderer* renderer = window::get_sdl_renderer();\n    SDL_RenderClear(renderer);\n",
     "    // choya: ventana offscreen (KNULLI): nadie la ve, no se rasteriza ImGui por software.\n"
     "    static const bool choyaOffscreen =\n"
     "        SDL_strcmp(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : \"\", \"offscreen\") == 0;\n"
     "    if (choyaOffscreen) {\n"
     "      return;\n"
     "    }\n"
     "    SDL_Renderer* renderer = window::get_sdl_renderer();\n    SDL_RenderClear(renderer);\n",
     "ImGui sin dibujar en la ventana offscreen"),
    # Motor de Choya, paso 2a: el cuadro que arma aurora se copia al motor GLES de Choya (lib/choya/choya_gx.cpp).
    ("cmake/aurora_gx.cmake", "        lib/gfx/common.cpp\n",
     "        lib/gfx/common.cpp\n        lib/choya/choya_gx.cpp\n        lib/choya/choya_gx_core.cpp\n",
     "motor de Choya en aurora_gx"),
    ("lib/gfx/common.cpp", '#include "../window.hpp"\n',
     '#include "../window.hpp"\n#include "../choya/choya_gx.hpp"  // choya: motor GLES de Choya\n',
     "motor de Choya (cabecera)"),
    ("lib/gfx/common.cpp",
     "PipelineRef pipeline_ref(const gx::PipelineConfig& config) {\n"
     "  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });\n}\n",
     "PipelineRef pipeline_ref(const gx::PipelineConfig& config) {\n"
     "  const PipelineRef ref = find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });\n"
     "  choya::register_gx_pipeline(ref, config);  // choya\n"
     "  return ref;\n}\n",
     "motor de Choya (pipelines GX)"),
    ("lib/gfx/common.cpp",
     "PipelineRef pipeline_ref(const clear::PipelineConfig& config) {\n"
     "  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });\n}\n",
     "PipelineRef pipeline_ref(const clear::PipelineConfig& config) {\n"
     "  const PipelineRef ref = find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });\n"
     "  choya::register_clear_pipeline(ref, config);  // choya\n"
     "  return ref;\n}\n",
     "motor de Choya (pipelines de borrado)"),
    ("lib/gfx/common.cpp",
     "  g_uniforms.append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer\n",
     "  g_uniforms.append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer\n" + """\
  // choya: copia del cuadro (vértices, uniformes, índices, arreglos y pases) para el motor GLES de Choya.
  // Los lotes partidos a mitad de cuadro (copias del EFB a RAM) todavía no se copian.
  if (choya::enabled()) {
    if (true) {  // choya: también los lotes partidos (staging lleno), que se dibujan sin mostrarse
      choya::capture_begin(g_verts.data(), g_verts.size(), g_uniforms.data(), g_uniforms.size(), g_indices.data(),
                           g_indices.size(), g_storage.data(), g_storage.size());
      for (const auto& pass : g_renderPasses) {
        const float rgba[4] = {pass.clearColorValue.x(), pass.clearColorValue.y(), pass.clearColorValue.z(),
                               pass.clearColorValue.w()};
        // La copia a pantalla (GXCopyDisp) termina el pase cuyo destino es la textura de la copia de pantalla.
        const bool displayCopy = pass.resolveTarget && pass.resolveTarget == gx::g_gxState.displayCopyTexture;
        choya::capture_pass(pass.targetSize.width, pass.targetSize.height, pass.clearColor, pass.clearDepth, rgba,
                            pass.clearDepthValue, displayCopy, pass.resolveRect);
        for (const auto& command : pass.commands) {
          switch (command.type) {
          case CommandType::SetViewport:
            choya::capture_viewport(command.data.setViewport);
            break;
          case CommandType::SetScissor:
            choya::capture_scissor(command.data.setScissor);
            break;
          case CommandType::Draw:
            if (command.data.draw.type == ShaderType::GX) {
              choya::capture_gx_draw(command.data.draw.gx);
            } else {
              choya::capture_clear(command.data.draw.clear);
            }
            break;
          default:
            break;
          }
        }
        // Copia del EFB a una textura (GXCopyTex): el motor la hace al terminar el pase.
        if (pass.resolveTarget && !displayCopy) {
          choya::capture_copy(pass.resolveTarget.get(), pass.resolveRect, pass.resolveFormat,
                              pass.resolveForceOpaqueAlpha);
        }
      }
      choya::capture_end(advanceFrame);
    } else {
      choya::capture_discard();
    }
  }
""",
     "motor de Choya (copia del cuadro)"),
    # Paso 2b-1: texturas. Los datos ya decodificados (RGBA8) van al motor al crearse o escribirse, el motor sabe
    # cuándo se liberan, y gx avisa qué texturas y muestreo tiene cada grupo de texturas.
    ("lib/gfx/texture.hpp", "  , gxFormat(gxFormat) {}\n};\n",
     "  , gxFormat(gxFormat) {}\n  ~TextureRef();  // choya: avisa al motor de Choya (lib/choya/choya_gx.cpp)\n};\n",
     "motor de Choya (texturas liberadas)"),
    ("lib/gfx/texture.cpp", '#include "texture_convert.hpp"\n',
     '#include "texture_convert.hpp"\n#include "../choya/choya_gx.hpp"  // choya: motor GLES de Choya\n',
     "motor de Choya (cabecera en texturas)"),
    ("lib/gfx/texture.cpp",
     "      ref.hasArbitraryMips = converted.hasArbitraryMips;\n    }\n  }\n\n  uint32_t offset = 0;\n",
     "      ref.hasArbitraryMips = converted.hasArbitraryMips;\n    }\n  }\n"
     "  choya::texture_uploaded(ref, data.data(), data.size());  // choya\n\n  uint32_t offset = 0;\n",
     "motor de Choya (texturas nuevas)"),
    ("lib/gfx/texture.cpp",
     "    if (!converted.data.empty()) {\n      data = converted.data;\n    }\n  }\n\n  uint32_t offset = 0;\n",
     "    if (!converted.data.empty()) {\n      data = converted.data;\n    }\n  }\n"
     "  choya::texture_uploaded(ref, data.data(), data.size());  // choya\n\n  uint32_t offset = 0;\n",
     "motor de Choya (texturas reescritas)"),
    ("lib/gx/gx.cpp", '#include "pipeline.hpp"\n',
     '#include "pipeline.hpp"\n#include "../choya/choya_gx.hpp"  // choya: motor GLES de Choya\n',
     "motor de Choya (cabecera en gx)"),
    ("lib/gx/gx.cpp", "  const auto ref = gfx::bind_group_ref(textureBindGroupDescriptor);\n",
     "  const auto ref = gfx::bind_group_ref(textureBindGroupDescriptor);\n"
     "  choya::register_texture_bind_group(ref, info);  // choya\n",
     "motor de Choya (grupos de texturas)"),
    # Recursos convertidos: cada textura estática del juego se busca primero en assets.wlpk (CHOYA_PAQUETE); si está,
    # aurora no la decodifica y el motor de Choya sube la versión convertida (ETC2/EAC, reducida).
    ("lib/gx/gx.cpp",
     "    handle = gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), obj.format(),\n"
     "                                        {static_cast<const uint8_t*>(obj.data), dataSize}, false, nameStr);\n",
     "    handle = choya::packed_static_texture(obj.width(), obj.height(), obj.mip_count(), obj.format(),\n"
     "                                          static_cast<const uint8_t*>(obj.data), dataSize, nameStr);  // choya\n"
     "    if (!handle) {\n"
     "      handle = gfx::new_static_texture_2d(obj.width(), obj.height(), obj.mip_count(), obj.format(),\n"
     "                                          {static_cast<const uint8_t*>(obj.data), dataSize}, false, nameStr);\n"
     "    }\n",
     "motor de Choya (recursos convertidos)"),
]

# Archivos del motor de Choya que se copian dentro de aurora (siempre se reemplazan: son de Choya).
AURORA_FILES = [("choya_gx.hpp", "lib/choya/choya_gx.hpp"), ("choya_gx.cpp", "lib/choya/choya_gx.cpp"),
                ("choya_gx_core.hpp", "lib/choya/choya_gx_core.hpp"), ("choya_gx_core.cpp", "lib/choya/choya_gx_core.cpp")]


def _capture_payloads() -> list[str]:
    """Bloques de copia del cuadro que han insertado las versiones de Choya en common.cpp (actual y anteriores)."""
    edit = next(e for e in AURORA_EDITS if e[3] == "motor de Choya (copia del cuadro)")
    current = edit[2][len(edit[1]):]
    v0150 = current.replace(
        "    if (true) {  // choya: también los lotes partidos (staging lleno), que se dibujan sin mostrarse\n",
        "    if (advanceFrame) {\n").replace("      choya::capture_end(advanceFrame);\n", "      choya::capture_end();\n")
    v0123 = v0150.replace(
        "        // Copia del EFB a una textura (GXCopyTex): el motor la hace al terminar el pase.\n"
        "        if (pass.resolveTarget && !displayCopy) {\n"
        "          choya::capture_copy(pass.resolveTarget.get(), pass.resolveRect, pass.resolveFormat,\n"
        "                              pass.resolveForceOpaqueAlpha);\n"
        "        }\n", "")
    v0122 = v0123.replace("pass.clearDepthValue, displayCopy, pass.resolveRect);",
                            "pass.clearDepthValue, displayCopy);")
    v0120 = v0122.replace(
        "        // La copia a pantalla (GXCopyDisp) termina el pase cuyo destino es la textura de la copia de pantalla.\n"
        "        const bool displayCopy = pass.resolveTarget && pass.resolveTarget == gx::g_gxState.displayCopyTexture;\n"
        "        choya::capture_pass(pass.targetSize.width, pass.targetSize.height, pass.clearColor, pass.clearDepth, rgba,\n"
        "                            pass.clearDepthValue, displayCopy);\n",
        "        choya::capture_pass(pass.targetSize.width, pass.targetSize.height, pass.clearColor, pass.clearDepth, rgba,\n"
        "                            pass.clearDepthValue);\n")
    assert len({v0120, v0122, v0123, v0150, current}) == 5
    return [current, v0150, v0123, v0122, v0120]


# Lo que versiones anteriores de Choya insertaron en aurora: se quita antes de aplicar los cambios actuales, para
# que actualizar Choya no deje bloques repetidos o viejos (0.12.2 dejaba el bloque de la 0.12.0 junto al nuevo).
AURORA_STRIPS = [("lib/gfx/common.cpp", s) for s in _capture_payloads()] + [
    ("cmake/aurora_gx.cmake", "        lib/choya/choya_gx_core.cpp\n"),
    ("cmake/aurora_gx.cmake", "        lib/choya/choya_gx.cpp\n"),
]


def patch_aurora(wiicompiled: str, changes: list[Change]) -> None:
    base = os.path.join(wiicompiled, "aurora-main")
    if not os.path.isdir(base):  # p. ej. un runtime ya compilado como paquete
        return
    data = os.path.join(os.path.dirname(__file__), "data", "aurora")
    for src, rel in AURORA_FILES:
        dst = os.path.join(base, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(os.path.join(data, src), "rb") as f:
            content = f.read()
        old_content = None
        if os.path.isfile(dst):
            with open(dst, "rb") as f:
                old_content = f.read()
        if content != old_content:  # sin reescribir si no cambió: no fuerza recompilar
            with open(dst, "wb") as f:
                f.write(content)
        changes.append(Change("aurora-main/" + rel, 0, "edicion", 0, None, "motor de Choya", "motor de Choya (archivo)"))
    files: dict[str, list] = {}
    for edit in AURORA_EDITS:
        files.setdefault(edit[0], []).append(edit)
    for rel, edits in files.items():
        path = os.path.join(base, rel)
        shown = "aurora-main/" + rel
        if not os.path.isfile(path):
            for _r, _o, _n, label in edits:
                changes.append(Change(shown, 0, "parche_fallido", 0, None, label, "no encontré el archivo"))
            continue
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            original = f.read()
        text = original
        for strip_rel, payload in AURORA_STRIPS:
            if strip_rel == rel:
                text = text.replace(payload, "")
        for _r, old, new, label in edits:
            if new in text:
                changes.append(Change(shown, _line_of(text, text.index(new)), "edicion", 0, None, label,
                                      label + " (ya estaba)"))
                continue
            if text.count(old) != 1:
                changes.append(Change(shown, 0, "parche_fallido", 0, None, label, "texto no encontrado (¿cambió aurora?)"))
                continue
            line = _line_of(text, text.index(old))
            text = text.replace(old, new, 1)
            changes.append(Change(shown, line, "edicion", 0, None, label, label))
        if text != original:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(text)


def apply_edits(text: str, rel: str, changes: list[Change]) -> str:
    for file, old, new, label in EDITS:
        if file != rel:
            continue
        n = text.count(old)
        if n != 1:
            changes.append(Change(rel, 0, "parche_fallido", 0, None, label,
                                  "texto no encontrado (¿cambió WiiCompiled?)" if n == 0 else f"aparece {n} veces"))
            continue
        line = _line_of(text, text.index(old))
        text = text.replace(old, new, 1)
        changes.append(Change(rel, line, "edicion", 0, None, label, label))
    return text


def apply_patches(text: str, rel: str, ws: Symbols, game: GameInfo, changes: list[Change]) -> str:
    for p in PATCHES:
        if p.file != rel:
            continue
        rx = re.compile(p.pattern)
        found = list(rx.finditer(text))
        value = p.value(ws, game)
        if not found or value is None:
            if p.optional:
                changes.append(Change(rel, 0, "parche_omitido", 0, None, p.label, "no hace falta en Wii Sports"))
                continue
            why = ("patrón no encontrado (¿cambió WiiCompiled?)" if not found else
                   "no encontré el dato en tu disco" if game.dol is not None else "falta correr recomp-prep")
            changes.append(Change(rel, 0, "parche_fallido", 0, None, p.label, why))
            continue
        for m in reversed(found):
            old = m.group("v")
            new = f"{value:08X}" if any(c in "ABCDEF" for c in old) or old.isdigit() else f"{value:08x}"
            line = _line_of(text, m.start())
            eol = text.find("\n", m.end())
            eol = len(text) if eol < 0 else eol
            tail = text[m.end("v"):eol]
            marker = "" if "choya:" in tail else ("  // choya: " if "//" not in tail else " · choya: ") + p.label
            text = text[:m.start("v")] + new + tail + marker + text[eol:]
            changes.append(Change(rel, line, "parche", int(old, 16), value, p.label, p.label))
    return text



def retarget(wiicompiled: str, simbolos: str | None = None, rehacer: bool = False) -> RetargetResult:
    src_runtime = os.path.join(wiicompiled, "runtime")
    if not os.path.isdir(os.path.join(src_runtime, "src")):
        raise FileNotFoundError(f"no encontré runtime/src dentro de {wiicompiled}")
    out = os.path.join(wiicompiled, OUT_DIR)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)) and not rehacer:
            raise FileExistsError(f"{out} existe y no lo creó Choya; usa --rehacer para reemplazarlo")
        shutil.rmtree(out)
    shutil.copytree(src_runtime, out, ignore=shutil.ignore_patterns("build*", ".git"))
    with open(os.path.join(out, MARKER), "w", encoding="utf-8") as f:
        f.write("Generado por choya recomp-native a partir de runtime/. Se borra y se rehace cada vez.\n")

    maps = load_game_maps(wiicompiled)
    res = _Resolver(load_wiisports_symbols(simbolos), maps)
    games = detect_source_games(src_runtime, maps)
    result = RetargetResult(out, ", ".join(games[:3]) or None)
    game = read_game_info(wiicompiled)
    res.sda = {"r13": game.sda, "r2": game.sda2}
    result.game = game
    result.structs = struct_checks(res.ws)
    patch_aurora(wiicompiled, result.changes)
    # Cambios de código fuera de src/ e include/ (CMake).
    for rel in sorted({e[0] for e in EDITS if not e[0].startswith(("src/", "include/"))}):
        path = os.path.join(out, rel)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
                text = f.read()
            new = apply_edits(text, rel, result.changes)
            if new != text:
                with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(new)
    # Primero se reactivan los registros apagados solo porque otro juego no tenía la función.
    for sub in ("src", "include"):
        for path in source_files(os.path.join(out, sub)):
            rel = os.path.relpath(path, out).replace("\\", "/")
            with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
                text = f.read()
            new = reactivate(text, rel, res, result.changes)
            if new != text:
                with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(new)
    _collect_moves(out, res)
    registered: dict[int, str] = {}
    for sub in ("src", "include"):
        for path in source_files(os.path.join(out, sub)):
            rel = os.path.relpath(path, out).replace("\\", "/")
            with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
                text = f.read()
            new = apply_edits(text, rel, result.changes)
            new = apply_patches(new, rel, res.ws, game, result.changes)
            new = retarget_text(new, rel, res, registered, result.changes)
            if new != text:
                with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(new)
    with open(os.path.join(out, MISSING_FILE), "w", encoding="utf-8", newline="\n") as f:
        f.write(missing_stubs(sorted(res.missing)))
    with open(os.path.join(out, HLE_FILE), "w", encoding="utf-8", newline="\n") as f:
        f.write(choya_natives(res.ws, registered, result.changes))
    with open(os.path.join(os.path.dirname(__file__), "data", "choya_perfil.cpp"), encoding="utf-8") as f:
        perfil_src = f.read()
    with open(os.path.join(out, PERFIL_FILE), "w", encoding="utf-8", newline="\n") as f:
        f.write(perfil_marcas(perfil_src, res.ws))
    shutil.copyfile(os.path.join(os.path.dirname(__file__), "data", "choya_gles.cpp"), os.path.join(out, GLES_FILE))
    for name in ("choya_mando.cpp", "choya_mando.h"):  # el mando de la consola como en Nebula
        shutil.copyfile(os.path.join(os.path.dirname(__file__), "data", name), os.path.join(out, "src", name))
    return result


# Las funciones de Wii Sports que separan los tramos del cuadro en la medición (choya_perfil.cpp).
TRAMO_CALC = "calc__Q23EGG12SceneManagerFv"   # actualizar el juego (escenas, actores, físicas, animaciones)
TRAMO_DRAW = "draw__Q23EGG12SceneManagerFv"   # preparar el dibujo (recorrer modelos y llamar a GX)
AUX_CALLBACK = "AuxCallbackFunc__Q44nw4r3snd6detail9AxManagerFPvPv"  # efectos de audio (CHOYA_SIN_REVERB)


def perfil_marcas(src: str, ws: Symbols) -> str:
    """Escribe en choya_perfil.cpp las direcciones y tamaños de las funciones que marcan los tramos."""
    for name, addr_tag, size_tag in ((TRAMO_CALC, "CHOYA_DIR_CALC", "CHOYA_TAM_CALC"),
                                     (TRAMO_DRAW, "CHOYA_DIR_DRAW", "CHOYA_TAM_DRAW")):
        addr = ws.funcs.get(name, 0)
        size = ws.size.get(addr, 0) if addr else 0
        src = src.replace(f"0x0u /*{addr_tag}*/", f"0x{addr:08X}u /*{addr_tag}*/")
        src = src.replace(f"0x0u /*{size_tag}*/", f"0x{size:X}u /*{size_tag}*/")
    aux = ws.funcs.get(AUX_CALLBACK, 0)
    return src.replace("0x0u /*CHOYA_DIR_AUX*/", f"0x{aux:08X}u /*CHOYA_DIR_AUX*/")


def choya_natives(ws: Symbols, registered: dict[int, str], changes: list[Change]) -> str:
    """Archivo con las funciones nativas propias de Choya, en las direcciones de Wii Sports."""
    parts = ["// Generado por choya recomp-native: funciones nativas para Wii Sports que WiiCompiled no trae.",
             '#include "hle_stubs.h"', "", "#include <cstdint>", ""]
    for name, body in CHOYA_NATIVES:
        addr = ws.get(name)
        if addr is None or addr in registered:
            continue                     # no existe, o WiiCompiled ya la reemplaza
        parts.append(f"// {name} (Wii Sports 0x{addr:08X})")
        parts.append(body.format(addr=f"{addr:08X}"))
        registered[addr] = HLE_FILE
        changes.append(Change(HLE_FILE, 0, "choya_nativa", 0, addr, name, name))
    return "\n".join(parts) + "\n"


def missing_stubs(addresses: list[int]) -> str:
    """Definiciones para las funciones de otros juegos que el runtime nombra pero Wii Sports no tiene.
    Solo las usan funciones nativas desactivadas; si alguna se llamara, el programa se detiene diciendo cuál."""
    lines = ["// Generado por choya recomp-native. No editar: se rehace cada vez.",
             "// Funciones de otros juegos (Brawl, NSMBW, MKW, SPM) que Wii Sports no tiene.",
             "#include <cstdio>", "#include <cstdlib>", "", "struct CpuContext;", "",
             "[[noreturn]] static void choya_ausente(unsigned address) {",
             "    std::fprintf(stderr, \"[choya] se llamo a func_%08X, que es de otro juego y Wii Sports no tiene\\n\", address);",
             "    std::abort();", "}", "", 'extern "C" {']
    for a in addresses:
        lines.append(f"void {MISSING_PREFIX}{a:08X}(CpuContext*) {{ choya_ausente(0x{a:08X}u); }}")
    lines += ["}", ""]
    return "\n".join(lines)


def update_manifest(yml_path: str) -> bool:
    """Apunta runtime.native_registration_root del recomp.yml a runtime-wiisports/src."""
    with open(yml_path, encoding="utf-8") as f:
        text = f.read()
    block = ("runtime:\n"
             "  # Runtime de WiiCompiled movido a Wii Sports por choya recomp-native.\n"
             f"  native_registration_root: {OUT_DIR}/src\n"
             "  native_abi_directories:\n"
             f"    - {OUT_DIR}/src/hle/gx\n\n")
    new, n = re.subn(r"runtime:\n(?:[ \t]+.*\n|[ \t]*\n)*", block, text, count=1)
    # La compilación de WiiCompiled busca lo generado en ../generated respecto del runtime.
    new = re.sub(r"(?m)^(output:\n(?:[ \t]*#.*\n)*[ \t]+root:)[ \t]*\S+", r"\1 generated", new, count=1)
    new = re.sub(r"(?m)^([ \t]+base_manifest:)[ \t]*base/", r"\1 ../build/base/", new, count=1)
    if n and new != text:
        with open(yml_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new)
    return bool(n)


def _hx(v) -> str:
    return "—" if v is None else f"{v:08X}"


def report_markdown(r: RetargetResult) -> str:
    by = defaultdict(list)
    for c in r.changes:
        by[c.kind].append(c)
    L = ["# Runtime de WiiCompiled movido a Wii Sports", "",
         f"Juegos de origen de las direcciones: **{r.source_game or 'desconocido'}**", "",
         "| Qué | Cantidad |", "|---|---:|",
         f"| Funciones nativas movidas | {r.count('registro')} |",
         f"| Funciones nativas desactivadas (Wii Sports no las tiene) | {r.count('desactivado')} |",
         f"| Funciones nativas reactivadas (otro juego no las tenía, Wii Sports sí) | {r.count('reactivado')} |",
         f"| Registros repetidos desactivados | {r.count('duplicado')} |",
         f"| Llamadas a funciones movidas | {r.count('funcion')} |",
         f"| Variables del SDK movidas | {r.count('dato')} |",
         f"| Variables por desfase SDA (r13/r2) movidas | {r.count('sda')} |",
         f"| Funciones deducidas por sus vecinas | {r.count('vecinos')} |",
         f"| Parches revisados a mano aplicados | {r.count('parche')} |",
         f"| Cambios de código para Wii Sports (p. ej. botón B) | {r.count('edicion')} |",
         f"| **Parches que no se pudieron aplicar** | {r.count('parche_fallido')} |",
         f"| Funciones nativas propias de Choya ({HLE_FILE}) | {r.count('choya_nativa')} |",
         f"| Funciones de otros juegos sin equivalente (stub en {MISSING_FILE}) | {r.count('ausente')} |",
         f"| **Direcciones sin resolver (revisar a mano)** | {r.count('sin_resolver')} |", ""]
    g = r.game
    if g is not None:
        def h(v):
            return "—" if v is None else f"0x{v:08X}"
        code = "—"
        if g.game_code is not None:
            code = g.game_code.to_bytes(4, "big").decode("ascii", "replace")
        L += ["## Datos de tu disco", "", "| Dato | Valor |", "|---|---|",
              f"| Juego | {code} (revisión {'—' if g.maker_word is None else g.maker_word & 0xFF}) |",
              f"| Entrada (__start) | {h(g.entry)} |", f"| SDA r13 / r2 | {h(g.sda)} / {h(g.sda2)} |",
              f"| Pila de arranque | {h(g.stack)} |", f"| MEM1 arena | {h(g.arena_lo)} – {h(g.arena_hi)} |",
              f"| Tamaño de la FST | {'—' if g.fst_size is None else hex(g.fst_size)} |", ""]
    if r.structs:
        L += ["## Estructuras del SDK de 2006", "", "| Objeto | El runtime espera | Wii Sports | |", "|---|---|---|---|"]
        for name, want, got, what in r.structs:
            ok = "✓" if got == want else "✗ revisar"
            L.append(f"| {name} ({what}) | 0x{want:X} | {'—' if got is None else f'0x{got:X}'} | {ok} |")
        L.append("")
    if by["parche_fallido"]:
        L += ["## Parches no aplicados", "", "| Archivo | Parche | Motivo |", "|---|---|---|"]
        L += [f"| {c.file} | {c.name} | {c.text} |" for c in by["parche_fallido"]]
        L.append("")
    if by["ausente"]:
        L += ["## Funciones de otros juegos sin equivalente", "",
              "Se renombraron a `choya_ausente_XXXXXXXX` para que no se enlacen con otra función de Wii Sports.", "",
              "| Archivo:línea | Dirección | En otros juegos | Código |", "|---|---|---|---|"]
        for c in sorted(by["ausente"], key=lambda c: (c.file, c.line)):
            L.append(f"| {c.file}:{c.line} | {_hx(c.old)} | {c.name} | `{c.text.replace('|', '¦')}` |")
        L.append("")
    files = Counter(c.file for c in by["sin_resolver"])
    if files:
        L += ["## Sin resolver por archivo", "", "| Archivo | Direcciones |", "|---|---:|"]
        L += [f"| {f} | {n} |" for f, n in files.most_common()]
        L += ["", "## Sin resolver", "", "| Archivo:línea | Dirección | En otros juegos | Código |", "|---|---|---|---|"]
        for c in sorted(by["sin_resolver"], key=lambda c: (c.file, c.line)):
            L.append(f"| {c.file}:{c.line} | {_hx(c.old)} | {c.name} | `{c.text.replace('|', '¦')}` |")
    for kind, title in (("reactivado", "Funciones nativas reactivadas"), ("parche", "Parches revisados a mano"), ("sda", "Desfases SDA (nuevo desfase en Wii Sports)"),
                        ("vecinos", "Funciones deducidas por sus vecinas"), ("dato", "Variables movidas"),
                        ("funcion", "Llamadas movidas"), ("registro", "Funciones nativas movidas")):
        if by[kind]:
            L += ["", f"## {title}", "", "| Nombre | Antes | Wii Sports | Archivo:línea |", "|---|---|---|---|"]
            for c in sorted(by[kind], key=lambda c: (c.file, c.line)):
                L.append(f"| {c.name} | {_hx(c.old)} | {_hx(c.new)} | {c.file}:{c.line} |")
    if by["desactivado"] or by["duplicado"]:
        L += ["", "## Desactivadas", "", "| Archivo:línea | Antes | Símbolo | Motivo |", "|---|---|---|---|"]
        for c in sorted(by["desactivado"] + by["duplicado"], key=lambda c: (c.file, c.line)):
            why = "repetido" if c.kind == "duplicado" else "no existe en Wii Sports"
            L.append(f"| {c.file}:{c.line} | {_hx(c.old)} | {c.name} | {why} |")
    return "\n".join(L) + "\n"
