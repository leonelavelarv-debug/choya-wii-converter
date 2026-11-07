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

paso "1/5 Revisar"
[ -f "$WC/generated/build_shards/shards.cmake" ] || falla "falta generated/: corre primero build-wiisports-wsl.sh"
command -v docker >/dev/null || falla "no encuentro docker (¿integración WSL de Docker Desktop apagada?)"
docker info >/dev/null 2>&1 || falla "Docker no responde: abre Docker Desktop"

paso "2/5 Ajustar el runtime (recomp-native)"
cd "$CHOYA" && python3 -m choya recomp-native "$WC" >>"$LOG" 2>&1 || falla "recomp-native"
grep -E "AVISO|Estructuras" "$LOG" | tail -5

paso "3/5 Imagen de Docker (la primera vez tarda: baja Clang 21 y las bibliotecas arm64)"
docker build -t "$IMG" "$CHOYA/docker/wiicompiled-arm64" >>"$LOG" 2>&1 || falla "docker build"

paso "4/5 Compilar para aarch64 (Cortex-A53)"
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

paso "5/5 Armar el port"
rm -rf "$PORT" && mkdir -p "$PORT/wiisports-choya/lib" "$PORT/wiisports-choya/game"
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
cp "$CHOYA/scripts/knulli/Wii Sports (Choya).sh" "$PORT/"
echo "Copia aquí la carpeta DATA de tu disco (la que tiene files y sys)." > "$PORT/wiisports-choya/game/LEEME.txt"
ls -la "$PORT/wiisports-choya" "$PORT/wiisports-choya/lib" | tee -a "$LOG"
du -sh "$PORT" | tee -a "$LOG"
rm -rf /mnt/c/Users/Public/choya-port && cp -r "$PORT" /mnt/c/Users/Public/choya-port \
  && echo "Listo: C:\\Users\\Public\\choya-port" | tee -a "$LOG"
cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
