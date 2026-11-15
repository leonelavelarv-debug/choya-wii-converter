#!/usr/bin/env bash
# Runtime de perfiles de Clang (libclang_rt.profile) para aarch64, necesario para CHOYA_OPT=pgo-generar.
# apt.llvm.org solo trae el de amd64, así que se compila de las fuentes de compiler-rt de la misma versión de Clang.
# Corre dentro de la imagen de Docker (la llama build-wiisports-arm64.sh):
#
#   bash pgo-runtime-aarch64.sh <carpeta de salida>
#
# Deja <salida>/libclang_rt.profile-aarch64.a y lo prueba enlazando un programa instrumentado.
set -euo pipefail

OUT="${1:?carpeta de salida}"
CC="${CHOYA_PGO_CC:-clang-21}"
CXX="${CHOYA_PGO_CXX:-clang++-21}"
AR="${CHOYA_PGO_AR:-llvm-ar-21}"
TARGET="${CHOYA_PGO_TARGET:-aarch64-linux-gnu}"
LIB="$OUT/libclang_rt.profile-aarch64.a"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$OUT"

V=$("$CC" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
[ -n "$V" ] || { echo "no sé qué versión es $CC"; exit 1; }
MAJ=${V%%.*}; REST=${V#*.}; MIN=${REST%%.*}; PAT=${REST#*.}
echo "Clang $V: busco compiler-rt $MAJ.$MIN.x"

# apt.llvm.org puede ir un poco por delante de la última versión publicada: se prueba hacia atrás en la misma serie
# (el formato de los perfiles no cambia dentro de una versión mayor).
BAJADO=""
for p in $(seq "$PAT" -1 0); do
  v="$MAJ.$MIN.$p"
  if curl -fsSL -o "$T/crt.tar.xz" \
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-$v/compiler-rt-$v.src.tar.xz" 2>/dev/null; then
    BAJADO="$v"; break
  fi
done
[ -n "$BAJADO" ] || { echo "no pude bajar compiler-rt $MAJ.$MIN.x de github.com"; exit 1; }
echo "compiler-rt $BAJADO bajado"

# Solo lib/profile e include/profile (sin depender de xz: python trae lzma).
python3 -I - "$T/crt.tar.xz" "$T/src" <<'EOF'
import sys, tarfile
with tarfile.open(sys.argv[1]) as t:
    m = [x for x in t.getmembers() if "/lib/profile/" in x.name or "/include/profile/" in x.name]
    t.extractall(sys.argv[2], members=m, filter="data")
EOF
P="$(echo "$T"/src/*/lib/profile)"
[ -f "$P/InstrProfiling.c" ] || { echo "el paquete de compiler-rt no trae lib/profile"; exit 1; }

# Las mismas fuentes y definiciones que compiler-rt usa en Linux (lib/profile/CMakeLists.txt).
FLAGS=(--target="$TARGET" -O2 -fPIC -Wno-pedantic -w -I"$P/.." -I"$P/../../include"
       -DCOMPILER_RT_HAS_ATOMICS=1 -DCOMPILER_RT_HAS_FCNTL_LCK=1 -DCOMPILER_RT_HAS_UNAME=1)
OBJS=()
for f in GCDAProfiling.c InstrProfiling.c InstrProfilingInternal.c InstrProfilingValue.c InstrProfilingBuffer.c \
         InstrProfilingFile.c InstrProfilingMerge.c InstrProfilingMergeFile.c InstrProfilingNameVar.c \
         InstrProfilingVersionVar.c InstrProfilingWriter.c InstrProfilingPlatformLinux.c InstrProfilingPlatformOther.c \
         InstrProfilingUtil.c; do
  [ -f "$P/$f" ] || continue
  "$CC" "${FLAGS[@]}" -c "$P/$f" -o "$T/${f%.c}.o"
  OBJS+=("$T/${f%.c}.o")
done
"$CXX" "${FLAGS[@]}" -fno-exceptions -fno-rtti -nostdinc++ -c "$P/InstrProfilingRuntime.cpp" -o "$T/InstrProfilingRuntime.o"
OBJS+=("$T/InstrProfilingRuntime.o")
rm -f "$LIB.tmp"
"$AR" rcs "$LIB.tmp" "${OBJS[@]}"

# Prueba: un programa instrumentado tiene que enlazar con este runtime (igual que WiiCompiled).
printf 'int f(int x) { return x * 3; }\nint main(int c, char** v) { (void)v; return f(c) == 7; }\n' > "$T/t.c"
"$CC" --target="$TARGET" -O2 -fprofile-generate -c "$T/t.c" -o "$T/t.o"
"$CC" --target="$TARGET" -fuse-ld=lld "$T/t.o" -Wl,-u,__llvm_profile_runtime "$LIB.tmp" -o "$T/t"
mv "$LIB.tmp" "$LIB"
echo "$BAJADO" > "$OUT/version.txt"
echo "Listo: $LIB (compiler-rt $BAJADO)"
