# DuckTinyCC Extension News

## ducktinycc 0.0.3.9000 (2026-03-03)

- harden scalar-function registration cleanup: temporary `duckdb_scalar_function` objects are now destroyed on both success and error paths to prevent leaks in repeated registration flows
- remove duplicated composite return writeback branches in `tcc_execute_compiled_scalar_udf`; composite return marshalling is now routed through the typedesc-based recursive writer path
- normalize empty descriptor semantics for generated wrappers/runtime: `blob`/`list`/`array` results with `len == 0` and `ptr == NULL` are treated as empty (not NULL), while non-empty results still require non-NULL pointers
- simplify type mapping internals with shared scalar traits macros and generic list/array handling in size/token/C-type/DuckDB-type conversion helpers to reduce drift risk
- make pointer helper registration table-driven to remove repetitive registration code and keep helper signatures centralized
- remove redundant composite-input fallback allocation/cleanup branches in `tcc_execute_compiled_scalar_udf`; typedesc bridge paths are now the single marshalling path for composite args
- add explicit C-function documentation tag guidance in `docs/C_FUNCTION_DOCS.md` and apply ownership/heap/stack/lock/error tags on critical runtime boundary functions

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
