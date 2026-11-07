#!/bin/bash
# Choya Wii Converter - información de la consola para el port de Wii Sports.
# Cópialo a la carpeta ports de la microSD (ROMS/ports) y ábrelo desde el menú Ports de KNULLI.
# Escribe choya-info.txt junto a este archivo. Solo lee; no cambia nada en la consola.

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/choya-info.txt"

{
echo "=== Choya info $(date) ==="
echo "--- sistema"
cat /etc/os-release 2>/dev/null | head -5
uname -a
echo "glibc: $( (ldd --version 2>&1 || /lib/libc.so.6 2>&1) | head -1)"
echo "--- cpu y memoria"
grep -m1 -i "model name\|Hardware" /proc/cpuinfo
grep -c ^processor /proc/cpuinfo
grep -E "MemTotal|MemAvailable|SwapTotal" /proc/meminfo
echo "--- pantalla"
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
ls /dev/dri 2>/dev/null
echo "--- driver de la GPU"
ls /dev/mali* /sys/class/misc/mali0 2>/dev/null
dmesg 2>/dev/null | grep -iE "panfrost|mali|lima" | tail -5
lsmod 2>/dev/null | grep -iE "panfrost|mali"
for l in libMali.so libmali.so libGLESv2.so.2 libEGL.so.1 libvulkan.so.1 libgbm.so.1 libSDL2-2.0.so.0 libSDL3.so.0; do
  f=$(ls /usr/lib/$l /usr/lib/aarch64-linux-gnu/$l /usr/lib64/$l 2>/dev/null | head -1)
  [ -n "$f" ] && echo "$l -> $(readlink -f "$f")" || echo "$l: no"
done
echo "--- vulkan"
ls /usr/share/vulkan/icd.d /etc/vulkan/icd.d 2>/dev/null
cat /usr/share/vulkan/icd.d/*.json 2>/dev/null | grep -E "library_path|api_version"
command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>&1 | head -30
echo "--- opengl es"
command -v eglinfo >/dev/null && eglinfo 2>&1 | grep -iE "version|renderer" | head -12
command -v glmark2-es2 >/dev/null && echo "glmark2-es2 disponible"
strings "$(readlink -f /usr/lib/libGLESv2.so.2 2>/dev/null)" 2>/dev/null | grep -m3 -iE "OpenGL ES 3\.[0-9]|Mesa [0-9]"
ls /usr/lib/dri 2>/dev/null | head -20
echo "--- portmaster"
ls -d /roms/ports/PortMaster /userdata/roms/ports/PortMaster /storage/roms/ports/PortMaster 2>/dev/null
echo "--- espacio"
df -h /userdata /roms /storage 2>/dev/null
echo "=== fin ==="
} > "$OUT" 2>&1

sync
echo "Listo: $OUT"
sleep 3
