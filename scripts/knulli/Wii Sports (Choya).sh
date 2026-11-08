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
# Perfil: a los 20 s empieza a tomar muestras durante 35 s y deja perfil.txt (en la PC: choya perfil perfil.txt).
export CHOYA_PERFIL=20,35
# Motor de Choya, paso 1: pantalla GLES propia sobre el framebuffer de la Mali (anota choya-gles.txt).
export CHOYA_GLES=1
# Diagnóstico: 1 = cada dibujo de un color distinto; 0 = colores reales del rasterizador.
export CHOYA_GX_COLORES=0
# Vuelca estos cuadros del motor a cuadro-<n>.bin (aviso de salud ~10 s, título ~40 s) para revisarlos en la PC.
export CHOYA_GX_VOLCADO=600,2400
rm -f "$DIR"/cuadro-*.bin
rm -f "$DIR/choya-gles.txt"
rm -f "$DIR/perfil.txt"

CPUF=/sys/devices/system/cpu/cpu0/cpufreq
{
  echo "--- procesador"
  echo "gobernador: $(cat $CPUF/scaling_governor 2>/dev/null)  MHz actual/max: $(( $(cat $CPUF/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))/$(( $(cat $CPUF/cpuinfo_max_freq 2>/dev/null || echo 0) / 1000 ))"
} > "$DIR/cpu.txt" 2>&1

timeout 90 ./WiiCompiled > "$DIR/run.log" 2>&1 &
TPID=$!
# A los 60 s: cuánto procesador gastó cada hilo (100 ticks = 1 s) y a qué frecuencia iba el procesador.
sleep 60
PID=""
for p in /proc/[0-9]*; do [ "$(cat "$p/comm" 2>/dev/null)" = "WiiCompiled" ] && PID="${p#/proc/}"; done
{
  echo "MHz a los 60 s: $(( $(cat $CPUF/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))"
  echo "--- hilos (ticks de CPU en 60 s; 6000 = un nucleo al 100%)"
  if [ -n "$PID" ]; then
    for t in /proc/$PID/task/*; do
      s=$(cat "$t/stat" 2>/dev/null) || continue
      set -- ${s##*) }
      echo "$(( ${12} + ${13} )) $(cat "$t/comm" 2>/dev/null)"
    done | sort -nr | head -12
    echo "memoria: $(grep -E 'VmRSS' /proc/$PID/status 2>/dev/null)"
  else
    echo "WiiCompiled ya no estaba corriendo"
  fi
} >> "$DIR/cpu.txt" 2>&1
wait $TPID
STATUS=$?
{
  echo "codigo de salida: $STATUS (124 = siguio corriendo hasta el limite de 90 s)"
  [ -f "$DIR/perfil.txt" ] && echo "perfil.txt: $(head -2 "$DIR/perfil.txt" | tail -1)" || echo "perfil.txt: no se escribio"
  cat "$DIR/cpu.txt"
  echo "--- cuadros volcados"
  ls -la "$DIR"/cuadro-*.bin 2>/dev/null || echo "ninguno"
  echo "--- pantalla GLES de Choya"
  [ -f "$DIR/choya-gles.txt" ] && cat "$DIR/choya-gles.txt" || echo "choya-gles.txt no se escribio"
  echo "--- cuadros por segundo"
  grep -E "\[vi\] fps" "$DIR/run.log" | tail -15
  echo "--- errores"
  grep -iE "fatal|error|abort|AccessViolation|choya\]|not found|cannot open" "$DIR/run.log" | head -20
  echo "--- final del log"
  tail -15 "$DIR/run.log"
} > "$DIR/resumen.txt" 2>&1
sync
