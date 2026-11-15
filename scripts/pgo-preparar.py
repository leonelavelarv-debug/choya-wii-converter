#!/usr/bin/env python3
"""Prepara los perfiles que tomó la consola (CHOYA_OPT=pgo-generar) para llvm-profdata.

    python3 pgo-preparar.py <carpeta con .profraw> <carpeta de salida>

- Salta los archivos vacíos (el juego se cerró justo cuando el runtime abría el archivo).
- La 0.29.0/0.30.x escribía cada 20 s y el runtime de Clang agrega al final, así que un archivo puede traer varios
  perfiles seguidos, cada uno con lo acumulado hasta ese momento. Sumarlos contaría lo primero muchas veces: de cada
  archivo se toma solo el último perfil completo (el total). Los de la 0.31.0 (%m) ya traen uno solo.
"""
import os
import struct
import sys

MAGIA = struct.pack("<Q", 0xFF6C70726F667281)  # perfil crudo de LLVM, 64 bits


def perfiles(datos: bytes):
    """Posiciones donde empieza cada perfil crudo del archivo (la magia seguida de la misma versión)."""
    if not datos.startswith(MAGIA) or len(datos) < 16:
        return []
    version = datos[8:16]
    pos, i = [], 0
    while i != -1:
        if datos[i + 8:i + 16] == version:
            pos.append(i)
        i = datos.find(MAGIA, i + 8)
    return pos


def main(entrada: str, salida: str) -> int:
    os.makedirs(salida, exist_ok=True)
    usados = 0
    for nombre in sorted(os.listdir(entrada)):
        if not nombre.endswith(".profraw"):
            continue
        ruta = os.path.join(entrada, nombre)
        tam = os.path.getsize(ruta)
        if tam == 0:
            print(f"  {nombre}: vacío, se salta")
            continue
        with open(ruta, "rb") as f:
            datos = f.read()
        pos = perfiles(datos)
        if not pos:
            print(f"  {nombre}: no es un perfil de LLVM, se salta")
            continue
        ultimo = datos[pos[-1]:]
        # El último debe medir como los anteriores (un corte al cerrar el juego lo deja más chico): si no, el penúltimo.
        if len(pos) >= 2 and len(ultimo) < pos[-1] - pos[-2]:
            print(f"  {nombre}: el último perfil está cortado, se usa el anterior")
            ultimo = datos[pos[-2]:pos[-1]]
        with open(os.path.join(salida, nombre), "wb") as f:
            f.write(ultimo)
        extra = f" (traía {len(pos)} seguidos: se toma el último, {len(ultimo) // 1048576} MB)" if len(pos) > 1 else ""
        print(f"  {nombre}: {tam // 1048576} MB{extra}")
        usados += 1
    print(f"perfiles listos: {usados}")
    return 0 if usados else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
