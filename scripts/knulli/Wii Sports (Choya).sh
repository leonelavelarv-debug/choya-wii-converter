#!/bin/bash
# Choya Wii Converter - Wii Sports recompilado (WiiCompiled/BrawlRecompiled) con el motor GLES de Choya.
# Para jugar. Salir: MENU + START (KNULLI, muOS) o SELECT + START (ArkOS, dArkOS). Controles como en Nebula.
# Modo diagnóstico (3 min, perfil, volcados de cuadros; MENU + R1 vuelca el cuadro que se ve): "Wii Sports (Choya) - diagnostico.sh" o CHOYA_MODO=diagnostico.
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
# El perfil de controles según el firmware, igual que en Nebula (Super Mario Galaxy):
#   KNULLI y muOS (Anbernic RG con H700): salir con MENU + START; ArkOS y dArkOS: SELECT + START; el resto: los dos.
#   CHOYA_FACE_LAYOUT=positions|labels  labels si en tu firmware A y B salen al revés (A debe ser el de la derecha)
#   CHOYA_QUIT_COMBO=menu|select|both   fuerza la combinación para salir
cfw=$(printf '%s' "${CFW_NAME:-}" | tr '[:upper:]' '[:lower:]')
case "$cfw" in
  *knulli*|*muos*) quit_combo=menu ;;
  *arkos*|*darkos*) quit_combo=select ;;
  *) quit_combo=both ;;
esac
export CHOYA_QUIT_COMBO="${CHOYA_QUIT_COMBO:-$quit_combo}"
export CHOYA_FACE_LAYOUT="${CHOYA_FACE_LAYOUT:-positions}"
# Se guarda el mapeo completo; si otro día no aparece PortMaster se usa el último bueno.
if [ -n "$SDL_GAMECONTROLLERCONFIG" ]; then
  printf "%s\n" "$SDL_GAMECONTROLLERCONFIG" > "$DIR/mapeo-sdl.txt"
elif [ -s "$DIR/mapeo-sdl.txt" ]; then
  export SDL_GAMECONTROLLERCONFIG="$(cat "$DIR/mapeo-sdl.txt")"
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
# Ojo: no exportar SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS; aurora ya lo activa y si la variable existe su ventana falla.
unset SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS
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
  echo "gptokeyb: ${GPTOKEYB:-no}  salir: $CHOYA_QUIT_COMBO + START  botones: $CHOYA_FACE_LAYOUT"
  echo "dispositivos de entrada: $(grep -h "^N: Name=" /proc/bus/input/devices 2>/dev/null | cut -d= -f2- | tr "\n" " ")"
} > "$DIR/cpu.txt" 2>&1

escribir_resumen() {
  {
    echo "modo: $MODO"
    echo "codigo de salida: ${STATUS:-sigue corriendo} (124 = siguio corriendo hasta el limite del modo diagnostico)"
    [ -f "$DIR/perfil.txt" ] && echo "perfil.txt: $(head -2 "$DIR/perfil.txt" | tail -1)"
    cat "$DIR/cpu.txt"
    echo "--- cuadros volcados"
    ls -la "$DIR"/cuadro-*.bin 2>/dev/null || echo "ninguno"
    echo "--- pantalla GLES de Choya"
    [ -f "$DIR/choya-gles.txt" ] && grep -v "extensiones:" "$DIR/choya-gles.txt" | tail -40 || echo "choya-gles.txt no se escribio"
    echo "--- cuadros por segundo"
    grep -E "\[vi\] fps" "$DIR/run.log" | tail -15
    echo "--- mandos"
    grep -hE "mandos|gamepad|joystick|\[input\]|volcado" "$DIR/choya-gles.txt" "$DIR/run.log" 2>/dev/null | grep -v "^\[choya-gles\]" | head -60
    echo "--- errores"
    grep -iE "fatal|error|abort|AccessViolation|choya\]|not found|cannot open" "$DIR/run.log" | head -20
    echo "--- final del log"
    tail -15 "$DIR/run.log"
  } > "$DIR/resumen.txt" 2>&1
}

# Salir: la combinación del firmware con Start (el juego la atiende y se cierra limpio; gptokeyb mata con la misma
# si no contesta): MENU (HOTKEY=guide) en KNULLI y muOS; SELECT (el de gptokeyb sin HOTKEY) en ArkOS y el resto.
iniciar_gptokeyb() {
  GPID=""
  if [ "$CHOYA_QUIT_COMBO" = menu ]; then export HOTKEY=guide; else unset HOTKEY; fi
  if [ -n "$GPTOKEYB" ]; then
    $GPTOKEYB "WiiCompiled" > "$DIR/gptokeyb.log" 2>&1 &
    GPID=$!
  fi
  command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$DIR/WiiCompiled" >/dev/null 2>&1
}
parar_gptokeyb() {
  [ -n "$GPID" ] && kill $GPID 2>/dev/null
  if command -v pm_finish >/dev/null 2>&1; then pm_finish >/dev/null 2>&1; else pkill -f gptokeyb 2>/dev/null; fi
}
# Resumen cada 10 s mientras corre, por si el juego se cierra con la tecla de salida del sistema.
esperar_juego() {
  while kill -0 "$1" 2>/dev/null; do
    sleep 10
    kill -0 "$1" 2>/dev/null && { escribir_resumen; sync; }
  done
}

if [ "$MODO" = "diagnostico" ]; then
  # Perfil: a los 20 s empieza a tomar muestras durante 35 s y deja perfil.txt (en la PC: choya perfil perfil.txt).
  export CHOYA_PERFIL=20,35
  # Vuelca estos cuadros del motor a cuadro-<n>.bin para revisarlos en la PC (aviso ~10 s, título ~40 s y luego
  # cada ~40 s, por si MENU + R1 no responde). MENU + R1 vuelca además el cuadro que se ve.
  export CHOYA_GX_VOLCADO=600,2400,4000,5600,7200,8800
  rm -f "$DIR"/cuadro-*.bin "$DIR/perfil.txt"
  # -k 5: si a los 3 min no se cierra con la señal normal, se mata.
  timeout -k 5 180 ./WiiCompiled > "$DIR/run.log" 2>&1 &
  TPID=$!
  iniciar_gptokeyb
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
  esperar_juego $TPID
  wait $TPID
  STATUS=$?
  parar_gptokeyb
else
  ./WiiCompiled > "$DIR/run.log" 2>&1 &
  JPID=$!
  iniciar_gptokeyb
  esperar_juego $JPID
  wait $JPID
  STATUS=$?
  parar_gptokeyb
fi

escribir_resumen
sync
