# DuckTinyCC Extension News

## ducktinycc 0.1.0.9000 (2026-04-29)

- **breaking change (wrapper mode naming)**: renamed public `wrapper_mode := 'batch'` to `wrapper_mode := 'chunk_scalar_loop'` to make clear that this mode is a chunk-local scalar loop, not an Arrow or whole-table batch ABI.
- **feature (UDF stability)**: `tcc_module(...)` now accepts `stability := 'consistent' | 'volatile'` for `compile`, `quick_compile`, and `codegen_preview`; `tinycc_bind` can stage the same setting for later compilation. Volatile generated UDFs call DuckDB's `duckdb_scalar_function_set_volatile`, forcing re-execution for every row and preventing constant-folding of side-effectful C functions. Generated C helper modes now assign explicit helper stability internally: pure metadata/enum helpers are consistent, while allocation/free/setter/mutable-memory getter helpers are volatile.
- **bugfix (embedded runtime extraction)**: `tcc_ensure_embedded_runtime` now hashes both `libtcc1.a` and all embedded manifest files (names and contents) when generating the deterministic extraction directory name. Previously, only `libtcc1.a` was hashed, which caused the extension to incorrectly reuse an older, incomplete extraction directory (missing `stdint.h`) after a user upgraded the extension via the community repository.
- **documentation (libc linking / trusted native-code boundary)**: clarified that DuckTinyCC compiles generated modules with `-nostdlib` by default so ordinary libc symbols are not linked implicitly; C code that calls libc functions not injected by DuckTinyCC must explicitly request them with `library := 'c'` or staged `add_library`. Including a header or writing an `extern` declaration is not enough to resolve the symbol. Generated UDFs are trusted in-process native code; DuckTinyCC does not sandbox or muffle `exit`, `abort`, `setjmp`/`longjmp`, inline syscalls, or other native control-flow escapes once users explicitly make them available.
- **test coverage (unsafe UDF control flow)**: added `scripts/test_unsafe_udf_control_flow.sh`, an opt-in subprocess-based probe documenting current behavior for `exit`, `setjmp`/`longjmp`, direct process-termination syscalls from generated UDFs, and the difference between unresolved `exit` by default versus process termination after explicit `library := 'c'`. DuckTinyCC does not currently sandbox or catch native control-flow escapes.
- **demo (R/ggplot2 callback)**: added `demo/ggplot2_via_ducktinycc.R`, which registers a volatile SQL scalar UDF backed by a TinyCC-compiled C trampoline into a pinned R callback. The demo calls `ggplot2::ggsave()` from SQL and uses `R_tryEval` so ordinary R errors do not long-jump through DuckDB frames. Added `demo/ggplot2_cli_demo.sh` / `demo/ggplot2_cli_udf.c` as the DuckDB CLI variant: DuckTinyCC compiles an embedded-R UDF, ggplot2 builds the mtcars plot, and the UDF returns an ANSI-colored terminal canvas that appears directly in the DuckDB CLI. `README.Rmd` now runs that CLI demo and renders the colored ANSI output into README.md via `fansi`.

## ducktinycc 0.1.0 (2026-04-02)

- **bugfix (community extension / no system headers)**: `compile`/`quick_compile` now work correctly on systems without development headers (e.g. no `libc6-dev`/`glibc-devel`). Four compounding bugs caused `E_COMPILE_FAILED` for end-users who installed from the community extension repository:
  1. `tcc_set_lib_path` was called after `tcc_set_output_type` in `tcc_build_module_artifact`. Because TinyCC expands `{B}` in `CONFIG_TCC_SYSINCLUDEPATHS` eagerly at `tcc_set_output_type` time, the embedded runtime extraction directory (`/tmp/ducktinycc_<hash>/include`) was not searched — only the stale build-time `CONFIG_TCCDIR` path was used. Fix: call `tcc_set_lib_path(s, runtime_path)` immediately after `tcc_new()`, before `tcc_set_output_type`.
  2. `stdint.h` was not included in the embedded TinyCC headers, so JIT-compiled wrappers failed with `include file 'stdint.h' not found` on systems without system C headers installed. Fix: added `third_party/tinycc/include/stdint.h` — a self-contained implementation derived from `tccdefs.h` predefined type macros (`__INT32_TYPE__`, `__INT64_TYPE__`, `__INTPTR_TYPE__`, etc.) that requires no system headers.
  3. The generated wrapper emitted an `extern` forward declaration of the user function using wrapper-internal C type names (`long long`, `unsigned long long`, etc.). On 64-bit Linux, TinyCC's `tccdefs.h` defines `__INT64_TYPE__` as `long` (LP64 ABI), so if the user wrote `int64_t func(int64_t x)` (which expands to `long`) the wrapper's `extern long long func(...)` caused `incompatible types for redefinition`. Fix: (a) changed `TCC_FFI_SCALAR_ROWS` to use `int8_t`/`uint8_t`/…/`int64_t`/`uint64_t` type names (matching the prelude's `stdint.h` typedefs); (b) suppressed the `extern` forward declaration entirely when user source is bundled in the same compilation unit — the definition already provides the prototype, so no extern is needed, and it avoids conflicts when the user wrote a raw primitive type (`long long`, `int`, etc.) that doesn't textually match the stdint typedef.
  4. TinyCC unconditionally calls `tcc_add_library(s, "c")` during `tcc_relocate` unless `-nostdlib` is set. In `TCC_OUTPUT_MEMORY` mode all undefined symbols are resolved from the host process via `dlsym(RTLD_DEFAULT, ...)`, so `libc.so` is never needed — but its absence on a bare system produced `library 'c' not found`. Fix: added `tcc_set_options(s, "-nostdlib")` in `tcc_build_module_artifact` before `tcc_set_output_type`.

- **test coverage**: added UNION edge-case tests — NULL union input produces NULL output (not a sentinel), `union<a:i32[];b:i64>` return from C (UNION output with nested list member), deeply nested `union<a:list<struct<x:i64;y:f64>>;b:i64>` input round-trip including empty-list and NULL cases.

- **embedded runtime portability (Windows / subdirectory support)**: `cmake/gen_embedded_runtime.cmake` and `tcc_ensure_embedded_runtime()` now use a relpath-based asset manifest instead of a flat filename list. Each manifest entry's `name` field is a forward-slash path relative to the extraction root (e.g. `"include/stdarg.h"`, `"include/winapi/windows.h"`, `"lib/kernel32.def"`). Extraction creates intermediate directories on demand via a new `tcc_mkdirs_for_relpath()` helper — no directory structure is hardcoded in C code anymore. On Windows (MINGW) builds, `CMakeLists.txt` now passes `WIN32_INCLUDE_DIR` (`third_party/tinycc/win32/include/`, recursive) and `WIN32_LIB_DIR` (`third_party/tinycc/win32/lib/`, `*.def` files) to the cmake script instead of the Unix-only flat `HEADERS_DIR`; this embeds the complete Windows CRT header tree (including `winapi/`, `sec_api/`, `sys/` subtrees) and import library definitions needed for `TCC_TARGET_PE` JIT compilation. The old flat-filename approach only covered `{B}/include/*.h` and could not represent `{B}/include/winapi/windows.h` or `{B}/lib/kernel32.def`. Community extension `windows_amd64` and `windows_arm64` targets are now enabled (removed from `excluded_platforms`). Note: the msvcrt.def bundled from `win32/lib/` targets legacy MSVCRT; UCRT environments (Rtools 4.2+) may need to regenerate it at first run (see AGENTS.md for the Rtools pattern).

## ducktinycc 0.0.4 (2026-03-31)

- **self-contained runtime**: `libtcc1.a` and all TinyCC include headers (`stdarg.h`, `stddef.h`, `tccdefs.h`, etc.) are now embedded as byte arrays directly in the extension binary via `cmake/gen_embedded_runtime.cmake`. On first use, `tcc_ensure_embedded_runtime()` extracts them to a content-hash-keyed temp directory (e.g. `/tmp/ducktinycc_<hash>/`) so that deployed extensions require no external TinyCC runtime files. The extraction is process-global, thread-safe via atomic spinlock, and cross-process reusable (same hash → same dir). `DUCKTINYCC_DEFAULT_RUNTIME_PATH` is removed; `tcc_default_runtime_path()` always uses the embedded extraction path so runtime behaviour is identical on all platforms.

- **bugfix (cross-platform)**: `compile`/`quick_compile` now check the internal SQL-name registry before attempting to call `duckdb_register_scalar_function`. If the name is already registered the mode returns `false`/`E_INIT_FAILED` immediately, instead of relying on DuckDB's registration return value — which silently succeeds on macOS (replacing the function) but fails on Linux. This makes duplicate-name rejection consistent across all platforms.

- fix `list_varchar_type` leak in `RegisterTccModuleFunction`: logical type is now destroyed on early-return path before the table function is fully registered
- consolidate `ducktinycc_register_signature` cleanup: replaced 6 duplicated cleanup blocks (~150 lines) with a single `goto fail` error path
- extract mode handler functions from monolithic `tcc_module_function` dispatcher: `tcc_mode_add_staged`, `tcc_mode_c_helpers`, `tcc_mode_codegen_preview`, `tcc_mode_compile` reduce the dispatcher from ~575 to ~125 lines
- add `const` qualifier to `bind` parameter in `tcc_effective_symbol` and `tcc_effective_sql_name` for const-correctness
- deduplicate struct/union meta-token parsing: new shared `tcc_parse_composite_fields_inner` eliminates ~170 lines of near-identical logic between `tcc_parse_struct_meta_token` and `tcc_parse_union_meta_token`
- add test coverage for `i8`, `u8`, `i16`, `u16`, `u32`, and `f32` scalar type quick_compile round-trips
- add test for bad C source (syntax error) producing `E_COMPILE_FAILED`
- harden scalar-function registration cleanup: temporary `duckdb_scalar_function` objects are now destroyed on both success and error paths to prevent leaks in repeated registration flows
- remove duplicated composite return writeback branches in `tcc_execute_compiled_scalar_udf`; composite return marshalling is now routed through the typedesc-based recursive writer path
- normalize empty descriptor semantics for generated wrappers/runtime: `blob`/`list`/`array` results with `len == 0` and `ptr == NULL` are treated as empty (not NULL), while non-empty results still require non-NULL pointers
- simplify type mapping internals with shared scalar traits macros and generic list/array handling in size/token/C-type/DuckDB-type conversion helpers to reduce drift risk
- make pointer helper registration table-driven to remove repetitive registration code and keep helper signatures centralized
- remove redundant composite-input fallback allocation/cleanup branches in `tcc_execute_compiled_scalar_udf`; typedesc bridge paths are now the single marshalling path for composite args
- add explicit C-function documentation tag guidance in `docs/C_FUNCTION_DOCS.md` and apply ownership/heap/stack/lock/error tags on critical runtime boundary functions
- add `TCC_FFI_COMPOUND_SCALAR_MAP` X-macro and generate `tcc_ffi_type_is_list`, `tcc_ffi_type_is_array`, `tcc_ffi_list_child_type`, `tcc_ffi_list_type_from_child`, `tcc_ffi_array_child_type`, `tcc_ffi_array_type_from_child` from it, replacing ~250 lines of hand-written switch/case boilerplate
- replace sequential `tcc_equals_ci` if/else chain in `tcc_parse_type_token` with table-driven lookup over `simple_tokens[]` array, eliminating ~60 lines
- consolidate `tcc_struct_meta_destroy` and `tcc_union_meta_destroy` via shared `tcc_composite_meta_free_inner` helper
- document `decimal` type bridge (`ducktinycc_decimal_t`) and add DECIMAL round-trip example in README.Rmd
- **bugfix**: fix UNION vector input bridge and output write-back to use correct DuckDB internal layout — tags are now read from child\[0\] (tag vector) via `duckdb_struct_vector_get_child(vector, 0)` instead of `duckdb_vector_get_data(vector)` which returns NULL for STRUCT/UNION physical type; member vectors are now accessed at child\[i+1\] instead of child\[i\] to skip the tag child
- add `ducktinycc_union_tag`, `ducktinycc_union_member_ptr`, `ducktinycc_union_member_is_valid` host-exported helper functions for UNION descriptor access, consistent with existing LIST/STRUCT/MAP helpers
- export `duckdb_validity_row_is_valid` as a host symbol available to TCC-compiled code for direct validity bitmap queries
- add UNION input round-trip and output return end-to-end tests
- deduplicate ROW and BATCH wrapper codegen null-check type-dispatch: new `tcc_codegen_ret_null_check(ret_type)` helper replaces two 8-branch `else-if` ladders (~120 lines removed)
- confirm `c_struct`/`c_union`/`c_bitfield` codegen already well-factored (shared `tcc_generate_c_composite_helpers_source`, single dispatcher, shared compile loop); no further dedup needed
- add `docs/LIFETIME_OWNERSHIP.md`: comprehensive reference for descriptor struct ownership, validity bitmap semantics, heap domains, pointer registry contracts, generated helper ownership rules, and DuckDB extension state lifecycle
- add deeper nested UNION tests: `union<a:i32[];b:i64>` (list member) and `union<a:i32;b:struct<x:i64;y:f64>>` (struct member) input round-trip tests
- **new mode**: `add_symbol` — stages user-supplied symbol name + pointer pairs into the session; replayed via `tcc_add_symbol` at compile time. Enables injection of arbitrary function pointers, shared buffers, or host callbacks into TCC-compiled code. Parameters: `symbol_name` (VARCHAR) + `symbol_ptr` (UBIGINT). Cleared on `tcc_new_state`.

## ducktinycc 0.0.3 (2026-03-02)

- Community extension release

## ducktinycc 0.0.2.9000 (2026-03-01)

- document the actual SQL signature grammar used by `tcc_module(...)`, including canonical scalar tokens and recursive composite tokens (`list<...>`, `type[]`, `type[N]`, `struct<...>`, `map<...>`, `union<...>`)
- document SQL to C bridge correspondences for scalar and composite signatures, including union support
- clarify runtime asset expectations for compile/codegen flows, including the role of `libtcc1.a`
- correct README "How It Works" details for host symbol injection and recursive bridge marshalling
- update community extension description text and add the public project reference URL
- remove legacy scalar/list alias tokens from signature parsing; only canonical tokens are accepted
- reduce duplicate runtime marshalling branches by routing composite arguments through typedesc bridges
- fix recursive composite writeback to avoid reading invalid nested values

## ducktinycc 0.0.2 (2026-02-27)

- public SQL entrypoint `tcc_module(...)` with session/config, build staging, and compile/codegen modes
- compile paths: `compile`, `quick_compile`, `codegen_preview`, plus helper generators `c_struct`, `c_union`, `c_bitfield`, `c_enum`
- runtime diagnostics helpers: `tcc_system_paths(...)`, `tcc_library_probe(...)`
- pointer and buffer SQL helpers: `tcc_alloc`, `tcc_free_ptr`, `tcc_dataptr`, `tcc_ptr_size`, `tcc_ptr_add`, `tcc_read_*`, `tcc_write_*`
- in-memory TinyCC compile + relocate model with generated wrapper registration in DuckDB
