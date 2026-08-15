import fs from 'node:fs/promises';
import { createWasmHost } from './wasm-host-runtime.mjs';

const wasmPath = process.argv[2] ?? 'add.wasm';
const bytes = await fs.readFile(wasmPath);
if (!WebAssembly.validate(bytes)) {
  throw new Error(`${wasmPath} is not valid WebAssembly`);
}
const host = createWasmHost();
const mod = await WebAssembly.instantiate(bytes, host.imports);
const exports = mod.instance.exports;
let checked = false;

const add = mod.instance.exports.add ?? mod.instance.exports._add;
if (typeof add === 'function') {
  if (add(2, 40) !== 42) {
    throw new Error('add(2, 40) did not return 42');
  }
  checked = true;
}

const runtimeImports = exports.runtime_imports ?? exports._runtime_imports;
if (typeof runtimeImports === 'function') {
  if (runtimeImports() !== 24) {
    throw new Error('runtime_imports() returned the wrong value');
  }
  checked = true;
}

const mathImports = exports.math_imports ?? exports._math_imports;
if (typeof mathImports === 'function') {
  const got = mathImports(81);
  if (Math.abs(got - 9) > 1e-9) {
    throw new Error(`math_imports(81) returned ${got}, expected 9`);
  }
  checked = true;
}

const tcc1Marker = exports.tcc1_marker ?? exports._tcc1_marker;
if (typeof tcc1Marker === 'function') {
  if (tcc1Marker() !== 1) {
    throw new Error('tcc1_marker() did not return 1');
  }
  const popcount = exports.__builtin_popcount ?? exports.___builtin_popcount;
  const clz = exports.__builtin_clz ?? exports.___builtin_clz;
  const bswap = exports.__bswapsi2 ?? exports.___bswapsi2;
  if (typeof popcount === 'function' && popcount(0xf0f0) !== 8) {
    throw new Error('__builtin_popcount(0xf0f0) did not return 8');
  }
  if (typeof clz === 'function' && clz(1) !== 31) {
    throw new Error('__builtin_clz(1) did not return 31');
  }
  if (typeof bswap === 'function' && bswap(0x01020304) !== 0x04030201) {
    throw new Error('__bswapsi2(0x01020304) returned the wrong value');
  }
  checked = true;
}

if (!checked) {
  throw new Error('missing known wasm32 smoke-test export');
}
console.log(`ok wasm32 tinycc smoke: ${wasmPath}`);
