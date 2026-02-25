# Detailed Plan: `tcc_*` UX + API + Build Integration for DuckDB Community Extension

## Current State (Implemented)

- Extension name and artifacts are lowercase `ducktinycc` for loader/CI compatibility.
- `tcc_module` is implemented as the public SQL table function.
- TinyCC is built from vendored source via CMake `ExternalProject_Add` and linked statically (`libtcc.a`).
- Runtime default path is auto-wired to the built TinyCC artifact directory.
- Real execution paths:
  - `mode='compile'`: real `tcc_new -> tcc_compile_string -> tcc_relocate -> tcc_get_symbol` flow
  - `mode='register'`: compiles a **fresh TinyCC state per compilation unit** and stores artifact in connection session registry
  - `mode='call'`: can execute one-shot (`source`+`symbol`) or execute later from registered artifact (`sql_name`)
  - `mode='unregister'`: removes and frees registered artifact/state
- Tests:
  - `make test_debug`: passing
  - `make test_release`: passing
- README:
  - function table first
  - pure SQL examples (no R execution chunks)
  - includes register-then-later-call examples and series/array style calls

## Summary
Build a DuckDB C extension that embeds TinyCC as a static dependency (`libtcc.a`) and exposes a **single public SQL entrypoint** `tcc_module(...)` (table function), with all operations controlled by `mode`.
This follows the constraints:

- TinyCC source unpacked in `third_party/tinycc`
- CMake builds static TinyCC and links `libtcc.a`
- No WASM execution support (fail fast)
- Windows support only for MSYS2/MinGW
- macOS `flat_namespace` hazard addressed
- multi-function registration per connection/session supported
- runtime asset paths configurable from SQL (`tcc_module(mode := 'config_set'|'config_get', ...)`)
- all public names prefixed `tcc_*`

## Public API / UX (single SQL function)

### `tcc_module(...)` table function
One function, mode-driven:

- `mode='config_set'`: set session defaults (runtime path, include paths, lib paths, libraries, options, defines)
- `mode='config_get'`: return current session config
- `mode='config_reset'`: reset session config
- `mode='compile'`: compile/check only, return diagnostics
- `mode='register'`: compile + relocate once + register one/many UDFs
- `mode='list'`: list registered `tcc_*` UDF metadata/artifacts in this connection
- `mode='unregister'` (logical unregister by registry tombstone; if hard unregister unavailable in C API, report unsupported and require new connection)

### Input shape (SQL structs/lists)
Named params accepted by `tcc_module` include:

- `sources LIST<STRUCT<name VARCHAR, code VARCHAR>>`
- `source_files LIST<VARCHAR>`
- `headers LIST<VARCHAR>`
- `include_paths LIST<VARCHAR>`
- `sysinclude_paths LIST<VARCHAR>`
- `library_paths LIST<VARCHAR>`
- `libraries LIST<VARCHAR>`
- `library_files LIST<VARCHAR>`
- `options LIST<VARCHAR>`
- `defines LIST<STRUCT<key VARCHAR, value VARCHAR>>`
- `undefines LIST<VARCHAR>`
- `runtime_path VARCHAR` (optional override; persists if `mode='config_set'`)
- `registrations LIST<STRUCT<sql_name VARCHAR, symbol VARCHAR, return_type VARCHAR, arg_types LIST<VARCHAR>, null_policy VARCHAR, side_effects BOOLEAN>>`
- `shared_artifact BOOLEAN` (`true` = compile once/register many; `false` = independent builds)
- `strict BOOLEAN` (fail all on first error vs partial row-by-row diagnostics)

### Output table schema (all modes)
Always returns rows with consistent columns:

`ok BOOLEAN, mode VARCHAR, phase VARCHAR, code VARCHAR, message VARCHAR, detail VARCHAR, sql_name VARCHAR, symbol VARCHAR, artifact_id VARCHAR, connection_scope VARCHAR`

This gives Rtinycc-like “table diagnostics up to compile/register”.

## Core runtime model (important invariant)
Per connection:

- `tcc_session` (mutable config only)
- `tcc_artifact` (immutable compiled/relocated module)

For every compile/register action: create **new `TCCState`**, apply session snapshot, compile, optionally relocate, then freeze artifact.
No incremental post-relocation mutation of same state.

## Full type support plan
`registrations.return_type/arg_types` accept full DuckDB type strings.
Implementation strategy:

1. Parse SQL type strings into DuckDB logical types.
2. Generate per-registration wrapper stubs for scalar/vector callbacks.
3. Type handlers:
   - Primitive numerics/bool/date/time/timestamp/interval/uuid/blob/varchar direct vector APIs.
   - Nested types (list/struct/map/array/union): marshaled using DuckDB vector child accessors and validity propagation; wrapper generator emits per-type traversal glue.
4. NULL semantics controlled by `null_policy` (`propagate`, `call_on_null`, `strict_not_null`).

If a specific type form is unsupported by current DuckDB C API helpers, return explicit diagnostic row with code (`E_TYPE_UNSUPPORTED`) and no silent fallback.

## CMake/build design (mimic/simplify `configure` + `configure.win` from Rtinycc)

### Source layout
- `third_party/tinycc/` (unpacked upstream source)
- `src/tcc_module.c`
- `src/tcc_session.c`
- `src/tcc_compile.c`
- `src/tcc_wrapgen.c`
- `src/tcc_registry.c`
- `src/tcc_platform.c`
- `src/include/tcc_*.h`

### Build steps
- Build TinyCC as static via CMake-driven external build:
  - Unix/macOS: configure + make `libtcc.a` and `libtcc1.a`
  - Windows MSYS2/MinGW: configure/make static targets in MSYS toolchain
- Link extension against `libtcc.a` only
- Package/provide runtime assets path for `libtcc1.a` + TinyCC headers/defs and expose SQL config to set/get path
- Default runtime behavior: auto-resolve configured path from session, override per call

### Platform gates
- WASM: compile-time guard and runtime fail-fast rows (`E_PLATFORM_WASM_UNSUPPORTED`)
- Windows: allow only MinGW/MSYS2; reject MSVC/non-MSYS2 with explicit diagnostics
- macOS:
  - remove/avoid `-flat_namespace` in TinyCC build inputs
  - inject required host symbols explicitly via `tcc_add_symbol` before relocate

## Security/behavior defaults
Compile/register is enabled after load (no safety gate).
Still add explicit diagnostics for:

- forbidden platform
- missing runtime assets path
- unresolved symbols
- unsupported type marshaling case
- duplicate function names

## Tests and acceptance scenarios

1. `tcc_module(mode='config_set')` then `config_get` round-trip for paths/options/libs.
2. `compile` mode with inline source returns successful diagnostics rows.
3. `register` one scalar function and execute SQL call.
4. `register` many symbols in one call with `shared_artifact=true` (single relocate, multiple UDFs).
5. same with `shared_artifact=false` (independent artifacts).
6. connection scoping: registrations visible only in registering connection.
7. relocation invariant: second registration after config change uses new state/artifact.
8. macOS host symbol injection path validated (no unresolved host symbols).
9. Windows MSYS2 build/test pass; non-MSYS2 returns fail-fast diagnostic.
10. WASM mode returns deterministic unsupported diagnostics.
11. runtime-path handling: missing path -> error row; explicit path -> success.
12. full-type signature parsing coverage matrix (including nested types) with at least one happy-path and one failure-path per family.
13. null policy behavior tests for primitive and nested values.
14. deterministic diagnostics schema across all modes.

## Assumptions and defaults
- DuckDB C API in repo remains compatible with current table/scalar function and extra-info APIs.
- Hard unregister may not be available; logical unregister behavior documented if needed.
- `libtcc1.a` is bundled/available and its path is managed via `tcc_module` config modes.
- Session and artifact registries are per connection; no cross-connection sharing in v1.
- API naming is strictly `tcc_*` prefixed.
- Single public SQL function remains `tcc_module` with mode-driven behavior.
