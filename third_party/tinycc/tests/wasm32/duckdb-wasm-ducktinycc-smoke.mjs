import fs from 'node:fs/promises';
import { createWasmHost } from './wasm-host-runtime.mjs';

const repo = process.env.DUCKTINYCC_REPO ?? 'https://github.com/sounkou-bioinfo/DuckTinyCC.git';
console.log(`DuckTinyCC integration source: ${repo}`);

for (const f of ['wasm32-emscripten-tcc', 'wasm32-emscripten-libtcc1.a', 'add.wasm']) {
  try { await fs.stat(f); }
  catch { throw new Error(`missing wasm32 artifact ${f}`); }
}

const bytes = await fs.readFile('add.wasm');
const host = createWasmHost();
const mod = await WebAssembly.instantiate(bytes, host.imports);
const add = mod.instance.exports.add ?? mod.instance.exports._add;
if (typeof add !== 'function' || add(21, 21) !== 42) {
  throw new Error('TinyCC-generated add.wasm failed DuckTinyCC artifact smoke');
}

console.log('ok DuckTinyCC artifact-level wasm smoke');
