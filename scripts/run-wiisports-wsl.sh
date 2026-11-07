#!/usr/bin/env bash
# Arranca Wii Sports recompilado en Ubuntu (WSL) y, si se detiene, dice en qué funciones del juego.
#
#   bash scripts/run-wiisports-wsl.sh [carpeta DATA del disco] [carpeta de WiiCompiled] [segundos]
set -uo pipefail

CHOYA="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="${1:-/mnt/c/wii/RSPE01/DATA}"
WC="${2:-$HOME/BrawlRecompiled/wiicompiled}"
SECS="${3:-300}"
CFG="$HOME/.local/share/WiiCompiled/Config.toml"
LOG="$HOME/wiisports-run.log"

[ -x "$WC/build-wiisports/WiiCompiled" ] || { echo "No está compilado: corre primero build-wiisports-wsl.sh"; exit 1; }
[ -f "$DATA/sys/boot.bin" ] || { echo "No encontré $DATA/sys/boot.bin"; exit 1; }
if [ "$HOME/choya-build.log" -nt "$WC/build-wiisports/WiiCompiled" ] && ! grep -q "COMPILÓ" "$HOME/choya-build.log"; then
  echo "AVISO: la última compilación falló; este ejecutable es de una compilación anterior."
  grep -E "Errores de compilación| error: " "$HOME/choya-build.log" | head -5
  echo "Arréglalo primero (o borra ~/choya-build.log para arrancar el viejo de todos modos)."
  exit 1
fi

mkdir -p "$(dirname "$CFG")"
if ! grep -q "dvd_root" "$CFG" 2>/dev/null; then
  cat > "$CFG" <<EOF
[paths]
dvd_root = "$DATA"

[video]
widescreen = false
force_16_9 = false
resolution_multiplier = 1
EOF
fi

echo "Arrancando (se corta solo a los $SECS s; Ctrl+C para salir antes)..."
cd "$WC/build-wiisports"
WIICOMPILED_NO_POPUP=1 timeout "$SECS" ./WiiCompiled >"$LOG" 2>&1
STATUS=$?

echo
echo "=== Últimas líneas ==="
tail -25 "$LOG"
echo
echo "=== Dónde estaba el juego (funciones de Wii Sports) ==="
grep -E "fatal|AccessViolation|choya\]|PC=|LR=|savedLR=" "$LOG" | tail -40 > /tmp/choya-fallo.txt
cd "$CHOYA" && python3 -m choya donde -a /tmp/choya-fallo.txt --lineas 40
case $STATUS in
  0)   echo "Terminó solo (código 0)." ;;
  124) echo "Siguió corriendo hasta el límite de $SECS s: no se detuvo por un error." ;;
  *)   echo "Se detuvo (código $STATUS)." ;;
esac
cp "$LOG" /mnt/c/Users/Public/wiisports-run.log 2>/dev/null && echo "Log completo: C:\\Users\\Public\\wiisports-run.log"
