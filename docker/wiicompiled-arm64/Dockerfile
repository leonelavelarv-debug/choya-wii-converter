# Compilador cruzado para WiiCompiled (Wii Sports) → RG35XX H con KNULLI (aarch64, glibc 2.40).
# Ubuntu 24.04 trae glibc 2.39: lo compilado aquí abre en KNULLI. Clang 21 de apt.llvm.org (el mismo que en WSL).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

# Paquetes de la PC (amd64) desde archive.ubuntu.com y los de la consola (arm64) desde ports.ubuntu.com.
RUN sed -i 's/^Types: deb$/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ubuntu.sources \
 && printf 'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble noble-updates noble-security\nComponents: main universe restricted multiverse\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' \
      > /etc/apt/sources.list.d/arm64.sources \
 && dpkg --add-architecture arm64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg cmake ninja-build git python3 pkg-config file \
      g++-14-aarch64-linux-gnu libstdc++-14-dev-arm64-cross binutils-aarch64-linux-gnu \
      libasound2-dev:arm64 libudev-dev:arm64 libdbus-1-dev:arm64 libegl-dev:arm64 libgles-dev:arm64 zlib1g-dev:arm64 \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb [arch=amd64] http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends clang-21 lld-21 llvm-21 \
 && rm -rf /var/lib/apt/lists/*

COPY aarch64-clang.cmake /opt/choya/aarch64-clang.cmake
WORKDIR /src
