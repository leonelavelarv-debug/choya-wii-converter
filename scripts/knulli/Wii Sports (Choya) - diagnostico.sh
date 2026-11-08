#!/bin/bash
# Choya Wii Converter - Wii Sports en modo diagnóstico: corre 90 s, mide hilos y MHz, toma un perfil y vuelca
# los cuadros 600 y 2400 del motor (cuadro-*.bin) para revisarlos en la PC. Todo queda en wiisports-choya.
export CHOYA_MODO=diagnostico
exec "$(cd "$(dirname "$0")" && pwd)/Wii Sports (Choya).sh"
