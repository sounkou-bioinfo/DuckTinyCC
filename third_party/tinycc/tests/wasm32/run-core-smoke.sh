#!/usr/bin/env bash
set -euo pipefail

# Core TinyCC wasm32 smoke test.
# This uses TinyCC, Node/WebAssembly, and Dockerized Emscripten
# MAIN_MODULE/SIDE_MODULE checks when available.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TCC="${TCC_WASM_TCC:-$ROOT/wasm32-emscripten-tcc}"
NODE="${NODE:-node}"

if [[ ! -x "$TCC" ]]; then
  echo "error: wasm32 compiler not found: $TCC" >&2
  echo "build it first, e.g.: make cross-wasm32-emscripten ONE_SOURCE=yes" >&2
  exit 1
fi

cd "$ROOT"

run_node_smoke() {
  local wasm="$1"
  "$NODE" tests/wasm32/node-smoke.mjs "$wasm"
}

inspect_wasm() {
  "$NODE" tests/wasm32/wasm-inspect.mjs "$@"
}

inspect_wasm --self-test-simd128

tls_log="$(mktemp)"
trap 'rm -f "$tls_log" thread_local_unsupported.wasm' EXIT
if "$TCC" -B. -nostdlib -shared -o thread_local_unsupported.wasm \
    tests/wasm32/thread_local_unsupported.c 2>"$tls_log"; then
  echo "error: wasm32 unexpectedly accepted thread-local storage" >&2
  exit 1
fi
if ! grep -Fq "wasm32: thread-local storage is not supported" "$tls_log"; then
  cat "$tls_log" >&2
  echo "error: wasm32 TLS rejection did not use the target-specific diagnostic" >&2
  exit 1
fi
rm -f "$tls_log" thread_local_unsupported.wasm
trap - EXIT

"$TCC" -B. -nostdlib -shared -o add.wasm tests/wasm32/add.c
inspect_wasm add.wasm --expect-section=type --expect-section=function --expect-section=export --expect-section=code --expect-type=i32 --expect-type=f64
run_node_smoke add.wasm

"$TCC" -B. -nostdlib -shared -o runtime_imports.wasm tests/wasm32/runtime_imports.c
inspect_wasm runtime_imports.wasm --expect-section=type --expect-section=import --expect-section=function --expect-section=export --expect-section=code --expect-type=i32 --expect-type=f64
run_node_smoke runtime_imports.wasm

"$TCC" -B. -shared -o libtcc1_helpers.wasm tests/wasm32/libtcc1_helpers.c
inspect_wasm libtcc1_helpers.wasm --expect-section=type --expect-section=function --expect-section=export --expect-section=code --expect-type=i32
run_node_smoke libtcc1_helpers.wasm

"$NODE" --input-type=module <<'NODE'
import fs from 'node:fs';
import { createWasmHost } from './tests/wasm32/wasm-host-runtime.mjs';
const bytes = fs.readFileSync('add.wasm');
if (!WebAssembly.validate(bytes)) throw new Error('add.wasm failed WebAssembly.validate');
const { instance } = await WebAssembly.instantiate(bytes, createWasmHost().imports);
if (instance.exports.add(2, 40) !== 42) throw new Error('add(2,40) failed');
if (instance.exports.fma2(2, 3, 4) !== 10) throw new Error('fma2(2,3,4) failed');
console.log('ok wasm32 direct WebAssembly API smoke');
NODE

if command -v emcc >/dev/null 2>&1; then
  emcc -O0 -sSIDE_MODULE=2 -shared tests/wasm32/add.c -o emcc-add-golden.wasm
  "$NODE" tests/wasm32/compare-wasm-shape.mjs add.wasm emcc-add-golden.wasm

  emcc -O0 tests/wasm32/dlopen-main.c \
    -sMAIN_MODULE=2 \
    -sALLOW_TABLE_GROWTH=1 \
    --preload-file add.wasm@/add.wasm \
    -o dlopen-main.mjs
  "$NODE" dlopen-main.mjs
elif command -v docker >/dev/null 2>&1; then
  tests/wasm32/run-emcc-docker-smoke.sh
else
  echo "emcc/docker not found; skipped emcc SIDE_MODULE comparison and dlopen harness"
fi

echo CORE_WASM32_SMOKE_OK
