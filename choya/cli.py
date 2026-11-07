"""Línea de comandos de Choya Wii Converter."""
from __future__ import annotations

import argparse
import json
import os
import sys

from . import __version__, engine, pack, portmaster, profiles, scan as sc
from .optimize import OUT_NAMES


def cmd_convert(a):
    prof = profiles.get(a.profile)
    if not os.path.isdir(a.src):
        sys.exit(f"no existe la carpeta {a.src}")
    engine.run(a.src, a.out, prof, jobs=a.jobs, preview=a.preview, measure=not a.no_psnr)
    print(f"Reporte: {os.path.join(a.out, 'report.md')}")


def cmd_scan(a):
    res = sc.scan(a.src)
    print(f"Raíz de archivos: {res.root}")
    d = sc.disc_info(a.src)
    print(f"ID del juego: {d['game_id'] or 'desconocido'}  revisión: {d['revision']}")
    if d["dol_sha1"]:
        estado = "coincide con ogws (US Rev 1)" if d["ogws_match"] else "NO coincide con ogws (necesita US Rev 1)"
        print(f"main.dol SHA-1: {d['dol_sha1']}  -> {estado}")
    print(f"{res.files} archivos, {res.disc_bytes / 1048576:.1f} MiB")
    for k, v in sorted(res.counts.items()):
        print(f"  {k:28s} {v}")
    print(f"Texturas encontradas: {len(res.textures)}  Audios: {len(res.audio)}")
    for e in res.errors[:20]:
        print("  error:", e)


def cmd_info(a):
    manifest, entries = pack.read_pack(a.pack)
    flags = manifest.pop("_flags")
    print(json.dumps(manifest, ensure_ascii=False, indent=1))
    print(f"banderas: {flags}  entradas: {len(entries)}")
    if a.list:
        for e in entries:
            if e["kind"] == pack.KIND_TEXTURE:
                print(f"TEX {e['key']:016x} {OUT_NAMES.get(e['fmt'], e['fmt']):14s} "
                      f"{e['width']}x{e['height']} m{e['count']} {e['size']:8d} B  {e['name']}")
            elif e["kind"] == pack.KIND_MODEL:
                print(f"MDL {e['key']:016x} {e['extra0']} vért. {e['extra2']} tri. {e['count']} submallas  {e['name']}")
            else:
                kind = "DSP-ADPCM" if e["fmt"] == pack.AUDIO_DSPADPCM else "PCM16"
                print(f"AUD {e['key']:016x} {kind} {e['extra0']} Hz {e['count']} can. "
                      f"{e['size']:8d} B  {e['name']}")


def cmd_package(a):
    port = portmaster.build(a.out, a.title, a.folder, a.binary, a.pack_dir, a.runtime, a.files)
    print(f"Port armado en {port}")


def cmd_profiles(_a):
    for p in profiles.PROFILES.values():
        print(f"{p.name:10s} {p.description}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="choya",
                                 description="Optimiza los recursos de juegos de Wii para la Anbernic RG35XX H.")
    ap.add_argument("--version", action="version", version=f"Choya Wii Converter {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert", help="convierte un disco extraído en un paquete .wlpk")
    c.add_argument("src", help="carpeta de 'Extraer disco completo' de Dolphin (o su carpeta files/)")
    c.add_argument("out", help="carpeta de salida")
    c.add_argument("-p", "--profile", default="rg35xxh", help="rg35xxh | ligero | calidad | gles2")
    c.add_argument("-j", "--jobs", type=int, default=0, help="procesos en paralelo (0 = todos los núcleos)")
    c.add_argument("--preview", action="store_true", help="guarda PNG y WAV de los resultados para revisarlos")
    c.add_argument("--no-psnr", action="store_true", help="no mide la calidad (más rápido)")
    c.set_defaults(fn=cmd_convert)

    s = sub.add_parser("scan", help="solo analiza el disco extraído")
    s.add_argument("src")
    s.set_defaults(fn=cmd_scan)

    i = sub.add_parser("info", help="muestra el contenido de un paquete .wlpk")
    i.add_argument("pack")
    i.add_argument("-l", "--list", action="store_true")
    i.set_defaults(fn=cmd_info)

    k = sub.add_parser("package", help="arma la carpeta del port para PortMaster")
    k.add_argument("out", help="carpeta 'ports' de destino")
    k.add_argument("--title", default="Wii Sports")
    k.add_argument("--folder", default="wiisports")
    k.add_argument("--binary", default="wl_game")
    k.add_argument("--pack-dir", help="carpeta de salida de 'convert'")
    k.add_argument("--runtime", help="binario aarch64 del runtime")
    k.add_argument("--files", help="carpeta files/ del disco extraído (se copia a gamedata/files)")
    k.set_defaults(fn=cmd_package)

    pr = sub.add_parser("profiles", help="lista los perfiles de optimización")
    pr.set_defaults(fn=cmd_profiles)

    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
