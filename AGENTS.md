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
  - Build staging: `add_include`, `add_sysinclude`, `add_library_path`, `add_library`, `add_option`, `add_define`, `add_header`, `add_source`, `add_symbol`, `tinycc_bind`
  - Compile/codegen: `compile`, `quick_compile`, `codegen_preview`
  - Discovery helpers (separate table functions): `tcc_system_paths(...)`, `tcc_library_probe(...)`
- Wrapper/runtime model (Rtinycc-style API-mode codegen):
  - DuckTinyCC generates C wrappers during compile that unpack typed args, call target C symbols, and re-pack result for DuckDB scalar UDF execution.
  - Wrapper modules are compiled+relocated in-memory via libtcc; no separate shared library artifact is produced.
  - Host symbols injected into each TCC state include `duckdb_ext_api`, `ducktinycc_register_signature`, and `ducktinycc_*` helper/runtime symbols used by generated wrappers.
  - Supported SQL-visible signature tokens include:
    - Scalars: `void`, `bool`, `i8/u8`, `i16/u16`, `i32/u32`, `i64/u64`, `f32/f64`, `ptr`, `varchar`, `blob`, `uuid`, `date`, `time`, `timestamp`, `interval`, `decimal`.
    - Nested/composite: `LIST` (`type[]`, `list<...>`), fixed-size `ARRAY` (`type[N]`), `STRUCT` (`struct<...>`), `MAP` (`map<k;v>`), `UNION` (`union<name:type;...>`), with recursive nesting.

## TinyCC State and Relocation Model
- TinyCC states (`TCCState`) are created only in compile/codegen paths (`tcc_build_module_artifact` via `tcc_new`), not by `tcc_new_state`.
- `tcc_new_state` resets staged session build inputs and increments `session.state_id`; it does not allocate a new `TCCState`.
- `compile`/`quick_compile` usually produce one relocated in-memory module artifact (one `TCCState`) per invocation.
- `c_struct`/`c_union`/`c_bitfield`/`c_enum` may produce multiple artifacts because helper bindings are compiled/registered per generated helper function.
- Relocation flow is: `tcc_new` -> configure paths/options/includes/defines/source -> `tcc_relocate` -> `tcc_get_symbol(module_init)` -> call `module_init(connection)`.
- Artifact finalization is explicit:
  - error path: `tcc_delete` immediately,
  - replacement or shutdown path: `tcc_artifact_destroy` via registry entry cleanup and module-state destructor.

## Symbol Injection Rules
- `add_symbol` stages a name+pointer pair into the session; replayed via `tcc_add_symbol(s, name, (void*)(uintptr_t)ptr)` before compilation.
- `symbol_name` must be a valid C identifier (VARCHAR); `symbol_ptr` is a UBIGINT raw address.
- Named parameters to table functions must be constants; use `tcc_dataptr(handle)` in a preceding query and pass the literal value, or use the address-as-value pattern (`extern char SYM[]; (uintptr_t)SYM`).
- User symbols are injected after host symbols (`TCC_HOST_SYMBOL_TABLE`) and before `tcc_compile_string`, during `tcc_apply_session_to_state`.
- Cleared by `tcc_new_state` along with all other staged build inputs.

## Library Linking Rules
- `add_library_path` appends explicit search directories for `tcc_add_library_path`.
- `add_library` accepts:
  - bare names (for example `m`, `z`, `sqlite3`),
  - names with platform suffixes (`libfoo.so`, `foo.dll`, `libfoo.dylib`, `.a`, `.lib`),
  - full library paths (absolute or relative path-like values).
- `tcc_library_probe(...)` should be used to confirm effective search paths/candidates before demos or release examples.

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

## Progress Snapshot (2026-03-31) — 0.0.4 release
- Self-contained embedded runtime landed:
  - `libtcc1.a` and all TinyCC include headers (`stdarg.h`, `stddef.h`, `tccdefs.h`, etc.) are baked into the extension binary as C byte arrays at build time by `cmake/gen_embedded_runtime.cmake`.
  - On first `compile`/`quick_compile` call, `tcc_ensure_embedded_runtime()` extracts them to a content-hash-keyed temp directory (`/tmp/ducktinycc_<fnv1a-hash-of-libtcc1.a>/`).  Extraction is thread-safe (atomic spinlock) and cross-process reusable (dir is reused if already present).
  - `DUCKTINYCC_DEFAULT_RUNTIME_PATH` compile-time definition removed; `tcc_default_runtime_path()` now always calls `tcc_ensure_embedded_runtime()`, giving identical runtime behaviour across all platforms.
  - `make test_embedded_debug` / `make test_embedded_release` targets added; `scripts/test_embedded_runtime.sh` hides the build-dir and runs the full sqllogictest suite to prove the embedded extraction path is exercised.
- Cross-platform duplicate SQL name fix:
  - `tcc_mode_compile` now checks `tcc_registry_find_sql_name()` before calling `tcc_codegen_compile_and_load_module`.  If the SQL name is already registered, returns `false`/`E_INIT_FAILED` immediately, bypassing `duckdb_register_scalar_function` whose return value differs by platform (Linux: error, macOS: silent replace).
- Released as `0.0.4`; current development branch is `0.0.4.9000`.

## Embedded Runtime — Architecture Notes
- `cmake/gen_embedded_runtime.cmake` is a pure-CMake `-P` script (no `objcopy`/`ld`/platform tools):
  - reads `libtcc1.a` via `file(READ ... HEX)` and emits `ducktinycc_embedded_libtcc1[]` + `ducktinycc_embedded_libtcc1_size`.
  - reads each `.h` from `third_party/tinycc/include/` and emits per-file byte arrays plus a NULL-terminated manifest table (`ducktinycc_embedded_headers[]`).
  - output is `${CMAKE_BINARY_DIR}/embedded_runtime.c`, compiled into the extension via `target_sources`.
- `tcc_ensure_embedded_runtime()` in `src/tcc_module.c`:
  - computes FNV-1a 32-bit hash of the embedded `libtcc1.a` content → extraction dir name.
  - writes `libtcc1.a` to `<dir>/libtcc1.a` and each header to `<dir>/include/<name>.h`.
  - calls `tcc_set_lib_path(s, dir)` so `{B}` expands to the extraction dir, satisfying `CONFIG_TCC_SYSINCLUDEPATHS` (`{B}/include`) without any explicit `tcc_add_sysinclude_path`.
- `tccdefs.h` is NOT extracted: TinyCC is built with `CONFIG_TCC_PREDEFS=1`, so `tccdefs_.h` (the preprocessed form) is compiled into `libtcc.a` and never loaded from disk.

## Rtools/MinGW Windows Support — Status and Blocking Issues
- Current state: `windows_amd64` and `windows_arm64` are in `excluded_platforms`.  The exclusion is not merely build complexity — there are runtime asset gaps described below.
- On `TCC_TARGET_PE`, `tcc_add_library()` searches library paths for `.def` files **before** `.dll`/`.a` (`libtcc.c:1291`: `%s/%s.def`, `%s/lib%s.def`, …).  Without `.def` files in `{B}/lib`, all `tcc_add_library` calls fail silently and standard C library symbols are unresolved in JIT code.
- `tcc.h` line 278: `CONFIG_TCC_SYSINCLUDEPATHS` expands to `{B}/include` **and** `{B}/include/winapi` on Windows.  The `winapi/` subtree (`windows.h`, `windef.h`, etc.) lives in `third_party/tinycc/win32/include/` and is not currently embedded.
- Static `.def` files in `third_party/tinycc/win32/lib/` (`kernel32.def`, `user32.def`, `gdi32.def`, `ws2_32.def`) are fixed content and can be embedded exactly like the include headers.
- `msvcrt.def` from `win32/lib/` is **wrong for Rtools 4.2+ (UCRT)**:
  - Rtools uses `ucrtbase.dll` as the CRT.  If JIT-compiled code resolves `calloc()` via the old `msvcrt.def` but the host (DuckDB/R) frees with UCRT's `free()`, a heap-mismatch crash results.
  - The Rtinycc fix (see `.sync/Rtinycc/configure.win`) is to run `tcc.exe -impdef ucrtbase.dll -o lib/msvcrt.def` at install time on the target machine — generating the correct symbol list from the actual DLL present.
  - This step is **machine-specific** and cannot be pre-baked at CI build time.
- What Rtools/MinGW support would require:
  1. Embed `win32/lib/kernel32.def`, `user32.def`, `gdi32.def`, `ws2_32.def` alongside `libtcc1.a` in `gen_embedded_runtime.cmake`.
  2. Embed `third_party/tinycc/win32/include/` headers (including `winapi/` subtree) into a separate manifest extracted to `{B}/include/winapi/`.
  3. In `tcc_ensure_embedded_runtime()`, after extracting the static `.def` files, generate the UCRT-correct `msvcrt.def` at first-run by invoking `tcc -impdef` (via libtcc in-process or a small subprocess using the embedded `tcc.exe` bytes) against the local `ucrtbase.dll`.
  4. Test the full suite with Rtools UCRT toolchain via the MinGW cross-compilation path.
- Until step 3 is solved, Windows targets remain excluded.  Reference: `.sync/Rtinycc/configure.win` for the proven Rtools pattern.

## Tasks Ahead (Near Term)
- Test coverage expansion:
  - deeper nested composites inside UNION members (e.g., `union<a:list<struct<...>>;b:map<k;v>>`).
  - UNION output with nested composite members (return union containing list/struct from C).
  - edge cases: NULL union values, empty lists in unions, deeply recursive nesting.
- Runtime bridge dedup opportunities:
  - row vs batch allocation/cleanup branches in `tcc_execute_compiled_scalar_udf` still have some repeated patterns.
  - list/array bridge paths are layout-identical and could share more code.
- Rtools/MinGW Windows support (see section above):
  - embed static `.def` files and `win32/include/winapi/` headers.
  - solve UCRT `msvcrt.def` generation at first-run.
  - remove Windows from `excluded_platforms` only after full suite passes under Rtools UCRT.
- Use R-style development versions: `x.y.z.9000` means in-development; `x.y.z` means release.
- Pre-`1.0.0` compatibility stance: prioritize correctness, simplification, and API clarity over backward compatibility.
- Pre-`1.0.0` breaking changes are acceptable without shims when they reduce complexity or fix incorrect behavior, but they must be called out in `NEWS.md`.
- Version bump rules:
  - On any user-visible API/behavior/docs change on development branch, keep/bump to `.9000`.
  - At release cut, increment `.9000` (for example `0.0.3.9000` -> `0.0.4`).
  - Immediately after release, bump next development version (for example `0.0.3` -> `0.0.3.9000`).
- `NEWS.md` is mandatory and manually maintained (not generated).
- Every change that affects SQL surface, C bridge behavior, runtime requirements, or helper ABI must add an entry under the current top development section in `NEWS.md`.
- `NEWS.md` entry format should stay concise and explicit:
  - Header: `## ducktinycc x.y.z(.9000) (YYYY-MM-DD)`
  - Bullets: what changed, why it matters, and any compatibility/runtime note.

## Tasks Ahead (Near Term)
- Test coverage expansion:
  - deeper nested composites inside UNION members (e.g., `union<a:list<struct<...>>;b:map<k;v>>`).
  - UNION output with nested composite members (return union containing list/struct from C).
  - edge cases: NULL union values, empty lists in unions, deeply recursive nesting.
- Runtime bridge dedup opportunities:
  - row vs batch allocation/cleanup branches in `tcc_execute_compiled_scalar_udf` still have some repeated patterns.
  - list/array bridge paths are layout-identical and could share more code.
- Release preparation:
  - version bump policy enforcement (`0.0.3.9000` → `0.0.4` release cut).
  - community extension submission review against latest `community-extensions/` templates.
  - README.Rmd → README.md render and final review.

## Working Rules for This Repo
- API is intentionally evolving; pre-`1.0.0`, do not spend effort on backward-compatibility shims unless explicitly required for a release.
- Prefix public SQL/API surface with `tcc_*`.
- Use `.sync/Rtinycc` for TinyCC build/runtime decisions.
- Use `.sync/duckhts` for DuckDB C extension API and CMake patterns.
- Keep runtime behavior explicit and diagnosable via SQL-returned tables.
