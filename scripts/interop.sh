#!/usr/bin/env bash
# Run the official-Python-SDK interop test inside the Linux container.
#
# Builds the image on first use, builds mcp_example, creates a venv with the
# official `mcp` SDK (Ubuntu 24.04 is PEP 668 externally-managed, so a venv is
# mandatory), then runs tests/interop_stdio.py against the example server.
#
# Usage: scripts/interop.sh
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=mcp-cpp-builder

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
  echo "==> Building image ${IMAGE}"
  docker build -t "${IMAGE}" -f Dockerfile .
fi

echo "==> Building examples and running SDK interop inside container"
docker run --rm -v "$PWD:/src" -w /src "${IMAGE}" bash -c '
  set -euo pipefail
  # Build stdio example with the default (workflow OFF) config.
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j"$(nproc)" --target mcp_example >/dev/null
  if [ ! -d .venv ]; then
    echo "==> creating venv + installing official mcp SDK"
    python3 -m venv .venv
  fi
  ./.venv/bin/pip install -q --upgrade mcp

  echo "==> stdio interop"
  ./.venv/bin/python tests/interop_stdio.py ./build/mcp_example

  echo "==> rebuilding with workflow enabled for HTTP interop"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCP_ENABLE_WORKFLOW=ON >/dev/null
  cmake --build build -j"$(nproc)" --target mcp_http_example >/dev/null

  echo "==> streamable HTTP interop (v1 legacy ClientSession)"
  ./.venv/bin/python tests/interop_http.py ./build/mcp_http_example

  echo "==> streamable HTTP interop (v2 stateless / no session)"
  ./.venv/bin/python tests/interop_http_v2.py ./build/mcp_http_example
'

echo "==> interop.sh: OK"
