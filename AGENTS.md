# AGENTS.md

This repository uses local precedent references under `.sync/` to guide implementation choices for TinyCC integration and DuckDB C extension patterns.

## Primary `.sync` References

### 1) TinyCC embedding precedent (`Rtinycc`)
- Repo mirror: `.sync/Rtinycc`
- TinyCC build scripts:
  - `.sync/Rtinycc/configure`
  - `.sync/Rtinycc/configure.win`
  - `.sync/Rtinycc/src/Makevars`
  - `.sync/Rtinycc/src/Makevars.win`
- Runtime/relocation and libtcc usage:
  - `.sync/Rtinycc/src/RC_libtcc.c`
- Rtinycc behavior/context:
  - `.sync/Rtinycc/README.md`
  - `.sync/Rtinycc/NEWS.md`

### 2) DuckDB C extension precedent (`duckhts`)
- Repo mirror: `.sync/duckhts`
- Extension CMake and platform patterns:
  - `.sync/duckhts/CMakeLists.txt`
- Extension entrypoint and function registration pattern:
  - `.sync/duckhts/src/duckhts.c`
- Example table-function-heavy implementation:
  - `.sync/duckhts/src/bcf_reader.c`
- Product/spec references:
  - `.sync/duckhts/.github/SPEC.md`
  - `.sync/duckhts/.github/PLAN.md`

## Current Implementation State
- Extension artifact/name: `ducktinycc`
- Public SQL entrypoint: `tcc_module(...)`
- Implemented modes:
  - Session/config: `config_get`, `config_set`, `config_reset`, `list`, `tcc_new_state`
  - Build staging: `add_include`, `add_sysinclude`, `add_library_path`, `add_library`, `add_option`, `add_define`, `add_header`, `add_source`, `tinycc_bind`
  - Compile/codegen: `compile`, `quick_compile`, `codegen_preview`
  - Discovery helpers (separate table functions): `tcc_system_paths(...)`, `tcc_library_probe(...)`
- Wrapper/runtime model (Rtinycc-style API-mode codegen):
  - DuckTinyCC generates C wrappers during compile that unpack typed args, call target C symbols, and re-pack result for DuckDB scalar UDF execution.
  - Wrapper modules are compiled+relocated in-memory via libtcc; no separate shared library artifact is produced.
  - Host symbols injected into each TCC state today are fixed: `duckdb_ext_api`, `ducktinycc_register_signature`.
  - Supported SQL-visible signature tokens today are scalar-only: `void`, `bool`, `i8/u8`, `i16/u16`, `i32/u32`, `i64/u64`, `f32/f64` with max arity 10.
- Tests currently passing: `make test_debug`, `make test_release`

## Working Rules for This Repo
- API is intentionally evolving; document and implement the current surface, not backward-compatibility shims.
- Prefix public SQL/API surface with `tcc_*`.
- Use `.sync/Rtinycc` for TinyCC build/runtime decisions.
- Use `.sync/duckhts` for DuckDB C extension API and CMake patterns.
- Keep runtime behavior explicit and diagnosable via SQL-returned tables.
