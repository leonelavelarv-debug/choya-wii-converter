#!/usr/bin/env bash
# Compila Wii Sports (ya traducido en WSL) para la RG35XX H (aarch64) con Docker y arma la carpeta del port.
#
#   bash scripts/build-wiisports-arm64.sh [carpeta de WiiCompiled]
#
# Antes hay que haber corrido build-wiisports-wsl.sh (deja generated/ y runtime-wiisports/).
# Resultado: C:\Users\Public\choya-port\  (copiar su contenido a la carpeta ports de la microSD).
set -uo pipefail

CHOYA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WC="${1:-$HOME/BrawlRecompiled/wiicompiled}"
LOG="$HOME/choya-build-arm64.log"
IMG=choya-wiicompiled-arm64
BUILD=build-wiisports-arm64
PORT="$HOME/choya-port"
: > "$LOG"

paso() { echo; echo "=== $* ===" | tee -a "$LOG"; }
falla() { echo "FALLÓ: $*  (detalles en $LOG)" | tee -a "$LOG"; exit 1; }

paso "1/6 Revisar"
[ -f "$WC/generated/build_shards/shards.cmake" ] || falla "falta generated/: corre primero build-wiisports-wsl.sh"
command -v docker >/dev/null || falla "no encuentro docker (¿integración WSL de Docker Desktop apagada?)"
docker info >/dev/null 2>&1 || falla "Docker no responde: abre Docker Desktop"

paso "2/6 Ajustar el runtime (recomp-native)"
cd "$CHOYA" && python3 -m choya recomp-native "$WC" >>"$LOG" 2>&1 || falla "recomp-native"
grep -E "AVISO|Estructuras" "$LOG" | tail -5

paso "3/6 Imagen de Docker (la primera vez tarda: baja Clang 21 y las bibliotecas arm64)"
docker build -t "$IMG" "$CHOYA/docker/wiicompiled-arm64" >>"$LOG" 2>&1 || falla "docker build"

paso "4/6 Compilar para aarch64 (Cortex-A53)"
MEM_GB=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
JOBS=$(( MEM_GB * 2 / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1; [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
echo "Memoria ${MEM_GB} GB → ${JOBS} trabajos a la vez" | tee -a "$LOG"
# Al compilar cruzado aurora quiere construir Dawn desde el código (y esa etiqueta no existe en google/dawn):
# se usa el paquete ya compilado para Linux aarch64, igual que en la PC.
# KNULLI no tiene X11, Wayland ni /dev/dri: SDL3 se compila "de consola" (ventana offscreen por ahora).
rm -rf "$WC/$BUILD/_deps/dawn-"*
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WC":"$WC" -w "$WC" "$IMG" bash -c "
  cmake -S runtime-wiisports -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/choya/aarch64-clang.cmake \
        -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=vendor \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF &&
  cmake --build $BUILD -j $JOBS -- -k 0" >>"$LOG" 2>&1
STATUS=$?
ERRS=$(grep -c " error: " "$LOG" || true)
echo "Errores de compilación: $ERRS" | tee -a "$LOG"
if [ "$STATUS" -ne 0 ] || [ ! -f "$WC/$BUILD/WiiCompiled" ]; then
  grep -nE -A4 "CMake Error| error: |undefined (reference|symbol)" "$LOG" | sed -E 's#^.*/(runtime-wiisports|generated|aurora-main|_deps)/#\1/#' | head -30 | tee -a "$LOG"
  cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
  falla "compilación aarch64"
fi

paso "5/6 Recursos convertidos (assets.wlpk del conversor de Choya)"
# El conversor reduce las texturas del disco y las recodifica a ETC2/EAC; el motor de Choya las usa en vez de las que
# aurora decodifica a RGBA8 (menos memoria, menos trabajo en el hilo del juego). Se convierte una vez y se vuelve a
# convertir solo si cambia el conversor o el perfil. CHOYA_DISCO=carpeta del disco extraído (por defecto
# /mnt/c/wii/RSPE01), PERFIL_TEXTURAS=rg35xxh|ligero|calidad, CHOYA_PAQUETE=un assets.wlpk ya hecho,
# CHOYA_SIN_PAQUETE=1 = el port sin recursos convertidos.
DISCO="${CHOYA_DISCO:-/mnt/c/wii/RSPE01}"
PERFIL_TEX="${PERFIL_TEXTURAS:-rg35xxh}"
PAQ_DIR="$HOME/choya-paquete"
PAQ=""
if [ "${CHOYA_SIN_PAQUETE:-0}" = 1 ]; then
  echo "Sin recursos convertidos (CHOYA_SIN_PAQUETE=1)" | tee -a "$LOG"
elif [ -n "${CHOYA_PAQUETE:-}" ]; then
  [ -f "$CHOYA_PAQUETE" ] && PAQ="$CHOYA_PAQUETE" || echo "AVISO: no existe CHOYA_PAQUETE=$CHOYA_PAQUETE" | tee -a "$LOG"
else
  SELLO="$(cd "$CHOYA/choya" && cat audio.py compress.py containers.py engine.py etc.py gxtex.py mdl0.py model_pack.py \
           optimize.py pack.py profiles.py scan.py tev.py u8.py 2>/dev/null | sha1sum | cut -c1-12) $PERFIL_TEX"
  if [ -f "$PAQ_DIR/assets.wlpk" ] && [ "$(cat "$PAQ_DIR/sello.txt" 2>/dev/null)" = "$SELLO" ]; then
    PAQ="$PAQ_DIR/assets.wlpk"
    echo "Ya convertido (perfil $PERFIL_TEX): $PAQ" | tee -a "$LOG"
  elif [ ! -d "$DISCO" ]; then
    echo "AVISO: no encontré el disco extraído en $DISCO (CHOYA_DISCO=...): el port usará las texturas del disco" | tee -a "$LOG"
  elif ! python3 -c "import numpy, PIL" 2>/dev/null; then
    echo "AVISO: para convertir faltan numpy y pillow:  sudo apt install -y python3-numpy python3-pil python3-scipy" | tee -a "$LOG"
  else
    echo "Convirtiendo $DISCO con el perfil $PERFIL_TEX (la primera vez tarda unos minutos)..." | tee -a "$LOG"
    rm -rf "$PAQ_DIR.tmp"
    if (cd "$CHOYA" && python3 -m choya convert "$DISCO" "$PAQ_DIR.tmp" -p "$PERFIL_TEX") >>"$LOG" 2>&1 \
       && [ -f "$PAQ_DIR.tmp/assets.wlpk" ]; then
      rm -rf "$PAQ_DIR" && mv "$PAQ_DIR.tmp" "$PAQ_DIR" && echo "$SELLO" > "$PAQ_DIR/sello.txt"
      PAQ="$PAQ_DIR/assets.wlpk"
      grep -m3 -iE "texturas|memoria|total" "$PAQ_DIR/report.md" | tee -a "$LOG"
    else
      echo "AVISO: la conversión falló (detalles en $LOG): el port usará las texturas del disco" | tee -a "$LOG"
    fi
  fi
fi

paso "6/6 Armar el port"
rm -rf "$PORT" && mkdir -p "$PORT/wiisports-choya/lib" "$PORT/wiisports-choya/game"
if [ -n "$PAQ" ]; then
  cp "$PAQ" "$PORT/wiisports-choya/assets.wlpk" && \
    echo "Recursos convertidos: wiisports-choya/assets.wlpk ($(du -h "$PAQ" | cut -f1))" | tee -a "$LOG"
fi
B="$WC/$BUILD"
docker run --rm -u "$(id -u):$(id -g)" -v "$WC":"$WC" -v "$PORT":"$PORT" "$IMG" bash -c "
  aarch64-linux-gnu-strip -o '$PORT/wiisports-choya/WiiCompiled' '$B/WiiCompiled' &&
  file '$PORT/wiisports-choya/WiiCompiled' &&
  aarch64-linux-gnu-readelf -d '$B/WiiCompiled' | grep NEEDED &&
  aarch64-linux-gnu-readelf -V '$B/WiiCompiled' | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1" \
  | tee "$PORT/wiisports-choya/necesita.txt" | tee -a "$LOG"
# La SD de KNULLI no guarda enlaces simbólicos: cada .so se copia con el nombre que pide el ejecutable (libpng16.so.16).
find "$B" -maxdepth 3 -name "*.so*" -type f -not -path "*/CMakeFiles/*" | while read -r f; do
  cp -L "$f" "$PORT/wiisports-choya/lib/$(basename "$f" | sed -E 's/(\.so\.[0-9]+)\..*/\1/')"
done
for f in dsp_coef.bin initial_pipeline_cache.db cacert.pem; do [ -f "$B/$f" ] && cp "$B/$f" "$PORT/wiisports-choya/"; done
[ -d "$B/wii_bootstrap" ] && cp -r "$B/wii_bootstrap" "$PORT/wiisports-choya/"
cp "$CHOYA/scripts/knulli/Wii Sports (Choya).sh" "$CHOYA/scripts/knulli/Wii Sports (Choya) - diagnostico.sh" "$PORT/"
echo "Copia aquí la carpeta DATA de tu disco (la que tiene files y sys)." > "$PORT/wiisports-choya/game/LEEME.txt"
ls -la "$PORT/wiisports-choya" "$PORT/wiisports-choya/lib" | tee -a "$LOG"
du -sh "$PORT" | tee -a "$LOG"
rm -rf /mnt/c/Users/Public/choya-port && cp -r "$PORT" /mnt/c/Users/Public/choya-port \
  && echo "Listo: C:\\Users\\Public\\choya-port" | tee -a "$LOG"
cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
