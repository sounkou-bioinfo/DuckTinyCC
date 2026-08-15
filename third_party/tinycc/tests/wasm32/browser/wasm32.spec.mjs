import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { test, expect } from '@playwright/test';
import { inspectWasm, simd128TypeFixture } from '../wasm-inspect.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../../..');
const defaultWasm = (name) => path.join(repoRoot, name);
const readBytes = async (p) => Array.from(await fs.readFile(path.isAbsolute(p) ? p : path.resolve(process.cwd(), p)));

async function instantiateInBrowser(page, wasmPath) {
  const bytes = await readBytes(wasmPath);
  const summary = inspectWasm(Uint8Array.from(bytes));
  expect(summary.valid).toBe(true);
  return page.evaluate(async (arr) => {
    const memory = new WebAssembly.Memory({ initial: 2 });
    const table = new WebAssembly.Table({ element: 'anyfunc', initial: 32 });
    const heap = () => new Uint8Array(memory.buffer);
    const align = (x, a) => (x + a - 1) & -a;
    const ensure = (end) => {
      const pages = 65536;
      if (end <= memory.buffer.byteLength) return;
      memory.grow(Math.ceil((end - memory.buffer.byteLength) / pages));
    };
    let bump = 4096;
    const malloc = (size) => {
      const ptr = align(bump, 8);
      bump = ptr + Number(size || 0);
      ensure(bump + 8);
      return ptr;
    };
    const free = () => {};
    const calloc = (nmemb, size) => {
      const n = Number(nmemb) * Number(size);
      const p = malloc(n);
      heap().fill(0, p, p + n);
      return p;
    };
    const realloc = (oldPtr, size) => {
      const p = malloc(size);
      if (oldPtr) heap().copyWithin(p, Number(oldPtr), Number(oldPtr) + Number(size));
      return p;
    };
    const memcpy = (dst, src, n) => { heap().copyWithin(Number(dst), Number(src), Number(src) + Number(n)); return dst; };
    const memmove = memcpy;
    const memset = (dst, c, n) => { heap().fill(Number(c) & 255, Number(dst), Number(dst) + Number(n)); return dst; };
    const memcmp = (a, b, n) => {
      const h = heap();
      for (let i = 0; i < Number(n); ++i) {
        const d = h[Number(a) + i] - h[Number(b) + i];
        if (d) return d;
      }
      return 0;
    };
    const strlen = (ptr) => { const h = heap(); let p = Number(ptr); while (h[p]) ++p; return p - Number(ptr); };
    const strcpy = (dst, src) => {
      const h = heap();
      let i = 0;
      do { h[Number(dst) + i] = h[Number(src) + i]; } while (h[Number(src) + i++]);
      return dst;
    };
    const strncpy = (dst, src, n) => {
      const h = heap();
      let i = 0;
      for (; i < Number(n) && h[Number(src) + i]; ++i) h[Number(dst) + i] = h[Number(src) + i];
      for (; i < Number(n); ++i) h[Number(dst) + i] = 0;
      return dst;
    };
    const abort = () => { throw new Error('wasm abort'); };
    const popcount32 = (x) => {
      x = Number(x) >>> 0;
      x = x - ((x >>> 1) & 0x55555555);
      x = (x & 0x33333333) + ((x >>> 2) & 0x33333333);
      return (((x + (x >>> 4)) & 0x0f0f0f0f) * 0x01010101) >>> 24;
    };
    const bswap32 = (x) => {
      x = Number(x) >>> 0;
      return (((x & 0xff) << 24) | ((x & 0xff00) << 8) |
              ((x >>> 8) & 0xff00) | ((x >>> 24) & 0xff)) >>> 0;
    };
    const imports = { env: {
      memory,
      __indirect_function_table: table,
      malloc,
      free,
      calloc,
      realloc,
      memcpy,
      memmove,
      memset,
      memcmp,
      strlen,
      strcpy,
      strncpy,
      abort,
      sqrt: Math.sqrt,
      sin: Math.sin,
      cos: Math.cos,
      exp: Math.exp,
      log: Math.log,
      sqrtf: Math.sqrt,
      sinf: Math.sin,
      cosf: Math.cos,
      __builtin_popcount: popcount32,
      __builtin_clz: (x) => Math.clz32(Number(x) >>> 0),
      __bswapsi2: bswap32,
      R_NilValue: () => 0,
      Rf_length: () => 0,
      Rf_protect: (x) => x,
      Rf_unprotect() {},
      Rf_error() { throw new Error('Rf_error'); },
      duckdb_data_chunk_get_size: () => 0,
      duckdb_data_chunk_get_vector: () => 0,
      duckdb_vector_get_data: () => 0,
      duckdb_malloc: malloc,
      duckdb_free: free,
      duckdb_scalar_function_set_error() {},
    }};
    const instance = await WebAssembly.instantiate(new Uint8Array(arr), imports);
    const exports = instance.instance.exports;
    const exportTypes = Object.fromEntries(Object.entries(exports).map(([k, v]) => [k, typeof v]));
    const checks = {};

    const add = exports.add ?? exports._add;
    if (typeof add === 'function') checks.add = add(2, 40);
    const fma2 = exports.fma2 ?? exports._fma2;
    if (typeof fma2 === 'function') checks.fma2 = fma2(2, 3, 4);

    const runtimeImports = exports.runtime_imports ?? exports._runtime_imports;
    if (typeof runtimeImports === 'function') checks.runtime_imports = runtimeImports();
    const mathImports = exports.math_imports ?? exports._math_imports;
    if (typeof mathImports === 'function') checks.math_imports = mathImports(81);

    const tcc1Marker = exports.tcc1_marker ?? exports._tcc1_marker;
    if (typeof tcc1Marker === 'function') checks.tcc1_marker = tcc1Marker();
    const popcount = exports.__builtin_popcount ?? exports.___builtin_popcount;
    if (typeof popcount === 'function') checks.popcount = popcount(0xf0f0);
    const clz = exports.__builtin_clz ?? exports.___builtin_clz;
    if (typeof clz === 'function') checks.clz = clz(1);
    const bswap = exports.__bswapsi2 ?? exports.___bswapsi2;
    if (typeof bswap === 'function') checks.bswap = bswap(0x01020304);

    return { exportTypes, checks };
  }, bytes);
}

test('validation infra recognizes simd128 type mnemonic and Chromium accepts it', async ({ page }) => {
  const summary = inspectWasm(simd128TypeFixture);
  expect(summary.valid).toBe(true);
  expect(summary.types[0].params).toEqual(['simd128']);
  expect(summary.types[0].results).toEqual(['simd128']);
  const browserValid = await page.evaluate(
    arr => WebAssembly.validate(new Uint8Array(arr)),
    Array.from(simd128TypeFixture));
  expect(browserValid).toBe(true);
});

test('generated add module executes in Chromium', async ({ page }) => {
  const result = await instantiateInBrowser(page, process.env.TCC_WASM_ADD ?? defaultWasm('add.wasm'));
  expect(result.exportTypes.add ?? result.exportTypes._add).toBe('function');
  expect(result.checks.add).toBe(42);
  expect(result.checks.fma2).toBe(10);
});

test('runtime import module executes in Chromium', async ({ page }) => {
  const result = await instantiateInBrowser(page, process.env.TCC_WASM_RUNTIME ?? defaultWasm('runtime_imports.wasm'));
  expect(result.exportTypes.runtime_imports ?? result.exportTypes._runtime_imports).toBe('function');
  expect(result.checks.runtime_imports).toBe(24);
  expect(result.checks.math_imports).toBeCloseTo(9, 9);
});

test('libtcc1 helper module executes in Chromium', async ({ page }) => {
  const result = await instantiateInBrowser(page, process.env.TCC_WASM_LIBTCC1 ?? defaultWasm('libtcc1_helpers.wasm'));
  expect(result.exportTypes.tcc1_marker ?? result.exportTypes._tcc1_marker).toBe('function');
  expect(result.checks.tcc1_marker).toBe(1);
  expect(result.checks.popcount).toBe(8);
  expect(result.checks.clz).toBe(31);
  expect(result.checks.bswap).toBe(0x04030201);
});
