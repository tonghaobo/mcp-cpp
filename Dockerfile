# mcp-cpp build/test image: everything runs inside a Linux container,
# so no toolchain is required on the host.
#
# Usage:
#   docker build -t mcp-cpp-builder -f Dockerfile .
#   docker run --rm -v "$PWD:/src" -w /src mcp-cpp-builder bash -c \
#     'cmake -S . -B build && cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure'
#
# See scripts/build_docker.sh and scripts/interop.sh.

FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    pkg-config \
    libssl-dev \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
CMD ["bash"]
