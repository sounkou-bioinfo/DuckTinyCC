#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TCC="${TCC_WASM_TCC:-$ROOT/wasm32-emscripten-tcc}"

cd "$ROOT"

if [[ ! -f add.wasm || ! -f runtime_imports.wasm || ! -f libtcc1_helpers.wasm ]]; then
  if [[ ! -x "$TCC" ]]; then
    echo "error: wasm32 compiler not found: $TCC" >&2
    echo "build it first, e.g.: make cross-wasm32-emscripten ONE_SOURCE=yes" >&2
    exit 1
  fi
  "$TCC" -B. -nostdlib -shared -o add.wasm tests/wasm32/add.c
  "$TCC" -B. -nostdlib -shared -o runtime_imports.wasm tests/wasm32/runtime_imports.c
  "$TCC" -B. -shared -o libtcc1_helpers.wasm tests/wasm32/libtcc1_helpers.c
fi

cd "$ROOT/tests/wasm32/browser"
if [[ ! -d node_modules ]]; then
  npm ci
fi
if [[ "${TCC_WASM_INSTALL_BROWSER:-0}" == 1 ]]; then
  npx playwright install --with-deps chromium
fi

TCC_WASM_ADD="${TCC_WASM_ADD:-$ROOT/add.wasm}" \
TCC_WASM_RUNTIME="${TCC_WASM_RUNTIME:-$ROOT/runtime_imports.wasm}" \
TCC_WASM_LIBTCC1="${TCC_WASM_LIBTCC1:-$ROOT/libtcc1_helpers.wasm}" \
  npx playwright test "$@"
