#!/usr/bin/env bash
# Puerta 3, paso 3: traduce Wii Sports con WiiCompiled y compila el runtime en Ubuntu (WSL2, x86_64).
#
#   bash scripts/build-wiisports-wsl.sh [main.dol] [carpeta de WiiCompiled]
#
# Por defecto usa /mnt/c/wii/RSPE01/DATA/sys/main.dol y ~/BrawlRecompiled/wiicompiled.
# Todo queda en ~/choya-build.log; al final imprime un resumen de los errores de compilación.
# RAPIDO=1 bash scripts/build-wiisports-wsl.sh  → no vuelve a traducir (solo runtime y enlace).
# Nada de lo que se genera se sube a GitHub: contiene código del juego.
set -uo pipefail

CHOYA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOL="${1:-/mnt/c/wii/RSPE01/DATA/sys/main.dol}"
WC="${2:-$HOME/BrawlRecompiled/wiicompiled}"
LOG="$HOME/choya-build.log"
: > "$LOG"

paso() { echo; echo "=== $* ===" | tee -a "$LOG"; }
falla() { echo "FALLÓ: $*  (detalles en $LOG)" | tee -a "$LOG"; exit 1; }
corre() { "$@" >>"$LOG" 2>&1; }

paso "1/7 Revisar herramientas"
[ -f "$DOL" ] || falla "no encontré $DOL"
[ -d "$WC/translator" ] || falla "no encontré WiiCompiled en $WC"
for t in clang clang++ ninja dotnet python3 git pkg-config; do
  command -v "$t" >/dev/null || falla "falta $t (corre el bloque de apt del README)"
done
CMAKE_OK=$(cmake --version 2>/dev/null | awk 'NR==1{split($3,v,"."); print (v[1]>3 || (v[1]==3 && v[2]>=25)) ? 1 : 0}')
[ "$CMAKE_OK" = "1" ] || falla "WiiCompiled necesita CMake 3.25 o más nuevo (tienes: $(cmake --version | head -1))"
echo "clang $(clang --version | head -1 | grep -o '[0-9]*\.[0-9]*\.[0-9]*' | head -1) · $(cmake --version | head -1) · dotnet $(dotnet --version)" | tee -a "$LOG"

paso "2/7 Preparar el proyecto (recomp-prep + recomp-native)"
cd "$CHOYA"
corre python3 -m choya recomp-prep "$DOL" "$WC" || falla "recomp-prep"
python3 -m choya recomp-native "$WC" 2>&1 | tee -a "$LOG" | grep -E "AVISO|Entrada|Estructuras|otros juegos|Sin resolver" || true
# recomp-prep otra vez para que recomp.yml apunte ya a runtime-wiisports
corre python3 -m choya recomp-prep "$DOL" "$WC" || falla "recomp-prep"

cd "$WC"
TR="dotnet translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
YML=projects/wiisports/recomp.yml

RAPIDO="${RAPIDO:-0}"
if [ "$RAPIDO" = "1" ] && [ -f generated/base_translation_output.json ] && [ -f "translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll" ]; then
  paso "3-4/7 Modo rápido: se reutiliza la traducción anterior"
else
paso "3/7 Compilar el traductor"
corre dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release || falla "dotnet build"

paso "4/7 Traducir Wii Sports"
rm -rf generated build/base
mkdir -p generated/functions generated/build_shards build/base
corre $TR translate-recursive 0x80006124 --project "$YML" --outdir generated/functions \
  --output-metadata generated/base_translation_output.json --threads "$(nproc)" || falla "translate-recursive"
grep -E "Recursively translated|DEVELOPER OUTPUT" "$LOG" | tail -2
corre $TR generate-data-init --project "$YML" || falla "generate-data-init"
fi

paso "5/7 Grafo de compilación"
corre $TR emit-build-shards --project "$YML" --base-metadata generated/base_translation_output.json \
  --base-functions-dir generated/functions --native-source-dir runtime-wiisports/src \
  --out generated/build_shards || falla "emit-build-shards"
ls generated | tee -a "$LOG"

paso "6/7 Configurar CMake (descarga Dawn la primera vez)"
corre cmake -S runtime-wiisports -B build-wiisports -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ || {
  grep -nE "CMake Error|FATAL|error" "$LOG" | tail -20
  falla "cmake (configurar)"
}

paso "7/7 Compilar (sigue aunque haya errores, para verlos todos)"
# Cada núcleo usa ~1.5 GB con los archivos grandes del juego traducido.
MEM_GB=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
JOBS=$(( MEM_GB * 2 / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1; [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
echo "Memoria ${MEM_GB} GB → ${JOBS} trabajos a la vez" | tee -a "$LOG"
cmake --build build-wiisports -j "$JOBS" -- -k 0 >>"$LOG" 2>&1
STATUS=$?

echo
echo "=== Resumen ===" | tee -a "$LOG"
ERRS=$(grep -c " error: " "$LOG" || true)
echo "Errores de compilación: $ERRS" | tee -a "$LOG"
if [ "$ERRS" != "0" ]; then
  echo "Archivos con más errores:" | tee -a "$LOG"
  grep " error: " "$LOG" | sed -E 's#^.*/(runtime-wiisports|generated)/#\1/#; s#:[0-9]+:[0-9]+: error:.*##' \
    | sort | uniq -c | sort -rn | head -15 | tee -a "$LOG"
  echo "Primeros errores:" | tee -a "$LOG"
  grep " error: " "$LOG" | sed -E 's#^.*/(runtime-wiisports|generated)/#\1/#' | head -15 | tee -a "$LOG"
fi
UND=$(grep -c "undefined reference\|undefined symbol" "$LOG" || true)
[ "$UND" != "0" ] && { echo "Símbolos sin definir al enlazar: $UND" | tee -a "$LOG";
  grep -oE "undefined (reference to|symbol:) [^ ]+" "$LOG" | sort | uniq -c | sort -rn | head -15 | tee -a "$LOG"; }
if [ $STATUS -eq 0 ]; then
  echo "COMPILÓ. Ejecutable en $WC/build-wiisports" | tee -a "$LOG"
  find build-wiisports -maxdepth 2 -type f -executable -newer "$YML" | head | tee -a "$LOG"
fi
cp "$LOG" /mnt/c/Users/Public/choya-build.log 2>/dev/null && echo "Copia del log: C:\\Users\\Public\\choya-build.log"
exit $STATUS
