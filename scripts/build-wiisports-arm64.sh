#!/usr/bin/env bash
# Compila Wii Sports (ya traducido en WSL) para la RG35XX H (aarch64) con Docker y arma la carpeta del port.
#
#   bash scripts/build-wiisports-arm64.sh [carpeta de WiiCompiled]
#
# Antes hay que haber corrido build-wiisports-wsl.sh (deja generated/ y runtime-wiisports/).
# Resultado: C:\Users\Public\choya-port\  (copiar su contenido a la carpeta ports de la microSD).
#
# CHOYA_OPT = cómo se optimiza el código traducido del juego (cada modo usa su propia carpeta de compilación):
#   (vacío)      -O2, como WiiCompiled
#   O3           -O3
#   pgo-generar  versión que toma el perfil en la consola (más lenta): jugar unos minutos, luego copiar
#                wiisports-choya/pgo/*.profraw a C:\Users\Public\choya-pgo (CHOYA_PGO_DIR=otra carpeta)
#   pgo          compilación guiada por ese perfil (-O2 + -fprofile-use)
#   pgo-O3       igual, con -O3
set -uo pipefail

CHOYA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WC="${1:-$HOME/BrawlRecompiled/wiicompiled}"
LOG="$HOME/choya-build-arm64.log"
IMG=choya-wiicompiled-arm64
BUILD=build-wiisports-arm64
VERSION_CHOYA=$(grep -m1 '^version' "$CHOYA/pyproject.toml" | cut -d'"' -f2)
OPT_MODO="${CHOYA_OPT:-}"
OPT_FLAGS=""
OPT_ENLACE=""
PGO="$WC/choya-pgo"
PERFIL="$PGO/wiisports.profdata"
PGO_USO="-fprofile-use=$PERFIL;-Wno-profile-instr-unprofiled;-Wno-profile-instr-out-of-date;-Wno-backend-plugin"
case "$OPT_MODO" in
  ""|O2|o2) OPT_MODO="O2" ;;
  O3|o3) BUILD="$BUILD-o3"; OPT_FLAGS="-O3" ;;
  pgo-generar) BUILD="$BUILD-pgogen"; OPT_FLAGS="-fprofile-generate"
               OPT_ENLACE="$PGO/runtime/libclang_rt.profile-aarch64.a;-Wl,-u,__llvm_profile_runtime" ;;
  pgo) BUILD="$BUILD-pgo"; OPT_FLAGS="$PGO_USO" ;;
  pgo-O3|pgo-o3) OPT_MODO="pgo-O3"; BUILD="$BUILD-pgo3"; OPT_FLAGS="-O3;$PGO_USO" ;;
  *) echo "CHOYA_OPT=$OPT_MODO no existe (usa O3, pgo-generar, pgo o pgo-O3)"; exit 1 ;;
esac
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

echo "Código traducido: $OPT_MODO (carpeta $BUILD)" | tee -a "$LOG"
if [ "$OPT_MODO" = pgo-generar ] && [ ! -f "$PGO/runtime/libclang_rt.profile-aarch64.a" ]; then
  # Una sola vez: el runtime de perfiles de Clang para aarch64 (apt.llvm.org no lo trae), de compiler-rt.
  echo "Compilando el runtime de perfiles para aarch64 (baja compiler-rt de github.com)..." | tee -a "$LOG"
  mkdir -p "$PGO"
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WC":"$WC" -v "$CHOYA/scripts":/choya-scripts:ro "$IMG" \
    bash /choya-scripts/pgo-runtime-aarch64.sh "$PGO/runtime" >>"$LOG" 2>&1 || falla "runtime de perfiles (PGO)"
fi
if [ "$OPT_MODO" = pgo ] || [ "$OPT_MODO" = pgo-O3 ]; then
  # Los perfiles que tomó la consola (wiisports-choya/pgo/*.profraw) se juntan en uno solo.
  CRUDOS="${CHOYA_PGO_DIR:-/mnt/c/Users/Public/choya-pgo}"
  ls "$CRUDOS"/*.profraw >/dev/null 2>&1 || falla "no hay perfiles en $CRUDOS: compila con CHOYA_OPT=pgo-generar, juega en la consola y copia wiisports-choya/pgo/*.profraw ahí"
  # 0.31.0: sin los vacíos, y de los que traen varios perfiles seguidos (0.29.0/0.30.x) solo el último (el total).
  rm -rf "$PGO/crudos"
  python3 "$CHOYA/scripts/pgo-preparar.py" "$CRUDOS" "$PGO/crudos" 2>&1 | tee -a "$LOG"
  ls "$PGO/crudos"/*.profraw >/dev/null 2>&1 || falla "ningún perfil utilizable en $CRUDOS (¿vacíos?)"
  echo "Perfiles de la consola: $(ls "$PGO/crudos" | wc -l) archivos ($(du -sh "$PGO/crudos" | cut -f1))" | tee -a "$LOG"
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WC":"$WC" "$IMG" bash -c "
    llvm-profdata-21 merge -o '$PERFIL.tmp' '$PGO'/crudos/*.profraw && mv '$PERFIL.tmp' '$PERFIL' &&
    llvm-profdata-21 show '$PERFIL' | tail -3" 2>&1 | tee -a "$LOG"
  [ -f "$PERFIL" ] || falla "llvm-profdata merge (¿los .profraw son de esta versión de Clang?)"
  # Ninja no sabe que el perfil cambió: si es otro, se recompila todo.
  SELLO_PGO=$(sha1sum "$PERFIL" | cut -c1-12)
  if [ "$(cat "$WC/$BUILD/choya-perfil.txt" 2>/dev/null)" != "$SELLO_PGO" ] && [ -f "$WC/$BUILD/build.ninja" ]; then
    echo "Perfil nuevo: se recompila el código traducido" | tee -a "$LOG"
    docker run --rm -u "$(id -u):$(id -g)" -v "$WC":"$WC" -w "$WC" "$IMG" cmake --build "$BUILD" --target clean >>"$LOG" 2>&1
  fi
fi

paso "4/6 Compilar para aarch64 (Cortex-A53)"
MEM_GB=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
JOBS=$(( MEM_GB * 2 / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1; [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
echo "Memoria ${MEM_GB} GB → ${JOBS} trabajos a la vez" | tee -a "$LOG"
# Sin Dawn (0.22.0): el motor GLES de Choya dibuja todo, así que aurora se compila con el WebGPU nulo de Choya
# (proveedor "choya", lib/choya/sin_dawn): ni la biblioteca de Dawn ni su paquete. Lo que quedó de Dawn en la carpeta
# de compilación se borra para que CMake no lo vuelva a usar.
# KNULLI no tiene X11, Wayland ni /dev/dri: SDL3 se compila "de consola" (ventana offscreen por ahora).
rm -rf "$WC/$BUILD/_deps/dawn-"*
# 0.30.0: el binario lleva el modo (sale en el log de la consola aunque falte compilado.txt).
COMPILADO="$OPT_MODO${SELLO_PGO:+ (perfil $SELLO_PGO)}; Choya $VERSION_CHOYA"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WC":"$WC" -w "$WC" "$IMG" bash -c "
  cmake -S runtime-wiisports -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/choya/aarch64-clang.cmake \
        -DAURORA_DAWN_PROVIDER=choya -UDawn_DIR -DAURORA_SDL3_PROVIDER=vendor \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
        -DCHOYA_OPT_TRADUCIDO='$OPT_FLAGS' -DCHOYA_ENLACE_EXTRA='$OPT_ENLACE' -DCHOYA_COMPILADO='$COMPILADO' &&
  cmake --build $BUILD -j $JOBS -- -k 0" >>"$LOG" 2>&1
STATUS=$?
ERRS=$(grep -c " error: " "$LOG" || true)
echo "Errores de compilación: $ERRS" | tee -a "$LOG"
if [ "$STATUS" -ne 0 ] || [ ! -f "$WC/$BUILD/WiiCompiled" ]; then
  grep -nE -A4 "CMake Error| error: |undefined (reference|symbol)" "$LOG" | sed -E 's#^.*/(runtime-wiisports|generated|aurora-main|_deps)/#\1/#' | head -30 | tee -a "$LOG"
  cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
  falla "compilación aarch64"
fi

[ -n "${SELLO_PGO:-}" ] && echo "$SELLO_PGO" > "$WC/$BUILD/choya-perfil.txt"

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
  SELLO="$(cd "$CHOYA/choya" && cat anim.py audio.py brsar.py compress.py containers.py engine.py etc.py gxtex.py mdl0.py model_pack.py \
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
    if (cd "$CHOYA" && python3 -m choya convert "$DISCO" "$PAQ_DIR.tmp" -p "$PERFIL_TEX" --sin-animaciones) >>"$LOG" 2>&1 \
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
case "$OPT_MODO" in
  pgo-generar) echo "pgo-generar (toma el perfil: va más lento; los perfiles quedan en wiisports-choya/pgo)" ;;
  pgo|pgo-O3) echo "$OPT_MODO (perfil $(cat "$WC/$BUILD/choya-perfil.txt" 2>/dev/null))" ;;
  *) echo "$OPT_MODO" ;;
esac > "$PORT/wiisports-choya/compilado.txt"
# 0.31.2: qué versión es el port y cuándo se armó (el resumen de la consola lo muestra en la primera línea, y
# scripts/copiar-a-consola.ps1 lo compara después de copiar).
echo "Choya $VERSION_CHOYA, armado el $(date '+%d/%m/%Y %H:%M:%S')" > "$PORT/wiisports-choya/version.txt"
ls -la "$PORT/wiisports-choya" "$PORT/wiisports-choya/lib" | tee -a "$LOG"
du -sh "$PORT" | tee -a "$LOG"
rm -rf /mnt/c/Users/Public/choya-port && cp -r "$PORT" /mnt/c/Users/Public/choya-port \
  && echo "Listo: C:\\Users\\Public\\choya-port" | tee -a "$LOG"
cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
echo "Para pasarlo a la consola (PowerShell, con la tarjeta en la PC):"
echo "  powershell -ExecutionPolicy Bypass -File \$HOME\\choya-wii-converter\\scripts\\copiar-a-consola.ps1"
