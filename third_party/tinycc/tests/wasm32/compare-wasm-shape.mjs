import fs from 'node:fs/promises';
import { inspectWasm } from './wasm-inspect.mjs';

const [generatedPath, goldenPath] = process.argv.slice(2);
if (!generatedPath || !goldenPath) {
  throw new Error('usage: node compare-wasm-shape.mjs generated.wasm golden.wasm');
}

function shape(summary) {
  return summary.sections.map(section => section.name);
}

const generated = inspectWasm(new Uint8Array(await fs.readFile(generatedPath)));
const golden = inspectWasm(new Uint8Array(await fs.readFile(goldenPath)));
if (!generated.valid) throw new Error(`${generatedPath} failed WebAssembly.validate`);
if (!golden.valid) throw new Error(`${goldenPath} failed WebAssembly.validate`);

const generatedShape = shape(generated);
const required = ['type', 'import', 'function', 'export', 'code'];
for (const name of required) {
  if (!generatedShape.includes(name)) {
    throw new Error(`${generatedPath} missing required ${name} section; shape=${generatedShape.join(',')}`);
  }
}
console.log(`generated=${generatedShape.join(',')}`);
console.log(`golden=${shape(golden).join(',')}`);
