"""Arma la carpeta del port para PortMaster/KNULLI."""
from __future__ import annotations

import json
import os
import shutil
import stat

LAUNCHER = r"""#!/bin/bash
# Lanzador de PortMaster generado por Choya Wii Converter.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/__DIR__"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

if [ ! -f "$GAMEDIR/gamedata/assets.wlpk" ]; then
  pm_message "Falta gamedata/assets.wlpk. Conviértelo en tu PC con: choya convert"
  sleep 5
  pm_finish
  exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export WL_PACK="$GAMEDIR/gamedata/assets.wlpk"
export WL_FILES="$GAMEDIR/gamedata/files"

# Un archivo vacío llamado "verificar" junto al port: wl_view compara cada textura ETC2/EAC como la lee la GPU con
# el decodificador de la CPU, lo escribe en log.txt y sale (sin abrir el visor).
WL_ARGS=""
[ -f "$GAMEDIR/verificar" ] && WL_ARGS="--verificar"

$GPTOKEYB "__BIN__" -c "$GAMEDIR/__BIN__.gptk" &
pm_platform_helper "$GAMEDIR/__BIN__"
./__BIN__ "$WL_PACK" $WL_ARGS

pm_finish
"""

# gptokeyb: Select y Start envían Escape (el visor sale con Escape o con Start).
GPTK = """back = esc
start = esc
"""


def build(out_root: str, title: str, folder: str, binary: str, pack_dir: str | None,
          runtime_bin: str | None, files_dir: str | None) -> str:
    port = os.path.join(out_root, folder)
    gamedata = os.path.join(port, "gamedata")
    os.makedirs(gamedata, exist_ok=True)
    os.makedirs(os.path.join(port, "libs.aarch64"), exist_ok=True)
    launcher = os.path.join(out_root, f"{title}.sh")
    with open(launcher, "w", newline="\n") as f:
        f.write(LAUNCHER.replace("__DIR__", folder).replace("__BIN__", binary))
    os.chmod(launcher, os.stat(launcher).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    with open(os.path.join(port, f"{binary}.gptk"), "w", newline="\n") as f:
        f.write(GPTK)
    port_json = {
        "version": 4,
        "name": f"{folder}.zip",
        "items": [f"{title}.sh", folder],
        "items_opt": [],
        "attr": {
            "title": title,
            "porter": ["Choya Wii Converter"],
            "desc": "Port nativo generado con Choya Wii Converter. Requiere tu propia copia del juego.",
            "inst": "Convierte tu disco extraído en la PC con Choya Wii Converter y copia gamedata/ a la carpeta del port.",
            "genres": ["other"],
            "image": None,
            "rtr": False,
            "arch": ["aarch64"],
            "runtime": None,
            "reqs": [],
        },
    }
    with open(os.path.join(port, "port.json"), "w", encoding="utf-8") as f:
        json.dump(port_json, f, ensure_ascii=False, indent=2)
    if pack_dir:
        for name in ("assets.wlpk", "report.md"):
            src = os.path.join(pack_dir, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(gamedata, name))
    if files_dir:
        dst = os.path.join(gamedata, "files")
        if not os.path.exists(dst):
            shutil.copytree(files_dir, dst)
    if runtime_bin:
        dst = os.path.join(port, binary)
        shutil.copy2(runtime_bin, dst)
        os.chmod(dst, os.stat(dst).st_mode | stat.S_IXUSR)
    with open(os.path.join(port, "LEEME.txt"), "w", encoding="utf-8") as f:
        f.write(
            f"{title} — generado con Choya Wii Converter\n\n"
            "Este port NO incluye datos del juego. Necesitas tu propia copia.\n"
            "1. En la PC: choya convert <disco_extraido> salida/\n"
            f"2. Copia salida/assets.wlpk a ports/{folder}/gamedata/\n"
            f"3. Copia la carpeta files/ del disco extraído a ports/{folder}/gamedata/files/\n"
            "4. Abre el port desde PortMaster.\n"
        )
    return port
