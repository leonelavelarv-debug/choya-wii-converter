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
    fecha: int = 0                                                          # cuándo se escribió (época), 0.20.1
    hilos: dict[int, tuple[int, str]] = field(default_factory=dict)       # tid -> (muestras, nombre)
    pcs: list[tuple[int, int, int]] = field(default_factory=list)          # (tid, muestras, desplazamiento)
    libs: list[tuple[int, int, str]] = field(default_factory=list)         # (tid, muestras, "lib:símbolo")
    # v2: medición por tramos del hilo del juego (reloj de pared) y llamadas por función.
    cuadros: int = 0
    hilo_juego: str = ""
    pared: int = 0
    calc: int = 0
    draw: int = 0
    # (muestras, llamada al sistema en la que dormía o -2 si corría, pc, lr, pila del anfitrión, pila del juego)
    pilas: list[tuple[int, int, str, str, list[str], list[int]]] = field(default_factory=list)
    arq: str = "aarch64"
    llamadas: dict[int, int] = field(default_factory=dict)


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
            elif parts[0] == "fecha":
                p.fecha = int(parts[1])
            elif parts[0] == "hilo":
                p.hilos[int(parts[1])] = (int(parts[2]), " ".join(parts[3:]) or "?")
            elif parts[0] == "pc":
                p.pcs.append((int(parts[1]), int(parts[2]), int(parts[3], 16)))
            elif parts[0] == "lib":
                p.libs.append((int(parts[1]), int(parts[2]), " ".join(parts[3:])))
            elif parts[0] == "cuadros":
                p.cuadros = int(parts[1])
            elif parts[0] == "hilo_juego":
                p.hilo_juego = " ".join(parts[1:])
            elif parts[0] == "pared":
                p.pared = int(parts[1])
            elif parts[0] == "dirs":
                p.calc, p.draw = int(parts[1], 16), int(parts[2], 16)
            elif parts[0] == "arq":
                p.arq = parts[1]
            elif parts[0] == "q":
                n, wait, pc, lr = int(parts[1]), int(parts[2]), parts[3], parts[4]
                rest = parts[5:]
                gi = rest.index("g") if "g" in rest else len(rest)
                hosts = [t for t in rest[1:gi]]
                guests = [int(t, 16) for t in rest[gi + 1:]]
                p.pilas.append((n, wait, pc, lr, hosts, guests))
            elif parts[0] == "llamadas":
                p.llamadas[int(parts[1], 16)] = int(parts[2])
    return p


def desplazamientos(p: Perfil) -> list[int]:
    """Todas las direcciones del ejecutable que hay que nombrar (CPU y pilas de los tramos).

    Las de la pila son direcciones de regreso: apuntan a la instrucción que sigue a la llamada, que puede ser ya
    de otra función (o de otra función integrada en línea). Se nombran también en dirección - 1, que cae dentro
    de la llamada misma (0.20.1)."""
    offs = {o for _t, _c, o in p.pcs}
    for _n, _w, pc, lr, hosts, _g in p.pilas:
        if pc.startswith("x"):
            offs.add(int(pc[1:], 16))
        for t in [lr] + hosts:
            if t.startswith("x"):
                o = int(t[1:], 16)
                offs.add(o)
                if o > 0:
                    offs.add(o - 1)
    return sorted(offs)


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


def nombrar_cadenas(exe: str, offsets: list[int]) -> dict[int, list[str]]:
    """Desplazamiento -> funciones en esa dirección, de la más interna (integrada en línea) a la que la contiene."""
    if not offsets:
        return {}
    cmd = _symbolizer_cmd(exe)
    out = subprocess.run(cmd, input="".join(f"0x{o:x}\n" for o in offsets), capture_output=True, text=True,
                         check=True).stdout
    chains: dict[int, list[str]] = {}
    blocks = [b for b in out.split("\n\n") if b.strip()]
    for off, block in zip(offsets, blocks):
        lines = [ln.strip() for ln in block.strip().splitlines()]
        funcs = [lines[i] for i in range(0, len(lines), 2) if lines[i] not in ("??", "")]
        chains[off] = funcs or ["??"]
    return chains


def nombrar(exe: str, offsets: list[int]) -> dict[int, str]:
    """Desplazamiento dentro del ejecutable -> nombre de función (con llvm-symbolizer)."""
    return {o: c[0] for o, c in nombrar_cadenas(exe, offsets).items()}


def _corto(name: str) -> str:
    """Quita los argumentos de la firma (y el tipo de regreso, si lo trae) para agrupar por función."""
    depth = 0
    cut = len(name)
    for i, ch in enumerate(name):
        if ch == "(" and depth == 0 and i > 0 and not name.startswith("(anonymous namespace)", i):
            cut = i
            break
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth = max(0, depth - 1)
    name = name[:cut]
    depth = 0
    last_space = -1
    for i, ch in enumerate(name):
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth = max(0, depth - 1)
        elif ch == " " and depth == 0:
            last_space = i
    return name[last_space + 1:] if last_space >= 0 else name


_AYUDANTES = ("std::", "__gnu_cxx", "__cxxabiv1", "absl::", "fmt::", "XXH", "operator", "__libc", "libc.so",
              "libm.so", "linux-vdso", "decltype", "??")


def _es_ayudante(name: str) -> bool:
    """Funciones de biblioteca que se integran en todo: no dicen de quién es el trabajo."""
    return name.startswith(_AYUDANTES) or ".so" in name.split(":", 1)[0]


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


# Llamadas que duermen o esperan (el contador de programa queda en la biblioteca del sistema o del driver).
_ESPERAS = ("futex", "lll_lock_wait", "cond_wait", "cond_timedwait", "nanosleep", "clock_nanosleep", "usleep",
            "poll", "epoll_wait", "select", "ioctl", "sched_yield", "sem_wait", "sem_timedwait", "__GI___wait",
            "syscall", "pthread_join", "read")


# Llamadas al sistema en las que duerme el hilo (número -> nombre), por arquitectura; -1 = fallo de página.
_SYSCALLS = {
    "aarch64": {98: "futex (lock o variable de condición)", 101: "nanosleep", 115: "clock_nanosleep", 73: "ppoll",
                22: "epoll_pwait", 29: "ioctl (driver: Mali, sonido...)", 63: "read", 64: "write",
                124: "sched_yield", 72: "pselect6", 260: "wait4"},
    "x86_64": {202: "futex (lock o variable de condición)", 35: "nanosleep", 230: "clock_nanosleep", 7: "poll",
               271: "ppoll", 232: "epoll_wait", 281: "epoll_pwait", 16: "ioctl (driver)", 0: "read", 1: "write",
               24: "sched_yield", 23: "select"},
}


def _syscall(arq: str, nr: int) -> str:
    if nr == -1:
        return "fallo de página (leyendo de la microSD: memoria llena)"
    return _SYSCALLS.get(arq, {}).get(nr, f"llamada al sistema {nr}")


def _es_espera(token: str) -> bool:
    if token.startswith("x") or ":" not in token:
        return False
    lib, sym = token.split(":", 1)
    return any(k in sym for k in _ESPERAS) or (lib.startswith("libc") and sym == "?")


def _funcion_wii(ws, rows, keys, addr: int) -> tuple[int, str] | None:
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return None
    start, name = rows[i]
    size = ws.size.get(start, 0)
    if size and addr >= start + size:
        return None
    return start, name


def tramos(p: Perfil, names: dict[int, str], ws, top: int = 20,
           cadenas: dict[int, list[str]] | None = None) -> list[str]:
    """Reparte el cuadro del hilo del juego en actualizar / preparar el dibujo / esperar (muestras de pared)."""
    rows = sorted((addr, n) for n, addr in ws.funcs.items())
    keys = [r[0] for r in rows]
    total = sum(n for n, *_ in p.pilas)
    if total == 0:
        return ["\n== Tramos: sin muestras de pared (¿no se encontró el hilo del juego?)"]
    frames = max(1, p.cuadros)
    frame_ms = p.segundos * 1000.0 / frames
    ms = lambda count: frame_ms * count / total  # noqa: E731
    wii_name = {start: name for start, name in rows}
    if cadenas is None:
        cadenas = {o: [n] for o, n in names.items()}

    def cadena(token: str, regreso: bool) -> list[str]:
        """Funciones (de adentro hacia afuera) en una dirección de la pila; las de regreso se miran en dirección - 1."""
        if not token.startswith("x"):
            return [token] if token != "-" else []
        o = int(token[1:], 16)
        c = cadenas.get(o - 1) if regreso and o > 0 else None
        c = c or cadenas.get(o) or [names.get(o, "??")]
        return [_corto(n) for n in c]

    def pila(pc: str, lr: str, hosts: list[str]) -> list[str]:
        """Toda la pila del anfitrión con las funciones integradas en línea, de adentro hacia afuera."""
        out = cadena(pc, False)
        if lr != "-" and _es_espera(pc):
            out += cadena(lr, True)
        for t in hosts:
            out += cadena(t, True)
        return out

    def guest_funcs(nombres: list[str], guests: list[int]) -> list[int]:
        out: list[int] = []
        for n in nombres:
            m = _FUNC.search(n)
            if m:
                a = int(m.group(1), 16)
                f = _funcion_wii(ws, rows, keys, a)
                a = f[0] if f else a
                if a not in out:
                    out.append(a)
        for lr_g in guests:
            f = _funcion_wii(ws, rows, keys, lr_g)
            if f and f[0] not in out:
                out.append(f[0])
        return out

    def duena(nombres: list[str]) -> str:
        """De quién es el trabajo de la muestra: la primera función de la pila que no es de biblioteca."""
        for n in nombres:
            if not _es_ayudante(n):
                return n
        return nombres[0] if nombres else "??"

    def reparto(n: str) -> str:
        if _FUNC.search(n):
            return "código del juego"
        if categoria(n).startswith("gráficos") or n.startswith("choya"):
            return "aurora y Choya (convierten el GX y arman el cuadro)"
        if categoria(n) == "GX (HLE)":
            return "GX del runtime (funciones GX de la Wii hechas en C++)"
        if categoria(n) == "audio (HLE)" or "snd" in n:
            return "audio del runtime"
        return "runtime y bibliotecas (" + (n.split("::")[0] if n else "?") + ")"

    parts: Counter[str] = Counter()
    hojas: dict[str, Counter[str]] = defaultdict(Counter)
    wait_in: Counter[str] = Counter()
    incl: dict[str, Counter[int]] = defaultdict(Counter)
    host_incl: dict[str, Counter[str]] = defaultdict(Counter)
    waits: Counter[str] = Counter()
    for n, wait, pc, lr, hosts, guests in p.pilas:
        nombres = pila(pc, lr, hosts)
        funcs = guest_funcs(nombres, guests)
        phase = "otros"
        for a in funcs:  # de adentro hacia afuera: el primero de los dos que aparezca
            if a == p.calc:
                phase = "actualizar"
                break
            if a == p.draw:
                phase = "dibujo"
                break
        if wait != -2 or _es_espera(pc):
            parts["esperar"] += n
            wait_in[phase] += n
            what = _syscall(p.arq, wait) if wait != -2 else pc.split(":", 1)[-1]
            sitio = [x for t in [lr] + hosts if t.startswith("x") for x in cadena(t, True)]
            waits[f"{what} ← {duena(sitio) if sitio else duena(cadena(pc, False))}"] += n
            continue
        parts[phase] += n
        for a in funcs:
            incl[phase][a] += n
        hojas[phase][reparto(duena(nombres))] += n
        seen = set()
        for hn in nombres:
            if hn not in seen and not _FUNC.search(hn) and not _es_ayudante(hn):
                seen.add(hn)
                host_incl[phase][hn] += n

    lines = ["\n== Tramos del cuadro (hilo del juego " + (p.hilo_juego or "?") + ", reloj de pared)"]
    lines.append(f"{p.cuadros} cuadros en {p.segundos} s: {p.cuadros / max(1, p.segundos):.1f} cuadros/s, "
                 f"{frame_ms:.1f} ms por cuadro ({total} muestras)")
    etiquetas = {"actualizar": "Actualizar el juego (EGG::SceneManager::calc)",
                 "dibujo": "Preparar el dibujo (EGG::SceneManager::draw)",
                 "esperar": "Esperar (dormido o bloqueado)", "otros": "Otros (fuera de calc y draw)"}
    for k in ("actualizar", "dibujo", "esperar", "otros"):
        lines.append(f"{ms(parts[k]):7.1f} ms  {100 * parts[k] / total:5.1f}%  {etiquetas[k]}")
        if k == "esperar":
            if parts[k]:
                lines.append("          esperas dentro de: " + ", ".join(f"{ph} {ms(c):.1f} ms"
                                                                     for ph, c in wait_in.most_common()))
            continue
        for name, c in hojas[k].most_common(6):
            if ms(c) >= 0.05:
                lines.append(f"          {ms(c):6.1f} ms  de eso: {name}")

    def tabla(phase: str, titulo: str) -> None:
        if not incl[phase]:
            return
        lines.append(f"\n== {titulo}: lo que más acumula (inclusivo) — ms por cuadro · llamadas por cuadro")
        for a, c in incl[phase].most_common(top):
            calls = p.llamadas.get(a, 0) / frames
            lines.append(f"{ms(c):7.2f} ms  {calls:9.1f}×  {wii_name.get(a, f'func_{a:08X}')}")

    def tabla_anfitrion(phase: str, titulo: str) -> None:
        if not host_incl[phase]:
            return
        lines.append(f"\n== {titulo}: funciones del runtime, aurora y Choya (inclusivo, ms por cuadro)")
        for name, c in host_incl[phase].most_common(15):
            lines.append(f"{ms(c):7.2f} ms  {name}")

    tabla("actualizar", "Actualizar el juego")
    tabla("dibujo", "Preparar el dibujo (código del juego)")
    tabla_anfitrion("dibujo", "Preparar el dibujo")
    tabla("otros", "Otros (código del juego)")
    tabla_anfitrion("otros", "Otros")
    if waits:
        lines.append("\n== Esperas: en qué duerme y quién la pidió (ms por cuadro)")
        for name, c in waits.most_common(12):
            lines.append(f"{ms(c):7.2f} ms  {name}")
    if p.llamadas:
        lines.append(f"\n== Las funciones más llamadas (por cuadro; muchas llamadas a lo mismo = trabajo repetido)")
        for a, c in sorted(p.llamadas.items(), key=lambda kv: -kv[1])[:top]:
            lines.append(f"{c / frames:10.1f}×  {wii_name.get(a, f'func_{a:08X}')}")
    return lines


def informe(p: Perfil, names: dict[int, str], simbolos: str | None = None, top: int = 35,
            cadenas: dict[int, list[str]] | None = None) -> str:
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
    if p.fecha:
        import time
        lines[0] += time.strftime(" · medido el %d/%m/%Y a las %H:%M", time.localtime(p.fecha))
    else:
        lines[0] += " · perfil sin fecha (de una versión anterior a la 0.20.1)"
    if p.hilo_juego:
        lines[0] += f" · hilo del juego {p.hilo_juego.split()[0]}"
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
    if p.pilas or p.llamadas:
        lines.extend(tramos(p, names, ws, cadenas=cadenas))
    return "\n".join(lines)
