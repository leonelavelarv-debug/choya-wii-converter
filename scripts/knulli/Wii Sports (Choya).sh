#!/bin/bash
# Choya Wii Converter - Wii Sports recompilado (WiiCompiled/BrawlRecompiled) con el motor GLES de Choya.
# Para jugar. Select + Start sostenidos 1.5 s cierran el juego.
# Modo diagnóstico (90 s, perfil, volcados de cuadros): "Wii Sports (Choya) - diagnostico.sh" o CHOYA_MODO=diagnostico.
# Deja run.log y resumen.txt en la carpeta wiisports-choya.

DIR="$(cd "$(dirname "$0")" && pwd)/wiisports-choya"
cd "$DIR" || exit 1
rm -f "$DIR/resumen.txt"  # que nunca quede uno viejo de otra corrida
# La carpeta del disco puede llamarse DATA, data o venir anidada (game/DATA/DATA): se busca sys/boot.bin.
GAME="$DIR/game/DATA"
if [ ! -f "$GAME/sys/boot.bin" ]; then
  BOOT="$(find "$DIR/game" -maxdepth 4 -type f -iname boot.bin -ipath '*/sys/boot.bin' 2>/dev/null | head -1)"
  [ -n "$BOOT" ] && GAME="$(dirname "$(dirname "$BOOT")")"
fi
if [ ! -f "$GAME/sys/boot.bin" ] && [ ! -f "$GAME/SYS/BOOT.BIN" ]; then
  {
    echo "Falta el juego: copia la carpeta DATA de tu disco (la que tiene files y sys) en $DIR/game/"
    echo "Debe quedar: $DIR/game/DATA/sys/boot.bin"
    echo "Lo que hay ahora en game/:"
    find "$DIR/game" -maxdepth 3 2>/dev/null | sed "s|$DIR/||" | head -30
  } > "$DIR/resumen.txt"
  exit 1
fi
MODO="${CHOYA_MODO:-jugar}"

# Perfil de controles de PortMaster (igual que cualquier port): da SDL_GAMECONTROLLERCONFIG con el mapeo del
# mando de la consola. Se busca antes de cambiar XDG_DATA_HOME (en KNULLI PortMaster vive en ~/.local/share).
PM_XDG="${XDG_DATA_HOME:-$HOME/.local/share}"
controlfolder=""
for cf in /opt/system/Tools/PortMaster /opt/tools/PortMaster "$PM_XDG/PortMaster" /userdata/system/.local/share/PortMaster \
          /roms/ports/PortMaster /userdata/roms/ports/PortMaster; do
  [ -f "$cf/control.txt" ] && { controlfolder="$cf"; break; }
done
if [ -n "$controlfolder" ]; then
  # shellcheck disable=SC1090,SC1091
  source "$controlfolder/control.txt" >/dev/null 2>&1
  [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt" >/dev/null 2>&1
  get_controls >/dev/null 2>&1
  [ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
fi
cd "$DIR" || exit 1  # por si control.txt cambió de carpeta

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
# La ventana offscreen nunca tiene el foco del teclado y SDL descarta los botones del mando de una ventana sin
# foco: con esto los acepta igual.
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
export NSMBW_LOG_FPS=1
export WIICOMPILED_NO_POPUP=1
# Motor de Choya: pantalla GLES propia sobre el framebuffer de la Mali (anota choya-gles.txt).
export CHOYA_GLES=1
# 1 = cada dibujo de un color distinto (diagnóstico); 0 = colores reales.
export CHOYA_GX_COLORES=0
rm -f "$DIR/choya-gles.txt"

CPUF=/sys/devices/system/cpu/cpu0/cpufreq
{
  echo "--- procesador"
  echo "gobernador: $(cat $CPUF/scaling_governor 2>/dev/null)  MHz actual/max: $(( $(cat $CPUF/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))/$(( $(cat $CPUF/cpuinfo_max_freq 2>/dev/null || echo 0) / 1000 ))"
  echo "juego: $GAME"
  echo "PortMaster: ${controlfolder:-no encontrado}  dispositivo: ${DEVICE_NAME:-?}  CFW: ${CFW_NAME:-?}"
  echo "mapeo del mando: ${SDL_GAMECONTROLLERCONFIG:-ninguno}" | cut -c1-200
  echo "dispositivos de entrada: $(grep -h "^N: Name=" /proc/bus/input/devices 2>/dev/null | cut -d= -f2- | tr "\n" " ")"
} > "$DIR/cpu.txt" 2>&1

escribir_resumen() {
  {
    echo "modo: $MODO"
    echo "codigo de salida: ${STATUS:-sigue corriendo} (124 = siguio corriendo hasta el limite de 90 s)"
    [ -f "$DIR/perfil.txt" ] && echo "perfil.txt: $(head -2 "$DIR/perfil.txt" | tail -1)"
    cat "$DIR/cpu.txt"
    echo "--- cuadros volcados"
    ls -la "$DIR"/cuadro-*.bin 2>/dev/null || echo "ninguno"
    echo "--- pantalla GLES de Choya"
    [ -f "$DIR/choya-gles.txt" ] && grep -v "extensiones:" "$DIR/choya-gles.txt" | tail -40 || echo "choya-gles.txt no se escribio"
    echo "--- cuadros por segundo"
    grep -E "\[vi\] fps" "$DIR/run.log" | tail -15
    echo "--- errores"
    grep -iE "fatal|error|abort|AccessViolation|choya\]|not found|cannot open" "$DIR/run.log" | head -20
    echo "--- final del log"
    tail -15 "$DIR/run.log"
  } > "$DIR/resumen.txt" 2>&1
}

if [ "$MODO" = "diagnostico" ]; then
  # Perfil: a los 20 s empieza a tomar muestras durante 35 s y deja perfil.txt (en la PC: choya perfil perfil.txt).
  export CHOYA_PERFIL=20,35
  # Vuelca estos cuadros del motor a cuadro-<n>.bin (aviso ~10 s, título ~40 s) para revisarlos en la PC.
  export CHOYA_GX_VOLCADO=600,2400
  rm -f "$DIR"/cuadro-*.bin "$DIR/perfil.txt"
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
else
  ./WiiCompiled > "$DIR/run.log" 2>&1 &
  JPID=$!
  # Un resumen a los 10 s, por si el juego se cierra con la tecla de salida del sistema (mata este script).
  sleep 10
  escribir_resumen; sync
  wait $JPID
  STATUS=$?
fi

escribir_resumen
sync
