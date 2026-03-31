# DuckTinyCC Extension News

## ducktinycc 0.0.4.9000 (development)

(no changes yet)

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
