"""Manifiesto del port: qué se compiló, con qué, y el SHA-256 de cada archivo distribuido (0.32.0).

    python3 -m choya manifiesto <port> --wc <WiiCompiled> --build <carpeta> --modo O2 ...   (lo llama el script)
    python3 -m choya verificar-port <carpeta con wiisports-choya>
    python3 -m choya publicar-port <port nuevo> <destino>

- wiisports-choya/manifiesto.json (para máquinas) y manifiesto.txt (para leer) van dentro del port.
- La identidad del port es la huella de todos sus archivos salvo manifiesto.json, manifiesto.txt y version.txt (que
  la lleva escrita): nada se calcula sobre sí mismo y la fecha no cuenta. Dos compilaciones con el mismo resultado
  dan la misma identidad.
- verificar-port comprueba cada archivo del manifiesto (que exista y tenga el mismo SHA-256) y no mira ni toca nada
  más: las partidas, los ajustes, los perfiles y el disco del usuario quedan fuera.
- publicar-port reemplaza el port anterior solo si el nuevo está completo y verificado (si no, el anterior se queda).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys

from . import __version__, huellas

CARPETA = "wiisports-choya"
MANIFIESTO = f"{CARPETA}/manifiesto.json"
RESUMEN = f"{CARPETA}/manifiesto.txt"
VERSION = f"{CARPETA}/version.txt"
FORMATO = "choya-manifiesto-1"
FUERA_DE_LISTA = (MANIFIESTO, RESUMEN)
FUERA_DE_IDENTIDAD = (MANIFIESTO, RESUMEN, VERSION)


def _git(raiz, *args):
    r = subprocess.run(["git", "-C", raiz, *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def info_choya() -> dict:
    raiz = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    commit = _git(raiz, "rev-parse", "HEAD")
    sucio = None
    if commit:
        st = _git(raiz, "status", "--porcelain", "--untracked-files=no")
        sucio = bool(st)
    return {"version": __version__, "commit": commit, "cambios_sin_commit": sucio,
            "nota": None if commit else "Choya no está en un repositorio git: solo se sabe la versión"}


def lista_archivos(port: str) -> list[dict]:
    out = []
    for rel in huellas.archivos(port):
        if rel in FUERA_DE_LISTA:
            continue
        ruta = os.path.join(port, rel)
        out.append({"ruta": rel, "sha256": huellas.sha256_archivo(ruta), "bytes": os.path.getsize(ruta)})
    return out


def identidad(entradas: list[dict]) -> str:
    return huellas.huella_lista((e["ruta"], e["sha256"]) for e in entradas if e["ruta"] not in FUERA_DE_IDENTIDAD)


def _cmake_cache(build: str) -> dict:
    out = {}
    ruta = os.path.join(build, "CMakeCache.txt")
    if not os.path.isfile(ruta):
        return out
    with open(ruta, encoding="utf-8", errors="replace") as f:
        for line in f:
            if ":" not in line or "=" not in line or line.startswith(("//", "#")):
                continue
            k, v = line.rstrip("\n").split("=", 1)
            nombre = k.split(":", 1)[0]
            if (nombre.startswith("AURORA_") and nombre.endswith(("_VERSION", "_PROVIDER"))) or nombre in (
                    "CMAKE_BUILD_TYPE", "CMAKE_TOOLCHAIN_FILE", "CHOYA_OPT_TRADUCIDO", "CHOYA_ENLACE_EXTRA",
                    "CHOYA_COMPILADO", "SDL_UNIX_CONSOLE_BUILD"):
                out[nombre] = v
    return out


def crear(port: str, datos: dict) -> dict:
    """Escribe version.txt, manifiesto.json y manifiesto.txt en el port. datos: lo que sabe el script de compilación."""
    entradas = lista_archivos(port)
    ident = identidad(entradas)
    choya = info_choya()
    modo = datos.get("compilacion", {}).get("modo", "?")
    with open(os.path.join(port, VERSION), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"Choya {__version__}{' (' + choya['commit'][:10] + ')' if choya['commit'] else ''}, {modo}, "
                f"port {ident[:12]}\n")
    entradas = lista_archivos(port)
    m = {"formato": FORMATO, "port": CARPETA, "identidad": ident, "choya": choya, **datos, "archivos": entradas}
    huellas.escribir_json(os.path.join(port, MANIFIESTO), m)
    with open(os.path.join(port, RESUMEN), "w", encoding="utf-8", newline="\n") as f:
        f.write(resumen(m))
    return m


def _corto(v, n=12):
    return "—" if not v else str(v)[:n]


def resumen(m: dict) -> str:
    c, comp = m["choya"], m.get("compilador") or {}
    deps = m.get("dependencias") or {}
    dep = deps.get("wiicompiled") or {}
    cb, juego, pgo = m.get("compilacion") or {}, m.get("juego") or {}, m.get("pgo")
    par, rec = m.get("parches") or {}, m.get("recursos") or {}
    L = [f"Port de Wii Sports (Choya) — identidad {m['identidad'][:16]}", "",
         f"Choya:        {c['version']} (commit {_corto(c.get('commit'), 10)}"
         f"{', con cambios sin commit' if c.get('cambios_sin_commit') else ''})",
         f"WiiCompiled:  commit {_corto(dep.get('commit'), 10)} · fijado: {(deps.get('lock') or {}).get('estado', '—')}"
         f" · cambios de Choya {dep.get('cambios_choya', '—')} · ajenos {len(dep.get('cambios_ajenos') or [])}",
         f"              {dep.get('nota', '')}",
         f"Compilador:   {comp.get('clang', '—')} → {comp.get('objetivo', '—')} ({comp.get('cpu', '—')}), "
         f"glibc {comp.get('glibc_requerida', '—')}, imagen {_corto(comp.get('imagen_docker'), 19)}",
         f"Compilación:  {cb.get('modo', '—')} · código traducido: {cb.get('flags_codigo_traducido') or '-O2'}"
         f" · carpeta {cb.get('carpeta', '—')}",
         f"Juego:        main.dol {_corto(juego.get('main_dol_sha256'), 16)} · código traducido "
         f"{_corto((juego.get('codigo_traducido') or {}).get('huella'), 16)}"]
    if pgo:
        L.append(f"Perfil PGO:   {_corto(pgo.get('sha256'), 16)} ({pgo.get('estado', '')}; {pgo.get('funciones', '?')} "
                 f"funciones; {pgo.get('clang', '')})")
        for a in pgo.get("avisos", []):
            L.append(f"              aviso: {a}")
    r = par.get("resumen", {})
    L.append(f"Parches:      {r.get('aplicado', 0)} aplicados, {r.get('ya_aplicado', 0)} ya aplicados, "
             f"{r.get('opcional_no_aplicable', 0)} opcionales no aplicables, "
             f"{r.get('obligatorio_fallido', 0)} obligatorios fallidos · estructuras del SDK: "
             f"{'iguales' if not par.get('estructuras_incompatibles') else 'INCOMPATIBLES'}")
    if rec.get("estado") in ("generado", "reutilizado", "externo"):
        L.append(f"Recursos:     assets.wlpk {rec['estado']} · SHA-256 {_corto(rec.get('sha256'), 16)} · "
                 f"disco {_corto((rec.get('clave') or {}).get('fuente'), 12)} · perfil "
                 f"{(rec.get('clave') or {}).get('perfil', '—')}")
    else:
        L.append(f"Recursos:     SIN assets.wlpk ({rec.get('motivo', 'no se pidió')}): el juego usa las texturas del disco")
    L += ["", "Archivos (SHA-256):"]
    for e in m["archivos"]:
        L.append(f"  {e['sha256']}  {e['bytes']:>11}  {e['ruta']}")
    L += ["", "Comprobar una copia:  python3 -m choya verificar-port <carpeta>   o   scripts/copiar-a-consola.ps1", ""]
    return "\n".join(L)


def verificar(raiz: str, manifiesto: dict | None = None) -> tuple[dict | None, list[str]]:
    """Comprueba los archivos de un port contra su manifiesto. → (manifiesto, problemas). No toca nada."""
    m = manifiesto or huellas.leer_json(os.path.join(raiz, MANIFIESTO))
    if not m:
        return None, [f"falta o no se puede leer {MANIFIESTO}"]
    if m.get("formato") != FORMATO:
        return m, [f"{MANIFIESTO}: formato desconocido ({m.get('formato')})"]
    problemas = []
    for e in m.get("archivos", []):
        ruta = os.path.join(raiz, e["ruta"])
        if not os.path.isfile(ruta):
            problemas.append(f"falta {e['ruta']}")
        elif os.path.getsize(ruta) != e["bytes"]:
            problemas.append(f"distinto {e['ruta']} (mide {os.path.getsize(ruta)} bytes, el manifiesto dice {e['bytes']})")
        elif huellas.sha256_archivo(ruta) != e["sha256"]:
            problemas.append(f"distinto {e['ruta']} (otro SHA-256)")
    if identidad(m.get("archivos", [])) != m.get("identidad"):
        problemas.append("la identidad del manifiesto no corresponde a su lista de archivos")
    return m, problemas


def publicar(nuevo: str, destino: str) -> list[str]:
    """Reemplaza destino por nuevo solo si nuevo está completo. → problemas (vacío si se publicó)."""
    _m, problemas = verificar(nuevo)
    if problemas:
        return problemas
    anterior = destino.rstrip("/\\") + ".anterior"
    if os.path.exists(anterior):
        shutil.rmtree(anterior)
    try:
        if os.path.exists(destino):
            os.replace(destino, anterior)
    except OSError as e:
        return [f"no pude apartar el port anterior en {destino}: {e} (¿una ventana o programa lo tiene abierto?)"]
    try:
        os.replace(nuevo, destino)
    except OSError as e:
        if os.path.exists(anterior) and not os.path.exists(destino):
            os.replace(anterior, destino)
        return [f"no pude poner el port nuevo en {destino}: {e} (el anterior sigue ahí)"]
    shutil.rmtree(anterior, ignore_errors=True)
    return []


def _glibc(port: str) -> str | None:
    ruta = os.path.join(port, CARPETA, "necesita.txt")
    if os.path.isfile(ruta):
        with open(ruta, encoding="utf-8", errors="replace") as f:
            vers = [ln.strip() for ln in f if ln.strip().startswith("GLIBC_")]
        return vers[-1] if vers else None
    return None


def datos_compilacion(a) -> dict:
    """Junta lo que sabe el script de compilación y lo que dejaron los pasos anteriores (archivos JSON)."""
    from . import retarget
    par = huellas.leer_json(os.path.join(a.wc, "projects", "wiisports", "parches.json")) or {}
    parches = {"ok": par.get("ok"), "resumen": par.get("resumen", {}),
               "estructuras_incompatibles": par.get("estructuras_incompatibles"),
               "obligatorios_fallidos": [p for p in par.get("parches", []) if p["estado"] == "obligatorio_fallido"],
               "opcionales_no_aplicables": [p for p in par.get("parches", []) if p["estado"] == "opcional_no_aplicable"],
               "opcionales": sorted(retarget.PARCHES_OPCIONALES)}
    if not par:
        parches["nota"] = "no encontré projects/wiisports/parches.json (recomp-native anterior a 0.32.0)"
    ident = huellas.leer_json(a.identidad) if a.identidad else None
    pgo = huellas.leer_json(a.pgo) if a.pgo else None
    dep = huellas.leer_json(a.deps) if a.deps else None
    despues = huellas.leer_json(a.deps_cambios) if getattr(a, "deps_cambios", None) else None
    if dep and despues:   # el lock como estaba al empezar; los cambios locales, después de recomp-native
        dep["wiicompiled"] = despues.get("wiicompiled", dep.get("wiicompiled"))
    return {
        "dependencias": dep,
        "compilador": {"clang": a.clang, "objetivo": "aarch64-linux-gnu", "cpu": "cortex-a53",
                       "imagen_docker": a.imagen, "glibc_requerida": _glibc(a.port)},
        "compilacion": {"modo": a.modo, "flags_codigo_traducido": a.flags, "enlace_extra": a.enlace,
                        "carpeta": os.path.basename(a.build.rstrip("/")), "cmake": _cmake_cache(a.build)},
        "juego": {"main_dol_sha256": (ident or {}).get("main_dol_sha256"), "codigo_traducido": ident},
        "pgo": pgo,
        "parches": parches,
        "recursos": huellas.leer_json(a.recursos) if a.recursos else {"estado": "sin_paquete", "motivo": "no se pidió"},
    }


def main_crear(a) -> int:
    m = crear(a.port, datos_compilacion(a))
    print(f"Manifiesto: {len(m['archivos'])} archivos, identidad del port {m['identidad'][:16]}")
    return 0


def main_verificar(a) -> int:
    m, problemas = verificar(a.carpeta)
    for p in problemas:
        print(f"ERROR: {p}")
    if problemas:
        print(f"El port en {a.carpeta} NO coincide con su manifiesto ({len(problemas)} problemas)")
        return 1
    print(f"Port verificado: {len(m['archivos'])} archivos iguales al manifiesto (identidad {m['identidad'][:16]})")
    return 0


def main_publicar(a) -> int:
    problemas = publicar(a.nuevo, a.destino)
    for p in problemas:
        print(f"ERROR: {p}")
    if problemas:
        print(f"No se publicó: {a.destino} sigue con el port anterior")
        return 1
    print(f"Publicado en {a.destino}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main_verificar(type("A", (), {"carpeta": sys.argv[1]})))
