#!/usr/bin/env bash
# Build the mcp-cpp Docker image and run configure/build/test inside it.
#
# Everything compiles and runs inside the Linux container, so no toolchain is
# needed on the host. Uses a bind mount so the container writes into ./build.
#
# Usage: scripts/build_docker.sh
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=mcp-cpp-builder

echo "==> Building image ${IMAGE}"
docker build -t "${IMAGE}" -f Dockerfile .

echo "==> Configuring / building / testing inside container"
docker run --rm -v "$PWD:/src" -w /src "${IMAGE}" bash -c '
  set -euo pipefail
  # Default build (stdio only) + ctest.
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j"$(nproc)" >/dev/null
  ctest --test-dir build --output-on-failure

  # Workflow build (adds Streamable HTTP transport + http_integration test).
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCP_ENABLE_WORKFLOW=ON >/dev/null
  cmake --build build -j"$(nproc)" >/dev/null
  ctest --test-dir build --output-on-failure
'

echo "==> build_docker.sh: OK"
