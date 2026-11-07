#!/bin/sh
# Compila choya_render para la PC (Ubuntu en WSL2): dibuja modelos del paquete a PNG con Mesa (llvmpipe).
set -e
cd "$(dirname "$0")/.."
if ! dpkg -s libegl-dev libgles-dev libgl1-mesa-dri zlib1g-dev cmake ninja-build g++ >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y g++ cmake ninja-build zlib1g-dev libegl-dev libgles-dev libgl1-mesa-dri
fi
cmake -S runtime -B runtime/build-pc -G Ninja -DWL_WITH_GL=OFF -DWL_BUILD_RENDER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build-pc
echo "Listo: runtime/build-pc/choya_render"
