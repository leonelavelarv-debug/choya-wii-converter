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
#
# Fiabilidad (0.32.0; detalle en el README, "Compilación fiable del port"):
#   - recomp-native se detiene si un parche obligatorio no se aplica o un objeto del SDK no mide lo esperado.
#   - WiiCompiled queda fijado en <WiiCompiled>/choya-dependencias.lock; si cambia, se detiene
#     (CHOYA_DEPS_ACTUALIZAR=1 acepta la revisión nueva).
#   - El perfil PGO se junta y valida en un temporal; si falla, se detiene (CHOYA_PGO_REUSAR=1 usa el anterior a
#     propósito).
#   - assets.wlpk se reutiliza solo si coinciden conversor, opciones, perfil y contenido del disco; si no se puede
#     convertir, se detiene (CHOYA_SIN_PAQUETE=1 arma sin él, y el manifiesto lo dice).
#   - El port se arma en una carpeta aparte con manifiesto.json (SHA-256 de cada archivo) y solo reemplaza al anterior
#     si quedó completo y verificado.
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
NUEVO="$PORT.nuevo"
WIN="/mnt/c/Users/Public/choya-port"
EST="$WC/$BUILD.choya"   # lo que cada paso deja para el manifiesto (dependencias, identidad, recursos)
: > "$LOG"

paso() { echo; echo "=== $* ===" | tee -a "$LOG"; }
falla() { echo "FALLÓ: $*  (detalles en $LOG)" | tee -a "$LOG"; exit 1; }

paso "1/6 Revisar"
[ -f "$WC/generated/build_shards/shards.cmake" ] || falla "falta generated/: corre primero build-wiisports-wsl.sh"
command -v docker >/dev/null || falla "no encuentro docker (¿integración WSL de Docker Desktop apagada?)"
docker info >/dev/null 2>&1 || falla "Docker no responde: abre Docker Desktop"
rm -rf "$EST" && mkdir -p "$EST"
# 0.32.0: la revisión de WiiCompiled (con aurora adentro) queda fijada; si cambió, se detiene y dice qué hacer.
(cd "$CHOYA" && python3 -m choya dependencias "$WC" --json "$EST/dependencias.json" \
   $([ "${CHOYA_DEPS_ACTUALIZAR:-0}" = 1 ] && echo --actualizar)) 2>&1 | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || falla "WiiCompiled no es la revisión fijada (ver arriba)"

paso "2/6 Ajustar el runtime (recomp-native)"
cd "$CHOYA" && python3 -m choya recomp-native "$WC" > "$EST/recomp-native.txt" 2>&1
RN=$?
cat "$EST/recomp-native.txt" >> "$LOG"
grep -E "^Parches:|AVISO|Estructuras" "$EST/recomp-native.txt" | tail -6
if [ "$RN" -ne 0 ]; then
  # 0.32.0: un parche obligatorio sin aplicar deja un runtime a medias que compila y parece correcto: se detiene.
  grep -A4 "^ERROR" "$EST/recomp-native.txt" | head -40
  cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
  falla "recomp-native (parches obligatorios o estructuras del SDK; detalle en $WC/projects/wiisports/retarget.md)"
fi
# Los cambios locales se clasifican después de recomp-native, con los parches de aurora de esta versión ya puestos.
python3 -m choya dependencias "$WC" --json "$EST/dependencias-cambios.json" >>"$LOG" 2>&1 || falla "dependencias"
python3 -m choya identidad-codigo "$WC" --json "$EST/identidad.json" 2>&1 | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || falla "identidad del código traducido"
CODIGO_ID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['huella'][:12])" "$EST/identidad.json")

paso "3/6 Imagen de Docker (la primera vez tarda: baja Clang 21 y las bibliotecas arm64)"
docker build -t "$IMG" "$CHOYA/docker/wiicompiled-arm64" >>"$LOG" 2>&1 || falla "docker build"
IMAGEN_ID=$(docker image inspect -f '{{.Id}}' "$IMG" 2>/dev/null)
CLANG_VER=$(docker run --rm "$IMG" clang-21 --version 2>/dev/null | head -1)
[ -n "$CLANG_VER" ] || falla "no pude leer la versión de Clang de la imagen $IMG"
echo "Compilador: $CLANG_VER (imagen ${IMAGEN_ID:7:12})" | tee -a "$LOG"

echo "Código traducido: $OPT_MODO (carpeta $BUILD)" | tee -a "$LOG"
if [ "$OPT_MODO" = pgo-generar ] && [ ! -f "$PGO/runtime/libclang_rt.profile-aarch64.a" ]; then
  # Una sola vez: el runtime de perfiles de Clang para aarch64 (apt.llvm.org no lo trae), de compiler-rt.
  echo "Compilando el runtime de perfiles para aarch64 (baja compiler-rt de github.com)..." | tee -a "$LOG"
  mkdir -p "$PGO"
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WC":"$WC" -v "$CHOYA/scripts":/choya-scripts:ro "$IMG" \
    bash /choya-scripts/pgo-runtime-aarch64.sh "$PGO/runtime" >>"$LOG" 2>&1 || falla "runtime de perfiles (PGO)"
fi
PGO_META=""
if [ "$OPT_MODO" = pgo ] || [ "$OPT_MODO" = pgo-O3 ]; then
  # Los perfiles que tomó la consola (wiisports-choya/pgo/*.profraw) se juntan en uno solo.
  CRUDOS="${CHOYA_PGO_DIR:-/mnt/c/Users/Public/choya-pgo}"
  if [ "${CHOYA_PGO_REUSAR:-0}" != 1 ]; then
    ls "$CRUDOS"/*.profraw >/dev/null 2>&1 || falla "no hay perfiles en $CRUDOS: compila con CHOYA_OPT=pgo-generar, juega en la consola y copia wiisports-choya/pgo/*.profraw ahí"
    echo "Perfiles de la consola en $CRUDOS: $(ls "$CRUDOS"/*.profraw | wc -l) archivos ($(du -shc "$CRUDOS"/*.profraw | tail -1 | cut -f1))" | tee -a "$LOG"
  fi
  mkdir -p "$PGO"
  # 0.32.0: se junta y valida en un temporal; el perfil anterior solo se reemplaza si el nuevo salió bien, y si
  # falla la compilación se detiene (antes un merge fallido dejaba el perfil viejo y seguía con él).
  python3 "$CHOYA/scripts/pgo-perfil.py" --crudos "$CRUDOS" --perfil "$PERFIL" --clang="$CLANG_VER" \
    --profdata="docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $WC:$WC $IMG llvm-profdata-21" \
    --identidad "$EST/identidad.json" --generar "$PGO/generar.json" \
    $([ "${CHOYA_PGO_REUSAR:-0}" = 1 ] && echo --reusar) 2>&1 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" -eq 0 ] || falla "perfil PGO (ver arriba; no se usa el anterior sin pedirlo)"
  PGO_META="$PERFIL.json"
  # Ninja no sabe que el perfil cambió: si es otro, se recompila todo.
  SELLO_PGO=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['sha256'][:12])" "$PGO_META")
  [ "$(sha256sum "$PERFIL" | cut -c1-12)" = "$SELLO_PGO" ] || falla "el perfil $PERFIL no es el que se validó"
  if [ "$(cat "$WC/$BUILD/choya-perfil.txt" 2>/dev/null)" != "$SELLO_PGO" ] && [ -f "$WC/$BUILD/build.ninja" ]; then
    echo "Perfil nuevo: se recompila el código traducido" | tee -a "$LOG"
    docker run --rm -u "$(id -u):$(id -g)" -v "$WC":"$WC" -w "$WC" "$IMG" cmake --build "$BUILD" --target clean >>"$LOG" 2>&1 \
      || falla "limpiar $BUILD para el perfil nuevo"
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
# aurora decodifica a RGBA8 (menos memoria, menos trabajo en el hilo del juego).
# 0.32.0: se vuelve a usar solo si coinciden el código del conversor, las opciones, el perfil, numpy/Pillow y el
# contenido del disco (huella de files/ y sys/ archivo por archivo, no fechas ni rutas). Si no se puede convertir se
# detiene. CHOYA_DISCO=carpeta del disco extraído (por defecto /mnt/c/wii/RSPE01), PERFIL_TEXTURAS=rg35xxh|ligero|
# calidad, CHOYA_PAQUETE=un assets.wlpk ya hecho, CHOYA_SIN_PAQUETE=1 = el port sin recursos convertidos (a propósito).
DISCO="${CHOYA_DISCO:-/mnt/c/wii/RSPE01}"
PERFIL_TEX="${PERFIL_TEXTURAS:-rg35xxh}"
PAQ_DIR="$HOME/choya-paquete"
PAQ=""
REC="$EST/recursos.json"
if [ "${CHOYA_SIN_PAQUETE:-0}" = 1 ]; then
  echo "Sin recursos convertidos (CHOYA_SIN_PAQUETE=1): el manifiesto lo anota" | tee -a "$LOG"
  echo '{"estado": "sin_paquete", "motivo": "CHOYA_SIN_PAQUETE=1 (pedido a propósito)"}' > "$REC"
elif [ -n "${CHOYA_PAQUETE:-}" ]; then
  python3 -m choya recursos --externo "$CHOYA_PAQUETE" --estado "$REC" 2>&1 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" -eq 0 ] || falla "CHOYA_PAQUETE=$CHOYA_PAQUETE no es un assets.wlpk válido"
else
  [ -d "$DISCO" ] || falla "no encontré el disco extraído en $DISCO (CHOYA_DISCO=...). Para armar sin recursos convertidos: CHOYA_SIN_PAQUETE=1"
  python3 -c "import numpy, PIL" 2>/dev/null || falla "para convertir faltan numpy y pillow:  sudo apt install -y python3-numpy python3-pil python3-scipy  (o CHOYA_SIN_PAQUETE=1)"
  echo "Revisando el disco y los recursos convertidos (si hay que convertir, la primera vez tarda unos minutos)..."
  python3 -m choya recursos "$DISCO" "$PAQ_DIR" -p "$PERFIL_TEX" --opcion=--sin-animaciones --estado "$REC" >>"$LOG" 2>&1
  RC=$?
  grep -E "^(Huella del disco|Hay que convertir|Recursos convertidos|ERROR)" "$LOG" | tail -4
  [ "$RC" -eq 0 ] || falla "conversión de recursos (el paquete anterior no se usa; CHOYA_SIN_PAQUETE=1 arma sin él)"
  grep -m3 -iE "texturas|memoria|total" "$PAQ_DIR/report.md" 2>/dev/null | tee -a "$LOG"
fi
PAQ=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('paquete') or '')" "$REC")

paso "6/6 Armar el port"
# 0.32.0: se arma en $NUEVO y reemplaza a $PORT solo si quedó completo y verificado contra su manifiesto.
rm -rf "$NUEVO" && mkdir -p "$NUEVO/wiisports-choya/lib" "$NUEVO/wiisports-choya/game" || falla "carpeta $NUEVO"
if [ -n "$PAQ" ]; then
  cp "$PAQ" "$NUEVO/wiisports-choya/assets.wlpk" || falla "copiar assets.wlpk"
  echo "Recursos convertidos: wiisports-choya/assets.wlpk ($(du -h "$PAQ" | cut -f1))" | tee -a "$LOG"
fi
B="$WC/$BUILD"
docker run --rm -u "$(id -u):$(id -g)" -v "$WC":"$WC" -v "$NUEVO":"$NUEVO" "$IMG" bash -c "
  set -o pipefail
  aarch64-linux-gnu-strip -o '$NUEVO/wiisports-choya/WiiCompiled' '$B/WiiCompiled' &&
  file '$NUEVO/wiisports-choya/WiiCompiled' &&
  aarch64-linux-gnu-readelf -d '$B/WiiCompiled' | grep NEEDED &&
  aarch64-linux-gnu-readelf -V '$B/WiiCompiled' | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1" \
  | tee "$NUEVO/wiisports-choya/necesita.txt" | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || falla "strip/readelf del ejecutable"
# La SD de KNULLI no guarda enlaces simbólicos: cada .so se copia con el nombre que pide el ejecutable (libpng16.so.16).
find "$B" -maxdepth 3 -name "*.so*" -type f -not -path "*/CMakeFiles/*" | while read -r f; do
  cp -L "$f" "$NUEVO/wiisports-choya/lib/$(basename "$f" | sed -E 's/(\.so\.[0-9]+)\..*/\1/')" || exit 1
done || falla "copiar las bibliotecas"
for f in dsp_coef.bin initial_pipeline_cache.db cacert.pem; do
  if [ -f "$B/$f" ]; then cp "$B/$f" "$NUEVO/wiisports-choya/" || falla "copiar $f"; fi
done
if [ -d "$B/wii_bootstrap" ]; then cp -r "$B/wii_bootstrap" "$NUEVO/wiisports-choya/" || falla "copiar wii_bootstrap"; fi
cp "$CHOYA/scripts/knulli/Wii Sports (Choya).sh" "$CHOYA/scripts/knulli/Wii Sports (Choya) - diagnostico.sh" "$NUEVO/" \
  || falla "copiar los lanzadores"
echo "Copia aquí la carpeta DATA de tu disco (la que tiene files y sys)." > "$NUEVO/wiisports-choya/game/LEEME.txt"
case "$OPT_MODO" in
  pgo-generar) echo "pgo-generar (toma el perfil: va más lento; los perfiles quedan en wiisports-choya/pgo)" ;;
  pgo|pgo-O3) echo "$OPT_MODO (perfil $SELLO_PGO)" ;;
  *) echo "$OPT_MODO" ;;
esac > "$NUEVO/wiisports-choya/compilado.txt"
# 0.32.0: los perfiles que tome esta versión llevan en el nombre la identidad del código traducido.
[ "$OPT_MODO" = pgo-generar ] && echo "$CODIGO_ID" > "$NUEVO/wiisports-choya/pgo-id.txt"
# Manifiesto (qué se compiló, con qué, SHA-256 de cada archivo) y version.txt con la identidad del port.
# (--flags=... con "=": los valores empiezan con guion, -O3, -fprofile-use=...)
python3 -m choya manifiesto "$NUEVO" --wc "$WC" --build "$B" --modo "$OPT_MODO" --flags="$OPT_FLAGS" \
  --enlace="$OPT_ENLACE" --clang="$CLANG_VER" --imagen="$IMAGEN_ID" --deps "$EST/dependencias.json" --deps-cambios "$EST/dependencias-cambios.json" \
  --recursos "$REC" --identidad "$EST/identidad.json" ${PGO_META:+--pgo "$PGO_META"} 2>&1 | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || falla "manifiesto del port"
python3 -m choya publicar-port "$NUEVO" "$PORT" 2>&1 | tee -a "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || falla "el port nuevo no quedó completo: $PORT sigue con el anterior"
if [ "$OPT_MODO" = pgo-generar ]; then
  python3 -c "import json,sys; json.dump({'codigo_traducido': json.load(open(sys.argv[1]))['huella'], 'choya': sys.argv[2]}, open(sys.argv[3], 'w'))" \
    "$EST/identidad.json" "$VERSION_CHOYA" "$PGO/generar.json"
fi
ls -la "$PORT/wiisports-choya" "$PORT/wiisports-choya/lib" | tee -a "$LOG"
du -sh "$PORT" | tee -a "$LOG"
head -12 "$PORT/wiisports-choya/manifiesto.txt" | tee -a "$LOG"
# La copia para Windows también se arma aparte y se verifica antes de reemplazar la anterior.
if [ -d /mnt/c/Users/Public ]; then
  rm -rf "$WIN.nuevo" && cp -r "$PORT" "$WIN.nuevo" || falla "copiar el port a $WIN.nuevo"
  python3 -m choya publicar-port "$WIN.nuevo" "$WIN" 2>&1 | tee -a "$LOG"
  [ "${PIPESTATUS[0]}" -eq 0 ] || falla "la copia en C:\\Users\\Public\\choya-port no quedó igual (sigue la anterior)"
  echo "Listo: C:\\Users\\Public\\choya-port ($(cat "$WIN/wiisports-choya/version.txt"))" | tee -a "$LOG"
fi
cp "$LOG" /mnt/c/Users/Public/choya-build-arm64.log 2>/dev/null
echo "Para pasarlo a la consola (PowerShell, con la tarjeta en la PC; comprueba cada archivo con el manifiesto):"
echo "  powershell -ExecutionPolicy Bypass -File \$HOME\\choya-wii-converter\\scripts\\copiar-a-consola.ps1"
