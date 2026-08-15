import fs from 'node:fs/promises';
import { pathToFileURL } from 'node:url';

export const sectionNames = new Map([
  [0, 'custom'], [1, 'type'], [2, 'import'], [3, 'function'], [4, 'table'],
  [5, 'memory'], [6, 'global'], [7, 'export'], [8, 'start'], [9, 'element'],
  [10, 'code'], [11, 'data'], [12, 'data_count'], [13, 'tag'],
]);

export const simd128TypeFixture = Uint8Array.from([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x06, 0x01, 0x60, 0x01, 0x7b, 0x01, 0x7b,
]);

const valueTypes = new Map([
  [0x7f, 'i32'],
  [0x7e, 'i64'],
  [0x7d, 'f32'],
  [0x7c, 'f64'],
  [0x7b, 'simd128'],
  [0x70, 'funcref'],
  [0x6f, 'externref'],
]);

function normalizeValueTypeName(name) {
  return name === 'v128' ? 'simd128' : name;
}

export function valueTypeMnemonic(code) {
  return valueTypes.get(code) ?? `type-0x${code.toString(16).padStart(2, '0')}`;
}

export function readULEB(bytes, pos) {
  let result = 0;
  let shift = 0;
  let p = pos;
  while (true) {
    if (p >= bytes.length) throw new Error('truncated uleb128');
    const b = bytes[p++];
    result += (b & 0x7f) * 2 ** shift;
    if (!(b & 0x80)) break;
    shift += 7;
    if (shift > 35) throw new Error('uleb128 too large for wasm32 validator');
  }
  return [result, p];
}

function readValueTypeVec(bytes, pos, end) {
  let count;
  [count, pos] = readULEB(bytes, pos);
  const out = [];
  for (let i = 0; i < count; ++i) {
    if (pos >= end) throw new Error('truncated value type vector');
    out.push(valueTypeMnemonic(bytes[pos++]));
  }
  return [out, pos];
}

function parseTypeSection(bytes, start, end) {
  let p = start;
  let count;
  const types = [];
  [count, p] = readULEB(bytes, p);
  for (let i = 0; i < count; ++i) {
    if (p >= end) throw new Error('truncated type section');
    const form = bytes[p++];
    if (form !== 0x60) throw new Error(`unsupported wasm type form 0x${form.toString(16)}`);
    let params, results;
    [params, p] = readValueTypeVec(bytes, p, end);
    [results, p] = readValueTypeVec(bytes, p, end);
    types.push({ index: i, params, results });
  }
  if (p !== end) throw new Error(`type section has ${end - p} trailing byte(s)`);
  return types;
}

export function inspectWasm(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (bytes.length < 8 || bytes[0] !== 0 || bytes[1] !== 0x61 || bytes[2] !== 0x73 || bytes[3] !== 0x6d) {
    throw new Error('bad wasm magic');
  }
  const version = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
  const sections = [];
  let types = [];
  let p = 8;
  while (p < bytes.length) {
    const id = bytes[p++];
    let size;
    [size, p] = readULEB(bytes, p);
    const start = p;
    const end = p + size;
    if (end > bytes.length) throw new Error(`section ${id} extends past end of file`);
    const name = sectionNames.get(id) ?? `section-${id}`;
    sections.push({ id, name, offset: start, size });
    if (id === 1) types = parseTypeSection(bytes, start, end);
    p = end;
  }
  return {
    valid: WebAssembly.validate(bytes),
    version,
    sections,
    types,
  };
}

export function formatType(type) {
  return `type[${type.index}](${type.params.join(',')})->(${type.results.join(',')})`;
}

function containsValueType(summary, name) {
  const wanted = normalizeValueTypeName(name);
  return summary.types.some(type =>
    type.params.some(t => normalizeValueTypeName(t) === wanted) ||
    type.results.some(t => normalizeValueTypeName(t) === wanted));
}

function runSimd128SelfTest() {
  const summary = inspectWasm(simd128TypeFixture);
  if (!summary.valid) throw new Error('simd128 fixture failed WebAssembly.validate');
  if (!containsValueType(summary, 'simd128')) {
    throw new Error(`simd128 mnemonic missing from ${summary.types.map(formatType).join(' ')}`);
  }
  console.log('ok wasm32 inspect simd128 type mnemonic');
}

async function main(argv = process.argv.slice(2)) {
  const expectSections = [];
  const expectTypes = [];
  const paths = [];
  let json = false;

  for (const arg of argv) {
    if (arg === '--self-test-simd128') {
      runSimd128SelfTest();
      continue;
    }
    if (arg === '--json') {
      json = true;
      continue;
    }
    if (arg.startsWith('--expect-section=')) {
      expectSections.push(arg.slice('--expect-section='.length));
      continue;
    }
    if (arg.startsWith('--expect-type=')) {
      expectTypes.push(normalizeValueTypeName(arg.slice('--expect-type='.length)));
      continue;
    }
    if (arg === '-h' || arg === '--help') {
      console.log('usage: node tests/wasm32/wasm-inspect.mjs [--self-test-simd128] [--json] file.wasm [--expect-section=name] [--expect-type=i32|i64|f32|f64|simd128]');
      return;
    }
    paths.push(arg);
  }

  for (const wasmPath of paths) {
    const bytes = new Uint8Array(await fs.readFile(wasmPath));
    const summary = inspectWasm(bytes);
    if (!summary.valid) throw new Error(`${wasmPath} failed WebAssembly.validate`);
    const shape = summary.sections.map(section => section.name);
    for (const section of expectSections) {
      if (!shape.includes(section)) {
        throw new Error(`${wasmPath} missing required ${section} section; shape=${shape.join(',')}`);
      }
    }
    for (const type of expectTypes) {
      if (!containsValueType(summary, type)) {
        throw new Error(`${wasmPath} missing expected ${type} type; types=${summary.types.map(formatType).join(' ')}`);
      }
    }
    if (json) {
      console.log(JSON.stringify({ path: wasmPath, ...summary }, null, 2));
    } else {
      const types = summary.types.map(formatType).join(' ');
      console.log(`ok wasm32 inspect: ${wasmPath}; sections=${shape.join(',')}; types=${types}`);
    }
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  await main();
}
