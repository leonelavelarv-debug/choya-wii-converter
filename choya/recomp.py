"""Prepara un proyecto de WiiCompiled para Wii Sports (RSPE01 Rev 1).

Choya no incluye código de WiiCompiled (GPLv3) ni del juego. Solo escribe archivos de
configuración a partir del main.dol del usuario y de los símbolos de ogws (CC0):

  projects/wiisports/recomp.yml   manifiesto del traductor (SDA, entrada, SHA-256)
  projects/wiisports/MAP.txt      direcciones y nombres de las 14 121 funciones
  projects/wiisports/native/      carpeta vacía para las funciones nativas (HLE) de Wii Sports
  projects/wiisports/hle_cobertura.md   qué funciones nativas de WiiCompiled existen en Wii Sports
"""
from __future__ import annotations

import gzip
import os
import re
from collections import defaultdict
from dataclasses import dataclass

from . import dol as doltools
from .scan import OGWS_SHA1, _sys_dir

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
BUNDLED_MAP = os.path.join(DATA, "RSPE01_01.map.gz")
WIISPORTS_START = 0x80006124
WIISPORTS_INIT_REGISTERS = 0x80006290

_SYM = re.compile(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]+);\s*//.*\btype:function\b")
_OVERRIDE = re.compile(r"\b(PPC_NATIVE_OVERRIDE\w*|REGISTER_NATIVE_FUNCTION\w*|REGISTER_TRANSLATED_FUNCTION\w*)"
                       r"\(\s*(?:0x)?([0-9A-Fa-f]{8})\s*,\s*([A-Za-z_]\w*)")


def is_placeholder(name: str) -> bool:
    """decomp-toolkit llama pad_* a huecos sin analizar. En Wii Sports el único que cae en código es
    pad_00_80004188_init: la tabla de vectores del depurador MetroTRK (empieza con el texto
    "Metrowerks..."), que el juego nunca llama. Traducirla como función deja una instrucción inválida."""
    return name.startswith("pad_")


def parse_symbols(text: str) -> list[tuple[int, str]]:
    """symbols.txt de decomp-toolkit (ogws) → [(dirección, nombre)] de funciones, ordenado."""
    out = {}
    for line in text.splitlines():
        m = _SYM.match(line)
        if m and not is_placeholder(m.group(1)):
            out.setdefault(int(m.group(3), 16), m.group(1))
    return sorted(out.items())


def parse_map(text: str) -> list[tuple[int, str]]:
    out = []
    for line in text.splitlines():
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        name = p[1] if len(p) > 1 else ""
        if not is_placeholder(name):
            out.append((int(p[0], 16), name))
    return out


def map_text(funcs: list[tuple[int, str]]) -> str:
    return "".join(f"{a:08x} {n}\n" for a, n in funcs)


def load_functions(simbolos: str | None) -> tuple[list[tuple[int, str]], str]:
    if simbolos:
        with open(simbolos, encoding="utf-8", errors="replace") as f:
            text = f.read()
        funcs = parse_symbols(text) if " = ." in text else parse_map(text)
        return funcs, os.path.abspath(simbolos)
    with gzip.open(BUNDLED_MAP, "rt", encoding="utf-8") as f:
        return parse_map(f.read()), "ogws config/RSPE01_01/symbols.txt (incluido, CC0)"


def find_dol(src: str) -> str:
    if os.path.isfile(src):
        return src
    sysd = _sys_dir(src)
    if sysd and os.path.isfile(os.path.join(sysd, "main.dol")):
        return os.path.join(sysd, "main.dol")
    raise FileNotFoundError(f"no encontré sys/main.dol dentro de {src}")


# --- cobertura de las funciones nativas de WiiCompiled ----------------------------------

@dataclass
class HleEntry:
    old_address: int
    symbol: str
    source: str
    name: str | None          # nombre del SDK que reconocimos
    new_address: int | None   # dirección en Wii Sports


def _base_names(symbol: str) -> list[str]:
    base = re.sub(r"_[0-9a-fA-F]{8}$", "", symbol)
    cands = [base]
    if "__" in base:                       # GX__SetTevOrder → GXSetTevOrder
        cands.append(base.replace("__", "", 1))
    return cands


def hle_coverage(wiicompiled: str, funcs: list[tuple[int, str]]) -> list[HleEntry]:
    src_root = os.path.join(wiicompiled, "runtime", "src")
    maps = {}
    proj = os.path.join(wiicompiled, "projects")
    for game in sorted(os.listdir(proj)) if os.path.isdir(proj) else []:
        p = os.path.join(proj, game, "MAP.txt")
        if game != "wiisports" and os.path.isfile(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                maps[game] = {a: n for a, n in parse_map(f.read()) if n and not n.startswith("0x")}
    by_name = {}
    for a, n in funcs:
        by_name.setdefault(n, a)
    out = []
    for dirpath, _dirs, files in os.walk(src_root):
        for fn in files:
            if not fn.endswith((".cpp", ".h", ".hpp", ".inc")):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            rel = os.path.relpath(path, src_root).replace("\\", "/")
            for m in _OVERRIDE.finditer(text):
                addr, sym = int(m.group(2), 16), m.group(3)
                cands = [mp[addr] for mp in maps.values() if addr in mp] + _base_names(sym)
                hit = next((c for c in cands if c in by_name), None)
                out.append(HleEntry(addr, sym, rel, hit, by_name.get(hit) if hit else None))
    out.sort(key=lambda e: (e.source, e.old_address))
    return out


def coverage_markdown(entries: list[HleEntry]) -> str:
    groups = defaultdict(list)
    for e in entries:
        parts = e.source.split("/")
        groups["/".join(parts[:2]) if parts[0] == "hle" and len(parts) > 2 else parts[0]].append(e)
    ok = sum(1 for e in entries if e.new_address is not None)
    lines = ["# Funciones nativas de WiiCompiled frente a Wii Sports", "",
             f"{ok} de {len(entries)} registros tienen la misma función del SDK en Wii Sports.",
             "Los que no la tienen son de otro juego o del SDK más nuevo y no hacen falta.", "",
             "| Carpeta | En Wii Sports | Sin equivalente |", "|---|---:|---:|"]
    for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        found = sum(1 for e in v if e.new_address is not None)
        lines.append(f"| {k} | {found} | {len(v) - found} |")
    lines += ["", "## Direcciones nuevas", "", "| Función | Antes | Wii Sports | Archivo |", "|---|---|---|---|"]
    for e in entries:
        if e.new_address is not None:
            lines.append(f"| {e.name} | {e.old_address:08X} | {e.new_address:08X} | {e.source} |")
    return "\n".join(lines) + "\n"


# --- manifiesto ---------------------------------------------------------------------

def manifest_yaml(dol_path: str, sha256: str, boot: doltools.BootInfo, game_id: str = "RSPE01") -> str:
    p = os.path.abspath(dol_path).replace("\\", "/")
    return f"""schema_version: 1
workspace_root: ../..

# Generado por choya recomp-prep. Wii Sports (USA rev 1, RSPE01 v1).
# Un solo main.dol, sin REL. Símbolos de doldecomp/ogws (CC0).
project:
  id: wiisports-rspe01
  display_name: Wii Sports (USA rev 1)
  game_id: {game_id}
  region: E

memory:
  base: 0x80000000
  size: 0x01800000
  sda_base: 0x{boot.sda_base:08X}    # r13, leído de __init_registers ({boot.init_registers:08X})
  sda2_base: 0x{boot.sda2_base:08X}   # r2

inputs:
  dol:
    path: "{p}"
    sha256: {sha256}

translation:
  entry_points:
    - 0x{boot.entry:08X}
  function_map:
    path: projects/wiisports/MAP.txt
  # Primera pasada: dejar pasar instrucciones raras y contarlas en el informe.
  allow_unsupported_instructions: true

runtime:
  # Carpeta propia: las funciones nativas de runtime/src están en direcciones de otro juego.
  native_registration_root: projects/wiisports/native
  native_abi_directories: []

output:
  root: build/wiisports
  functions: functions
  runtime_config: RuntimeConfig.h
  data_initializer: data_sections_init.cpp
  base_manifest: base/wiisports_base_manifest.json
"""


@dataclass
class PrepResult:
    project_dir: str
    dol_path: str
    sha1: str
    ogws_match: bool
    boot: doltools.BootInfo
    functions: int
    outside_text: int
    symbols_source: str
    coverage: list[HleEntry] | None
    warnings: list[str]


def prep(src: str, out: str, simbolos: str | None = None, force: bool = False) -> PrepResult:
    dol_path = find_dol(src)
    with open(dol_path, "rb") as f:
        data = f.read()
    d = doltools.read_dol(data)
    warnings = []
    ogws_match = d.sha1 == OGWS_SHA1
    if not ogws_match and not simbolos and not force:
        raise ValueError(f"este main.dol (SHA-1 {d.sha1}) no es Wii Sports USA Rev 1; "
                         "los símbolos incluidos no le sirven (usa --simbolos o --forzar)")
    if not ogws_match:
        warnings.append("el main.dol no es el de ogws; revisa que los símbolos correspondan")

    funcs, source = load_functions(simbolos)
    names = {n: a for a, n in funcs}
    init = names.get("__init_registers")
    boot = doltools.boot_info(d)
    if init is not None and init != boot.init_registers:
        warnings.append(f"__init_registers según símbolos {init:08X}, según el DOL {boot.init_registers:08X}")
    start = names.get("__start")
    if start is not None and start != d.entry:
        warnings.append(f"__start según símbolos {start:08X}, la entrada del DOL es {d.entry:08X}")
    outside = sum(1 for a, _ in funcs
                  if (s := d.section_at(a)) is None or s.kind != "text")
    if outside:
        warnings.append(f"{outside} funciones del mapa caen fuera del código del DOL")

    proj = os.path.join(out, "projects", "wiisports")
    os.makedirs(os.path.join(proj, "native"), exist_ok=True)
    with open(os.path.join(proj, "MAP.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(map_text(funcs))
    with open(os.path.join(proj, "recomp.yml"), "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest_yaml(dol_path, d.sha256, boot))
    readme = os.path.join(proj, "native", "LEEME.txt")
    if not os.path.exists(readme):
        with open(readme, "w", encoding="utf-8", newline="\n") as f:
            f.write("Funciones nativas (HLE) de Wii Sports, con las direcciones de RSPE01 Rev 1.\n"
                    "Vacía en la primera pasada: todo el juego se traduce tal cual.\n")

    coverage = None
    if os.path.isdir(os.path.join(out, "runtime", "src")):
        coverage = hle_coverage(out, funcs)
        with open(os.path.join(proj, "hle_cobertura.md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(coverage_markdown(coverage))
    return PrepResult(proj, dol_path, d.sha1, ogws_match, boot, len(funcs), outside, source, coverage, warnings)
