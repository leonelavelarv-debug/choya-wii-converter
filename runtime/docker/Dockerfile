# Debian 11 (glibc 2.31): binarios compatibles con KNULLI y con la mayoría de CFW de PortMaster.
FROM debian:bullseye
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build make g++-aarch64-linux-gnu pkg-config ca-certificates \
      libsdl2-dev:arm64 libgles-dev:arm64 zlib1g-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
CMD cmake -S runtime -B runtime/build-aarch64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=runtime/cmake/aarch64-linux-gnu.cmake -DCMAKE_BUILD_TYPE=Release \
    && cmake --build runtime/build-aarch64
