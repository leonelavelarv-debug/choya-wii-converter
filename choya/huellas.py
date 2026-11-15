"""Huellas (SHA-256) de archivos y carpetas para saber qué se compiló con qué (0.32.0).

Todo se identifica por contenido, nunca por fecha, ruta o nombre: dos carpetas con los mismos archivos dan la misma
huella aunque se hayan copiado o tocado en otro momento. Se usa al preparar el port (en la PC), nunca al abrir el juego.
"""
from __future__ import annotations

import ast
import hashlib
import json
import os

BLOQUE = 1 << 20


def sha256_archivo(ruta: str) -> str:
    h = hashlib.sha256()
    with open(ruta, "rb") as f:
        while True:
            b = f.read(BLOQUE)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def archivos(raiz: str, excluir=()) -> list[str]:
    """Rutas relativas (con /) de todos los archivos bajo raiz, ordenadas. excluir: prefijos relativos a saltar."""
    out = []
    for d, dirs, files in os.walk(raiz):
        dirs.sort()
        for n in sorted(files):
            rel = os.path.relpath(os.path.join(d, n), raiz).replace(os.sep, "/")
            if not any(rel == e or rel.startswith(e.rstrip("/") + "/") for e in excluir):
                out.append(rel)
    return out


def huella_lista(pares) -> str:
    """Huella de una lista de (ruta relativa, sha256): cambia si cambia un archivo, su nombre o si falta/sobra uno."""
    h = hashlib.sha256()
    for rel, sha in sorted(pares):
        h.update(rel.encode("utf-8") + b"\0" + sha.encode("ascii") + b"\n")
    return h.hexdigest()


def huella_carpeta(raiz: str, excluir=()) -> tuple[str, int, int]:
    """(huella, cuántos archivos, bytes) del contenido de una carpeta."""
    pares, total = [], 0
    for rel in archivos(raiz, excluir):
        ruta = os.path.join(raiz, rel)
        pares.append((rel, sha256_archivo(ruta)))
        total += os.path.getsize(ruta)
    return huella_lista(pares), len(pares), total


def identidad_codigo(wiicompiled: str) -> dict:
    """Identidad del código traducido del juego: el contenido de generated/ (lo que escribe el traductor) y el
    main.dol del que salió (su SHA-256 está en projects/wiisports/recomp.yml)."""
    gen = os.path.join(wiicompiled, "generated")
    if not os.path.isdir(gen):
        return {"huella": None, "motivo": "no existe generated/ (falta traducir)"}
    huella, n, total = huella_carpeta(gen)
    dol = None
    yml = os.path.join(wiicompiled, "projects", "wiisports", "recomp.yml")
    if os.path.isfile(yml):
        with open(yml, encoding="utf-8") as f:
            for line in f:
                s = line.strip()
                if s.startswith("sha256:"):
                    dol = s.split(":", 1)[1].strip().strip("'\"") or None
                    break
    return {"huella": huella, "archivos": n, "bytes": total, "main_dol_sha256": dol}


def modulos_de(paquete: str, inicio: str) -> list[str]:
    """Módulos del paquete que importa inicio (y los que importan ellos), leyendo los import relativos: así la huella
    del conversor cambia con su código y no con el de otras partes de Choya (recomp-native, el motor GLES...)."""
    vistos, pendientes = set(), [inicio]
    while pendientes:
        m = pendientes.pop()
        if m in vistos:
            continue
        ruta = os.path.join(paquete, m + ".py")
        if not os.path.isfile(ruta):
            continue
        vistos.add(m)
        with open(ruta, encoding="utf-8") as f:
            arbol = ast.parse(f.read(), ruta)
        for nodo in ast.walk(arbol):
            if isinstance(nodo, ast.ImportFrom) and nodo.level == 1:
                if nodo.module:
                    pendientes.append(nodo.module.split(".")[0])
                else:
                    pendientes.extend(a.name for a in nodo.names)
    return sorted(vistos)


def huella_conversor() -> dict:
    """Huella del código que convierte los recursos (choya convert) y de los datos que lee."""
    paquete = os.path.dirname(os.path.abspath(__file__))
    mods = modulos_de(paquete, "engine")
    pares = [(m + ".py", sha256_archivo(os.path.join(paquete, m + ".py"))) for m in mods]
    return {"huella": huella_lista(pares), "modulos": mods}


def escribir_json(ruta: str, datos) -> None:
    """Escribe un JSON de forma atómica (archivo temporal y os.replace)."""
    tmp = ruta + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(datos, f, ensure_ascii=False, indent=1, sort_keys=False)
        f.write("\n")
    os.replace(tmp, ruta)


def leer_json(ruta: str):
    try:
        with open(ruta, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None
