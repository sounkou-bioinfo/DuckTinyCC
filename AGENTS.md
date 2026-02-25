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

## Current Project Plan Reference
- `PLAN_tcc_extension.md`

## Current Implementation State
- Extension artifact/name: `ducktinycc`
- Public SQL entrypoint: `tcc_module(...)`
- Implemented modes:
  - `config_get`, `config_set`, `config_reset`, `list`
  - `compile` (real TinyCC compile+relocate)
  - `register` (fresh TinyCC state per compilation unit, stored in session registry)
  - `call` (one-shot compile+execute OR call by registered `sql_name`)
  - `unregister` (remove and free session artifact)
- Tests currently passing: `make test_debug`, `make test_release`

## Working Rules for This Repo
- Prefix public SQL/API surface with `tcc_*`.
- Use `.sync/Rtinycc` for TinyCC build/runtime decisions.
- Use `.sync/duckhts` for DuckDB C extension API and CMake patterns.
- Keep runtime behavior explicit and diagnosable via SQL-returned tables.
