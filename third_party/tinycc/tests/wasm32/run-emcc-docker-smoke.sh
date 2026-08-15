#!/usr/bin/env bash
set -euo pipefail

# Run the Emscripten-only compatibility checks in a Docker container.
# This keeps emcc/emsdk out of the host dependency chain while still testing
# the documented MAIN_MODULE/SIDE_MODULE dynamic-linking path when Docker is
# available.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOCKER="${DOCKER:-docker}"
IMAGE="${TCC_WASM_EMCC_DOCKER_IMAGE:-emscripten/emsdk:6.0.5@sha256:76a44fff907397784decc435115d07fcb9587a4f1504977f39f3745e538e3a1e}"

if [[ ! -f "$ROOT/add.wasm" ]]; then
  echo "error: $ROOT/add.wasm not found; run tests/wasm32/run-core-smoke.sh first" >&2
  exit 1
fi

if ! command -v "$DOCKER" >/dev/null 2>&1; then
  echo "error: docker not found; cannot run emcc container smoke" >&2
  exit 1
fi

if ! "$DOCKER" image inspect "$IMAGE" >/dev/null 2>&1; then
  "$DOCKER" pull "$IMAGE"
fi

echo "using emcc docker image: $IMAGE"

"$DOCKER" run --rm \
  -v "$ROOT:/src" \
  -w /src \
  -e NODE_OPTIONS="${NODE_OPTIONS:-}" \
  "$IMAGE" \
  bash -lc '
    set -euo pipefail
    emcc -O0 -sSIDE_MODULE=2 -shared tests/wasm32/add.c -o emcc-add-golden.wasm
    node tests/wasm32/compare-wasm-shape.mjs add.wasm emcc-add-golden.wasm

    emcc -O0 tests/wasm32/dlopen-main.c \
      -sMAIN_MODULE=2 \
      -sALLOW_TABLE_GROWTH=1 \
      --preload-file add.wasm@/add.wasm \
      -o dlopen-main.mjs
    node dlopen-main.mjs
  '

echo EMCC_DOCKER_SMOKE_OK
