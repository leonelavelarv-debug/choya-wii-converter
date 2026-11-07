"""Lee perfil.txt (muestras de la consola) y dice en qué funciones se fue el tiempo.

La consola solo anota direcciones dentro del ejecutable (está sin símbolos para ocupar menos). Aquí se les pone
nombre con llvm-symbolizer usando el WiiCompiled sin recortar de la carpeta de compilación, y las funciones
traducidas (func_8XXXXXXX) se traducen a su nombre en Wii Sports con los símbolos de ogws.
"""
from __future__ import annotations

import bisect
import os
import re
import shutil
import subprocess
from collections import Counter, defaultdict
from dataclasses import dataclass, field

SYMBOLIZERS = ("llvm-symbolizer-21", "llvm-symbolizer", "llvm-symbolizer-20", "llvm-symbolizer-19",
               "llvm-symbolizer-18")
DOCKER_IMAGE = "choya-wiicompiled-arm64"
_FUNC = re.compile(r"\bfunc_([0-9A-Fa-f]{8})\b")


@dataclass
class Perfil:
    muestras: int = 0
    segundos: int = 0
    hilos: dict[int, tuple[int, str]] = field(default_factory=dict)       # tid -> (muestras, nombre)
    pcs: list[tuple[int, int, int]] = field(default_factory=list)          # (tid, muestras, desplazamiento)
    libs: list[tuple[int, int, str]] = field(default_factory=list)         # (tid, muestras, "lib:símbolo")


def leer(path: str) -> Perfil:
    p = Perfil()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "muestras":
                p.muestras = int(parts[1])
            elif parts[0] == "segundos":
                p.segundos = int(parts[1])
            elif parts[0] == "hilo":
                p.hilos[int(parts[1])] = (int(parts[2]), " ".join(parts[3:]) or "?")
            elif parts[0] == "pc":
                p.pcs.append((int(parts[1]), int(parts[2]), int(parts[3], 16)))
            elif parts[0] == "lib":
                p.libs.append((int(parts[1]), int(parts[2]), " ".join(parts[3:])))
    return p


def _symbolizer_cmd(exe: str) -> list[str]:
    for name in SYMBOLIZERS:
        found = shutil.which(name)
        if found:
            return [found, f"--obj={exe}", "--demangle"]
    if shutil.which("docker"):
        d = os.path.dirname(os.path.abspath(exe))
        return ["docker", "run", "--rm", "-i", "-v", f"{d}:{d}", DOCKER_IMAGE, "llvm-symbolizer-21",
                f"--obj={os.path.abspath(exe)}", "--demangle"]
    raise FileNotFoundError("no encontré llvm-symbolizer (instala llvm o deja la imagen de Docker de Choya)")


def nombrar(exe: str, offsets: list[int]) -> dict[int, str]:
    """Desplazamiento dentro del ejecutable -> nombre de función (con llvm-symbolizer)."""
    if not offsets:
        return {}
    cmd = _symbolizer_cmd(exe)
    out = subprocess.run(cmd, input="".join(f"0x{o:x}\n" for o in offsets), capture_output=True, text=True,
                         check=True).stdout
    names: dict[int, str] = {}
    blocks = [b for b in out.split("\n\n") if b.strip()]
    for off, block in zip(offsets, blocks):
        first = block.strip().splitlines()[0].strip()
        names[off] = "??" if first in ("??", "") else first
    return names


def _corto(name: str) -> str:
    """Quita los argumentos de la firma para agrupar por función."""
    depth = 0
    for i, ch in enumerate(name):
        if ch == "(" and depth == 0 and i > 0:
            return name[:i]
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
    return name


def _wii_nombre(ws, rows, keys, addr: int) -> str | None:
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return None
    start, name = rows[i]
    size = ws.size.get(start, 0)
    if size and addr >= start + size:
        return None
    return name if start == addr else f"{name}+0x{addr - start:X}"


def categoria(name: str) -> str:
    if _FUNC.search(name):
        return "juego (código traducido)"
    if name.startswith(("aurora", "webgpu", "dawn", "wgpu", "tint")) or "::webgpu" in name or "dawn::" in name:
        return "gráficos (aurora/Dawn)"
    if "GX" in name or "gx" in name.split("::")[0]:
        return "GX (HLE)"
    if any(k in name for k in ("AX", "Ax", "Audio", "audio", "Dsp", "DSP")):
        return "audio (HLE)"
    if name == "??":
        return "sin nombre"
    return "runtime"


def informe(p: Perfil, names: dict[int, str], simbolos: str | None = None, top: int = 35) -> str:
    from . import retarget
    ws = retarget.load_wiisports_symbols(simbolos)
    rows = sorted((addr, n) for n, addr in ws.funcs.items())
    keys = [r[0] for r in rows]

    total = max(1, sum(c for _t, c, _o in p.pcs) + sum(c for _t, c, _l in p.libs))
    por_func: Counter[str] = Counter()
    por_cat: Counter[str] = Counter()
    por_hilo_func: dict[int, Counter[str]] = defaultdict(Counter)
    for tid, count, off in p.pcs:
        full = names.get(off, "??")
        fn = _corto(full)
        m = _FUNC.search(fn)
        if m:
            wii = _wii_nombre(ws, rows, keys, int(m.group(1), 16))
            if wii:
                fn = f"{fn} [{wii}]"
        por_func[fn] += count
        por_cat[categoria(full)] += count
        por_hilo_func[tid][fn] += count
    for tid, count, where in p.libs:
        por_func[where] += count
        por_cat["bibliotecas (" + where.split(":")[0] + ")"] += count
        por_hilo_func[tid][where] += count

    lines = [f"Muestras: {total} en {p.segundos} s"]
    lines.append("\n== Por hilo")
    for tid, (count, name) in sorted(p.hilos.items(), key=lambda kv: -kv[1][0]):
        lines.append(f"{100 * count / total:5.1f}%  {name} ({tid})")
    lines.append("\n== Por tipo")
    for cat, count in por_cat.most_common():
        lines.append(f"{100 * count / total:5.1f}%  {cat}")
    lines.append(f"\n== Funciones (las {top} más caras)")
    for fn, count in por_func.most_common(top):
        lines.append(f"{100 * count / total:5.1f}%  {fn}")
    for tid, (count, name) in sorted(p.hilos.items(), key=lambda kv: -kv[1][0])[:3]:
        lines.append(f"\n== Hilo {name} ({tid}): sus 12 funciones más caras")
        for fn, c in por_hilo_func[tid].most_common(12):
            lines.append(f"{100 * c / max(1, count):5.1f}%  {fn}")
    return "\n".join(lines)
