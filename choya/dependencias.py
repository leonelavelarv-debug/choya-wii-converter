"""Revisiones de WiiCompiled y aurora con las que se compila el port, y sus cambios locales (0.32.0).

    python3 -m choya dependencias <WiiCompiled> [--json salida.json] [--actualizar]

- WiiCompiled (con aurora-main adentro) se identifica por el commit de su repositorio git.
- La primera compilación fija ese commit en <WiiCompiled>/choya-dependencias.lock. Si después el commit cambia, la
  compilación se detiene y explica cómo volver o cómo aceptar el nuevo (--actualizar / CHOYA_DEPS_ACTUALIZAR=1): así
  un `git pull` en WiiCompiled no cambia el port sin que se note. Choya nunca hace checkout ni restaura nada.
- dependencias.json (en Choya) dice con qué commit se probaron los parches; si es otro solo se avisa, porque los
  parches obligatorios ya comprueban su texto uno por uno.
- Los cambios locales se separan en los que hace Choya (runtime-wiisports, projects/wiisports, generated, las
  carpetas de compilación y los parches de aurora, comprobados deshaciéndolos y comparando con el commit) y los
  ajenos. Con cambios ajenos el commit solo no basta para repetir la compilación, y el manifiesto lo dice.
"""
from __future__ import annotations

import os
import subprocess

from . import __version__, huellas

LOCK = "choya-dependencias.lock"
PROBADAS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dependencias.json")

# Lo que genera o compila Choya dentro de WiiCompiled (relativo a la carpeta de WiiCompiled).
GENERADO_POR_CHOYA = ("runtime-wiisports/", "projects/wiisports/", "generated/", "build/", "build-wiisports",
                      "choya-pgo/", LOCK, "aurora-main/lib/choya/")
# Lo que compila WiiCompiled mismo (dotnet) y no cambia el código fuente.
COMPILACION = ("translator/", "Launcher/")


def _git(raiz: str, *args: str, binario: bool = False, crudo: bool = False):
    r = subprocess.run(["git", "-C", raiz, *args], capture_output=True)
    if r.returncode != 0:
        return None
    if binario:
        return r.stdout
    texto = r.stdout.decode("utf-8", "replace")
    return texto if crudo else texto.strip()


def _deshacer_aurora(texto: str, rel: str) -> str:
    """El archivo de aurora sin los cambios que le hace Choya (AURORA_EDITS, AURORA_EXCLUSIVOS)."""
    from . import retarget as R
    for r, old, new, _label in reversed(R.AURORA_EDITS):
        if r == rel and new in texto:
            texto = texto.replace(new, old, 1)
    for r, header, _names in R.AURORA_EXCLUSIVOS:
        if r == rel:
            texto = texto.replace(f'#include "{header}"  // choya: hilo GX\n', "")
            texto = texto.replace("\n  " + R._EXCLUSIVO + "  // choya: hilo GX", "")
            texto = texto.replace(" " + R._EXCLUSIVO + " ", " ")
    return texto


def _clasificar(wc: str, raiz: str, prefijo: str, linea: str) -> tuple[str, str]:
    """(categoría, ruta relativa a WiiCompiled) de una línea de git status --porcelain."""
    from . import retarget as R
    estado, ruta = linea[:2], linea[3:]
    if " -> " in ruta:
        ruta = ruta.split(" -> ", 1)[1]
    ruta = ruta.strip('"')
    if prefijo and not (ruta == prefijo.rstrip("/") or ruta.startswith(prefijo)):
        return "fuera", ruta
    rel = ruta[len(prefijo):] if prefijo else ruta
    if any(rel == g.rstrip("/") or rel.startswith(g) for g in GENERADO_POR_CHOYA):
        return "choya", rel
    if any(rel.startswith(c) and ("/bin/" in rel or "/obj/" in rel) for c in COMPILACION):
        return "compilacion", rel
    if rel.startswith("aurora-main/") and estado.strip() == "M" and rel[len("aurora-main/"):] in R.archivos_aurora():
        actual_ruta = os.path.join(wc, rel)
        original = _git(raiz, "show", f"HEAD:{ruta}", binario=True)
        if original is not None and os.path.isfile(actual_ruta):
            with open(actual_ruta, "rb") as f:
                actual = f.read().decode("utf-8", "surrogateescape").replace("\r\n", "\n")
            orig = original.decode("utf-8", "surrogateescape").replace("\r\n", "\n")
            if _deshacer_aurora(actual, rel[len("aurora-main/"):]) == orig:
                return "choya", rel
        return "ajeno", rel
    return "ajeno", rel


def estado(wc: str) -> dict:
    wc = os.path.abspath(wc)
    out = {"wiicompiled": {"carpeta": wc}}
    w = out["wiicompiled"]
    raiz = _git(wc, "rev-parse", "--show-toplevel")
    if not raiz:
        # Sin git no hay commit: se identifica runtime/ por contenido (Choya no lo toca) y se avisa.
        rt = os.path.join(wc, "runtime")
        h = None
        if os.path.isdir(rt):
            fuera = [n for n in os.listdir(rt) if n.startswith("build") or n == ".git"]
            h = huellas.huella_carpeta(rt, excluir=fuera)[0]
        w.update({"git": False, "commit": None, "runtime_huella": h, "reproducible": False,
                  "nota": "WiiCompiled no es un repositorio git: no se puede fijar su revisión"})
        return out
    raiz = os.path.abspath(raiz)
    prefijo = os.path.relpath(wc, raiz).replace(os.sep, "/")
    prefijo = "" if prefijo == "." else prefijo + "/"
    w.update({"git": True, "raiz_git": raiz, "commit": _git(raiz, "rev-parse", "HEAD"),
              "remoto": _git(raiz, "config", "--get", "remote.origin.url"),
              "rama": _git(raiz, "rev-parse", "--abbrev-ref", "HEAD")})
    st = _git(raiz, "status", "--porcelain=v1", "--untracked-files=normal", crudo=True)  # sin strip: columnas fijas
    cambios = {"choya": [], "compilacion": [], "ajeno": [], "fuera": []}
    for linea in (st or "").splitlines():
        if len(linea) > 3:
            cat, rel = _clasificar(wc, raiz, prefijo, linea)
            cambios[cat].append(f"{linea[:2].strip() or '?'} {rel}")
    w["cambios_choya"] = len(cambios["choya"])
    w["cambios_ajenos"] = cambios["ajeno"]
    w["fuera_de_wiicompiled"] = len(cambios["fuera"])
    if cambios["ajeno"]:
        pares = []
        for item in cambios["ajeno"]:
            rel = item.split(" ", 1)[1]
            p = os.path.join(wc, rel)
            pares.append((rel, huellas.sha256_archivo(p) if os.path.isfile(p) else "no-es-archivo"))
        w["cambios_ajenos_huella"] = huellas.huella_lista(pares)
    w["reproducible"] = not cambios["ajeno"]
    w["nota"] = ("el commit identifica el código: solo hay cambios hechos por Choya" if not cambios["ajeno"] else
                 f"hay {len(cambios['ajeno'])} cambios locales que no son de Choya: el commit solo no basta para "
                 f"repetir esta compilación")
    w["aurora"] = {"carpeta": "aurora-main", "revision": "la del commit de WiiCompiled (viene dentro del repositorio)"}
    return out


def probadas() -> dict:
    return huellas.leer_json(PROBADAS) or {}


def comprobar(wc: str, actualizar: bool = False) -> tuple[bool, dict, list[str]]:
    """Compara con el lock (lo crea la primera vez). → (ok, estado, mensajes)."""
    est = estado(wc)
    w = est["wiicompiled"]
    msgs = []
    lock_ruta = os.path.join(wc, LOCK)
    lock = huellas.leer_json(lock_ruta)
    actual = {"wiicompiled_commit": w.get("commit"), "remoto": w.get("remoto"),
              "runtime_huella": w.get("runtime_huella"), "fijado_con_choya": __version__}
    clave = "wiicompiled_commit" if w.get("git") else "runtime_huella"
    ok = True
    if lock is None or actualizar:
        if lock is not None and lock.get(clave) != actual[clave]:
            msgs.append(f"WiiCompiled: se fija la revisión nueva {str(actual[clave])[:12]} "
                        f"(antes {str(lock.get(clave))[:12]})")
        huellas.escribir_json(lock_ruta, actual)
        est["lock"] = {"estado": "fijado ahora" if lock is None else "actualizado", **actual}
    elif lock.get(clave) != actual[clave]:
        ok = False
        antes, ahora = str(lock.get(clave)), str(actual[clave])
        volver = (f"git -C {w.get('raiz_git')} checkout {antes}" if w.get("git") else
                  "vuelve a poner el runtime/ con el que se fijó")
        msgs.append(f"WiiCompiled cambió desde que se fijó: {antes[:12]} → {ahora[:12]}. Para volver: {volver}. "
                    f"Si lo actualizaste a propósito: CHOYA_DEPS_ACTUALIZAR=1 (o python3 -m choya dependencias "
                    f"{wc} --actualizar), y revisa que recomp-native no tenga parches fallidos.")
        est["lock"] = {"estado": "distinto", **lock}
    else:
        est["lock"] = {"estado": "igual", **lock}
    prob = probadas().get("wiicompiled", {})
    if w.get("git") and prob.get("commit") and w.get("commit") and not w["commit"].startswith(prob["commit"]):
        msgs.append(f"AVISO: WiiCompiled {w['commit'][:12]} no es la revisión con la que se probaron los parches de "
                    f"Choya ({prob['commit'][:12]}); los parches obligatorios lo comprueban uno por uno")
        est["probada"] = False
    elif prob.get("commit"):
        est["probada"] = bool(w.get("git"))
    if w.get("cambios_ajenos"):
        msgs.append("AVISO: " + w["nota"] + ": " + ", ".join(w["cambios_ajenos"][:8])
                    + (" …" if len(w["cambios_ajenos"]) > 8 else ""))
    if not w.get("git"):
        msgs.append("AVISO: " + w["nota"])
    return ok, est, msgs


def main(a) -> int:
    ok, est, msgs = comprobar(a.wiicompiled, actualizar=a.actualizar)
    w = est["wiicompiled"]
    print(f"WiiCompiled: {w.get('commit') or 'sin git'} ({est['lock']['estado']}) · cambios de Choya: "
          f"{w.get('cambios_choya', '—')} · ajenos: {len(w.get('cambios_ajenos', []))}")
    for m in msgs:
        print(m if m.startswith("AVISO") else ("ERROR: " + m if not ok else m))
    if a.json:
        huellas.escribir_json(a.json, est)
    return 0 if ok else 4
