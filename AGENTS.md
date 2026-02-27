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
    - Nested/composite: `LIST` (`list_*`, `type[]`, `list<...>`), fixed-size `ARRAY` (`type[N]`), `STRUCT` (`struct<...>`), `MAP` (`map<k;v>`), `UNION` (`union<name:type;...>`), with recursive nesting.

## Progress Snapshot (2026-02-27)
- Parser/type metadata refactor completed for recursive signatures:
  - top-level token splitting for nested delimiters (`<...>`, `[...]`).
  - typedesc tree parsing (`tcc_typedesc_*`) now handles recursive `LIST/ARRAY/STRUCT/MAP/UNION`.
  - union metadata now keeps member raw tokens (`member_tokens`) for recursive lowering.
- Signature/runtime registration now stores typedescs in host signature context and derives DuckDB logical types recursively from typedescs.
- Runtime bridge refactor landed:
  - added generic recursive vector bridge (`tcc_value_bridge_*`) for composite input marshalling.
  - added recursive descriptor-to-vector writer (`tcc_write_value_to_vector`) for composite returns.
  - `UNION` signatures are enabled (no longer blocked at parse time).
- Compatibility notes:
  - legacy list/array tokens (`list_i64`, `i64[]`, `i64[3]`) are still accepted.
  - descriptor semantics for list/map/array keep `ptr` as row-sliced base with `offset` retained for validity/global indexing (matching existing helper behavior).
- Validation:
  - `make debug`, `make release`, `make test_debug`, and `make test_release` pass.

## Working Rules for This Repo
- API is intentionally evolving; document and implement the current surface, not backward-compatibility shims.
- Prefix public SQL/API surface with `tcc_*`.
- Use `.sync/Rtinycc` for TinyCC build/runtime decisions.
- Use `.sync/duckhts` for DuckDB C extension API and CMake patterns.
- Keep runtime behavior explicit and diagnosable via SQL-returned tables.
