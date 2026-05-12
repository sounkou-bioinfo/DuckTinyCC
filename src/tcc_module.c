/*
 * DuckTinyCC
 * SPDX-License-Identifier: MIT
 *
 * Strategy overview:
 * - `tcc_module(...)` is the control plane: it stages session-scoped TinyCC inputs (headers, sources, include/lib
 *   paths, options, defines) and dispatches modes through DuckDB table-function lifecycle callbacks.
 * - Compile/codegen paths build wrappers as C source, compile + relocate them in memory via libtcc, and resolve a
 *   module init symbol (no per-UDF shared-library artifact on disk).
 * - TinyCC state creation is compile-triggered (`tcc_new` in artifact builder); `tcc_new_state` only resets staged
 *   session inputs and increments `state_id`.
 * - Each generated module self-registers scalar UDFs by calling `ducktinycc_register_signature(...)` against a
 *   persistent host DuckDB connection, using host-exported symbols injected into the TCC state.
 *   This shape is intentional: TinyCC-relocated code has no direct SQL DDL context, so registration must cross back
 *   through host C-API callbacks with a stable connection handle.
 * - Resulting UDF entries are extension/C-API registered catalog entries, which explains current lifecycle behavior
 *   (e.g., SQL `DROP FUNCTION` does not remove these internal entries).
 * - Runtime execution (`tcc_execute_compiled_scalar_udf`) bridges DuckDB vectors to C descriptors for row/chunk-scalar-loop
 *   wrappers, including recursive LIST/ARRAY/STRUCT/MAP/UNION marshalling and write-back.
 * - Link configuration supports both search-path + bare names and explicit full library paths.
 * - This file intentionally centralizes SQL surface, compile/load, and runtime bridge logic to keep behavior
 *   diagnosable while the pre-1.0 API remains fast-moving.
 *
 * TinyCC embedding precedent:
 * - https://github.com/sounkou-bioinfo/Rtinycc
 */

/*
 * Allocation/Lifetime Model (heap domains):
 * - DuckDB-owned heap (`duckdb_malloc`/`duckdb_free`):
 *   used for extension state, bind/init payloads, parsed metadata, bridge scratch buffers, and generated source text.
 * - libc heap (`malloc`/`free`):
 *   used by pointer-registry payload allocations and generated helper `*_new`/`*_free` functions.
 * - Borrowed DuckDB vector/chunk memory:
 *   pointers fetched from vectors/validity buffers/string payloads are non-owning views and valid only for call scope.
 *
 * Ownership rules:
 * - `destroy_*` callbacks release DuckDB-owned bind/init/extra-info payloads.
 * - `tcc_host_sig_ctx_destroy` owns/releases parsed signature metadata attached to registered UDFs.
 * - `tcc_artifact_destroy` owns/releases in-memory relocated TinyCC modules.
 * - Descriptor structs (`ducktinycc_list_t`/`array_t`/`struct_t`/`map_t`/`union_t`) are borrowed views, never freed by wrappers.
 */

#include "duckdb_extension.h"
#include "tcc_module.h"
#include "ducktinycc_udf_abi_source.h"

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
#include "libtcc.h"
#endif

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>   /* _mkdir */
#define TCC_ACCESS _access
#define TCC_ACCESS_FOK 0
#define TCC_ENV_PATH_SEP ';'
#define TCC_MKDIR(p) (_mkdir(p) == 0 || errno == EEXIST)
#else
#include <unistd.h>
#include <sys/stat.h> /* mkdir */
#include <errno.h>
#define TCC_ACCESS access
#define TCC_ACCESS_FOK F_OK
#define TCC_ENV_PATH_SEP ':'
#define TCC_MKDIR(p) (mkdir((p), 0755) == 0 || errno == EEXIST)
#endif

DUCKDB_EXTENSION_EXTERN

/* BEGIN: TCC_FUNCTION_CATALOG
 * Function Catalog (`src/tcc_module.c`)
 *
 * Scope: complete function list compiled from this translation unit.
 * Purpose: quick ownership/audit reference when changing runtime, codegen, and bridge logic.
 */
/* - RegisterTccModuleFunction: Registers `tcc_module` plus diagnostic/probe table functions on a DuckDB connection. */
/* - destroy_tcc_diag_bind_data: Destructor callback for bind/init/state allocations owned by DuckDB function/table contexts. */
/* - destroy_tcc_diag_init_data: Destructor callback for bind/init/state allocations owned by DuckDB function/table contexts. */
/* - destroy_tcc_module_bind_data: Destructor callback for bind/init/state allocations owned by DuckDB function/table contexts. */
/* - destroy_tcc_module_init_data: Destructor callback for bind/init/state allocations owned by DuckDB function/table contexts. */
/* - destroy_tcc_module_state: Destructor callback for bind/init/state allocations owned by DuckDB function/table contexts. */
/* - ducktinycc_array_elem_ptr: ARRAY descriptor accessor helper for generated wrappers. */
/* - ducktinycc_array_is_valid: ARRAY descriptor accessor helper for generated wrappers. */
/* - ducktinycc_buf_ptr_at: Range-checked pointer lookup inside raw byte buffers. */
/* - ducktinycc_buf_ptr_at_mut: Range-checked pointer lookup inside raw byte buffers. */
/* - ducktinycc_list_elem_ptr: LIST descriptor accessor helper for generated wrappers. */
/* - ducktinycc_list_is_valid: LIST descriptor accessor helper for generated wrappers. */
/* - ducktinycc_map_key_is_valid: MAP descriptor accessor helper for generated wrappers. */
/* - ducktinycc_map_key_ptr: MAP descriptor accessor helper for generated wrappers. */
/* - ducktinycc_map_value_is_valid: MAP descriptor accessor helper for generated wrappers. */
/* - ducktinycc_map_value_ptr: MAP descriptor accessor helper for generated wrappers. */
/* - ducktinycc_ptr_add: Pointer arithmetic helper for generated wrapper code. */
/* - ducktinycc_ptr_add_mut: Pointer arithmetic helper for generated wrapper code. */
/* - ducktinycc_read_bytes: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_f32: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_f64: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_i16: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_i32: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_i64: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_i8: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_ptr: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_u16: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_u32: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_u64: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_read_u8: Typed read helper from raw memory or bridge descriptors. */
/* - ducktinycc_register_signature: Registers a generated wrapper symbol as a DuckDB scalar UDF with parsed type metadata. */
/* - ducktinycc_span_contains: Bounds-check helper used by pointer/bridge accessors. */
/* - ducktinycc_span_fits: Bounds-check helper used by pointer/bridge accessors. */
/* - ducktinycc_struct_field_is_valid: STRUCT descriptor accessor helper for generated wrappers. */
/* - ducktinycc_struct_field_ptr: STRUCT descriptor accessor helper for generated wrappers. */
/* - ducktinycc_union_tag: UNION descriptor accessor helper for generated wrappers. */
/* - ducktinycc_union_member_ptr: UNION descriptor accessor helper for generated wrappers. */
/* - ducktinycc_union_member_is_valid: UNION descriptor accessor helper for generated wrappers. */
/* - ducktinycc_valid_is_set: Validity bitmap helper for generated wrappers and bridge descriptors. */
/* - ducktinycc_valid_set: Validity bitmap helper for generated wrappers and bridge descriptors. */
/* - ducktinycc_write_bytes: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_f32: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_f64: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_i16: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_i32: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_i64: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_i8: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_ptr: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_u16: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_u32: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_u64: Typed write helper into raw memory or bridge descriptors. */
/* - ducktinycc_write_u8: Typed write helper into raw memory or bridge descriptors. */
/* - register_tcc_library_probe_function: Registers extension helper functions/tables into DuckDB. */
/* - register_tcc_pointer_helper_functions: Registers extension helper functions/tables into DuckDB. */
/* - register_tcc_system_paths_function: Registers extension helper functions/tables into DuckDB. */
/* - tcc_add_host_symbols: Registers host-exported symbols into each TinyCC state for generated wrappers. */
/* - tcc_add_platform_library_paths: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_alloc_scalar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_append_env_path_list: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_append_error: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_apply_bind_overrides_to_state: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_apply_session_to_state: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_artifact_destroy: Releases compiled TinyCC module artifact resources. */
/* - tcc_basename_ptr: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_bind_read_named_arg_types: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_bind_read_named_varchar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_build_c_composite_bindings: Builder helper for bridge objects, helper source/bindings, module artifacts, or search candidates. */
/* - tcc_build_c_enum_bindings: Builder helper for bridge objects, helper source/bindings, module artifacts, or search candidates. */
/* - tcc_build_library_candidates: Builder helper for bridge objects, helper source/bindings, module artifacts, or search candidates. */
/* - tcc_build_module_artifact: Builder helper for bridge objects, helper source/bindings, module artifacts, or search candidates. */
/* - tcc_build_struct_bridge_from_vector: Builder helper for bridge objects, helper source/bindings, module artifacts, or search candidates. */
/* - tcc_build_value_bridge: Builder helper for bridge objects, helper source/bindings, module artifacts, or search candidates. */
/* - tcc_c_field_list_append: Dynamic field metadata list utility for c_struct/c_union/c_bitfield helper codegen. */
/* - tcc_c_field_list_destroy: Dynamic field metadata list utility for c_struct/c_union/c_bitfield helper codegen. */
/* - tcc_c_field_list_reserve: Dynamic field metadata list utility for c_struct/c_union/c_bitfield helper codegen. */
/* - tcc_codegen_build_compilation_unit: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_classify_error_message: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_compile_and_load_module: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_generate_wrapper_source: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_prepare_sources: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_signature_ctx_destroy: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_signature_ctx_init: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_signature_parse_types: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_signature_parse_wrapper_mode: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_source_ctx_destroy: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_codegen_source_ctx_init: Code generation helper for wrapper source assembly, classification, and compile/load flow. */
/* - tcc_collect_include_paths: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_collect_library_search_paths: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_compile_generated_binding: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_configure_runtime_paths: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_copy_duckdb_string_as_cstr: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_dataptr_scalar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_default_runtime_path: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_ensure_embedded_runtime: Extracts embedded libtcc1.a + headers to a temp dir; returns the stable extraction path. */
/* - tcc_fnv1a_hash: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_write_file_bytes: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_diag_rows_add: Diagnostic row buffer utility used by probe/system-path table functions. */
/* - tcc_diag_rows_destroy: Diagnostic row buffer utility used by probe/system-path table functions. */
/* - tcc_diag_rows_reserve: Diagnostic row buffer utility used by probe/system-path table functions. */
/* - tcc_diag_set_result_schema: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_diag_table_function: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_diag_table_init: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_duckdb_string_to_blob: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_effective_sql_name: Resolves effective symbol/SQL name from bind args and session defaults. */
/* - tcc_effective_symbol: Resolves effective symbol/SQL name from bind args and session defaults. */
/* - tcc_equals_ci: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_execute_compiled_scalar_udf: Main runtime bridge for executing compiled row/chunk-scalar-loop wrappers and marshaling values. */
/* - tcc_ffi_array_child_type: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_ffi_array_type_from_child: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_ffi_list_child_type: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_ffi_list_type_from_child: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_ffi_type_create_logical_type: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_is_array: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_is_fixed_width_scalar: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_is_list: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_is_map: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_is_struct: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_is_union: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_size: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_to_c_type_name: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_to_duckdb_type: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_ffi_type_to_token: FFI type conversion helper across tokens, C types, DuckDB logical types, and byte widths. */
/* - tcc_find_top_level_char: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_format_cstr: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_free_ptr_scalar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_generate_c_composite_helpers_source: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_generate_c_enum_helpers_source: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_get_ptr_registry: Fetches pointer registry from scalar function context, reporting errors to DuckDB on failure. */
/* - tcc_has_library_suffix: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_helper_binding_list_add: Dynamic helper-binding list utility for generated helper UDF registration. */
/* - tcc_helper_binding_list_add_prefixed: Dynamic helper-binding list utility for generated helper UDF registration. */
/* - tcc_helper_binding_list_destroy: Dynamic helper-binding list utility for generated helper UDF registration. */
/* - tcc_helper_binding_list_reserve: Dynamic helper-binding list utility for generated helper UDF registration. */
/* - tcc_host_sig_ctx_destroy: Releases UDF signature context, including parsed type metadata and descriptors. */
/* - tcc_is_identifier_token: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_is_path_like: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_library_link_name_from_path: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_library_probe_bind: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_map_meta_array_destroy: MAP metadata lifecycle helper for parsed signatures. */
/* - tcc_map_meta_destroy: MAP metadata lifecycle helper for parsed signatures. */
/* - tcc_mode_requires_write_lock: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_module_bind: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_module_function: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_module_init: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_nested_struct_bridge_destroy: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_next_top_level_part: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_parse_c_enum_constants: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_c_field_spec_token: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_c_field_specs: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_map_meta_token: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_signature: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_struct_meta_token: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_type_token: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_union_meta_token: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_parse_wrapper_mode: Parser helper for signatures, wrapper mode, C helper field specs, or nested type tokens. */
/* - tcc_path_exists: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_path_join: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_ptr_add_scalar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_ptr_helper_ctx_destroy: Destructor for scalar helper extra-info context holding pointer registry references. */
/* - tcc_ptr_registry_alloc: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_create: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_destroy: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_find_handle_unlocked: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_free: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_get_ptr_size: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_lock: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_read: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_ref: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_reserve_unlocked: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_unlock: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_unref: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_registry_write: Pointer registry allocator/lookup/IO primitive for `tcc_alloc` and pointer helper UDFs. */
/* - tcc_ptr_size_scalar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_ptr_span_fits: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_read_bytes_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_f32_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_f64_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_i16_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_i32_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_i64_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_i8_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_u16_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_u32_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_u64_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_read_u8_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_register_pointer_scalar: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_registry_entry_destroy_metadata: Compiled-artifact metadata registry helper for SQL name to artifact lookup/storage. */
/* - tcc_registry_find_sql_name: Compiled-artifact metadata registry helper for SQL name to artifact lookup/storage. */
/* - tcc_registry_reserve: Compiled-artifact metadata registry helper for SQL name to artifact lookup/storage. */
/* - tcc_registry_store_metadata: Compiled-artifact metadata registry helper for SQL name to artifact lookup/storage. */
/* - tcc_rwlock_init: Spin-based read/write lock primitive for extension state coordination. */
/* - tcc_rwlock_read_lock: Spin-based read/write lock primitive for extension state coordination. */
/* - tcc_rwlock_read_unlock: Spin-based read/write lock primitive for extension state coordination. */
/* - tcc_rwlock_write_lock: Spin-based read/write lock primitive for extension state coordination. */
/* - tcc_rwlock_write_unlock: Spin-based read/write lock primitive for extension state coordination. */
/* - tcc_session_clear_bind: Session state helper for runtime path, staged sources, and symbol bindings. */
/* - tcc_session_clear_build_state: Session state helper for runtime path, staged sources, and symbol bindings. */
/* - tcc_session_runtime_path: Session state helper for runtime path, staged sources, and symbol bindings. */
/* - tcc_session_set_runtime_path: Session state helper for runtime path, staged sources, and symbol bindings. */
/* - tcc_set_error: Value/error/validity setter helper for vectors and diagnostics output. */
/* - tcc_set_output_row_null: Value/error/validity setter helper for vectors and diagnostics output. */
/* - tcc_set_varchar_col: Value/error/validity setter helper for vectors and diagnostics output. */
/* - tcc_set_vector_row_validity: Value/error/validity setter helper for vectors and diagnostics output. */
/* - tcc_skip_space: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_split_csv_tokens: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_strdup: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_string_ends_with: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_string_equals_path: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_string_list_append: Dynamic string list container utility (reserve/append/destroy/rollback/uniqueness). */
/* - tcc_string_list_append_unique: Dynamic string list container utility (reserve/append/destroy/rollback/uniqueness). */
/* - tcc_string_list_contains: Dynamic string list container utility (reserve/append/destroy/rollback/uniqueness). */
/* - tcc_string_list_destroy: Dynamic string list container utility (reserve/append/destroy/rollback/uniqueness). */
/* - tcc_string_list_pop_last: Dynamic string list container utility (reserve/append/destroy/rollback/uniqueness). */
/* - tcc_string_list_reserve: Dynamic string list container utility (reserve/append/destroy/rollback/uniqueness). */
/* - tcc_struct_meta_array_destroy: STRUCT metadata lifecycle helper for parsed signatures. */
/* - tcc_struct_meta_destroy: STRUCT metadata lifecycle helper for parsed signatures. */
/* - tcc_system_paths_bind: DuckDB bind/init/execute callback for module or diagnostics/probe table functions. */
/* - tcc_text_buf_appendf: Growable text buffer utility used by code generation paths. */
/* - tcc_text_buf_destroy: Growable text buffer utility used by code generation paths. */
/* - tcc_text_buf_reserve: Growable text buffer utility used by code generation paths. */
/* - tcc_trim_inplace: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_try_resolve_candidate: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_typedesc_create_logical_type: Recursive typedesc parser/converter used for nested SQL/C type bridging. */
/* - tcc_typedesc_destroy: Recursive typedesc parser/converter used for nested SQL/C type bridging. */
/* - tcc_typedesc_is_composite: Recursive typedesc parser/converter used for nested SQL/C type bridging. */
/* - tcc_typedesc_parse_token: Recursive typedesc parser/converter used for nested SQL/C type bridging. */
/* - tcc_union_meta_array_destroy: UNION metadata lifecycle helper for parsed signatures. */
/* - tcc_union_meta_destroy: UNION metadata lifecycle helper for parsed signatures. */
/* - tcc_valid_input_row: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_validity_set_all: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_value_bridge_destroy: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_wrapper_mode_token: Utility/helper function supporting parsing, diagnostics, paths, locking, or runtime configuration. */
/* - tcc_write_bytes_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_f32_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_f64_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_i16_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_i32_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_i64_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_i8_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_row: Internal helper in the TinyCC module/runtime pipeline. */
/* - tcc_write_u16_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_u32_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_u64_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_u8_scalar: Scalar UDF implementation for pointer/memory helper SQL functions. */
/* - tcc_write_value_to_vector: Internal helper in the TinyCC module/runtime pipeline. */
/* END: TCC_FUNCTION_CATALOG */

/* Generic growable list of owned strings. */
typedef struct {
	char **items;
	idx_t count;
	idx_t capacity;
} tcc_string_list_t;

/* Minimal spin-based RW lock for connection-local module state. */
typedef struct {
	atomic_bool writer;
	atomic_uint readers;
	atomic_uint pending_writers;
} tcc_rwlock_t;

typedef struct {
	uint64_t handle;
	void *ptr;
	uint64_t size;
	bool owned;
} tcc_ptr_entry_t;

/* Process-global pointer registry used by SQL helpers (`tcc_alloc`/`tcc_free_ptr`).
 * Ownership contract:
 * - `owned=true`: registry owns allocation and frees with libc `free`.
 * - `owned=false`: borrowed pointer; registry only tracks metadata.
 */
typedef struct {
	atomic_uint ref_count;
	atomic_flag lock;
	tcc_ptr_entry_t *entries;
	idx_t count;
	idx_t capacity;
	uint64_t next_handle;
} tcc_ptr_registry_t;

/* Mutable per-connection TinyCC build session (staged inputs + bind defaults). */
typedef struct {
	char *runtime_path;
	char *bound_symbol;
	char *bound_sql_name;
	char *bound_stability;
	tcc_string_list_t include_paths;
	tcc_string_list_t sysinclude_paths;
	tcc_string_list_t library_paths;
	tcc_string_list_t libraries;
	tcc_string_list_t options;
	tcc_string_list_t headers;
	tcc_string_list_t sources;
	tcc_string_list_t define_names;
	tcc_string_list_t define_values;
	tcc_string_list_t symbol_names;
	uint64_t *symbol_ptrs;
	idx_t symbol_count;
	idx_t symbol_capacity;
	uint64_t config_version;
	uint64_t state_id;
} tcc_session_t;

/* Internal FFI type universe used across parser, codegen, and runtime bridge. */
typedef enum {
	TCC_FFI_VOID = 0,
	TCC_FFI_BOOL = 1,
	TCC_FFI_I8 = 2,
	TCC_FFI_U8 = 3,
	TCC_FFI_I16 = 4,
	TCC_FFI_U16 = 5,
	TCC_FFI_I32 = 6,
	TCC_FFI_U32 = 7,
	TCC_FFI_I64 = 8,
	TCC_FFI_U64 = 9,
	TCC_FFI_F32 = 10,
	TCC_FFI_F64 = 11,
	TCC_FFI_VARCHAR = 12,
	TCC_FFI_BLOB = 13,
	TCC_FFI_UUID = 14,
	TCC_FFI_DATE = 15,
	TCC_FFI_TIME = 16,
	TCC_FFI_TIMESTAMP = 17,
	TCC_FFI_INTERVAL = 18,
	TCC_FFI_DECIMAL = 19,
	TCC_FFI_STRUCT = 20,
	TCC_FFI_MAP = 21,
	TCC_FFI_PTR = 22,
	TCC_FFI_UNION = 23,
	TCC_FFI_LIST = 24,
	TCC_FFI_ARRAY = 25,
	TCC_FFI_LIST_BOOL = 64,
	TCC_FFI_LIST_I8 = 65,
	TCC_FFI_LIST_U8 = 66,
	TCC_FFI_LIST_I16 = 67,
	TCC_FFI_LIST_U16 = 68,
	TCC_FFI_LIST_I32 = 69,
	TCC_FFI_LIST_U32 = 70,
	TCC_FFI_LIST_I64 = 71,
	TCC_FFI_LIST_U64 = 72,
	TCC_FFI_LIST_F32 = 73,
	TCC_FFI_LIST_F64 = 74,
	TCC_FFI_LIST_UUID = 75,
	TCC_FFI_LIST_DATE = 76,
	TCC_FFI_LIST_TIME = 77,
	TCC_FFI_LIST_TIMESTAMP = 78,
	TCC_FFI_LIST_INTERVAL = 79,
	TCC_FFI_LIST_DECIMAL = 80,
	TCC_FFI_ARRAY_BOOL = 96,
	TCC_FFI_ARRAY_I8 = 97,
	TCC_FFI_ARRAY_U8 = 98,
	TCC_FFI_ARRAY_I16 = 99,
	TCC_FFI_ARRAY_U16 = 100,
	TCC_FFI_ARRAY_I32 = 101,
	TCC_FFI_ARRAY_U32 = 102,
	TCC_FFI_ARRAY_I64 = 103,
	TCC_FFI_ARRAY_U64 = 104,
	TCC_FFI_ARRAY_F32 = 105,
	TCC_FFI_ARRAY_F64 = 106,
	TCC_FFI_ARRAY_UUID = 107,
	TCC_FFI_ARRAY_DATE = 108,
	TCC_FFI_ARRAY_TIME = 109,
	TCC_FFI_ARRAY_TIMESTAMP = 110,
	TCC_FFI_ARRAY_INTERVAL = 111,
	TCC_FFI_ARRAY_DECIMAL = 112
} tcc_ffi_type_t;

/* Scalar bridge value types (layout-compatible with DuckDB C API primitives). */
typedef struct {
	uint64_t lower;
	int64_t upper;
} ducktinycc_hugeint_t;

typedef struct {
	const void *ptr;
	uint64_t len;
} ducktinycc_blob_t;

typedef struct {
	int32_t days;
} ducktinycc_date_t;

typedef struct {
	int64_t micros;
} ducktinycc_time_t;

typedef struct {
	int64_t micros;
} ducktinycc_timestamp_t;

typedef struct {
	int32_t months;
	int32_t days;
	int64_t micros;
} ducktinycc_interval_t;

typedef struct {
	uint8_t width;
	uint8_t scale;
	ducktinycc_hugeint_t value;
} ducktinycc_decimal_t;

/* Composite bridge descriptors are borrowed views over DuckDB vectors.
 * The generated wrapper must not free or persist these pointers after the call.
 * `offset` is a global row offset for validity/indexing into child vectors.
 */
typedef struct {
	const void *ptr;
	const uint64_t *validity;
	uint64_t offset;
	uint64_t len;
} ducktinycc_list_t;

typedef struct {
	const void *ptr;
	const uint64_t *validity;
	uint64_t offset;
	uint64_t len;
} ducktinycc_array_t;

typedef struct {
	const void *const *field_ptrs;
	const uint64_t *const *field_validity;
	uint64_t field_count;
	uint64_t offset;
} ducktinycc_struct_t;

typedef struct {
	const void *key_ptr;
	const uint64_t *key_validity;
	const void *value_ptr;
	const uint64_t *value_validity;
	uint64_t offset;
	uint64_t len;
} ducktinycc_map_t;

typedef struct {
	const uint8_t *tag_ptr;
	const void *const *member_ptrs;
	const uint64_t *const *member_validity;
	uint64_t member_count;
	uint64_t offset;
} ducktinycc_union_t;

/* Wrapper ABI mode for generated C entrypoints. */
typedef enum {
	TCC_WRAPPER_MODE_ROW = 0,
	TCC_WRAPPER_MODE_BATCH = 1
} tcc_wrapper_mode_t;

typedef enum {
	TCC_FUNCTION_STABILITY_CONSISTENT = 0,
	TCC_FUNCTION_STABILITY_VOLATILE = 1
} tcc_function_stability_t;

/* Function pointer shapes exported by generated modules. */
typedef bool (*tcc_dynamic_init_fn_t)(duckdb_connection connection);
typedef bool (*tcc_host_row_wrapper_fn_t)(void **args, void *out_value, bool *out_is_null);
typedef bool (*tcc_host_batch_wrapper_fn_t)(void **arg_data, uint64_t **arg_validity, uint64_t count, void *out_data,
                                            uint64_t *out_validity);

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* Owns one relocated TinyCC module artifact and its init symbol. */
typedef struct {
	TCCState *tcc;
	bool is_module;
	tcc_dynamic_init_fn_t module_init;
	char *sql_name;
	char *symbol;
	uint64_t state_id;
} tcc_registered_artifact_t;
#endif

/* Registry entry mapping SQL name to compiled module metadata. */
typedef struct {
	char *sql_name;
	char *symbol;
	uint64_t state_id;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	tcc_registered_artifact_t *artifact;
#endif
} tcc_registered_entry_t;

/* Root extension state stored as table-function extra info. */
typedef struct {
	duckdb_connection connection;
	duckdb_database database;
	tcc_rwlock_t lock;
	tcc_ptr_registry_t *ptr_registry;
	tcc_session_t session;
	tcc_registered_entry_t *entries;
	idx_t entry_count;
	idx_t entry_capacity;
} tcc_module_state_t;

/* Parsed named arguments for one `tcc_module(...)` invocation. */
typedef struct {
	char *mode;
	char *runtime_path;
	char *source;
	char *symbol;
	char *sql_name;
	char *arg_types;
	char *return_type;
	char *wrapper_mode;
	char *stability;
	char *include_path;
	char *sysinclude_path;
	char *library_path;
	char *library;
	char *option;
	char *header;
	char *define_name;
	char *define_value;
	char *symbol_name;
	uint64_t symbol_ptr;
	bool has_symbol_ptr;
} tcc_module_bind_data_t;

/* Per-scan init state: ensures table-function emits once. */
typedef struct {
	atomic_bool emitted;
} tcc_module_init_data_t;

/* Shared error buffer for parser/codegen/compile diagnostics. */
typedef struct {
	char message[4096];
} tcc_error_buffer_t;

/* Forward declaration for recursive type descriptor tree. */
typedef struct tcc_typedesc tcc_typedesc_t;

/* Parsed STRUCT signature metadata (flat representation). */
typedef struct {
	int field_count;
	char **field_names;
	char **field_tokens;
	tcc_ffi_type_t *field_types;
	size_t *field_sizes;
} tcc_ffi_struct_meta_t;

/* Parsed MAP signature metadata. */
typedef struct {
	char *key_token;
	char *value_token;
	tcc_ffi_type_t key_type;
	tcc_ffi_type_t value_type;
	size_t key_size;
	size_t value_size;
} tcc_ffi_map_meta_t;

/* Parsed UNION signature metadata (flat representation). */
typedef struct {
	int member_count;
	char **member_names;
	char **member_tokens;
	tcc_ffi_type_t *member_types;
	size_t *member_sizes;
} tcc_ffi_union_meta_t;

/* Runtime UDF signature context attached to DuckDB scalar function extra info. */
typedef struct {
	tcc_wrapper_mode_t wrapper_mode;
	tcc_host_row_wrapper_fn_t row_wrapper;
	tcc_host_batch_wrapper_fn_t batch_wrapper;
	int arg_count;
	tcc_ffi_type_t return_type;
	tcc_ffi_type_t *arg_types;
	size_t *arg_sizes;
	size_t return_array_size;
	size_t *arg_array_sizes;
	tcc_ffi_struct_meta_t return_struct_meta;
	tcc_ffi_map_meta_t return_map_meta;
	tcc_ffi_union_meta_t return_union_meta;
	tcc_ffi_struct_meta_t *arg_struct_metas;
	tcc_ffi_map_meta_t *arg_map_metas;
	tcc_ffi_union_meta_t *arg_union_metas;
	tcc_typedesc_t *return_desc;
	tcc_typedesc_t **arg_descs;
} tcc_host_sig_ctx_t;

/* Nested bridge container variants for recursive composite marshalling. */
typedef enum {
	TCC_NESTED_BRIDGE_STRUCT = 1,
	TCC_NESTED_BRIDGE_LIST = 2,
	TCC_NESTED_BRIDGE_ARRAY = 3,
	TCC_NESTED_BRIDGE_MAP = 4
} tcc_nested_bridge_kind_t;

/* Cached nested STRUCT/collection bridge built from DuckDB vectors. */
typedef struct tcc_nested_struct_bridge {
	tcc_nested_bridge_kind_t kind;
	ducktinycc_struct_t *rows;
	const void **field_ptrs;
	const uint64_t **field_validity;
	idx_t field_count;
	struct tcc_nested_struct_bridge **field_bridges;
	uint64_t *row_validity_mask;
} tcc_nested_struct_bridge_t;

/* Generic recursive value bridge used for inputs and outputs. */
typedef struct tcc_value_bridge tcc_value_bridge_t;
struct tcc_value_bridge {
	const tcc_typedesc_t *desc;
	idx_t count;
	size_t elem_size;
	void *rows;
	bool owns_rows;
	const uint64_t *validity;
	uint64_t *owned_validity;
	const void **child_ptrs;
	const uint64_t **child_validity_ptrs;
	tcc_value_bridge_t **children;
	idx_t child_count;
};

/* One diagnostics table row. */
typedef struct {
	char *kind;
	char *key;
	char *value;
	bool exists;
	char *detail;
} tcc_diag_row_t;

/* Growable diagnostics row collection. */
typedef struct {
	tcc_diag_row_t *rows;
	idx_t count;
	idx_t capacity;
} tcc_diag_rows_t;

/* Bind payload for diagnostics table functions. */
typedef struct {
	tcc_diag_rows_t rows;
} tcc_diag_bind_data_t;

/* Init payload for diagnostics table functions (streaming row cursor). */
typedef struct {
	atomic_uint_fast64_t offset;
} tcc_diag_init_data_t;

/* Extra-info payload for pointer helper scalar UDFs. */
typedef struct {
	tcc_ptr_registry_t *registry;
} tcc_ptr_helper_ctx_t;

/* Parsed c_struct/c_union/c_bitfield field specification. */
typedef struct {
	char *name;
	tcc_ffi_type_t type;
	size_t array_size;
	bool is_bitfield;
} tcc_c_field_spec_t;

/* Growable field-spec list for helper codegen. */
typedef struct {
	tcc_c_field_spec_t *items;
	idx_t count;
	idx_t capacity;
} tcc_c_field_list_t;

/* One generated helper binding description (symbol + SQL signature + stability). */
typedef struct {
	char *symbol;
	char *sql_name;
	char *return_type;
	char *arg_types_csv;
	char *stability;
} tcc_helper_binding_t;

/* Growable generated helper binding list. */
typedef struct {
	tcc_helper_binding_t *items;
	idx_t count;
	idx_t capacity;
} tcc_helper_binding_list_t;

/* Recursive typedesc node kinds. */
typedef enum {
	TCC_TYPEDESC_PRIMITIVE = 1,
	TCC_TYPEDESC_LIST = 2,
	TCC_TYPEDESC_ARRAY = 3,
	TCC_TYPEDESC_STRUCT = 4,
	TCC_TYPEDESC_MAP = 5,
	TCC_TYPEDESC_UNION = 6
} tcc_typedesc_kind_t;

/* Named child node in STRUCT/UNION typedesc nodes. */
typedef struct {
	char *name;
	tcc_typedesc_t *type;
} tcc_typedesc_field_t;

/* Recursive parsed type descriptor tree for nested signature grammar. */
struct tcc_typedesc {
	tcc_typedesc_kind_t kind;
	tcc_ffi_type_t ffi_type;
	size_t array_size;
	char *token;
	union {
		struct {
			tcc_typedesc_t *child;
		} list_like;
		struct {
			tcc_typedesc_field_t *fields;
			idx_t count;
		} struct_like;
		struct {
			tcc_typedesc_t *key;
			tcc_typedesc_t *value;
		} map_like;
		struct {
			tcc_typedesc_field_t *members;
			idx_t count;
		} union_like;
	} as;
};

/* Codegen signature context derived from bind arguments. */
typedef struct {
	tcc_ffi_type_t return_type;
	size_t return_array_size;
	tcc_ffi_type_t *arg_types;
	size_t *arg_array_sizes;
	tcc_ffi_struct_meta_t return_struct_meta;
	tcc_ffi_map_meta_t return_map_meta;
	tcc_ffi_union_meta_t return_union_meta;
	tcc_ffi_struct_meta_t *arg_struct_metas;
	tcc_ffi_map_meta_t *arg_map_metas;
	tcc_ffi_union_meta_t *arg_union_metas;
	tcc_wrapper_mode_t wrapper_mode;
	const char *wrapper_mode_token;
	tcc_function_stability_t stability;
	const char *stability_token;
	int arg_count;
} tcc_codegen_signature_ctx_t;

/* Codegen source context (wrapper source + compilation unit + module symbol). */
typedef struct {
	tcc_codegen_signature_ctx_t signature;
	char module_symbol[128];
	char *wrapper_loader_source;
	char *compilation_unit_source;
} tcc_codegen_source_ctx_t;

/* Forward declarations grouped by subsystem (parser/types/bridge/codegen). */
static bool tcc_parse_signature(const char *return_type, const char *arg_types_csv, tcc_ffi_type_t *out_return_type,
		                                size_t *out_return_array_size, tcc_ffi_type_t **out_arg_types,
		                                size_t **out_arg_array_sizes, tcc_ffi_struct_meta_t *out_return_struct_meta,
		                                tcc_ffi_map_meta_t *out_return_map_meta,
		                                tcc_ffi_union_meta_t *out_return_union_meta,
		                                tcc_ffi_struct_meta_t **out_arg_struct_metas,
		                                tcc_ffi_map_meta_t **out_arg_map_metas,
		                                tcc_ffi_union_meta_t **out_arg_union_metas, int *out_arg_count,
		                                tcc_error_buffer_t *error_buf);
static bool tcc_equals_ci(const char *a, const char *b);
static bool tcc_parse_wrapper_mode(const char *wrapper_mode, tcc_wrapper_mode_t *out_mode,
                                   tcc_error_buffer_t *error_buf);
static const char *tcc_function_stability_token(tcc_function_stability_t stability);
static bool tcc_parse_function_stability(const char *stability, tcc_function_stability_t *out_stability,
                                         tcc_error_buffer_t *error_buf);
static bool tcc_parse_type_token(const char *token, bool allow_void, tcc_ffi_type_t *out_type, size_t *out_array_size);
static bool tcc_split_csv_tokens(const char *csv, tcc_string_list_t *out_tokens, tcc_error_buffer_t *error_buf);
static duckdb_logical_type tcc_ffi_type_create_logical_type(tcc_ffi_type_t type, size_t array_size,
	                                                             const tcc_ffi_struct_meta_t *struct_meta,
	                                                             const tcc_ffi_map_meta_t *map_meta,
	                                                             const tcc_ffi_union_meta_t *union_meta);
static duckdb_logical_type tcc_typedesc_create_logical_type(const tcc_typedesc_t *desc);
static bool tcc_ffi_type_is_list(tcc_ffi_type_t type);
static bool tcc_ffi_list_child_type(tcc_ffi_type_t list_type, tcc_ffi_type_t *out_child);
static bool tcc_ffi_list_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_list_type);
static bool tcc_ffi_type_is_array(tcc_ffi_type_t type);
static bool tcc_ffi_array_child_type(tcc_ffi_type_t array_type, tcc_ffi_type_t *out_child);
static bool tcc_ffi_array_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_array_type);
static bool tcc_ffi_type_is_struct(tcc_ffi_type_t type);
static bool tcc_ffi_type_is_map(tcc_ffi_type_t type);
static bool tcc_ffi_type_is_union(tcc_ffi_type_t type);
static size_t tcc_ffi_type_size(tcc_ffi_type_t type);
static bool tcc_ffi_type_is_fixed_width_scalar(tcc_ffi_type_t type);
static const char *tcc_ffi_type_to_token(tcc_ffi_type_t type);
static char *tcc_trim_inplace(char *value);
static void tcc_struct_meta_destroy(tcc_ffi_struct_meta_t *meta);
static void tcc_struct_meta_array_destroy(tcc_ffi_struct_meta_t *metas, int count);
static void tcc_map_meta_destroy(tcc_ffi_map_meta_t *meta);
static void tcc_map_meta_array_destroy(tcc_ffi_map_meta_t *metas, int count);
static void tcc_union_meta_destroy(tcc_ffi_union_meta_t *meta);
static void tcc_union_meta_array_destroy(tcc_ffi_union_meta_t *metas, int count);
static bool tcc_parse_composite_fields_inner(char *inner, bool allow_auto_names, const char *kind_label,
                                             int *out_count, char ***out_names, char ***out_tokens,
                                             tcc_ffi_type_t **out_types, size_t **out_sizes,
                                             tcc_error_buffer_t *error_buf);
static bool tcc_parse_struct_meta_token(const char *token, tcc_ffi_struct_meta_t *out_meta,
                                        tcc_error_buffer_t *error_buf);
static bool tcc_parse_map_meta_token(const char *token, tcc_ffi_map_meta_t *out_meta, tcc_error_buffer_t *error_buf);
static bool tcc_parse_union_meta_token(const char *token, tcc_ffi_union_meta_t *out_meta,
                                       tcc_error_buffer_t *error_buf);
static void tcc_nested_struct_bridge_destroy(tcc_nested_struct_bridge_t *bridge);
static tcc_nested_struct_bridge_t *tcc_build_struct_bridge_from_vector(duckdb_vector struct_vector,
                                                                       const tcc_ffi_struct_meta_t *meta, idx_t n,
                                                                       const char **out_error);
static bool tcc_typedesc_is_composite(const tcc_typedesc_t *desc);
static void tcc_value_bridge_destroy(tcc_value_bridge_t *bridge);
static tcc_value_bridge_t *tcc_build_value_bridge(duckdb_vector vector, const tcc_typedesc_t *desc, idx_t count,
                                                  const char **out_error);
static bool tcc_set_vector_row_validity(duckdb_vector vector, idx_t row, bool valid);
static bool tcc_write_value_to_vector(duckdb_vector vector, const tcc_typedesc_t *desc, idx_t row,
                                      const void *src_base, uint64_t src_offset, const uint64_t *src_validity,
                                      const char **out_error);
static void tcc_codegen_signature_ctx_init(tcc_codegen_signature_ctx_t *ctx);
static void tcc_codegen_signature_ctx_destroy(tcc_codegen_signature_ctx_t *ctx);
static bool tcc_codegen_signature_parse_types(const tcc_module_bind_data_t *bind, tcc_codegen_signature_ctx_t *ctx,
                                              tcc_error_buffer_t *error_buf);
static bool tcc_codegen_signature_parse_wrapper_mode(const tcc_module_bind_data_t *bind,
                                                     tcc_codegen_signature_ctx_t *ctx,
                                                     tcc_error_buffer_t *error_buf);
static void tcc_codegen_source_ctx_init(tcc_codegen_source_ctx_t *ctx);
static void tcc_codegen_source_ctx_destroy(tcc_codegen_source_ctx_t *ctx);
static bool tcc_codegen_prepare_sources(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                        const char *sql_name, const char *target_symbol,
                                        tcc_codegen_source_ctx_t *ctx, tcc_error_buffer_t *error_buf);
static const char *tcc_effective_stability(tcc_module_state_t *state, const tcc_module_bind_data_t *bind);
static void tcc_codegen_classify_error_message(const char *error_message, const char **phase, const char **code,
                                               const char **message);
static void tcc_typedesc_destroy(tcc_typedesc_t *desc);
static bool tcc_typedesc_parse_token(const char *token, bool allow_void, tcc_typedesc_t **out_desc,
                                     tcc_error_buffer_t *error_buf);
static char *tcc_codegen_generate_wrapper_source(const char *module_symbol, const char *target_symbol,
                                                 const char *sql_name, const char *return_type,
                                                 const char *arg_types_csv, const char *wrapper_mode_token,
                                                 tcc_wrapper_mode_t wrapper_mode, const char *stability_token,
                                                 tcc_ffi_type_t ret_type, const tcc_ffi_type_t *arg_types,
                                                 int arg_count, bool emit_extern_decl);
static char *tcc_codegen_build_compilation_unit(const char *user_source, const char *wrapper_loader_source);

/* RW-lock primitives used to guard shared module/session state during mode execution. */
static void tcc_rwlock_init(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	atomic_store_explicit(&lock->writer, false, memory_order_relaxed);
	atomic_store_explicit(&lock->readers, 0, memory_order_relaxed);
	atomic_store_explicit(&lock->pending_writers, 0, memory_order_relaxed);
}

/* tcc_rwlock_read_lock: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_rwlock_read_lock(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	for (;;) {
		while (atomic_load_explicit(&lock->writer, memory_order_acquire) ||
		       atomic_load_explicit(&lock->pending_writers, memory_order_acquire) > 0) {
		}
		atomic_fetch_add_explicit(&lock->readers, 1, memory_order_acquire);
		if (!atomic_load_explicit(&lock->writer, memory_order_acquire) &&
		    atomic_load_explicit(&lock->pending_writers, memory_order_acquire) == 0) {
			break;
		}
		atomic_fetch_sub_explicit(&lock->readers, 1, memory_order_release);
	}
}

/* tcc_rwlock_read_unlock: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_rwlock_read_unlock(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	atomic_fetch_sub_explicit(&lock->readers, 1, memory_order_release);
}

/* tcc_rwlock_write_lock: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_rwlock_write_lock(tcc_rwlock_t *lock) {
	bool expected = false;
	if (!lock) {
		return;
	}
	atomic_fetch_add_explicit(&lock->pending_writers, 1, memory_order_acq_rel);
	for (;;) {
		expected = false;
		if (atomic_compare_exchange_weak_explicit(&lock->writer, &expected, true, memory_order_acq_rel,
		                                          memory_order_acquire)) {
			break;
		}
	}
	while (atomic_load_explicit(&lock->readers, memory_order_acquire) != 0) {
	}
	atomic_fetch_sub_explicit(&lock->pending_writers, 1, memory_order_release);
}

/* tcc_rwlock_write_unlock: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_rwlock_write_unlock(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	atomic_store_explicit(&lock->writer, false, memory_order_release);
}

#include "tcc_module_pointer.inc"

static char *tcc_strdup(const char *value) {
	size_t len;
	char *copy;
	if (!value) {
		return NULL;
	}
	len = strlen(value) + 1;
	copy = (char *)duckdb_malloc(len);
	if (copy) {
		memcpy(copy, value, len);
	}
	return copy;
}

typedef struct {
	char *data;
	size_t len;
	size_t capacity;
} tcc_text_buf_t;

/* tcc_text_buf_destroy: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_text_buf_destroy(tcc_text_buf_t *buf) {
	if (!buf) {
		return;
	}
	if (buf->data) {
		duckdb_free(buf->data);
	}
	buf->data = NULL;
	buf->len = 0;
	buf->capacity = 0;
}

/* tcc_text_buf_reserve: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_text_buf_reserve(tcc_text_buf_t *buf, size_t wanted) {
	char *new_data;
	size_t new_cap;
	if (!buf) {
		return false;
	}
	if (buf->capacity >= wanted) {
		return true;
	}
	new_cap = buf->capacity == 0 ? 256 : buf->capacity;
	while (new_cap < wanted) {
		if (new_cap > (SIZE_MAX / 2)) {
			return false;
		}
		new_cap *= 2;
	}
	new_data = (char *)duckdb_malloc(new_cap);
	if (!new_data) {
		return false;
	}
	if (buf->data && buf->len > 0) {
		memcpy(new_data, buf->data, buf->len);
	}
	if (buf->data) {
		duckdb_free(buf->data);
	}
	buf->data = new_data;
	buf->capacity = new_cap;
	if (buf->len == 0) {
		buf->data[0] = '\0';
	}
	return true;
}

/* tcc_text_buf_appendf: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_text_buf_appendf(tcc_text_buf_t *buf, const char *fmt, ...) {
	va_list args;
	va_list args_copy;
	int needed;
	if (!buf || !fmt) {
		return false;
	}
	va_start(args, fmt);
	va_copy(args_copy, args);
	needed = vsnprintf(NULL, 0, fmt, args_copy);
	va_end(args_copy);
	if (needed < 0) {
		va_end(args);
		return false;
	}
	if (!tcc_text_buf_reserve(buf, buf->len + (size_t)needed + 1)) {
		va_end(args);
		return false;
	}
	(void)vsnprintf(buf->data + buf->len, buf->capacity - buf->len, fmt, args);
	va_end(args);
	buf->len += (size_t)needed;
	return true;
}

static char *tcc_trim_inplace(char *value) {
	char *end;
	if (!value) {
		return NULL;
	}
	while (*value && isspace((unsigned char)*value)) {
		value++;
	}
	end = value + strlen(value);
	while (end > value && isspace((unsigned char)end[-1])) {
		end--;
	}
	*end = '\0';
	return value;
}

/* tcc_c_field_list_destroy: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_c_field_list_destroy(tcc_c_field_list_t *fields) {
	idx_t i;
	if (!fields) {
		return;
	}
	for (i = 0; i < fields->count; i++) {
		if (fields->items[i].name) {
			duckdb_free(fields->items[i].name);
		}
	}
	if (fields->items) {
		duckdb_free(fields->items);
	}
	fields->items = NULL;
	fields->count = 0;
	fields->capacity = 0;
}

/* tcc_c_field_list_reserve: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_c_field_list_reserve(tcc_c_field_list_t *fields, idx_t wanted) {
	tcc_c_field_spec_t *new_items;
	idx_t new_capacity;
	if (!fields) {
		return false;
	}
	if (fields->capacity >= wanted) {
		return true;
	}
	new_capacity = fields->capacity == 0 ? 8 : fields->capacity;
	while (new_capacity < wanted) {
		if ((size_t)new_capacity > (SIZE_MAX / (sizeof(tcc_c_field_spec_t) * 2))) {
			return false;
		}
		new_capacity *= 2;
	}
	new_items = (tcc_c_field_spec_t *)duckdb_malloc(sizeof(tcc_c_field_spec_t) * (size_t)new_capacity);
	if (!new_items) {
		return false;
	}
	memset(new_items, 0, sizeof(tcc_c_field_spec_t) * (size_t)new_capacity);
	if (fields->items && fields->count > 0) {
		memcpy(new_items, fields->items, sizeof(tcc_c_field_spec_t) * (size_t)fields->count);
		duckdb_free(fields->items);
	}
	fields->items = new_items;
	fields->capacity = new_capacity;
	return true;
}

/* tcc_c_field_list_append: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_c_field_list_append(tcc_c_field_list_t *fields, const tcc_c_field_spec_t *field) {
	if (!fields || !field) {
		return false;
	}
	if (!tcc_c_field_list_reserve(fields, fields->count + 1)) {
		return false;
	}
	fields->items[fields->count] = *field;
	fields->count++;
	return true;
}

/* tcc_helper_binding_list_destroy: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_helper_binding_list_destroy(tcc_helper_binding_list_t *bindings) {
	idx_t i;
	if (!bindings) {
		return;
	}
	for (i = 0; i < bindings->count; i++) {
		if (bindings->items[i].symbol) {
			duckdb_free(bindings->items[i].symbol);
		}
		if (bindings->items[i].sql_name) {
			duckdb_free(bindings->items[i].sql_name);
		}
		if (bindings->items[i].return_type) {
			duckdb_free(bindings->items[i].return_type);
		}
		if (bindings->items[i].arg_types_csv) {
			duckdb_free(bindings->items[i].arg_types_csv);
		}
		if (bindings->items[i].stability) {
			duckdb_free(bindings->items[i].stability);
		}
	}
	if (bindings->items) {
		duckdb_free(bindings->items);
	}
	bindings->items = NULL;
	bindings->count = 0;
	bindings->capacity = 0;
}

/* tcc_helper_binding_list_reserve: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_helper_binding_list_reserve(tcc_helper_binding_list_t *bindings, idx_t wanted) {
	tcc_helper_binding_t *new_items;
	idx_t new_capacity;
	if (!bindings) {
		return false;
	}
	if (bindings->capacity >= wanted) {
		return true;
	}
	new_capacity = bindings->capacity == 0 ? 16 : bindings->capacity;
	while (new_capacity < wanted) {
		if ((size_t)new_capacity > (SIZE_MAX / (sizeof(tcc_helper_binding_t) * 2))) {
			return false;
		}
		new_capacity *= 2;
	}
	new_items = (tcc_helper_binding_t *)duckdb_malloc(sizeof(tcc_helper_binding_t) * (size_t)new_capacity);
	if (!new_items) {
		return false;
	}
	memset(new_items, 0, sizeof(tcc_helper_binding_t) * (size_t)new_capacity);
	if (bindings->items && bindings->count > 0) {
		memcpy(new_items, bindings->items, sizeof(tcc_helper_binding_t) * (size_t)bindings->count);
		duckdb_free(bindings->items);
	}
	bindings->items = new_items;
	bindings->capacity = new_capacity;
	return true;
}

/* tcc_helper_binding_list_add: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_helper_binding_list_add(tcc_helper_binding_list_t *bindings, const char *symbol, const char *sql_name,
                                        const char *return_type, const char *arg_types_csv,
                                        const char *stability) {
	tcc_helper_binding_t entry;
	if (!bindings || !symbol || !sql_name || !return_type || !arg_types_csv || !stability) {
		return false;
	}
	if (!tcc_helper_binding_list_reserve(bindings, bindings->count + 1)) {
		return false;
	}
	memset(&entry, 0, sizeof(entry));
	entry.symbol = tcc_strdup(symbol);
	entry.sql_name = tcc_strdup(sql_name);
	entry.return_type = tcc_strdup(return_type);
	entry.arg_types_csv = tcc_strdup(arg_types_csv);
	entry.stability = tcc_strdup(stability);
	if (!entry.symbol || !entry.sql_name || !entry.return_type || !entry.arg_types_csv || !entry.stability) {
		if (entry.symbol) {
			duckdb_free(entry.symbol);
		}
		if (entry.sql_name) {
			duckdb_free(entry.sql_name);
		}
		if (entry.return_type) {
			duckdb_free(entry.return_type);
		}
		if (entry.arg_types_csv) {
			duckdb_free(entry.arg_types_csv);
		}
		if (entry.stability) {
			duckdb_free(entry.stability);
		}
		return false;
	}
	bindings->items[bindings->count] = entry;
	bindings->count++;
	return true;
}

/* tcc_append_error: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_append_error(void *opaque, const char *msg) {
	tcc_error_buffer_t *buffer = (tcc_error_buffer_t *)opaque;
	size_t cur;
	size_t left;
	if (!buffer || !msg) {
		return;
	}
	cur = strlen(buffer->message);
	left = sizeof(buffer->message) - cur;
	if (left <= 1) {
		return;
	}
	snprintf(buffer->message + cur, left, "%s%s", cur > 0 ? " | " : "", msg);
}

/* tcc_set_error: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_set_error(tcc_error_buffer_t *error_buf, const char *message) {
	if (!error_buf || !message) {
		return;
	}
	snprintf(error_buf->message, sizeof(error_buf->message), "%s", message);
}

/* tcc_path_exists: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_path_exists(const char *path) {
	return path && path[0] != '\0' && TCC_ACCESS(path, TCC_ACCESS_FOK) == 0;
}

#include "tcc_module_embedded_runtime.inc"

#include "tcc_module_support.inc"

#include "tcc_module_types.inc"

#include "tcc_module_exec.inc"

/**
 * @function ducktinycc_register_signature
 * @brief Register one generated wrapper symbol as a DuckDB scalar UDF.
 * @param[in] con Borrowed DuckDB connection.
 * @param[in] name SQL function name.
 * @param[in] fn_ptr Wrapper function pointer.
 * @param[in] return_type Canonical return token string.
 * @param[in] arg_types_csv Canonical argument token CSV.
 * @param[in] wrapper_mode "row" or "chunk_scalar_loop".
 * @return true on successful registration.
 * @ownership borrows(con,name,fn_ptr,return_type,arg_types_csv,wrapper_mode)
 * @heap allocates parsed signature/type metadata and function objects; ownership moves to ctx on success
 * @stack fixed-size locals only
 * @thread_safety registration-time only; runtime safety handled by DuckDB + immutable ctx
 * @locks none
 * @errors reports failure via boolean return (caller maps to SQL diagnostics)
 */
static bool ducktinycc_register_signature(duckdb_connection con, const char *name, void *fn_ptr,
                                          const char *return_type, const char *arg_types_csv,
                                          const char *wrapper_mode, const char *stability) {
	duckdb_scalar_function fn = NULL;
	tcc_host_sig_ctx_t *ctx = NULL;
	duckdb_state rc;
	tcc_ffi_type_t ret_type = TCC_FFI_I64;
	size_t ret_array_size = 0;
	tcc_ffi_type_t *arg_types = NULL;
	size_t *arg_array_sizes = NULL;
	tcc_ffi_struct_meta_t ret_struct_meta;
	tcc_ffi_map_meta_t ret_map_meta;
	tcc_ffi_union_meta_t ret_union_meta;
	tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
	tcc_ffi_map_meta_t *arg_map_metas = NULL;
	tcc_ffi_union_meta_t *arg_union_metas = NULL;
	tcc_wrapper_mode_t mode = TCC_WRAPPER_MODE_ROW;
	tcc_function_stability_t function_stability = TCC_FUNCTION_STABILITY_CONSISTENT;
	int arg_count = 0;
	tcc_error_buffer_t err;
	tcc_typedesc_t *return_desc = NULL;
	tcc_typedesc_t **arg_descs = NULL;
	tcc_string_list_t arg_tokens;
	int i;
	memset(&err, 0, sizeof(err));
	memset(&arg_tokens, 0, sizeof(arg_tokens));
	memset(&ret_struct_meta, 0, sizeof(ret_struct_meta));
	memset(&ret_map_meta, 0, sizeof(ret_map_meta));
	memset(&ret_union_meta, 0, sizeof(ret_union_meta));
	if (!con || !name || name[0] == '\0' || !fn_ptr) {
		return false;
	}
	if (!tcc_parse_signature(return_type, arg_types_csv, &ret_type, &ret_array_size, &arg_types, &arg_array_sizes,
	                         &ret_struct_meta, &ret_map_meta, &ret_union_meta, &arg_struct_metas, &arg_map_metas,
	                         &arg_union_metas, &arg_count, &err)) {
		return false;
	}
	if (!tcc_parse_wrapper_mode(wrapper_mode, &mode, &err)) {
		goto fail;
	}
	if (!tcc_parse_function_stability(stability, &function_stability, &err)) {
		goto fail;
	}
	if (!tcc_typedesc_parse_token(return_type, true, &return_desc, &err)) {
		goto fail;
	}
	if (!tcc_split_csv_tokens(arg_types_csv, &arg_tokens, &err)) {
		goto fail;
	}
	if ((int)arg_tokens.count != arg_count) {
		goto fail;
	}
	if (arg_count > 0) {
		arg_descs = (tcc_typedesc_t **)duckdb_malloc(sizeof(tcc_typedesc_t *) * (size_t)arg_count);
		if (!arg_descs) {
			goto fail;
		}
		memset(arg_descs, 0, sizeof(tcc_typedesc_t *) * (size_t)arg_count);
		for (i = 0; i < arg_count; i++) {
			if (!tcc_typedesc_parse_token(arg_tokens.items[i], false, &arg_descs[i], &err)) {
				goto fail;
			}
		}
	}
	tcc_string_list_destroy(&arg_tokens);

	fn = duckdb_create_scalar_function();
	if (!fn) {
		goto fail;
	}
	ctx = (tcc_host_sig_ctx_t *)duckdb_malloc(sizeof(tcc_host_sig_ctx_t));
	if (!ctx) {
		goto fail;
	}
	memset(ctx, 0, sizeof(tcc_host_sig_ctx_t));
	ctx->wrapper_mode = mode;
	if (mode == TCC_WRAPPER_MODE_BATCH) {
		ctx->batch_wrapper = (tcc_host_batch_wrapper_fn_t)fn_ptr;
	} else {
		ctx->row_wrapper = (tcc_host_row_wrapper_fn_t)fn_ptr;
	}
	ctx->arg_count = arg_count;
	ctx->return_type = ret_type;
	ctx->return_array_size = ret_array_size;
	ctx->arg_types = arg_types;
	ctx->arg_array_sizes = arg_array_sizes;
	ctx->return_struct_meta = ret_struct_meta;
	ctx->return_map_meta = ret_map_meta;
	ctx->return_union_meta = ret_union_meta;
	ctx->arg_struct_metas = arg_struct_metas;
	ctx->arg_map_metas = arg_map_metas;
	ctx->arg_union_metas = arg_union_metas;
	ctx->return_desc = return_desc;
	ctx->arg_descs = arg_descs;
	arg_types = NULL;
	arg_array_sizes = NULL;
	return_desc = NULL;
	arg_descs = NULL;
	memset(&ret_struct_meta, 0, sizeof(ret_struct_meta));
	memset(&ret_map_meta, 0, sizeof(ret_map_meta));
	memset(&ret_union_meta, 0, sizeof(ret_union_meta));
	arg_struct_metas = NULL;
	arg_map_metas = NULL;
	arg_union_metas = NULL;
	if (ctx->arg_count > 0) {
		ctx->arg_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
		if (!ctx->arg_sizes) {
			tcc_host_sig_ctx_destroy(ctx);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		for (i = 0; i < ctx->arg_count; i++) {
			ctx->arg_sizes[i] = tcc_ffi_type_size(ctx->arg_types[i]);
			if (ctx->arg_sizes[i] == 0) {
				tcc_host_sig_ctx_destroy(ctx);
				duckdb_destroy_scalar_function(&fn);
				return false;
			}
		}
	}

	duckdb_scalar_function_set_name(fn, name);
	for (i = 0; i < arg_count; i++) {
		duckdb_logical_type arg_type = tcc_typedesc_create_logical_type(ctx->arg_descs ? ctx->arg_descs[i] : NULL);
		if (!arg_type) {
			tcc_host_sig_ctx_destroy(ctx);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		duckdb_scalar_function_add_parameter(fn, arg_type);
		duckdb_destroy_logical_type(&arg_type);
	}
	{
		duckdb_logical_type ret_type_obj = tcc_typedesc_create_logical_type(ctx->return_desc);
		if (!ret_type_obj) {
			tcc_host_sig_ctx_destroy(ctx);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		duckdb_scalar_function_set_return_type(fn, ret_type_obj);
		duckdb_destroy_logical_type(&ret_type_obj);
	}
	if (function_stability == TCC_FUNCTION_STABILITY_VOLATILE) {
		duckdb_scalar_function_set_volatile(fn);
	}
	duckdb_scalar_function_set_function(fn, tcc_execute_compiled_scalar_udf);
	duckdb_scalar_function_set_extra_info(fn, ctx, tcc_host_sig_ctx_destroy);
	rc = duckdb_register_scalar_function(con, fn);
	duckdb_destroy_scalar_function(&fn);
	return rc == DuckDBSuccess;

fail:
	tcc_string_list_destroy(&arg_tokens);
	if (arg_descs) {
		for (i = 0; i < arg_count; i++) {
			if (arg_descs[i]) {
				tcc_typedesc_destroy(arg_descs[i]);
			}
		}
		duckdb_free(arg_descs);
	}
	if (return_desc) {
		tcc_typedesc_destroy(return_desc);
	}
	if (arg_types) {
		duckdb_free(arg_types);
	}
	if (arg_array_sizes) {
		duckdb_free(arg_array_sizes);
	}
	if (arg_struct_metas) {
		tcc_struct_meta_array_destroy(arg_struct_metas, arg_count);
	}
	if (arg_map_metas) {
		tcc_map_meta_array_destroy(arg_map_metas, arg_count);
	}
	if (arg_union_metas) {
		tcc_union_meta_array_destroy(arg_union_metas, arg_count);
	}
	tcc_struct_meta_destroy(&ret_struct_meta);
	tcc_map_meta_destroy(&ret_map_meta);
	tcc_union_meta_destroy(&ret_union_meta);
	if (fn) {
		duckdb_destroy_scalar_function(&fn);
	}
	return false;
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
#include "tcc_module_host.inc"
#endif

#include "tcc_module_lifecycle.inc"

#include "tcc_module_parse.inc"

/* tcc_codegen_signature_ctx_init: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_codegen_signature_ctx_init(tcc_codegen_signature_ctx_t *ctx) {
	if (!ctx) {
		return;
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->return_type = TCC_FFI_I64;
	ctx->wrapper_mode = TCC_WRAPPER_MODE_ROW;
	ctx->stability = TCC_FUNCTION_STABILITY_CONSISTENT;
}

/* tcc_codegen_signature_ctx_destroy: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_codegen_signature_ctx_destroy(tcc_codegen_signature_ctx_t *ctx) {
	if (!ctx) {
		return;
	}
	if (ctx->arg_types) {
		duckdb_free(ctx->arg_types);
		ctx->arg_types = NULL;
	}
	if (ctx->arg_array_sizes) {
		duckdb_free(ctx->arg_array_sizes);
		ctx->arg_array_sizes = NULL;
	}
	if (ctx->arg_struct_metas) {
		tcc_struct_meta_array_destroy(ctx->arg_struct_metas, ctx->arg_count);
		ctx->arg_struct_metas = NULL;
	}
	if (ctx->arg_map_metas) {
		tcc_map_meta_array_destroy(ctx->arg_map_metas, ctx->arg_count);
		ctx->arg_map_metas = NULL;
	}
	if (ctx->arg_union_metas) {
		tcc_union_meta_array_destroy(ctx->arg_union_metas, ctx->arg_count);
		ctx->arg_union_metas = NULL;
	}
	tcc_struct_meta_destroy(&ctx->return_struct_meta);
	tcc_map_meta_destroy(&ctx->return_map_meta);
	tcc_union_meta_destroy(&ctx->return_union_meta);
	ctx->arg_count = 0;
	ctx->return_type = TCC_FFI_I64;
	ctx->return_array_size = 0;
	ctx->wrapper_mode = TCC_WRAPPER_MODE_ROW;
	ctx->wrapper_mode_token = NULL;
	ctx->stability = TCC_FUNCTION_STABILITY_CONSISTENT;
	ctx->stability_token = NULL;
}

/* tcc_codegen_signature_parse_types: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_codegen_signature_parse_types(const tcc_module_bind_data_t *bind, tcc_codegen_signature_ctx_t *ctx,
                                              tcc_error_buffer_t *error_buf) {
	if (!bind || !ctx) {
		tcc_set_error(error_buf, "invalid codegen signature arguments");
		return false;
	}
	return tcc_parse_signature(bind->return_type, bind->arg_types, &ctx->return_type, &ctx->return_array_size,
	                           &ctx->arg_types, &ctx->arg_array_sizes, &ctx->return_struct_meta,
	                           &ctx->return_map_meta, &ctx->return_union_meta, &ctx->arg_struct_metas,
	                           &ctx->arg_map_metas, &ctx->arg_union_metas, &ctx->arg_count, error_buf);
}

/* tcc_codegen_signature_parse_wrapper_mode: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_codegen_signature_parse_wrapper_mode(const tcc_module_bind_data_t *bind,
                                                     tcc_codegen_signature_ctx_t *ctx,
                                                     tcc_error_buffer_t *error_buf) {
	if (!bind || !ctx) {
		tcc_set_error(error_buf, "invalid codegen wrapper_mode arguments");
		return false;
	}
	if (!tcc_parse_wrapper_mode(bind->wrapper_mode, &ctx->wrapper_mode, error_buf)) {
		return false;
	}
	ctx->wrapper_mode_token = tcc_wrapper_mode_token(ctx->wrapper_mode);
	if (!ctx->wrapper_mode_token) {
		tcc_set_error(error_buf, "wrapper_mode contains unsupported token");
		return false;
	}
	return true;
}

/* tcc_codegen_source_ctx_init: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_codegen_source_ctx_init(tcc_codegen_source_ctx_t *ctx) {
	if (!ctx) {
		return;
	}
	memset(ctx, 0, sizeof(*ctx));
	tcc_codegen_signature_ctx_init(&ctx->signature);
}

/* tcc_codegen_source_ctx_destroy: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_codegen_source_ctx_destroy(tcc_codegen_source_ctx_t *ctx) {
	if (!ctx) {
		return;
	}
	if (ctx->compilation_unit_source) {
		duckdb_free(ctx->compilation_unit_source);
		ctx->compilation_unit_source = NULL;
	}
	if (ctx->wrapper_loader_source) {
		duckdb_free(ctx->wrapper_loader_source);
		ctx->wrapper_loader_source = NULL;
	}
	tcc_codegen_signature_ctx_destroy(&ctx->signature);
	memset(ctx->module_symbol, 0, sizeof(ctx->module_symbol));
}

/* tcc_codegen_prepare_sources: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_codegen_prepare_sources(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                        const char *sql_name, const char *target_symbol,
                                        tcc_codegen_source_ctx_t *ctx, tcc_error_buffer_t *error_buf) {
	if (!state || !bind || !sql_name || !target_symbol || !ctx) {
		tcc_set_error(error_buf, "invalid codegen source arguments");
		return false;
	}
	if (!tcc_codegen_signature_parse_types(bind, &ctx->signature, error_buf)) {
		return false;
	}
	if (!tcc_codegen_signature_parse_wrapper_mode(bind, &ctx->signature, error_buf)) {
		return false;
	}
	if (!tcc_parse_function_stability(tcc_effective_stability(state, bind), &ctx->signature.stability, error_buf)) {
		return false;
	}
	ctx->signature.stability_token = tcc_function_stability_token(ctx->signature.stability);
	if (!ctx->signature.stability_token) {
		tcc_set_error(error_buf, "stability contains unsupported token");
		return false;
	}
	snprintf(ctx->module_symbol, sizeof(ctx->module_symbol), "__ducktinycc_ffi_init_%llu_%llu",
	         (unsigned long long)state->session.state_id, (unsigned long long)state->session.config_version);
	ctx->wrapper_loader_source =
	    tcc_codegen_generate_wrapper_source(ctx->module_symbol, target_symbol, sql_name,
	                                        bind->return_type ? bind->return_type : "i64",
	                                        bind->arg_types ? bind->arg_types : "",
	                                        ctx->signature.wrapper_mode_token, ctx->signature.wrapper_mode,
	                                        ctx->signature.stability_token, ctx->signature.return_type,
	                                        ctx->signature.arg_types, ctx->signature.arg_count,
	                                        /* emit_extern_decl: only when user source is NOT bundled in the
	                                         * compilation unit.  When user_source is present, the definition
	                                         * already provides the prototype; emitting an extern with our
	                                         * stdint.h-derived type (e.g. int64_t) would conflict with user
	                                         * code that uses a raw primitive (e.g. long long) for the same
	                                         * 64-bit type, because on LP64 Linux int64_t == long != long long. */
	                                        !(bind->source && bind->source[0] != '\0'));
	if (!ctx->wrapper_loader_source) {
		tcc_set_error(error_buf, "failed to generate codegen wrapper");
		return false;
	}
	ctx->compilation_unit_source = tcc_codegen_build_compilation_unit(bind->source, ctx->wrapper_loader_source);
	if (!ctx->compilation_unit_source) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	return true;
}

/* tcc_codegen_classify_error_message: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_codegen_classify_error_message(const char *error_message, const char **phase, const char **code,
                                               const char **message) {
	if (!error_message || !phase || !code || !message) {
		return;
	}
	if (strstr(error_message, "wrapper_mode")) {
		*phase = "bind";
		*code = "E_BAD_WRAPPER_MODE";
		*message = "invalid wrapper_mode";
	} else if (strstr(error_message, "stability")) {
		*phase = "bind";
		*code = "E_BAD_STABILITY";
		*message = "invalid stability";
	} else if (strstr(error_message, "return_type") || strstr(error_message, "arg_types") ||
	           strstr(error_message, "struct token") || strstr(error_message, "map token") ||
	           strstr(error_message, "fixed-width scalar tokens only")) {
		*phase = "bind";
		*code = "E_BAD_SIGNATURE";
		*message = "invalid return_type/arg_types";
	} else if (strstr(error_message, "failed to generate codegen wrapper") || strstr(error_message, "out of memory")) {
		*phase = "codegen";
		*code = "E_CODEGEN_FAILED";
		*message = "ffi codegen failed";
	} else if (strstr(error_message, "no persistent extension connection")) {
		*phase = "load";
		*code = "E_NO_CONNECTION";
		*message = "no persistent extension connection available";
	} else if (strstr(error_message, "generated module init returned false")) {
		*phase = "load";
		*code = "E_INIT_FAILED";
		*message = "generated module init returned false";
	}
}

/* Resolves target symbol from explicit args or session-level bound symbol. */
static const char *tcc_effective_symbol(tcc_module_state_t *state, const tcc_module_bind_data_t *bind) {
	if (bind->symbol && bind->symbol[0] != '\0') {
		return bind->symbol;
	}
	if (state->session.bound_symbol && state->session.bound_symbol[0] != '\0') {
		return state->session.bound_symbol;
	}
	return NULL;
}

/* Resolves SQL function name from explicit args or session-level fallback. */
static const char *tcc_effective_sql_name(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                          const char *effective_symbol) {
	if (bind->sql_name && bind->sql_name[0] != '\0') {
		return bind->sql_name;
	}
	if (state->session.bound_sql_name && state->session.bound_sql_name[0] != '\0') {
		return state->session.bound_sql_name;
	}
	return effective_symbol;
}

/* Resolves function stability from explicit args, session-level binding, or default. */
static const char *tcc_effective_stability(tcc_module_state_t *state, const tcc_module_bind_data_t *bind) {
	if (bind && bind->stability && bind->stability[0] != '\0') {
		return bind->stability;
	}
	if (state && state->session.bound_stability && state->session.bound_stability[0] != '\0') {
		return state->session.bound_stability;
	}
	return "consistent";
}

static const char *tcc_ffi_type_to_token(tcc_ffi_type_t type) {
	if (tcc_ffi_type_is_list(type)) {
		return "list";
	}
	if (tcc_ffi_type_is_array(type)) {
		return "array";
	}
	switch (type) {
#define TCC_FFI_TOKEN_CASE(type_id, token_str, c_type_name, duckdb_type_id, width_bytes)                                   \
	case type_id:                                                                                                          \
		return token_str;
		TCC_FFI_SCALAR_ROWS(TCC_FFI_TOKEN_CASE)
#undef TCC_FFI_TOKEN_CASE
	case TCC_FFI_PTR:
		return "ptr";
	case TCC_FFI_VARCHAR:
		return "varchar";
	case TCC_FFI_BLOB:
		return "blob";
	case TCC_FFI_STRUCT:
		return "struct";
	case TCC_FFI_MAP:
		return "map";
	case TCC_FFI_UNION:
		return "union";
	default:
		return NULL;
	}
}

static const char *tcc_ffi_type_to_c_type_name(tcc_ffi_type_t type) {
	if (tcc_ffi_type_is_list(type)) {
		return "ducktinycc_list_t";
	}
	if (tcc_ffi_type_is_array(type)) {
		return "ducktinycc_array_t";
	}
	switch (type) {
	case TCC_FFI_VOID:
		return "void";
#define TCC_FFI_CTYPE_CASE(type_id, token_str, c_type_name, duckdb_type_id, width_bytes)                                   \
	case type_id:                                                                                                          \
		return c_type_name;
		TCC_FFI_SCALAR_ROWS(TCC_FFI_CTYPE_CASE)
#undef TCC_FFI_CTYPE_CASE
	case TCC_FFI_PTR:
		return "void *";
	case TCC_FFI_VARCHAR:
		return "const char *";
	case TCC_FFI_BLOB:
		return "ducktinycc_blob_t";
	case TCC_FFI_UUID:
		return "ducktinycc_hugeint_t";
	case TCC_FFI_DATE:
		return "ducktinycc_date_t";
	case TCC_FFI_TIME:
		return "ducktinycc_time_t";
	case TCC_FFI_TIMESTAMP:
		return "ducktinycc_timestamp_t";
	case TCC_FFI_INTERVAL:
		return "ducktinycc_interval_t";
	case TCC_FFI_DECIMAL:
		return "ducktinycc_decimal_t";
	case TCC_FFI_STRUCT:
		return "ducktinycc_struct_t";
	case TCC_FFI_MAP:
		return "ducktinycc_map_t";
	case TCC_FFI_UNION:
		return "ducktinycc_union_t";
	default:
		return NULL;
	}
}
#undef TCC_FFI_SCALAR_ROWS

#include "tcc_module_codegen.inc"

#include "tcc_module_modes.inc"

#include "tcc_module_diag.inc"

/* Public extension registration entrypoint for module and helper SQL surfaces. */
bool RegisterTccModuleFunction(duckdb_connection connection, duckdb_database database) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type list_varchar_type = duckdb_create_list_type(varchar_type);
	tcc_module_state_t *state;
	duckdb_state rc;

	state = (tcc_module_state_t *)duckdb_malloc(sizeof(tcc_module_state_t));
	if (!state) {
		duckdb_destroy_logical_type(&list_varchar_type);
		duckdb_destroy_logical_type(&varchar_type);
		duckdb_destroy_table_function(&tf);
		return false;
	}
	memset(state, 0, sizeof(tcc_module_state_t));
	tcc_rwlock_init(&state->lock);
	state->connection = connection;
	state->database = database;
	state->ptr_registry = tcc_ptr_registry_create();
	if (!state->ptr_registry) {
		duckdb_free(state);
		duckdb_destroy_logical_type(&list_varchar_type);
		duckdb_destroy_logical_type(&varchar_type);
		duckdb_destroy_table_function(&tf);
		return false;
	}

	duckdb_table_function_set_name(tf, "tcc_module");
	duckdb_table_function_add_named_parameter(tf, "mode", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "runtime_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "source", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "symbol", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "sql_name", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "arg_types", list_varchar_type);
	duckdb_table_function_add_named_parameter(tf, "return_type", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "wrapper_mode", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "stability", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "include_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "sysinclude_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "library_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "library", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "option", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "header", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "define_name", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "define_value", varchar_type);
	{
		duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
		duckdb_table_function_add_named_parameter(tf, "symbol_name", varchar_type);
		duckdb_table_function_add_named_parameter(tf, "symbol_ptr", ubigint_type);
		duckdb_destroy_logical_type(&ubigint_type);
	}

	duckdb_table_function_set_extra_info(tf, state, destroy_tcc_module_state);
	duckdb_table_function_set_bind(tf, tcc_module_bind);
	duckdb_table_function_set_init(tf, tcc_module_init);
	duckdb_table_function_set_function(tf, tcc_module_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);

	rc = duckdb_register_table_function(connection, tf);
	if (rc == DuckDBSuccess) {
		rc = register_tcc_system_paths_function(connection) && register_tcc_library_probe_function(connection) &&
		             register_tcc_pointer_helper_functions(connection, state->ptr_registry)
		         ? DuckDBSuccess
		         : DuckDBError;
	}

	duckdb_destroy_logical_type(&list_varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
	return rc == DuckDBSuccess;
}
