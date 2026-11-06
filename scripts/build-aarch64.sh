#!/bin/sh
# Compila el runtime para la RG35XX H. Ejecútalo en WSL2 (Ubuntu) con Docker Desktop encendido.
set -e
cd "$(dirname "$0")/.."
docker build -t choya-aarch64 runtime/docker
docker run --rm -v "$PWD":/src -u "$(id -u):$(id -g)" choya-aarch64
echo "Binarios en runtime/build-aarch64/: wl_view, wl_info, wl_selftest"
