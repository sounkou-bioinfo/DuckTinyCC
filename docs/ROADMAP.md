---
layout: page
title: Roadmap to 1.0 and WebAssembly
---

# Roadmap to 1.0 and WebAssembly

## Correctness before API freeze

- Keep multi-row LIST, ARRAY, MAP, STRUCT, and UNION offset/validity properties
  under deterministic and fuzz coverage.
- Publish embedded runtime trees atomically and verify every manifest entry
  before reuse.
- Resolve Windows UCRT import-definition generation without relying on an
  external `tcc.exe`.
- Specify duplicate registration, replacement, DROP, unload, and multi-
  connection lifetime behavior.
- Add explicit generated-helper ABI versioning before declaring stability.

## WebAssembly milestones

1. Complete TinyCC lowering needed by a generated `i32 -> i32` wrapper: local
   calls, imports, data segments, byte accesses, and structured control flow.
2. Build hosted libtcc under Emscripten and embed matching WASM runtime assets.
3. Own emitted side-module bytes, MEMFS paths, loader handles, and cleanup.
4. Prove DuckDB-wasm main-module exports, shared memory/table, and table growth.
5. Execute one compiled UDF through SQL on `wasm_mvp`.
6. Expand through nulls, f64/i64, strings, composites, EH, then threads.

A green WASM static-library build is not execution support. Platforms remain
excluded until CI compiles and calls a generated SQL UDF.

## 1.0 release gate

- SQL and helper ABI documented and versioned.
- Debug/release, embedded runtime, community simulation, fuzzing, ASan, UBSan,
  subprocess safety, and supported-platform CI green.
- No unresolved ownership or catalog-lifetime ambiguity.
- Documentation site reflects the shipped capability matrix and trusted-code
  boundary.
