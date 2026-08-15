import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createWasmHost } from './wasm-host-runtime.mjs';
import { inspectWasm } from './wasm-inspect.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const manifestPath = process.env.TCC_WASM_DIFF_MANIFEST ?? path.join(here, 'differential/cases.json');
const nativeTcc = process.env.TCC_NATIVE_TCC ?? path.join(root, 'tcc');
const wasmTcc = process.env.TCC_WASM_TCC ?? path.join(root, 'wasm32-emscripten-tcc');
const keep = process.env.TCC_WASM_DIFF_KEEP === '1';

function run(argv, options = {}) {
  const [cmd, ...args] = argv;
  const res = spawnSync(cmd, args, {
    cwd: root,
    encoding: 'utf8',
    stdio: options.capture ? ['ignore', 'pipe', 'pipe'] : 'inherit',
  });
  if (res.status !== 0) {
    const detail = options.capture ? `\nstdout:\n${res.stdout}\nstderr:\n${res.stderr}` : '';
    throw new Error(`${argv.join(' ')} failed with status ${res.status}${detail}`);
  }
  return res.stdout ?? '';
}

function cString(s) {
  return JSON.stringify(s);
}

function cNumberLiteral(value, type) {
  if (type === 'f64') {
    if (!Number.isFinite(value)) throw new Error(`non-finite f64 literal unsupported: ${value}`);
    return Number(value).toPrecision(17);
  }
  return String(Number(value) | 0);
}

function renderNativeRunner(sourcePath, calls) {
  const lines = [
    '#include <stdio.h>',
    `#include ${cString(sourcePath)}`,
    'int main(void) {',
  ];
  for (const call of calls) {
    call.cases.forEach((args, index) => {
      const label = `${call.name}:${index}`;
      const renderedArgs = args.map((arg, i) => cNumberLiteral(arg, call.args[i])).join(', ');
      if (call.ret === 'f64') {
        lines.push(`  printf(${cString(label + '=%.17g\n')}, ${call.name}(${renderedArgs}));`);
      } else if (call.ret === 'i32') {
        lines.push(`  printf(${cString(label + '=%d\n')}, ${call.name}(${renderedArgs}));`);
      } else {
        throw new Error(`unsupported return type ${call.ret}`);
      }
    });
  }
  lines.push('  return 0;', '}');
  return lines.join('\n');
}

function parseNativeOutput(text) {
  const out = new Map();
  for (const line of text.trim().split(/\n+/)) {
    if (!line) continue;
    const eq = line.indexOf('=');
    if (eq < 0) throw new Error(`bad native output line: ${line}`);
    out.set(line.slice(0, eq), Number(line.slice(eq + 1)));
  }
  return out;
}

function wasmExport(exports, name) {
  const fn = exports[name] ?? exports[`_${name}`];
  if (typeof fn !== 'function') throw new Error(`missing wasm export ${name}`);
  return fn;
}

function equalEnough(type, actual, expected) {
  if (type === 'i32') return Object.is(actual | 0, expected | 0);
  const scale = Math.max(1, Math.abs(actual), Math.abs(expected));
  return Math.abs(actual - expected) <= 1e-12 * scale;
}

const manifest = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
const work = await fs.mkdtemp(path.join(os.tmpdir(), 'tcc-wasm-diff-'));
let checked = 0;

try {
  for (const group of manifest) {
    const source = path.join(here, 'differential/cases', group.file);
    const stem = path.basename(group.file, '.c');
    const runner = path.join(work, `${stem}-native-runner.c`);
    const nativeExe = path.join(work, `${stem}-native`);
    const wasmPath = path.join(work, `${stem}.wasm`);

    await fs.writeFile(runner, renderNativeRunner(source, group.calls));
    run([nativeTcc, '-B', root, '-I', path.join(root, 'include'), '-I', root, '-o', nativeExe, runner], { capture: true });
    const native = parseNativeOutput(run([nativeExe], { capture: true }));

    run([wasmTcc, '-B.', '-nostdlib', '-shared', '-o', wasmPath, source], { capture: true });
    const bytes = new Uint8Array(await fs.readFile(wasmPath));
    const summary = inspectWasm(bytes);
    if (!summary.valid) throw new Error(`${wasmPath} failed WebAssembly.validate`);
    const { instance } = await WebAssembly.instantiate(bytes, createWasmHost().imports);

    for (const call of group.calls) {
      const fn = wasmExport(instance.exports, call.name);
      for (let i = 0; i < call.cases.length; ++i) {
        const label = `${call.name}:${i}`;
        const expected = native.get(label);
        if (expected === undefined) throw new Error(`native result missing for ${label}`);
        const actual = Number(fn(...call.cases[i]));
        const matched = equalEnough(call.ret, actual, expected);
        if (call.xfail) {
          if (matched) {
            throw new Error(`${group.file} ${label} unexpectedly matched; remove xfail: ${call.xfail}`);
          }
          console.log(`xfail wasm32 differential: ${group.file} ${label}: ${call.xfail}`);
          continue;
        }
        if (!matched) {
          throw new Error(`${group.file} ${label} mismatch: wasm=${actual}, native=${expected}`);
        }
        checked++;
      }
    }
    console.log(`ok wasm32 differential: ${group.file}`);
  }
} finally {
  if (keep) {
    console.log(`kept differential workdir: ${work}`);
  } else {
    await fs.rm(work, { recursive: true, force: true });
  }
}

console.log(`WASM32_DIFFERENTIAL_SMOKE_OK ${checked} checks`);
