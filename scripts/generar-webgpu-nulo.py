#!/usr/bin/env python3
"""Genera el resto del WebGPU nulo de Choya (choya/data/aurora/sin_dawn/webgpu_nulo_*.inc).

Lee las funciones de la API C de las cabeceras de Dawn (sin_dawn/include/dawn/webgpu.h) y las que ya están
escritas a mano en sin_dawn/webgpu_nulo.cpp; para las demás escribe una versión que no hace nada:

  - AddRef/Release cuentan referencias de verdad (los wgpu::Xxx de C++ dependen de eso);
  - las que devuelven un objeto crean uno nuevo;
  - las que devuelven WGPUStatus devuelven Success; números, enums y punteros, cero.

Las funciones con futuro (WGPUFuture) tienen que estar escritas a mano: llaman a un callback.
Solo hace falta volver a correrlo si cambian las cabeceras de Dawn.

    python3 scripts/generar-webgpu-nulo.py
"""
import os
import re
import sys

RAIZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "choya", "data", "aurora", "sin_dawn")
CABECERA = os.path.join(RAIZ, "include", "dawn", "webgpu.h")
A_MANO = os.path.join(RAIZ, "webgpu_nulo.cpp")

AVISO = ("// Generado por scripts/generar-webgpu-nulo.py a partir de include/dawn/webgpu.h. No editar a mano.\n")


def funciones(cabecera: str) -> list[tuple[str, str, str]]:
    """(tipo de retorno, nombre, parámetros) de cada función exportada."""
    out = []
    for linea in cabecera.splitlines():
        if not linea.startswith("WGPU_EXPORT "):
            continue
        m = re.match(r"WGPU_EXPORT (.+?)\b(wgpu\w+)\((.*)\) WGPU_FUNCTION_ATTRIBUTE;", linea)
        if m is None:
            continue
        out.append((m.group(1).strip(), m.group(2), m.group(3).strip()))
    return out


def objetos(cabecera: str) -> list[str]:
    return re.findall(r"^typedef struct (WGPU\w+)Impl\* WGPU\w+ WGPU_OBJECT_ATTRIBUTE;", cabecera, re.M)


def cuerpo(ret: str, nombre: str, params: str, tipos: set[str]) -> str:
    base = ret.replace("WGPU_NULLABLE", "").strip()
    primero = params.split(",")[0].strip().split()[-1] if params and params != "void" else ""
    if nombre.endswith("AddRef") and base == "void":
        return f"{{ choya_nulo::sumar({primero}); }}"
    if nombre.endswith("Release") and base == "void":
        return f"{{ choya_nulo::soltar({primero}); }}"
    if base == "void":
        return "{}"
    if base in tipos:
        return f"{{ return choya_nulo::nuevo<{base}Impl>(); }}"
    if base == "WGPUStatus":
        return "{ return WGPUStatus_Success; }"
    if base == "WGPUFuture":
        raise SystemExit(f"{nombre}: las funciones con futuro se escriben a mano en webgpu_nulo.cpp")
    if base.endswith("*") or base == "WGPUProc":
        return "{ return nullptr; }"
    return f"{{ return {base}{{}}; }}"


def generar(cabecera: str, mano: str) -> tuple[str, str, str]:
    """(tipos.inc, resto.inc, resumen) a partir de la cabecera de Dawn y de webgpu_nulo.cpp."""
    hechas = set(re.findall(r"^[A-Za-z][\w *]*?\b(wgpu\w+)\([^;]*?\)\s*\{", mano, re.M | re.S))
    tipos = set(objetos(cabecera))
    tipos_mano = set(re.findall(r"^struct (WGPU\w+)Impl : Objeto", mano, re.M))
    tipos_inc = AVISO + "".join(f"struct {t}Impl : Objeto {{}};\n" for t in sorted(tipos - tipos_mano))
    lineas = [AVISO, 'extern "C" {\n']
    n = 0
    for ret, nombre, params in funciones(cabecera):
        if nombre in hechas:
            continue
        lineas.append(f"{ret} {nombre}({params}) {cuerpo(ret, nombre, params, tipos)}\n")
        n += 1
    lineas.append('}  // extern "C"\n')
    resumen = f"{len(hechas)} funciones a mano, {n} generadas, {len(tipos - tipos_mano)} objetos sin datos"
    return tipos_inc, "".join(lineas), resumen


def main() -> int:
    with open(CABECERA, encoding="utf-8") as f:
        cabecera = f.read()
    with open(A_MANO, encoding="utf-8") as f:
        mano = f.read()
    tipos_inc, resto_inc, resumen = generar(cabecera, mano)
    with open(os.path.join(RAIZ, "webgpu_nulo_tipos.inc"), "w", encoding="utf-8") as f:
        f.write(tipos_inc)
    with open(os.path.join(RAIZ, "webgpu_nulo_resto.inc"), "w", encoding="utf-8") as f:
        f.write(resto_inc)
    print(resumen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
