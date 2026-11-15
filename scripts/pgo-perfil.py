#!/usr/bin/env python3
"""Arma el perfil PGO (wiisports.profdata) con los perfiles que tomó la consola, sin reutilizar uno viejo por error.

    python3 pgo-perfil.py --crudos DIR --perfil RUTA.profdata --profdata "llvm-profdata-21" \\
        [--clang "Ubuntu clang version 21.x"] [--identidad identidad.json] [--generar generar.json] [--reusar]

1. Prepara los .profraw (pgo-preparar.py: sin vacíos, el último perfil de cada archivo) en una carpeta temporal.
2. `profdata merge` a un archivo temporal; se comprueba el código de salida del comando (no el de una tubería).
3. Lo valida con `profdata show` (que se lea y traiga funciones).
4. Solo entonces reemplaza el perfil anterior (os.replace) y escribe RUTA.json con su SHA-256, la versión de Clang,
   los .profraw usados y la identidad del código traducido.
Si algo falla termina con código 1 y deja el perfil anterior como estaba, pero el script de compilación se detiene:
nunca se compila en silencio con un perfil viejo. Con --reusar (CHOYA_PGO_REUSAR=1) no junta nada y usa el perfil
anterior a propósito, si su SHA-256 coincide con su .json; queda anotado así en el manifiesto.

Avisos (no detienen): el código traducido cambió desde que se compiló pgo-generar (Clang ignora las funciones que
cambiaron: conviene volver a tomar el perfil), o hay .profraw de otra compilación pgo-generar.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))


def _preparar_mod():
    spec = importlib.util.spec_from_file_location("pgo_preparar", os.path.join(AQUI, "pgo-preparar.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha256(ruta: str) -> str:
    h = hashlib.sha256()
    with open(ruta, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def leer_json(ruta):
    try:
        with open(ruta, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError, TypeError):
        return None


def escribir_json(ruta, datos):
    tmp = ruta + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(datos, f, ensure_ascii=False, indent=1)
        f.write("\n")
    os.replace(tmp, ruta)


def clang_mayor(v: str | None) -> str | None:
    m = re.search(r"version (\d+)\.", v or "")
    return m.group(1) if m else None


def correr(cmd: list[str]) -> subprocess.CompletedProcess:
    print("  $ " + " ".join(shlex.quote(c) for c in cmd), flush=True)
    return subprocess.run(cmd, capture_output=True, text=True)


def _id_de(nombre: str) -> str | None:
    """wiisports-<id>-<n>.profraw (0.32.0, el lanzador pone la identidad del código) → id."""
    m = re.match(r"wiisports-([0-9a-f]{12})-", nombre)
    return m.group(1) if m else None


def avisos_identidad(crudos: list[str], ident: dict | None, generar: dict | None) -> list[str]:
    out = []
    actual = (ident or {}).get("huella")
    gen = (generar or {}).get("codigo_traducido")
    if actual and gen and gen != actual:
        out.append("el código traducido cambió desde la última compilación pgo-generar: las funciones que cambiaron "
                   "se compilan sin perfil; para aprovecharlo entero, vuelve a compilar con CHOYA_OPT=pgo-generar y a "
                   "tomar el perfil en la consola")
    ids = {_id_de(os.path.basename(c)) for c in crudos}
    ids.discard(None)
    if gen and ids and ids != {gen[:12]}:
        otros = sorted(i for i in ids if i != gen[:12])
        if otros:
            out.append(f"hay .profraw de otra compilación pgo-generar ({', '.join(otros)}); Clang solo usa lo que "
                       f"sigue igual. Si sobran, bórralos de la carpeta de perfiles")
    if ident and not gen:
        out.append("no sé con qué código se compiló pgo-generar (anterior a 0.32.0): no se puede comprobar si el "
                   "perfil está al día")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--crudos", required=True)
    ap.add_argument("--perfil", required=True)
    ap.add_argument("--profdata", required=True, help="comando de llvm-profdata (puede llevar 'docker run ...')")
    ap.add_argument("--clang", default=None)
    ap.add_argument("--identidad", default=None)
    ap.add_argument("--generar", default=None)
    ap.add_argument("--reusar", action="store_true")
    a = ap.parse_args(argv)

    perfil, meta_ruta = a.perfil, a.perfil + ".json"
    ident = leer_json(a.identidad) if a.identidad else None
    generar = leer_json(a.generar) if a.generar else None
    meta_ant = leer_json(meta_ruta)

    if a.reusar:
        if not os.path.isfile(perfil) or not meta_ant:
            print(f"ERROR: CHOYA_PGO_REUSAR=1 pero no hay un perfil anterior con su {os.path.basename(meta_ruta)}")
            return 1
        sha = sha256(perfil)
        if sha != meta_ant.get("sha256"):
            print("ERROR: el perfil anterior no es el que se validó (otro SHA-256): no se reutiliza")
            return 1
        if a.clang and clang_mayor(a.clang) != clang_mayor(meta_ant.get("clang")):
            print(f"ERROR: el perfil anterior es de Clang {clang_mayor(meta_ant.get('clang'))} y ahora se compila con "
                  f"Clang {clang_mayor(a.clang)}: hay que juntarlo de nuevo (sin CHOYA_PGO_REUSAR)")
            return 1
        meta = dict(meta_ant, estado="reutilizado a propósito (CHOYA_PGO_REUSAR=1)",
                    avisos=avisos_identidad([c["nombre"] for c in meta_ant.get("crudos", [])], ident, generar))
        escribir_json(meta_ruta, meta)
        print(f"Perfil PGO reutilizado a propósito: {sha[:16]}")
        for av in meta["avisos"]:
            print(f"AVISO: {av}")
        return 0

    os.makedirs(os.path.dirname(os.path.abspath(perfil)), exist_ok=True)
    trabajo = perfil + ".trabajo"
    shutil.rmtree(trabajo, ignore_errors=True)
    os.makedirs(trabajo)
    nuevo = os.path.join(trabajo, "nuevo.profdata")
    anterior = f"se conserva el perfil anterior ({meta_ant['sha256'][:12]}) pero no se usa" if meta_ant and \
        os.path.isfile(perfil) else "no hay perfil anterior"

    def falla(msg: str) -> int:
        print(f"ERROR: perfil PGO: {msg}")
        print(f"  {anterior}; la compilación se detiene para no usar un perfil viejo sin saberlo.")
        print("  Si quieres compilar con el perfil anterior a propósito: CHOYA_PGO_REUSAR=1")
        shutil.rmtree(trabajo, ignore_errors=True)
        return 1

    crudos_dir = os.path.join(trabajo, "crudos")
    if _preparar_mod().main(a.crudos, crudos_dir) != 0:
        return falla(f"ningún perfil utilizable en {a.crudos} (¿vacíos o no son de LLVM?)")
    crudos = sorted(os.path.join(crudos_dir, n) for n in os.listdir(crudos_dir) if n.endswith(".profraw"))
    base = shlex.split(a.profdata)
    r = correr(base + ["merge", "-o", nuevo] + crudos)
    sys.stdout.write(r.stdout[-4000:])
    sys.stdout.write(r.stderr[-4000:])
    if r.returncode != 0:
        return falla(f"llvm-profdata merge terminó con código {r.returncode} (¿los .profraw son de otra versión de "
                     f"Clang? se compilan con {a.clang or 'Clang 21'})")
    if not os.path.isfile(nuevo) or os.path.getsize(nuevo) == 0:
        return falla("llvm-profdata merge no escribió el perfil")
    r = correr(base + ["show", nuevo])
    sys.stdout.write("\n".join(r.stdout.strip().splitlines()[-3:]) + "\n")
    if r.returncode != 0:
        return falla(f"el perfil nuevo no se puede leer (llvm-profdata show terminó con código {r.returncode})")
    m = re.search(r"Total functions:\s*(\d+)", r.stdout)
    funciones = int(m.group(1)) if m else None
    if funciones == 0:
        return falla("el perfil nuevo no tiene ninguna función (¿se jugó con la versión pgo-generar?)")

    sha = sha256(nuevo)
    origen = sorted(os.listdir(a.crudos))
    meta = {"sha256": sha, "estado": "nuevo", "funciones": funciones, "clang": a.clang,
            "codigo_traducido": (ident or {}).get("huella"),
            "codigo_pgo_generar": (generar or {}).get("codigo_traducido"),
            "crudos": [{"nombre": os.path.basename(c), "sha256": sha256(c), "bytes": os.path.getsize(c)} for c in crudos],
            "carpeta_crudos": os.path.abspath(a.crudos), "archivos_en_la_carpeta": len(origen)}
    meta["avisos"] = avisos_identidad([c["nombre"] for c in meta["crudos"]], ident, generar)
    if meta_ant and meta_ant.get("sha256") == sha:
        meta["estado"] = "nuevo (igual al anterior)"
    escribir_json(meta_ruta + ".nuevo", meta)
    os.replace(nuevo, perfil)          # primero el perfil, después su descripción: si se corta entre los dos, el
    os.replace(meta_ruta + ".nuevo", meta_ruta)  # SHA-256 no coincide y la próxima compilación lo detecta
    shutil.rmtree(trabajo, ignore_errors=True)
    print(f"Perfil PGO listo: {sha[:16]} · {funciones if funciones is not None else '?'} funciones · "
          f"{len(crudos)} perfiles de la consola")
    for av in meta["avisos"]:
        print(f"AVISO: {av}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
