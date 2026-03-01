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
  - Host symbols injected into each TCC state include `duckdb_ext_api`, `ducktinycc_register_signature`, and `ducktinycc_*` helper/runtime symbols used by generated wrappers.
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
  - `make debug` and `make release` pass.
  - With `TARGET_DUCKDB_VERSION=v1.4.3`, extension loading currently requires ABI-mode/runtime matching:
    - stable ABI (`C_STRUCT`) is capped by current DuckDB loader policy (`v1.2.0` and lower),
    - unstable ABI (`USE_UNSTABLE_C_API=1`) requires an exact DuckDB runtime version match (`v1.4.3`).

## Progress Snapshot (2026-03-01)
- Community submission/docs alignment work started:
  - corrected claims to match actual signature grammar and recursive type support.
  - documented that `libtcc1.a` is conditional for runtime compile/codegen paths (not universally required).
  - added `NEWS.md` to track API/runtime/documentation changes going forward.
- C helper/codegen coverage in current API surface is explicit:
  - helper generators: `c_struct`, `c_union`, `c_bitfield`, `c_enum`.
  - codegen/compile modes: `codegen_preview`, `compile`, `quick_compile`.

## Versioning and NEWS Policy
- Use R-style development versions: `x.y.z.9000` means in-development; `x.y.z` means release.
- Version bump rules:
  - On any user-visible API/behavior/docs change on development branch, keep/bump to `.9000`.
  - At release cut, drop `.9000` (for example `0.0.3.9000` -> `0.0.3`).
  - Immediately after release, bump next development version (for example `0.0.3` -> `0.0.4.9000`).
- `NEWS.md` is mandatory and manually maintained (not generated).
- Every change that affects SQL surface, C bridge behavior, runtime requirements, or helper ABI must add an entry under the current top development section in `NEWS.md`.
- `NEWS.md` entry format should stay concise and explicit:
  - Header: `## ducktinycc x.y.z(.9000) (YYYY-MM-DD)`
  - Bullets: what changed, why it matters, and any compatibility/runtime note.

## Tasks Ahead (Near Term)
- Deduplicate code paths in runtime bridge/marshalling:
  - row vs batch decode/encode for nested `LIST/ARRAY/STRUCT/MAP/UNION`.
  - repeated allocation/cleanup branches in `tcc_execute_compiled_scalar_udf`.
- Deduplicate codegen helper assembly for `c_struct`/`c_union`/`c_bitfield` where patterns are shared.
- Add explicit lifetime/allocation documentation for C-facing helper/runtime functions:
  - borrowed vs owned memory for descriptor structs (`ducktinycc_list_t`, `ducktinycc_array_t`, `ducktinycc_struct_t`, `ducktinycc_map_t`, `ducktinycc_union_t`).
  - validity bitmap lifetime and indexing semantics (`offset` handling).
  - pointer helper ownership rules (`tcc_alloc`, `tcc_free_ptr`, read/write helpers).
  - contracts for generated helper functions (`*_new`, `*_free`, getters/setters, enum helpers).

## Working Rules for This Repo
- API is intentionally evolving; document and implement the current surface, not backward-compatibility shims.
- Prefix public SQL/API surface with `tcc_*`.
- Use `.sync/Rtinycc` for TinyCC build/runtime decisions.
- Use `.sync/duckhts` for DuckDB C extension API and CMake patterns.
- Keep runtime behavior explicit and diagnosable via SQL-returned tables.
