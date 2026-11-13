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
#   CHOYA_MANDO_CODIGOS="east=304,south=305,..."  botones por código del kernel (east = A, south = B, north = X,
#                       west = Y, leftshoulder, rightshoulder, lefttrigger, righttrigger, back, start, guide = MENU)
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
# Caché de programas en disco (como los shaders listos de galaxy-vita): la primera vez se compilan, después se cargan.
export CHOYA_GX_CACHE="${CHOYA_GX_CACHE:-$DIR/cache-shaders}"
# Efectos de audio (reverberación): apagados por omisión, cuestan ~10 ms por cuadro del hilo del juego en Golf
# (0.19.1). Un archivo vacío llamado "con-reverb" junto al juego los vuelve a encender.
if [ -f "$DIR/con-reverb" ]; then export CHOYA_SIN_REVERB=0; else export CHOYA_SIN_REVERB=1; fi
# Memoria del juego: si un bloque grande (como el de un Mii) no cabe en su montón, otro montón con espacio lo presta
# en vez de cerrarse el juego (choya_perfil.cpp). Un archivo vacío llamado "sin-prestamos" lo deja solo anotando.
if [ -f "$DIR/sin-prestamos" ]; then export CHOYA_PRESTAMOS=0; else export CHOYA_PRESTAMOS=1; fi
# MENU + R1 guarda el cuadro con sus texturas (para dibujarlo igual en la PC). Desde la 0.20.10 siempre: un volcado
# sin texturas no sirve para revisar colores (ocupa unos MB más mientras se juega). Un archivo vacío llamado
# "volcados-sin-texturas" lo apaga.
if [ -f "$DIR/volcados-sin-texturas" ]; then export CHOYA_GX_GUARDAR_TEXTURAS=0; else export CHOYA_GX_GUARDAR_TEXTURAS=1; fi
# TEV exacto (0.20.9): la combinación de colores se cuenta en enteros de 8 bits como la Wii. Un archivo vacío llamado
# "tev-flotante" junto al juego vuelve a la cuenta en flotante de antes (para comparar velocidad o colores).
if [ -f "$DIR/tev-flotante" ]; then export CHOYA_GX_TEV=flotante; fi
# Recursos convertidos por el conversor de Choya (assets.wlpk junto al juego): texturas en ETC2/EAC ya reducidas.
# CHOYA_PAQUETE=0 usa solo las del disco.
if [ "${CHOYA_PAQUETE:-}" != "0" ] && [ -f "$DIR/assets.wlpk" ]; then
  export CHOYA_PAQUETE="$DIR/assets.wlpk"
else
  export CHOYA_PAQUETE=0
fi
mkdir -p "$CHOYA_GX_CACHE"
rm -f "$DIR/choya-gles.txt" "$DIR/procesador.txt" "$DIR/ultimo-cierre.tar.gz"

CPUF=/sys/devices/system/cpu/cpu0/cpufreq
{
  echo "--- procesador"
  echo "gobernador: $(cat $CPUF/scaling_governor 2>/dev/null)  MHz actual/max: $(( $(cat $CPUF/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))/$(( $(cat $CPUF/cpuinfo_max_freq 2>/dev/null || echo 0) / 1000 ))"
  echo "juego: $GAME"
  echo "PortMaster: ${controlfolder:-no encontrado}  dispositivo: ${DEVICE_NAME:-?}  CFW: ${CFW_NAME:-?}"
  echo "mapeo del mando: ${SDL_GAMECONTROLLERCONFIG:-ninguno}" | cut -c1-200
  echo "efectos de audio: $([ "${CHOYA_SIN_REVERB:-0}" = 1 ] && echo "apagados (para oírlos: archivo con-reverb)" || echo "encendidos (archivo con-reverb)")"
  echo "TEV: $([ "${CHOYA_GX_TEV:-}" = flotante ] && echo "en flotante (archivo tev-flotante)" || echo "exacto en enteros (para el de antes: archivo vacío tev-flotante)")"
  echo "memoria del juego: $([ "$CHOYA_PRESTAMOS" = 0 ] && echo "solo anota (archivo sin-prestamos)" || echo "un montón presta si a otro no le cabe un bloque (para solo anotar: archivo vacío sin-prestamos)")"
  echo "volcados con texturas (MENU + R1): $([ "${CHOYA_GX_GUARDAR_TEXTURAS:-0}" = 1 ] && echo "sí (para quitarlas: archivo vacío volcados-sin-texturas)" || echo "no (archivo volcados-sin-texturas)")"
  echo "recursos convertidos: $([ "$CHOYA_PAQUETE" = 0 ] && echo "no (texturas del disco)" || echo "$CHOYA_PAQUETE ($(du -h "$CHOYA_PAQUETE" 2>/dev/null | cut -f1))")"
  echo "gptokeyb: ${GPTOKEYB:-no}  salir: $CHOYA_QUIT_COMBO + START  botones: $CHOYA_FACE_LAYOUT"
  echo "dispositivos de entrada: $(grep -h "^N: Name=" /proc/bus/input/devices 2>/dev/null | cut -d= -f2- | tr "\n" " ")"
} > "$DIR/cpu.txt" 2>&1

escribir_resumen() {
  {
    echo "modo: $MODO"
    echo "codigo de salida: ${STATUS:-sigue corriendo} (124 = siguio corriendo hasta el limite del modo diagnostico)"
    [ -f "$DIR/perfil.txt" ] && echo "perfil.txt: $(grep -m1 '^muestras' "$DIR/perfil.txt"), escrito el $(date -r "$DIR/perfil.txt" '+%d/%m %H:%M' 2>/dev/null), hilo del juego $(awk '/^hilo_juego/{print $2}' "$DIR/perfil.txt")"
    cat "$DIR/cpu.txt"
    cat "$DIR/procesador.txt" 2>/dev/null
    echo "--- cuadros volcados"
    ls -la "$DIR"/cuadro-*.bin 2>/dev/null || echo "ninguno"
    echo "--- pantalla GLES de Choya"
    [ -f "$DIR/choya-gles.txt" ] && grep -v "extensiones:" "$DIR/choya-gles.txt" | tail -40 || echo "choya-gles.txt no se escribio"
    echo "--- cuadros por segundo"
    grep -E "\[vi\] fps" "$DIR/run.log" | tail -15
    echo "--- rendimiento (CHOYA_PERF)"
    cat "$DIR/rendimiento.txt" 2>/dev/null
    echo "--- memoria cada 10 s"
    tail -120 "$DIR/memoria.txt" 2>/dev/null
    echo "--- memoria del motor"
    grep -h "motor: memoria del proceso" "$DIR/choya-gles.txt" 2>/dev/null | tail -8
    echo "--- memoria de la Mali por partes del cuadro"
    grep -hE "motor: (Mali|la Mali|midiendo)" "$DIR/choya-gles.txt" 2>/dev/null | head -60
    echo "--- tramos del cuadro (MENU sola 2 s mide 20 s; detalle en la PC: choya perfil perfil.txt)"
    grep -hE "\[choya\] (midiendo|tramos|esperas|perfil escrito|la medición)|MENU sola" "$DIR/run.log" "$DIR/choya-gles.txt" 2>/dev/null | tail -7
    echo "--- hilo de aurora y listas de dibujo (por cuadro del juego)"
    grep -h "aurora: por cuadro" "$DIR/choya-gles.txt" 2>/dev/null | tail -6
    echo "--- núcleos"
    grep -h "motor: hilo del juego\|no pude fijar" "$DIR/choya-gles.txt" 2>/dev/null | tail -2
    echo "--- texturas sin datos en el motor (las primeras; tamaño y formato que dio aurora)"
    grep -h "textura sin datos" "$DIR/choya-gles.txt" 2>/dev/null | head -40
    echo "--- cola de cuadros del motor (copias que el juego no repite, como las caras de los Miis)"
    grep -h "cola de cuadros" "$DIR/choya-gles.txt" 2>/dev/null | tail -2
    echo "--- recursos convertidos (assets.wlpk)"
    grep -hE "paquete" "$DIR/choya-gles.txt" 2>/dev/null | tail -4
    echo "--- motor: estado GL y programas"
    grep -hE "motor: (estado GL|caché de programas|programas cargados)" "$DIR/choya-gles.txt" 2>/dev/null | tail -8
    echo "programas en el caché: $(ls "$CHOYA_GX_CACHE" 2>/dev/null | grep -c '\.bin$')"
    echo "--- mandos"
    grep -hE "mando|gamepad|joystick|\[input\]|volcado" "$DIR/choya-gles.txt" "$DIR/run.log" 2>/dev/null | grep -v "^\[choya-gles\]" | head -80
    echo "--- memoria del juego (montones de Wii Sports; si un bloque no cabe, otro montón lo presta; CHOYA_PRESTAMOS=0 solo anota)"
    grep -hE "\[choya\] (memoria del juego|montones del juego|el juego saltó)|\[choya\]   " "$DIR/run.log" 2>/dev/null | head -80
    echo "--- cierre (pila completa del juego)"
    if grep -q "crash artifacts written" "$DIR/run.log" 2>/dev/null; then
      grep -E "^\[runtime\] +#[0-9]+ |^\[runtime\] (PC|SRR0|SP\(r1\)|GPR)" "$DIR/run.log" | sed 's/ tail:.*//' | head -60
      carpeta=$(grep -m1 -o "crash artifacts written to .*" "$DIR/run.log" | sed 's/crash artifacts written to //')
      if [ -n "$carpeta" ] && [ -d "$carpeta" ] && \
         tar -czf "$DIR/ultimo-cierre.tar.gz" -C "$(dirname "$carpeta")" "$(basename "$carpeta")" 2>/dev/null; then
        echo "carpeta del cierre: wiisports-choya/ultimo-cierre.tar.gz (mándala junto con este resumen)"
      fi
    else
      echo "sin cierre"
    fi
    echo "--- errores"
    grep -iE "fatal|error|abort|AccessViolation|choya\]|not found|cannot open" "$DIR/run.log" | \
      grep -vE "memoria del juego|montones del juego|\[choya\]   |el juego saltó" | head -20
    echo "--- final del log"
    tail -15 "$DIR/run.log"
  } > "$DIR/resumen.txt" 2>&1
}

# CHOYA_PERF=1 (por defecto): CPU, GPU y memoria a su frecuencia máxima mientras se juega, como NEBULA_PERF en el
# port de Super Mario Galaxy (Nebula); se restauran al salir. Lo que cambia queda en rendimiento.txt.
PERF_SAVED=""
perf_restore() {  # al revés de como se cambiaron (el mínimo vuelve antes que el máximo)
  local list="" entry
  for entry in $PERF_SAVED; do list="$entry $list"; done
  for entry in $list; do
    echo "${entry#*=}" > "${entry%%=*}" 2>/dev/null || { [ -n "${ESUDO:-}" ] && echo "${entry#*=}" | $ESUDO tee "${entry%%=*}" >/dev/null 2>&1; }
  done
  PERF_SAVED=""
}
perf_set() {  # archivo valor: lo escribe y apunta el valor anterior para restaurarlo al salir
  local old; old=$(cat "$1" 2>/dev/null)
  [ "$old" = "$2" ] && return 0
  if echo "$2" > "$1" 2>/dev/null || { [ -n "${ESUDO:-}" ] && echo "$2" | $ESUDO tee "$1" >/dev/null 2>&1; }; then
    PERF_SAVED="$PERF_SAVED $1=$old"; echo "rendimiento: $1 $old -> $2"
  else echo "rendimiento: no se pudo escribir $2 en $1 (sigue en $old)"; fi
}
CPU_BASE=""; CPU_TOPE=""; CPU_CALIENTE=0
cpu_tope() {  # kHz: el tope de todos los núcleos, sin tocar lo que se restaura al salir
  local dir
  for dir in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$dir" ] || continue
    echo "$1" > "$dir/scaling_max_freq" 2>/dev/null || { [ -n "${ESUDO:-}" ] && echo "$1" | $ESUDO tee "$dir/scaling_max_freq" >/dev/null 2>&1; }
  done
}
top_freq() { tr ' ' '\n' < "$1" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tail -1; }
{
  for dir in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$dir" ] && echo "cpu $(basename "$dir"): $(cat "$dir/scaling_governor" 2>/dev/null) [$(cat "$dir/scaling_available_governors" 2>/dev/null)] $(cat "$dir/scaling_cur_freq" 2>/dev/null) kHz ($(cat "$dir/scaling_min_freq" 2>/dev/null)-$(cat "$dir/scaling_max_freq" 2>/dev/null), hasta $(cat "$dir/cpuinfo_max_freq" 2>/dev/null))"
  done
  for dir in /sys/class/devfreq/*; do
    [ -d "$dir" ] && echo "devfreq $(basename "$dir"): $(cat "$dir/governor" 2>/dev/null) [$(cat "$dir/available_governors" 2>/dev/null)] $(cat "$dir/cur_freq" 2>/dev/null) Hz ($(cat "$dir/min_freq" 2>/dev/null)-$(cat "$dir/max_freq" 2>/dev/null); $(cat "$dir/available_frequencies" 2>/dev/null))"
  done
} > "$DIR/rendimiento.txt" 2>&1
if [ "${CHOYA_PERF:-1}" = "1" ]; then
  for dir in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$dir" ] || continue
    if grep -qw performance "$dir/scaling_available_governors" 2>/dev/null; then
      perf_set "$dir/scaling_governor" performance
    else
      top=$(cat "$dir/cpuinfo_max_freq" 2>/dev/null)
      [ -n "$top" ] && perf_set "$dir/scaling_max_freq" "$top" && perf_set "$dir/scaling_min_freq" "$top"
    fi
  done >> "$DIR/rendimiento.txt" 2>&1
  # Tope del procesador (0.20.1): KNULLI deja scaling_max_freq en 1512 MHz aunque el H700 anuncia más
  # (cpuinfo_max_freq 1704 MHz). El hilo del juego es lo que limita los cuadros/s, así que son ~12 % más.
  # Si pasa de 80 °C vuelve al tope de KNULLI hasta bajar de 72 °C (lo vigila el ciclo de cada 10 s).
  # Un archivo vacío llamado "cpu-normal" junto al juego lo deja como lo tiene KNULLI.
  if [ ! -f "$DIR/cpu-normal" ]; then
    for dir in /sys/devices/system/cpu/cpufreq/policy*; do
      [ -d "$dir" ] || continue
      top=$(cat "$dir/cpuinfo_max_freq" 2>/dev/null); cur=$(cat "$dir/scaling_max_freq" 2>/dev/null)
      if [ -n "$top" ] && [ -n "$cur" ] && [ "$top" -gt "$cur" ]; then
        CPU_BASE=$cur; CPU_TOPE=$top
        perf_set "$dir/scaling_max_freq" "$top"
      fi
    done >> "$DIR/rendimiento.txt" 2>&1
  fi
  for dir in /sys/class/devfreq/*; do
    case "$(basename "$dir")" in *gpu*|*mali*|*1800000*|*dmc*) ;; *) continue ;; esac
    if grep -qw performance "$dir/available_governors" 2>/dev/null; then
      perf_set "$dir/governor" performance
    fi
    # Sin "performance" (o si el driver no le hace caso): el mínimo en la frecuencia más alta.
    top=$(top_freq "$dir/available_frequencies")
    if [ -n "$top" ]; then
      perf_set "$dir/max_freq" "$top"
      perf_set "$dir/min_freq" "$top"
    fi
  done >> "$DIR/rendimiento.txt" 2>&1
  trap perf_restore EXIT
fi

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
  : > "$DIR/memoria.txt"
  local t=0
  while kill -0 "$1" 2>/dev/null; do
    sleep 10
    t=$((t + 10))
    # Memoria cada 10 s: el juego (VmRSS), la libre del sistema y el swap, y los MHz (por si se calienta).
    {
      printf "%4ss  juego %s  libre %s  swap libre %s  MHz %s  temp %s\n" "$t" \
        "$(grep -h VmRSS /proc/"$(pgrep -n WiiCompiled)"/status 2>/dev/null | awk '{print int($2/1024)" MB"}')" \
        "$(awk '/MemAvailable/{print int($2/1024)" MB"}' /proc/meminfo)" \
        "$(awk '/SwapFree/{print int($2/1024)" MB"}' /proc/meminfo)" \
        "$(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))" \
        "$(( $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0) / 1000 ))C"
    } >> "$DIR/memoria.txt" 2>/dev/null
    echo "procesador al jugar (a los ${t} s): tope $(( $(cat $CPUF/scaling_max_freq 2>/dev/null || echo 0) / 1000 )) MHz, ahora $(( $(cat $CPUF/scaling_cur_freq 2>/dev/null || echo 0) / 1000 )) MHz, $(( $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0) / 1000 )) °C$([ -f "$DIR/cpu-normal" ] && echo " (archivo cpu-normal: tope de KNULLI)")$([ "$CPU_CALIENTE" = 1 ] && echo " (bajado por temperatura)")" > "$DIR/procesador.txt"
    # Vigilancia de temperatura con el tope del procesador subido.
    if [ -n "$CPU_TOPE" ]; then
      local temp
      temp=$(( $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0) / 1000 ))
      if [ "$temp" -ge 80 ] && [ "$CPU_CALIENTE" = 0 ]; then
        cpu_tope "$CPU_BASE"; CPU_CALIENTE=1
        echo "procesador: ${t}s ${temp} °C -> tope $((CPU_BASE / 1000)) MHz hasta que baje de 72 °C" >> "$DIR/rendimiento.txt"
      elif [ "$temp" -le 72 ] && [ "$CPU_CALIENTE" = 1 ]; then
        cpu_tope "$CPU_TOPE"; CPU_CALIENTE=0
        echo "procesador: ${t}s ${temp} °C -> tope $((CPU_TOPE / 1000)) MHz otra vez" >> "$DIR/rendimiento.txt"
      fi
    fi
    # Qué zonas de memoria del juego son las grandes (smaps: archivo o dispositivo de cada zona; /dev/mali0 o
    # dmabuf = la GPU) y la memoria total de la Mali si el kernel la muestra.
    local jp
    jp=$(pgrep -n WiiCompiled)
    if [ -n "$jp" ]; then
      awk '/^[0-9a-f]+-[0-9a-f]+ /{n=$6; if (n=="") n="[anon]"} /^Rss:/{r[n]+=$2}
           END{for (k in r) if (r[k] >= 8192) printf "%d %s\n", r[k]/1024, k}' /proc/"$jp"/smaps 2>/dev/null |
        sort -rn | head -6 | awk '{printf "        %s MB %s\n", $1, $2}' >> "$DIR/memoria.txt"
    fi
    for g in /sys/kernel/debug/mali0/gpu_memory /sys/kernel/debug/mali/gpu_memory; do
      [ -r "$g" ] && { awk 'NR==1{printf "        Mali: %d MB\n", $2*4/1024}' "$g" >> "$DIR/memoria.txt"; break; }
    done
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

perf_restore
escribir_resumen
sync
