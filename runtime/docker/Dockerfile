# Compilación cruzada para la RG35XX H (aarch64).
# Debian 12 (glibc 2.36): Debian 11 ya no tiene soporte y sus paquetes arm64 de seguridad dan 404.
FROM debian:bookworm
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build make g++-aarch64-linux-gnu pkg-config ca-certificates file \
      libsdl2-dev:arm64 libgles-dev:arm64 zlib1g-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
CMD ["sh", "-c", "cmake -S runtime -B runtime/build-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/src/runtime/cmake/aarch64-linux-gnu.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build runtime/build-aarch64"]
