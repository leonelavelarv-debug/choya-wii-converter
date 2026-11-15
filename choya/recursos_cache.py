"""Recursos convertidos (assets.wlpk) con caché por contenido (0.32.0).

    python3 -m choya recursos <disco extraído> <carpeta del paquete> -p rg35xxh [--estado estado.json]

El paquete se vuelve a usar solo si coinciden la huella del conversor (su código), las opciones, el perfil, las
versiones de numpy/Pillow/SciPy y el contenido del disco (files/ y sys/, archivo por archivo). Cambiar de disco, de
revisión o tocar un recurso lo invalida aunque la ruta y las fechas sean las mismas. La huella del disco se calcula
aquí, al preparar el port, nunca al abrir el juego.

Se convierte en una carpeta temporal, se valida (el .wlpk se lee entero) y solo entonces reemplaza al anterior. Si la
conversión falla, el paquete anterior se queda en su carpeta pero no se usa (el estado dice "fallido") y el comando
termina con error: nunca se presenta un paquete viejo como recién convertido.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time

from . import huellas

CLAVE = "clave.json"
PAQUETE = "assets.wlpk"


def _versiones_bibliotecas() -> dict:
    from importlib import metadata
    out = {}
    for nombre in ("numpy", "Pillow", "scipy"):
        try:
            out[nombre] = metadata.version(nombre)
        except metadata.PackageNotFoundError:
            out[nombre] = None
    return out


def huella_disco(disco: str) -> dict:
    """Contenido del disco que lee el conversor: files/ y sys/ (con DATA/ o sin ella)."""
    from .scan import _sys_dir, find_files_root
    pares, total = [], 0
    for etiqueta, raiz in (("files", find_files_root(disco)), ("sys", _sys_dir(disco))):
        if not raiz or not os.path.isdir(raiz):
            continue
        for rel in huellas.archivos(raiz):
            ruta = os.path.join(raiz, rel)
            pares.append((f"{etiqueta}/{rel}", huellas.sha256_archivo(ruta)))
            total += os.path.getsize(ruta)
    return {"huella": huellas.huella_lista(pares), "archivos": len(pares), "bytes": total}


def clave(disco: str, perfil: str, opciones: list[str], conversor: dict | None = None) -> dict:
    """Todo lo que decide el contenido del paquete. Dos claves iguales → el mismo paquete."""
    conversor = conversor or huellas.huella_conversor()
    fuente = huella_disco(disco)
    datos = {"conversor": conversor["huella"], "perfil": perfil, "opciones": list(opciones),
             "bibliotecas": _versiones_bibliotecas(), "fuente": fuente["huella"]}
    pares = [(k, str(v)) for k, v in sorted(datos.items())]
    import hashlib
    resumen = hashlib.sha256(repr(pares).encode("utf-8")).hexdigest()
    return {"clave": resumen, **datos, "fuente_archivos": fuente["archivos"], "fuente_bytes": fuente["bytes"],
            "conversor_modulos": conversor["modulos"]}


def validar_paquete(ruta: str) -> tuple[bool, str]:
    """El .wlpk se puede leer entero (cabecera, índice y manifiesto)."""
    from . import pack
    if not os.path.isfile(ruta):
        return False, "no existe"
    if os.path.getsize(ruta) == 0:
        return False, "está vacío"
    try:
        _manifest, entradas = pack.read_pack(ruta)
    except Exception as e:  # noqa: BLE001 - cualquier error de lectura invalida el paquete
        return False, f"no se puede leer ({e})"
    return True, f"{len(entradas)} recursos"


def _publicar_carpeta(nueva: str, destino: str) -> None:
    """Reemplaza destino por nueva sin dejar nunca una carpeta a medias en destino."""
    vieja = destino + ".anterior"
    if os.path.exists(vieja):
        shutil.rmtree(vieja)
    if os.path.exists(destino):
        os.replace(destino, vieja)
    os.replace(nueva, destino)
    if os.path.exists(vieja):
        shutil.rmtree(vieja, ignore_errors=True)


def preparar(disco: str, destino: str, perfil: str, opciones: list[str], conversor_cmd: list[str] | None = None,
             forzar: bool = False, log=print) -> dict:
    """Devuelve el estado: {"estado": "reutilizado"|"generado"|"fallido", "paquete", "sha256", ...}."""
    t0 = time.time()
    k = clave(disco, perfil, opciones)
    log(f"Huella del disco: {k['fuente'][:12]} ({k['fuente_archivos']} archivos, "
        f"{k['fuente_bytes'] / 1048576:.0f} MB, {time.time() - t0:.1f} s) · conversor {k['conversor'][:12]} · "
        f"perfil {perfil}")
    if k["fuente_archivos"] == 0:
        return {"estado": "fallido", "motivo": f"no encontré files/ ni sys/ en {disco}", "clave": k}
    paq = os.path.join(destino, PAQUETE)
    previa = huellas.leer_json(os.path.join(destino, CLAVE))
    if not forzar and previa and previa.get("clave") == k["clave"] and os.path.isfile(paq):
        sha = huellas.sha256_archivo(paq)
        if sha == previa.get("sha256"):
            ok, que = validar_paquete(paq)
            if ok:
                return {"estado": "reutilizado", "paquete": paq, "sha256": sha, "bytes": os.path.getsize(paq),
                        "detalle": que, "clave": k}
            motivo = f"el paquete guardado no se puede leer ({que})"
        else:
            motivo = "el paquete guardado no es el que se convirtió (otro SHA-256)"
    elif previa:
        cambio = [c for c in ("conversor", "perfil", "opciones", "bibliotecas", "fuente") if previa.get(c) != k[c]]
        motivo = "cambió: " + (", ".join(cambio) if cambio else "la clave") if not forzar else "forzado"
    else:
        motivo = "no había paquete convertido"
    log(f"Hay que convertir ({motivo})")
    tmp = f"{destino}.nuevo-{os.getpid()}"
    shutil.rmtree(tmp, ignore_errors=True)
    cmd = (conversor_cmd or [sys.executable, "-m", "choya", "convert"]) + [disco, tmp, "-p", perfil] + list(opciones)
    r = subprocess.run(cmd)
    paq_nuevo = os.path.join(tmp, PAQUETE)
    if r.returncode != 0:
        shutil.rmtree(tmp, ignore_errors=True)
        return {"estado": "fallido", "motivo": f"la conversión terminó con código {r.returncode}", "clave": k}
    ok, que = validar_paquete(paq_nuevo)
    if not ok:
        shutil.rmtree(tmp, ignore_errors=True)
        return {"estado": "fallido", "motivo": f"el paquete convertido {que}", "clave": k}
    sha = huellas.sha256_archivo(paq_nuevo)
    huellas.escribir_json(os.path.join(tmp, CLAVE), {**k, "sha256": sha})
    _publicar_carpeta(tmp, destino)
    return {"estado": "generado", "paquete": paq, "sha256": sha, "bytes": os.path.getsize(paq), "detalle": que,
            "motivo": motivo, "clave": k}


def externo(ruta: str) -> dict:
    """Un assets.wlpk dado a mano (CHOYA_PAQUETE): se valida y se anota su SHA-256, pero no se sabe de qué disco salió."""
    ok, que = validar_paquete(ruta)
    if not ok:
        return {"estado": "fallido", "motivo": f"CHOYA_PAQUETE={ruta}: {que}"}
    return {"estado": "externo", "paquete": os.path.abspath(ruta), "sha256": huellas.sha256_archivo(ruta),
            "bytes": os.path.getsize(ruta), "detalle": que + "; dado a mano, no comprobado contra el disco"}


def main(a) -> int:
    if a.externo:
        est = externo(a.externo)
    else:
        if not os.path.isdir(a.disco):
            est = {"estado": "fallido", "motivo": f"no encontré el disco extraído en {a.disco}"}
        else:
            conv = a.conversor.split() if a.conversor else None
            est = preparar(a.disco, a.destino, a.perfil, a.opcion or [], conversor_cmd=conv, forzar=a.forzar)
    if a.estado:
        huellas.escribir_json(a.estado, est)
    if est["estado"] == "fallido":
        print(f"ERROR: recursos convertidos: {est['motivo']}")
        return 1
    extra = f" ({est['motivo']})" if est.get("motivo") else ""
    print(f"Recursos convertidos: {est['estado']}{extra} · {est['paquete']} · SHA-256 {est['sha256'][:16]} · "
          f"{est['bytes'] / 1048576:.1f} MB · {est.get('detalle', '')}")
    return 0
