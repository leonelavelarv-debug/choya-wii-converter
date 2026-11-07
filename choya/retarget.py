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

    def count(self, kind: str) -> int:
        return sum(1 for c in self.changes if c.kind == kind)


class _Resolver:
    def __init__(self, ws: Symbols, maps: dict[str, dict[int, str]]):
        self.ws = ws
        self.maps = list(maps.values())
        self.moved: dict[int, tuple[str, int]] = {}    # dirección vieja → (nombre, nueva), de los registros
        self.anchors: dict[str, int] = {}               # símbolo → dirección vieja, de los comentarios de todo el runtime

    def function(self, addr: int, symbol: str = "", comment: str = "") -> tuple[str, int] | None:
        """El nombre del símbolo manda (lo puso quien escribió la función nativa); después el
        comentario de la línea; al final los mapas de los otros juegos."""
        cands = base_names(symbol) if symbol else []
        m = _CREF.match(comment)
        if m and not (m.group("poss") or m.group("off") or m.group("field")):
            cands.append(m.group("name"))
        cands += [mp[addr] for mp in self.maps if addr in mp]
        for c in cands:
            a = self.ws.funcs.get(c) if c not in self.ws.ambiguous else None
            if a is not None:
                return c, a
        return None


def _comment_ref(comment: str):
    m = _CREF.match(comment)
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
    return None


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
    for i, raw in enumerate(lines, 1):
        s = raw.strip()
        if i in dead or s.startswith(("//", "*", "/*", "#")) or _STATEMENTS.search(raw):
            continue
        code, sep, comment = raw.partition("//")
        lits = _literals(code)

        def sub_func(m, i=i, s=s):
            old = int(m.group("addr"), 16)
            if old in res.moved:
                name, new = res.moved[old]
                changes.append(Change(rel, i, "funcion", old, new, name, s[:140]))
                return "func_" + _fmt(m.group("addr"), new)
            changes.append(Change(rel, i, "sin_resolver", old, None, _suggest(old, res), s[:140]))
            return m.group(0)

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
    res.moved = {old: next(iter(h)) for old, h in seen.items() if len(h) == 1}
    res.anchors = {n: next(iter(b)) for n, b in bases.items() if len(b) == 1 and res.ws.get(n) is not None}


def detect_source_games(runtime: str, maps: dict[str, dict[int, str]]) -> list[str]:
    votes = Counter()
    for path in source_files(os.path.join(runtime, "src")):
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in _STATEMENTS.finditer(f.read()):
                a = int(m.group("addr"), 16)
                votes.update(g for g, mp in maps.items() if a in mp)
    return [g for g, _ in votes.most_common()]


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
    _collect_moves(out, res)
    registered: dict[int, str] = {}
    for sub in ("src", "include"):
        for path in source_files(os.path.join(out, sub)):
            rel = os.path.relpath(path, out).replace("\\", "/")
            with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
                text = f.read()
            new = retarget_text(text, rel, res, registered, result.changes)
            if new != text:
                with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(new)
    return result


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
    if n and new != text:
        with open(yml_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new)
    return bool(n)


def report_markdown(r: RetargetResult) -> str:
    by = defaultdict(list)
    for c in r.changes:
        by[c.kind].append(c)
    L = ["# Runtime de WiiCompiled movido a Wii Sports", "",
         f"Juegos de origen de las direcciones: **{r.source_game or 'desconocido'}**", "",
         "| Qué | Cantidad |", "|---|---:|",
         f"| Funciones nativas movidas | {r.count('registro')} |",
         f"| Funciones nativas desactivadas (Wii Sports no las tiene) | {r.count('desactivado')} |",
         f"| Registros repetidos desactivados | {r.count('duplicado')} |",
         f"| Llamadas a funciones movidas | {r.count('funcion')} |",
         f"| Variables del SDK movidas | {r.count('dato')} |",
         f"| **Direcciones sin resolver (revisar a mano)** | {r.count('sin_resolver')} |", ""]
    files = Counter(c.file for c in by["sin_resolver"])
    if files:
        L += ["## Sin resolver por archivo", "", "| Archivo | Direcciones |", "|---|---:|"]
        L += [f"| {f} | {n} |" for f, n in files.most_common()]
        L += ["", "## Sin resolver", "", "| Archivo:línea | Dirección | En otros juegos | Código |", "|---|---|---|---|"]
        for c in sorted(by["sin_resolver"], key=lambda c: (c.file, c.line)):
            L.append(f"| {c.file}:{c.line} | {c.old:08X} | {c.name} | `{c.text.replace('|', '¦')}` |")
    for kind, title in (("dato", "Variables movidas"), ("funcion", "Llamadas movidas"),
                        ("registro", "Funciones nativas movidas")):
        if by[kind]:
            L += ["", f"## {title}", "", "| Nombre | Antes | Wii Sports | Archivo:línea |", "|---|---|---|---|"]
            for c in sorted(by[kind], key=lambda c: (c.file, c.line)):
                L.append(f"| {c.name} | {c.old:08X} | {c.new:08X} | {c.file}:{c.line} |")
    if by["desactivado"] or by["duplicado"]:
        L += ["", "## Desactivadas", "", "| Archivo:línea | Antes | Símbolo | Motivo |", "|---|---|---|---|"]
        for c in sorted(by["desactivado"] + by["duplicado"], key=lambda c: (c.file, c.line)):
            why = "repetido" if c.kind == "duplicado" else "no existe en Wii Sports"
            L.append(f"| {c.file}:{c.line} | {c.old:08X} | {c.name} | {why} |")
    return "\n".join(L) + "\n"
