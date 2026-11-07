#!/bin/bash
# Choya Wii Converter - Wii Sports recompilado (WiiCompiled/BrawlRecompiled) para la RG35XX H.
# Prueba 1: gráficos "null" (sin imagen). Mide cuántos cuadros por segundo da el procesador; debería oírse el juego.
# Deja run.log y resumen.txt en la carpeta wiisports-choya.

DIR="$(cd "$(dirname "$0")" && pwd)/wiisports-choya"
cd "$DIR" || exit 1
GAME="$DIR/game/DATA"
[ -f "$GAME/sys/boot.bin" ] || { echo "Falta $GAME (copia la carpeta DATA de tu disco)" > "$DIR/resumen.txt"; exit 1; }

export XDG_DATA_HOME="$DIR/userdata"
CFG="$XDG_DATA_HOME/WiiCompiled/Config.toml"
mkdir -p "$(dirname "$CFG")"
cat > "$CFG" <<CONF
[paths]
dvd_root = "$GAME"

[video]
graphics_api = "null"
widescreen = false
force_16_9 = false
resolution_multiplier = 1
CONF

export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
export SDL_VIDEO_DRIVER=offscreen
export SDL_AUDIO_DRIVER=alsa
export NSMBW_LOG_FPS=1
export WIICOMPILED_NO_POPUP=1

timeout 90 ./WiiCompiled > "$DIR/run.log" 2>&1
STATUS=$?
{
  echo "codigo de salida: $STATUS (124 = siguio corriendo hasta el limite de 90 s)"
  echo "--- cuadros por segundo"
  grep -E "\[vi\] fps" "$DIR/run.log" | tail -15
  echo "--- errores"
  grep -iE "fatal|error|abort|AccessViolation|choya\]|not found|cannot open" "$DIR/run.log" | head -20
  echo "--- final del log"
  tail -15 "$DIR/run.log"
} > "$DIR/resumen.txt" 2>&1
sync
