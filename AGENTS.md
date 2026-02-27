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
  - Supported SQL-visible signature tokens include:
    - Scalars: `void`, `bool`, `i8/u8`, `i16/u16`, `i32/u32`, `i64/u64`, `f32/f64`, `ptr`, `varchar/cstring`, `blob`, `uuid`, `date`, `time`, `timestamp`, `interval`, `decimal`.
    - Nested: fixed-child `LIST` (`list_*` / `type[]`), fixed-size `ARRAY` (`type[N]`), `STRUCT` (`struct<...>`), `MAP` (`map<...>`).
    - `UNION` (`union<name:type;...>`) is parsed, but SQL runtime marshalling is intentionally rejected with `E_BAD_SIGNATURE` until C API vector access is available.

## Progress Snapshot (2026-02-27)
- Parser/type metadata refactor completed for nested signatures:
  - top-level split helpers for nested tokens.
  - struct metadata now retains per-field raw type tokens.
  - map metadata now retains key/value raw type tokens.
- Recursive logical-type construction groundwork is in place:
  - nested `STRUCT`/`MAP` child logical types are built recursively from stored tokens.
- Memory hygiene improvements landed for nested metadata:
  - explicit map metadata destroy helpers and cleanup wiring added across parse/bind/codegen paths.
- Runtime bridge status:
  - full recursive DuckDB vector marshalling for nested/composite values is still WIP.
  - nested `STRUCT` argument bridging now materializes inner `ducktinycc_struct_t` descriptors (one recursive level) so `struct<...struct<...>>` arguments are callable from C UDFs.
  - struct-field composite bridging now also materializes `LIST`/`ARRAY`/`MAP` descriptor rows inside struct arguments, so C UDFs can safely consume `struct<...list_...>`, `struct<...T[N]>`, and `struct<...map<...;...>>` patterns.
  - `UNION` signatures remain intentionally blocked at runtime (`E_BAD_SIGNATURE`) until bridge support is implemented.
- Validation:
  - `make debug`, `make release`, `make test_debug`, and `make test_release` pass after this refactor.

## Working Rules for This Repo
- API is intentionally evolving; document and implement the current surface, not backward-compatibility shims.
- Prefix public SQL/API surface with `tcc_*`.
- Use `.sync/Rtinycc` for TinyCC build/runtime decisions.
- Use `.sync/duckhts` for DuckDB C extension API and CMake patterns.
- Keep runtime behavior explicit and diagnosable via SQL-returned tables.
