---
layout: page
title: Design and invariants
---

# Design and invariants

DuckTinyCC follows the principle in Salvatore Sanfilippo's
[“Control the ideas, not the code”](https://antirez.com/news/169): the durable
artifact is the explicit model of the system, while implementation is judged by
properties, differentials, sanitizers, and end-to-end behavior.

## Product model

DuckTinyCC is a control plane for trusted runtime C compilation inside DuckDB.
`tcc_module(...)` stages build inputs, parses SQL-visible signatures, generates
one C wrapper, compiles and loads it, and registers a scalar UDF. Diagnostics
remain queryable through SQL.

It is not a language sandbox, a remote compiler, an Arrow batch ABI, or a
replacement for DuckDB's optimizer. User C executes in the DuckDB process.

## Compile and registration flow

1. Session modes stage includes, defines, libraries, source, symbols, and one
   binding.
2. Signature tokens become bounded recursive descriptors.
3. Codegen emits a wrapper that converts DuckDB vectors to the documented C
   descriptors and converts results back.
4. Native TinyCC follows `tcc_new` → configure → compile → relocate → resolve
   module init.
5. Module init calls the host registration bridge.
6. The registry owns the relocated artifact until replacement or shutdown.

`tcc_new_state` resets staged inputs; it does not allocate a `TCCState`.

## Non-negotiable invariants

- A registered function never outlives its executable artifact.
- DuckDB, TinyCC, libc, and registry allocations are freed only by their owner.
- Descriptor data pointers are row-sliced; descriptor offsets index global
  validity bitmaps.
- Recursive syntax has a finite depth and every accepted tree can be destroyed
  after partial construction.
- Generated wrappers and runtime bridge logic implement one descriptor model.
- Embedded assets are self-contained and content-addressed; publication must be
  complete before another process can reuse them.
- Unsupported platforms return explicit diagnostics rather than pretending that
  a build-only stub is execution support.

## Source organization

`src/tcc_module.c` is the amalgamation root. The `src/tcc_module_*.c` files are
textually included implementation units sharing one private static namespace;
CMake intentionally compiles only the root. They use `.c` because they contain
C implementation, receive C-aware review and tooling, and map directly to
ownership domains. They are not independent translation units.

A future split into separately compiled objects is justified only when a small
private header can express ownership without exporting a large accidental API.
Renaming files alone must not masquerade as modularity.

## Sister project and documentation

[Rtinycc](https://github.com/sounkou-bioinfo/Rtinycc) is the sister embedding
project and the precedent for TinyCC configuration, CRT selection, relocation,
and platform behavior. Runtime changes that affect shared assumptions should be
checked against both projects rather than independently reinvented.

The documentation site uses GitHub Pages/Jekyll. Rtinycc appropriately uses
pkgdown because it is an R package; making DuckTinyCC pretend to be an R package
only to obtain pkgdown navigation would create a false packaging contract. The
information architecture is shared without importing that accidental runtime.

## Platform model

Native builds link embedded libtcc and extract the matching runtime archive and
headers on demand. The experimental TinyCC fork can emit Emscripten side
modules, but DuckTinyCC still needs hosted libtcc, dynamic-loader ownership,
main-module exports, and generated-wrapper lowering before WASM execution is a
real capability.

## Proof model

No single green gate is a release claim. Evidence is layered:

- deterministic SQLLogicTests;
- native libFuzzer under ASan and UBSan;
- C-generated, replayable SQL mutation campaigns;
- complete-extension sanitizer builds, including vendored TinyCC objects;
- embedded-runtime and community-install simulations;
- subprocess tests for unsafe native control flow;
- platform and compiler-fork CI.

A minimized fuzz failure becomes a tracked corpus seed and an ordinary
regression whenever it represents a semantic invariant.
