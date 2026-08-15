---
layout: home
title: DuckTinyCC documentation
---

DuckTinyCC embeds TinyCC in a DuckDB C extension, compiles trusted C source at
runtime, generates typed scalar-UDF wrappers, relocates modules in process, and
keeps their code alive for the lifetime of registered SQL functions.

## Start here

- [Design and invariants](DESIGN.html)
- [Fuzzing and sanitizer discipline](FUZZING.html)
- [Lifetime and ownership](LIFETIME_OWNERSHIP.html)
- [Roadmap to 1.0 and WebAssembly](ROADMAP.html)
- [C function documentation conventions](C_FUNCTION_DOCS.html)
- [Project README](https://github.com/sounkou-bioinfo/DuckTinyCC#readme)

## Current support boundary

Native DuckTinyCC compiles and executes recursive scalar signatures, including
LIST, ARRAY, STRUCT, MAP, and UNION. Generated code is trusted in-process native
code, not a sandbox. WebAssembly builds currently expose diagnostics and
codegen preview but do not load generated modules.
