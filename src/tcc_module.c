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
 * - Runtime execution (`tcc_execute_compiled_scalar_udf`) bridges DuckDB vectors to C descriptors for row/batch
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
#define TCC_ACCESS _access
#define TCC_ACCESS_FOK 0
#define TCC_ENV_PATH_SEP ';'
#else
#include <unistd.h>
#define TCC_ACCESS access
#define TCC_ACCESS_FOK F_OK
#define TCC_ENV_PATH_SEP ':'
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
/* - tcc_execute_compiled_scalar_udf: Main runtime bridge for executing compiled row/batch wrappers and marshaling values. */
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
	tcc_string_list_t include_paths;
	tcc_string_list_t sysinclude_paths;
	tcc_string_list_t library_paths;
	tcc_string_list_t libraries;
	tcc_string_list_t options;
	tcc_string_list_t headers;
	tcc_string_list_t sources;
	tcc_string_list_t define_names;
	tcc_string_list_t define_values;
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
	char *include_path;
	char *sysinclude_path;
	char *library_path;
	char *library;
	char *option;
	char *header;
	char *define_name;
	char *define_value;
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

/* One generated helper binding description (symbol + SQL signature). */
typedef struct {
	char *symbol;
	char *sql_name;
	char *return_type;
	char *arg_types_csv;
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
static void tcc_codegen_classify_error_message(const char *error_message, const char **phase, const char **code,
                                               const char **message);
static void tcc_typedesc_destroy(tcc_typedesc_t *desc);
static bool tcc_typedesc_parse_token(const char *token, bool allow_void, tcc_typedesc_t **out_desc,
                                     tcc_error_buffer_t *error_buf);
static char *tcc_codegen_generate_wrapper_source(const char *module_symbol, const char *target_symbol,
                                                 const char *sql_name, const char *return_type,
                                                 const char *arg_types_csv, const char *wrapper_mode_token,
                                                 tcc_wrapper_mode_t wrapper_mode, tcc_ffi_type_t ret_type,
                                                 const tcc_ffi_type_t *arg_types, int arg_count);
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

/* Pointer-registry primitives backing `tcc_alloc` and pointer helper scalar UDFs. */
static void tcc_ptr_registry_lock(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	while (atomic_flag_test_and_set_explicit(&registry->lock, memory_order_acquire)) {
	}
}

/* tcc_ptr_registry_unlock: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_ptr_registry_unlock(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	atomic_flag_clear_explicit(&registry->lock, memory_order_release);
}

static tcc_ptr_registry_t *tcc_ptr_registry_create(void) {
	tcc_ptr_registry_t *registry = (tcc_ptr_registry_t *)duckdb_malloc(sizeof(tcc_ptr_registry_t));
	if (!registry) {
		return NULL;
	}
	memset(registry, 0, sizeof(*registry));
	atomic_init(&registry->ref_count, 1);
	atomic_flag_clear(&registry->lock);
	registry->next_handle = 1;
	return registry;
}

/* tcc_ptr_registry_destroy: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_ptr_registry_destroy(tcc_ptr_registry_t *registry) {
	idx_t i;
	if (!registry) {
		return;
	}
	for (i = 0; i < registry->count; i++) {
		if (registry->entries[i].owned && registry->entries[i].ptr) {
			free(registry->entries[i].ptr);
		}
	}
	if (registry->entries) {
		duckdb_free(registry->entries);
	}
	duckdb_free(registry);
}

/* tcc_ptr_registry_ref: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_ptr_registry_ref(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	atomic_fetch_add_explicit(&registry->ref_count, 1, memory_order_relaxed);
}

/* tcc_ptr_registry_unref: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_ptr_registry_unref(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	if (atomic_fetch_sub_explicit(&registry->ref_count, 1, memory_order_acq_rel) == 1) {
		tcc_ptr_registry_destroy(registry);
	}
}

/* tcc_ptr_registry_find_handle_unlocked: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static idx_t tcc_ptr_registry_find_handle_unlocked(tcc_ptr_registry_t *registry, uint64_t handle) {
	idx_t i;
	if (!registry || handle == 0) {
		return (idx_t)-1;
	}
	for (i = 0; i < registry->count; i++) {
		if (registry->entries[i].handle == handle) {
			return i;
		}
	}
	return (idx_t)-1;
}

/* tcc_ptr_registry_reserve_unlocked: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_reserve_unlocked(tcc_ptr_registry_t *registry, idx_t wanted) {
	tcc_ptr_entry_t *new_entries;
	idx_t new_capacity;
	if (!registry) {
		return false;
	}
	if (registry->capacity >= wanted) {
		return true;
	}
	new_capacity = registry->capacity == 0 ? 16 : registry->capacity * 2;
	while (new_capacity < wanted) {
		if (new_capacity > (idx_t)-1 / 2) {
			return false;
		}
		new_capacity *= 2;
	}
	new_entries = (tcc_ptr_entry_t *)duckdb_malloc(sizeof(tcc_ptr_entry_t) * (size_t)new_capacity);
	if (!new_entries) {
		return false;
	}
	memset(new_entries, 0, sizeof(tcc_ptr_entry_t) * (size_t)new_capacity);
	if (registry->entries && registry->count > 0) {
		memcpy(new_entries, registry->entries, sizeof(tcc_ptr_entry_t) * (size_t)registry->count);
		duckdb_free(registry->entries);
	}
	registry->entries = new_entries;
	registry->capacity = new_capacity;
	return true;
}

/* tcc_ptr_span_fits: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_span_fits(uint64_t len, uint64_t offset, uint64_t width) {
	if (offset > len) {
		return false;
	}
	return width <= (len - offset);
}

/* tcc_ptr_registry_alloc: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_ptr_registry_alloc(tcc_ptr_registry_t *registry, uint64_t size, uint64_t *out_handle) {
	void *ptr;
	tcc_ptr_entry_t *entry;
	uint64_t handle;
	if (!registry || !out_handle || size == 0 || size > (uint64_t)(~(size_t)0)) {
		return false;
	}
	ptr = malloc((size_t)size);
	if (!ptr) {
		return false;
	}
	memset(ptr, 0, (size_t)size);
	tcc_ptr_registry_lock(registry);
	if (!tcc_ptr_registry_reserve_unlocked(registry, registry->count + 1)) {
		tcc_ptr_registry_unlock(registry);
		free(ptr);
		return false;
	}
	handle = registry->next_handle++;
	if (handle == 0) {
		handle = registry->next_handle++;
	}
	entry = &registry->entries[registry->count++];
	memset(entry, 0, sizeof(*entry));
	entry->handle = handle;
	entry->ptr = ptr;
	entry->size = size;
	entry->owned = true;
	tcc_ptr_registry_unlock(registry);
	*out_handle = handle;
	return true;
}

/* tcc_ptr_registry_free: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static bool tcc_ptr_registry_free(tcc_ptr_registry_t *registry, uint64_t handle) {
	void *ptr = NULL;
	bool owned = false;
	idx_t idx;
	idx_t last;
	if (!registry || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	ptr = registry->entries[idx].ptr;
	owned = registry->entries[idx].owned;
	last = registry->count - 1;
	if (idx != last) {
		registry->entries[idx] = registry->entries[last];
	}
	memset(&registry->entries[last], 0, sizeof(registry->entries[last]));
	registry->count--;
	tcc_ptr_registry_unlock(registry);
	if (owned && ptr) {
		free(ptr);
	}
	return true;
}

/* tcc_ptr_registry_get_ptr_size: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_get_ptr_size(tcc_ptr_registry_t *registry, uint64_t handle, uintptr_t *out_ptr,
                                          uint64_t *out_size) {
	idx_t idx;
	if (!registry || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	if (out_ptr) {
		*out_ptr = (uintptr_t)registry->entries[idx].ptr;
	}
	if (out_size) {
		*out_size = registry->entries[idx].size;
	}
	tcc_ptr_registry_unlock(registry);
	return true;
}

/* tcc_ptr_registry_read: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_read(tcc_ptr_registry_t *registry, uint64_t handle, uint64_t offset, void *out,
                                  uint64_t width) {
	idx_t idx;
	uint8_t *src;
	if (!registry || !out || width == 0 || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1 || !registry->entries[idx].ptr ||
	    !tcc_ptr_span_fits(registry->entries[idx].size, offset, width)) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	src = (uint8_t *)registry->entries[idx].ptr + (size_t)offset;
	memcpy(out, src, (size_t)width);
	tcc_ptr_registry_unlock(registry);
	return true;
}

/* tcc_ptr_registry_write: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_write(tcc_ptr_registry_t *registry, uint64_t handle, uint64_t offset, const void *in,
                                   uint64_t width) {
	idx_t idx;
	uint8_t *dst;
	if (!registry || !in || width == 0 || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1 || !registry->entries[idx].ptr ||
	    !tcc_ptr_span_fits(registry->entries[idx].size, offset, width)) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	dst = (uint8_t *)registry->entries[idx].ptr + (size_t)offset;
	memcpy(dst, in, (size_t)width);
	tcc_ptr_registry_unlock(registry);
	return true;
}

/* tcc_ptr_helper_ctx_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_ptr_helper_ctx_destroy(void *ptr) {
	tcc_ptr_helper_ctx_t *ctx = (tcc_ptr_helper_ctx_t *)ptr;
	if (!ctx) {
		return;
	}
	tcc_ptr_registry_unref(ctx->registry);
	duckdb_free(ctx);
}

/* Vector validity helpers for scalar UDF implementations. */
static bool tcc_valid_input_row(uint64_t *validity, idx_t row) {
	return !validity || duckdb_validity_row_is_valid(validity, row);
}

/* tcc_set_output_row_null: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_set_output_row_null(uint64_t *validity, idx_t row) {
	if (!validity) {
		return;
	}
	duckdb_validity_set_row_invalid(validity, row);
}

static tcc_ptr_registry_t *tcc_get_ptr_registry(duckdb_function_info info) {
	tcc_ptr_helper_ctx_t *ctx = (tcc_ptr_helper_ctx_t *)duckdb_scalar_function_get_extra_info(info);
	if (!ctx || !ctx->registry) {
		duckdb_scalar_function_set_error(info, "tcc pointer helper missing registry context");
		return NULL;
	}
	return ctx->registry;
}

/* Pointer helper SQL functions (`tcc_alloc`, `tcc_free_ptr`, `tcc_dataptr`, reads/writes). */
static void tcc_alloc_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_size;
	uint64_t *in_validity;
	uint64_t *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_size = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (uint64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uint64_t handle = 0;
		if (!tcc_valid_input_row(in_validity, row) || !tcc_ptr_registry_alloc(registry, in_size[row], &handle)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = handle;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_free_ptr_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_free_ptr_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_handle;
	uint64_t *in_validity;
	bool *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_handle = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (bool *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		if (!tcc_valid_input_row(in_validity, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = tcc_ptr_registry_free(registry, in_handle[row]);
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_dataptr_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_dataptr_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_handle;
	uint64_t *in_validity;
	uint64_t *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_handle = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (uint64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uintptr_t addr = 0;
		if (!tcc_valid_input_row(in_validity, row) ||
		    !tcc_ptr_registry_get_ptr_size(registry, in_handle[row], &addr, NULL)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = (uint64_t)addr;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_ptr_size_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_ptr_size_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_handle;
	uint64_t *in_validity;
	uint64_t *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_handle = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (uint64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uint64_t size = 0;
		if (!tcc_valid_input_row(in_validity, row) ||
		    !tcc_ptr_registry_get_ptr_size(registry, in_handle[row], NULL, &size)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = size;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_ptr_add_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_ptr_add_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t row_count = duckdb_data_chunk_get_size(input);
	idx_t row;
	duckdb_vector in0 = duckdb_data_chunk_get_vector(input, 0);
	duckdb_vector in1 = duckdb_data_chunk_get_vector(input, 1);
	uint64_t *base = (uint64_t *)duckdb_vector_get_data(in0);
	uint64_t *off = (uint64_t *)duckdb_vector_get_data(in1);
	uint64_t *valid0 = duckdb_vector_get_validity(in0);
	uint64_t *valid1 = duckdb_vector_get_validity(in1);
	uint64_t *out_data = (uint64_t *)duckdb_vector_get_data(output);
	uint64_t *out_validity;
	(void)info;
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uintptr_t addr;
		if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		addr = (uintptr_t)base[row];
		addr += (uintptr_t)off[row];
		out_data[row] = (uint64_t)addr;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

#define TCC_DEFINE_PTR_READ_SCALAR(name, ctype)                                                                             \
	static void tcc_read_##name##_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {     \
		tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);                                                     \
		idx_t row_count;                                                                                                \
		idx_t row;                                                                                                      \
		duckdb_vector in0;                                                                                              \
		duckdb_vector in1;                                                                                              \
		uint64_t *handles;                                                                                              \
		uint64_t *offsets;                                                                                              \
		uint64_t *valid0;                                                                                                \
		uint64_t *valid1;                                                                                                \
		ctype *out_data;                                                                                                \
		uint64_t *out_validity;                                                                                          \
		if (!registry) {                                                                                                \
			return;                                                                                                 \
		}                                                                                                               \
		row_count = duckdb_data_chunk_get_size(input);                                                                  \
		in0 = duckdb_data_chunk_get_vector(input, 0);                                                                   \
		in1 = duckdb_data_chunk_get_vector(input, 1);                                                                   \
		handles = (uint64_t *)duckdb_vector_get_data(in0);                                                             \
		offsets = (uint64_t *)duckdb_vector_get_data(in1);                                                             \
		valid0 = duckdb_vector_get_validity(in0);                                                                       \
		valid1 = duckdb_vector_get_validity(in1);                                                                       \
		out_data = (ctype *)duckdb_vector_get_data(output);                                                             \
		duckdb_vector_ensure_validity_writable(output);                                                                 \
		out_validity = duckdb_vector_get_validity(output);                                                              \
		for (row = 0; row < row_count; row++) {                                                                         \
			ctype value;                                                                                            \
			if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) ||                        \
			    !tcc_ptr_registry_read(registry, handles[row], offsets[row], &value, (uint64_t)sizeof(ctype))) { \
				tcc_set_output_row_null(out_validity, row);                                                       \
				continue;                                                                                         \
			}                                                                                                       \
			out_data[row] = value;                                                                                  \
			duckdb_validity_set_row_validity(out_validity, row, true);                                             \
		}                                                                                                               \
	}

#define TCC_DEFINE_PTR_WRITE_SCALAR(name, ctype)                                                                            \
	static void tcc_write_##name##_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {    \
		tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);                                                     \
		idx_t row_count;                                                                                                \
		idx_t row;                                                                                                      \
		duckdb_vector in0;                                                                                              \
		duckdb_vector in1;                                                                                              \
		duckdb_vector in2;                                                                                              \
		uint64_t *handles;                                                                                              \
		uint64_t *offsets;                                                                                              \
		ctype *values;                                                                                                  \
		uint64_t *valid0;                                                                                                \
		uint64_t *valid1;                                                                                                \
		uint64_t *valid2;                                                                                                \
		bool *out_data;                                                                                                 \
		uint64_t *out_validity;                                                                                          \
		if (!registry) {                                                                                                \
			return;                                                                                                 \
		}                                                                                                               \
		row_count = duckdb_data_chunk_get_size(input);                                                                  \
		in0 = duckdb_data_chunk_get_vector(input, 0);                                                                   \
		in1 = duckdb_data_chunk_get_vector(input, 1);                                                                   \
		in2 = duckdb_data_chunk_get_vector(input, 2);                                                                   \
		handles = (uint64_t *)duckdb_vector_get_data(in0);                                                             \
		offsets = (uint64_t *)duckdb_vector_get_data(in1);                                                             \
		values = (ctype *)duckdb_vector_get_data(in2);                                                                  \
		valid0 = duckdb_vector_get_validity(in0);                                                                       \
		valid1 = duckdb_vector_get_validity(in1);                                                                       \
		valid2 = duckdb_vector_get_validity(in2);                                                                       \
		out_data = (bool *)duckdb_vector_get_data(output);                                                              \
		duckdb_vector_ensure_validity_writable(output);                                                                 \
		out_validity = duckdb_vector_get_validity(output);                                                              \
		for (row = 0; row < row_count; row++) {                                                                         \
			if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) ||                         \
			    !tcc_valid_input_row(valid2, row)) {                                                                \
				tcc_set_output_row_null(out_validity, row);                                                       \
				continue;                                                                                         \
			}                                                                                                       \
			out_data[row] =                                                                                         \
			    tcc_ptr_registry_write(registry, handles[row], offsets[row], &values[row], (uint64_t)sizeof(ctype)); \
			duckdb_validity_set_row_validity(out_validity, row, true);                                             \
		}                                                                                                               \
	}

TCC_DEFINE_PTR_READ_SCALAR(i8, int8_t)
TCC_DEFINE_PTR_READ_SCALAR(u8, uint8_t)
TCC_DEFINE_PTR_READ_SCALAR(i16, int16_t)
TCC_DEFINE_PTR_READ_SCALAR(u16, uint16_t)
TCC_DEFINE_PTR_READ_SCALAR(i32, int32_t)
TCC_DEFINE_PTR_READ_SCALAR(u32, uint32_t)
TCC_DEFINE_PTR_READ_SCALAR(i64, int64_t)
TCC_DEFINE_PTR_READ_SCALAR(u64, uint64_t)
TCC_DEFINE_PTR_READ_SCALAR(f32, float)
TCC_DEFINE_PTR_READ_SCALAR(f64, double)

TCC_DEFINE_PTR_WRITE_SCALAR(i8, int8_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u8, uint8_t)
TCC_DEFINE_PTR_WRITE_SCALAR(i16, int16_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u16, uint16_t)
TCC_DEFINE_PTR_WRITE_SCALAR(i32, int32_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u32, uint32_t)
TCC_DEFINE_PTR_WRITE_SCALAR(i64, int64_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u64, uint64_t)
TCC_DEFINE_PTR_WRITE_SCALAR(f32, float)
TCC_DEFINE_PTR_WRITE_SCALAR(f64, double)

#undef TCC_DEFINE_PTR_READ_SCALAR
#undef TCC_DEFINE_PTR_WRITE_SCALAR

/* tcc_read_bytes_scalar: Pointer helper scalar UDF implementation. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_read_bytes_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	duckdb_vector in1;
	duckdb_vector in2;
	uint64_t *handles;
	uint64_t *offsets;
	uint64_t *widths;
	uint64_t *valid0;
	uint64_t *valid1;
	uint64_t *valid2;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in1 = duckdb_data_chunk_get_vector(input, 1);
	in2 = duckdb_data_chunk_get_vector(input, 2);
	handles = (uint64_t *)duckdb_vector_get_data(in0);
	offsets = (uint64_t *)duckdb_vector_get_data(in1);
	widths = (uint64_t *)duckdb_vector_get_data(in2);
	valid0 = duckdb_vector_get_validity(in0);
	valid1 = duckdb_vector_get_validity(in1);
	valid2 = duckdb_vector_get_validity(in2);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		char *buffer = NULL;
		if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) || !tcc_valid_input_row(valid2, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		if (widths[row] == 0) {
			duckdb_vector_assign_string_element_len(output, row, "", 0);
			duckdb_validity_set_row_validity(out_validity, row, true);
			continue;
		}
		if (widths[row] > (uint64_t)(~(size_t)0)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		buffer = (char *)duckdb_malloc((size_t)widths[row]);
		if (!buffer) {
			duckdb_scalar_function_set_error(info, "tcc_read_bytes out of memory");
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		if (!tcc_ptr_registry_read(registry, handles[row], offsets[row], buffer, widths[row])) {
			duckdb_free(buffer);
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		duckdb_vector_assign_string_element_len(output, row, buffer, (idx_t)widths[row]);
		duckdb_validity_set_row_validity(out_validity, row, true);
		duckdb_free(buffer);
	}
}

/* tcc_write_bytes_scalar: Pointer helper scalar UDF implementation. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_write_bytes_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	duckdb_vector in1;
	duckdb_vector in2;
	uint64_t *handles;
	uint64_t *offsets;
	duckdb_string_t *blobs;
	uint64_t *valid0;
	uint64_t *valid1;
	uint64_t *valid2;
	bool *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in1 = duckdb_data_chunk_get_vector(input, 1);
	in2 = duckdb_data_chunk_get_vector(input, 2);
	handles = (uint64_t *)duckdb_vector_get_data(in0);
	offsets = (uint64_t *)duckdb_vector_get_data(in1);
	blobs = (duckdb_string_t *)duckdb_vector_get_data(in2);
	valid0 = duckdb_vector_get_validity(in0);
	valid1 = duckdb_vector_get_validity(in1);
	valid2 = duckdb_vector_get_validity(in2);
	out_data = (bool *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		const char *blob_data;
		uint64_t blob_len;
		if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) || !tcc_valid_input_row(valid2, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		blob_data = duckdb_string_t_data(&blobs[row]);
		blob_len = (uint64_t)duckdb_string_t_length(blobs[row]);
		if (blob_len == 0) {
			out_data[row] = true;
			duckdb_validity_set_row_validity(out_validity, row, true);
			continue;
		}
		out_data[row] = tcc_ptr_registry_write(registry, handles[row], offsets[row], blob_data, blob_len);
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_register_pointer_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static bool tcc_register_pointer_scalar(duckdb_connection connection, const char *name, duckdb_scalar_function_t fn_ptr,
                                        duckdb_type return_type, const duckdb_type *arg_types, idx_t arg_count,
                                        tcc_ptr_registry_t *registry) {
	duckdb_scalar_function fn = duckdb_create_scalar_function();
	duckdb_logical_type ret_obj = NULL;
	duckdb_logical_type *args = NULL;
	tcc_ptr_helper_ctx_t *ctx = NULL;
	idx_t i;
	duckdb_state rc;
	if (!fn) {
		return false;
	}
	ret_obj = duckdb_create_logical_type(return_type);
	if (!ret_obj) {
		duckdb_destroy_scalar_function(&fn);
		return false;
	}
	duckdb_scalar_function_set_name(fn, name);
	duckdb_scalar_function_set_return_type(fn, ret_obj);
	duckdb_scalar_function_set_volatile(fn);
	duckdb_scalar_function_set_function(fn, fn_ptr);
	if (arg_count > 0) {
		args = (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)arg_count);
		if (!args) {
			duckdb_destroy_logical_type(&ret_obj);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		memset(args, 0, sizeof(duckdb_logical_type) * (size_t)arg_count);
		for (i = 0; i < arg_count; i++) {
			args[i] = duckdb_create_logical_type(arg_types[i]);
			if (!args[i]) {
				for (idx_t j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&args[j]);
				}
				duckdb_free(args);
				duckdb_destroy_logical_type(&ret_obj);
				duckdb_destroy_scalar_function(&fn);
				return false;
			}
			duckdb_scalar_function_add_parameter(fn, args[i]);
		}
	}
	if (registry) {
		ctx = (tcc_ptr_helper_ctx_t *)duckdb_malloc(sizeof(tcc_ptr_helper_ctx_t));
		if (!ctx) {
			if (args) {
				for (i = 0; i < arg_count; i++) {
					duckdb_destroy_logical_type(&args[i]);
				}
				duckdb_free(args);
			}
			duckdb_destroy_logical_type(&ret_obj);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		ctx->registry = registry;
		tcc_ptr_registry_ref(registry);
		duckdb_scalar_function_set_extra_info(fn, ctx, tcc_ptr_helper_ctx_destroy);
	}
	rc = duckdb_register_scalar_function(connection, fn);
	if (args) {
		for (i = 0; i < arg_count; i++) {
			duckdb_destroy_logical_type(&args[i]);
		}
		duckdb_free(args);
	}
	duckdb_destroy_logical_type(&ret_obj);
	if (rc != DuckDBSuccess) {
		duckdb_destroy_scalar_function(&fn);
		return false;
	}
	return true;
}

/* register_tcc_pointer_helper_functions: Registers extension SQL helper/table functions. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool register_tcc_pointer_helper_functions(duckdb_connection connection, tcc_ptr_registry_t *registry) {
	static const duckdb_type sig_u64[] = {DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64_u64[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64_blob[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_BLOB};
	static const duckdb_type sig_u64_u64_i8[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_TINYINT};
	static const duckdb_type sig_u64_u64_u8[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UTINYINT};
	static const duckdb_type sig_u64_u64_i16[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_SMALLINT};
	static const duckdb_type sig_u64_u64_u16[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_USMALLINT};
	static const duckdb_type sig_u64_u64_i32[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_INTEGER};
	static const duckdb_type sig_u64_u64_u32[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UINTEGER};
	static const duckdb_type sig_u64_u64_i64[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_BIGINT};
	static const duckdb_type sig_u64_u64_u64w[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64_f32[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_FLOAT};
	static const duckdb_type sig_u64_u64_f64[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_DOUBLE};
	return tcc_register_pointer_scalar(connection, "tcc_alloc", tcc_alloc_scalar, DUCKDB_TYPE_UBIGINT, sig_u64, 1,
	                                   registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_free_ptr", tcc_free_ptr_scalar, DUCKDB_TYPE_BOOLEAN, sig_u64, 1,
	                                   registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_dataptr", tcc_dataptr_scalar, DUCKDB_TYPE_UBIGINT, sig_u64, 1,
	                                   registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_ptr_size", tcc_ptr_size_scalar, DUCKDB_TYPE_UBIGINT, sig_u64, 1,
	                                   registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_ptr_add", tcc_ptr_add_scalar, DUCKDB_TYPE_UBIGINT, sig_u64_u64, 2,
	                                   NULL) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_bytes", tcc_read_bytes_scalar, DUCKDB_TYPE_BLOB,
	                                   sig_u64_u64_u64, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_bytes", tcc_write_bytes_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_blob, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_i8", tcc_read_i8_scalar, DUCKDB_TYPE_TINYINT, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_i8", tcc_write_i8_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_i8, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_u8", tcc_read_u8_scalar, DUCKDB_TYPE_UTINYINT, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_u8", tcc_write_u8_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_u8, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_i16", tcc_read_i16_scalar, DUCKDB_TYPE_SMALLINT, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_i16", tcc_write_i16_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_i16, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_u16", tcc_read_u16_scalar, DUCKDB_TYPE_USMALLINT,
	                                   sig_u64_u64, 2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_u16", tcc_write_u16_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_u16, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_i32", tcc_read_i32_scalar, DUCKDB_TYPE_INTEGER, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_i32", tcc_write_i32_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_i32, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_u32", tcc_read_u32_scalar, DUCKDB_TYPE_UINTEGER,
	                                   sig_u64_u64, 2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_u32", tcc_write_u32_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_u32, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_i64", tcc_read_i64_scalar, DUCKDB_TYPE_BIGINT, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_i64", tcc_write_i64_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_i64, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_u64", tcc_read_u64_scalar, DUCKDB_TYPE_UBIGINT,
	                                   sig_u64_u64, 2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_u64", tcc_write_u64_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_u64w, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_f32", tcc_read_f32_scalar, DUCKDB_TYPE_FLOAT, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_f32", tcc_write_f32_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_f32, 3, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_read_f64", tcc_read_f64_scalar, DUCKDB_TYPE_DOUBLE, sig_u64_u64,
	                                   2, registry) &&
	       tcc_register_pointer_scalar(connection, "tcc_write_f64", tcc_write_f64_scalar, DUCKDB_TYPE_BOOLEAN,
	                                   sig_u64_u64_f64, 3, registry);
}

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
                                        const char *return_type, const char *arg_types_csv) {
	tcc_helper_binding_t entry;
	if (!bindings || !symbol || !sql_name || !return_type || !arg_types_csv) {
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
	if (!entry.symbol || !entry.sql_name || !entry.return_type || !entry.arg_types_csv) {
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

static const char *tcc_default_runtime_path(void) {
#ifdef DUCKTINYCC_DEFAULT_RUNTIME_PATH
	return DUCKTINYCC_DEFAULT_RUNTIME_PATH;
#else
	return "third_party/tinycc";
#endif
}

/* tcc_path_exists: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_path_exists(const char *path) {
	return path && path[0] != '\0' && TCC_ACCESS(path, TCC_ACCESS_FOK) == 0;
}

/* tcc_string_ends_with: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_ends_with(const char *value, const char *suffix) {
	size_t value_len;
	size_t suffix_len;
	if (!value || !suffix) {
		return false;
	}
	value_len = strlen(value);
	suffix_len = strlen(suffix);
	if (suffix_len > value_len) {
		return false;
	}
	return strcmp(value + value_len - suffix_len, suffix) == 0;
}

/* tcc_diag_rows_reserve: Diagnostics/probe table-function helper. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_diag_rows_reserve(tcc_diag_rows_t *rows, idx_t wanted) {
	tcc_diag_row_t *new_rows;
	idx_t new_capacity;
	if (!rows) {
		return false;
	}
	if (rows->capacity >= wanted) {
		return true;
	}
	new_capacity = rows->capacity == 0 ? 16 : rows->capacity * 2;
	while (new_capacity < wanted) {
		new_capacity *= 2;
	}
	new_rows = (tcc_diag_row_t *)duckdb_malloc(sizeof(tcc_diag_row_t) * new_capacity);
	if (!new_rows) {
		return false;
	}
	memset(new_rows, 0, sizeof(tcc_diag_row_t) * new_capacity);
	if (rows->rows && rows->count > 0) {
		memcpy(new_rows, rows->rows, sizeof(tcc_diag_row_t) * rows->count);
		duckdb_free(rows->rows);
	}
	rows->rows = new_rows;
	rows->capacity = new_capacity;
	return true;
}

/* tcc_diag_rows_add: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_diag_rows_add(tcc_diag_rows_t *rows, const char *kind, const char *key, const char *value, bool exists,
                              const char *detail) {
	tcc_diag_row_t *row;
	if (!rows || !kind || !key) {
		return false;
	}
	if (!tcc_diag_rows_reserve(rows, rows->count + 1)) {
		return false;
	}
	row = &rows->rows[rows->count];
	memset(row, 0, sizeof(tcc_diag_row_t));
	row->kind = tcc_strdup(kind);
	row->key = tcc_strdup(key);
	row->value = value ? tcc_strdup(value) : NULL;
	row->exists = exists;
	row->detail = detail ? tcc_strdup(detail) : NULL;
	if (!row->kind || !row->key || (value && !row->value) || (detail && !row->detail)) {
		if (row->kind) {
			duckdb_free(row->kind);
		}
		if (row->key) {
			duckdb_free(row->key);
		}
		if (row->value) {
			duckdb_free(row->value);
		}
		if (row->detail) {
			duckdb_free(row->detail);
		}
		memset(row, 0, sizeof(tcc_diag_row_t));
		return false;
	}
	rows->count++;
	return true;
}

/* tcc_diag_rows_destroy: Diagnostics/probe table-function helper. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_diag_rows_destroy(tcc_diag_rows_t *rows) {
	idx_t i;
	if (!rows) {
		return;
	}
	for (i = 0; i < rows->count; i++) {
		if (rows->rows[i].kind) {
			duckdb_free(rows->rows[i].kind);
		}
		if (rows->rows[i].key) {
			duckdb_free(rows->rows[i].key);
		}
		if (rows->rows[i].value) {
			duckdb_free(rows->rows[i].value);
		}
		if (rows->rows[i].detail) {
			duckdb_free(rows->rows[i].detail);
		}
	}
	if (rows->rows) {
		duckdb_free(rows->rows);
	}
	memset(rows, 0, sizeof(tcc_diag_rows_t));
}

static char *tcc_path_join(const char *base, const char *leaf) {
	size_t base_len;
	size_t leaf_len;
	bool needs_sep;
	char *joined;
	if (!base || !leaf || base[0] == '\0' || leaf[0] == '\0') {
		return NULL;
	}
	base_len = strlen(base);
	leaf_len = strlen(leaf);
	needs_sep = !(base[base_len - 1] == '/' || base[base_len - 1] == '\\');
	joined = (char *)duckdb_malloc(base_len + leaf_len + (needs_sep ? 2 : 1));
	if (!joined) {
		return NULL;
	}
	memcpy(joined, base, base_len);
	if (needs_sep) {
		joined[base_len] = '/';
		memcpy(joined + base_len + 1, leaf, leaf_len);
		joined[base_len + 1 + leaf_len] = '\0';
	} else {
		memcpy(joined + base_len, leaf, leaf_len);
		joined[base_len + leaf_len] = '\0';
	}
	return joined;
}

/* tcc_string_equals_path: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_equals_path(const char *a, const char *b) {
#ifdef _WIN32
	while (*a && *b) {
		unsigned char ca = (unsigned char)tolower((unsigned char)*a);
		unsigned char cb = (unsigned char)tolower((unsigned char)*b);
		if (ca != cb) {
			return false;
		}
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
#else
	return strcmp(a, b) == 0;
#endif
}

/* tcc_string_list_contains: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_list_contains(const tcc_string_list_t *list, const char *value) {
	idx_t i;
	if (!list || !value || value[0] == '\0') {
		return false;
	}
	for (i = 0; i < list->count; i++) {
		if (list->items[i] && tcc_string_equals_path(list->items[i], value)) {
			return true;
		}
	}
	return false;
}

static bool tcc_append_env_path_list(tcc_string_list_t *list, const char *path_list);

/* tcc_string_list_destroy: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_string_list_destroy(tcc_string_list_t *list) {
	idx_t i;
	if (!list) {
		return;
	}
	for (i = 0; i < list->count; i++) {
		if (list->items[i]) {
			duckdb_free(list->items[i]);
		}
	}
	if (list->items) {
		duckdb_free(list->items);
	}
	memset(list, 0, sizeof(tcc_string_list_t));
}

/* tcc_string_list_reserve: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_string_list_reserve(tcc_string_list_t *list, idx_t wanted) {
	char **new_items;
	idx_t new_capacity;
	if (!list) {
		return false;
	}
	if (list->capacity >= wanted) {
		return true;
	}
	new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
	while (new_capacity < wanted) {
		new_capacity *= 2;
	}
	new_items = (char **)duckdb_malloc(sizeof(char *) * new_capacity);
	if (!new_items) {
		return false;
	}
	memset(new_items, 0, sizeof(char *) * new_capacity);
	if (list->items && list->count > 0) {
		memcpy(new_items, list->items, sizeof(char *) * list->count);
		duckdb_free(list->items);
	}
	list->items = new_items;
	list->capacity = new_capacity;
	return true;
}

/* tcc_string_list_append: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_list_append(tcc_string_list_t *list, const char *value) {
	char *copy;
	if (!list || !value || value[0] == '\0') {
		return false;
	}
	if (!tcc_string_list_reserve(list, list->count + 1)) {
		return false;
	}
	copy = tcc_strdup(value);
	if (!copy) {
		return false;
	}
	list->items[list->count++] = copy;
	return true;
}

/* tcc_string_list_pop_last: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_list_pop_last(tcc_string_list_t *list) {
	if (!list || list->count == 0) {
		return false;
	}
	list->count--;
	if (list->items[list->count]) {
		duckdb_free(list->items[list->count]);
		list->items[list->count] = NULL;
	}
	return true;
}

/* tcc_string_list_append_unique: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_list_append_unique(tcc_string_list_t *list, const char *value) {
	if (!list || !value || value[0] == '\0') {
		return false;
	}
	if (tcc_string_list_contains(list, value)) {
		return true;
	}
	return tcc_string_list_append(list, value);
}

/* tcc_is_path_like: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_is_path_like(const char *value) {
	if (!value || value[0] == '\0') {
		return false;
	}
	if (strchr(value, '/') || strchr(value, '\\')) {
		return true;
	}
#ifdef _WIN32
	if (strlen(value) >= 2 && isalpha((unsigned char)value[0]) && value[1] == ':') {
		return true;
	}
#endif
	if (value[0] == '.') {
		return true;
	}
	return false;
}

/* tcc_has_library_suffix: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_has_library_suffix(const char *value) {
	if (!value || value[0] == '\0') {
		return false;
	}
#ifdef _WIN32
	if (tcc_string_ends_with(value, ".dll") || tcc_string_ends_with(value, ".DLL") ||
	    tcc_string_ends_with(value, ".lib") || tcc_string_ends_with(value, ".LIB") ||
	    tcc_string_ends_with(value, ".a") || tcc_string_ends_with(value, ".A")) {
		return true;
	}
#else
	if (tcc_string_ends_with(value, ".so") || strstr(value, ".so.") || tcc_string_ends_with(value, ".dylib") ||
	    tcc_string_ends_with(value, ".a")) {
		return true;
	}
#endif
	return false;
}

/* tcc_append_env_path_list: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_append_env_path_list(tcc_string_list_t *list, const char *path_list) {
	const char *start;
	const char *p;
	if (!list || !path_list || path_list[0] == '\0') {
		return true;
	}
	start = path_list;
	p = path_list;
	while (true) {
		if (*p == '\0' || *p == TCC_ENV_PATH_SEP) {
			const char *token_start = start;
			const char *token_end = p;
			char *token;
			size_t len;
			while (token_start < token_end && isspace((unsigned char)*token_start)) {
				token_start++;
			}
			while (token_end > token_start && isspace((unsigned char)token_end[-1])) {
				token_end--;
			}
			len = (size_t)(token_end - token_start);
			if (len > 0) {
				token = (char *)duckdb_malloc(len + 1);
				if (!token) {
					return false;
				}
				memcpy(token, token_start, len);
				token[len] = '\0';
				if (!tcc_string_list_append_unique(list, token)) {
					duckdb_free(token);
					return false;
				}
				duckdb_free(token);
			}
			if (*p == '\0') {
				break;
			}
			start = p + 1;
		}
		p++;
	}
	return true;
}

/* tcc_add_platform_library_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_add_platform_library_paths(tcc_string_list_t *paths) {
	idx_t i;
#ifdef _WIN32
	const char *candidates[] = {
	    "C:/msys64/mingw64/lib", "C:/msys64/mingw32/lib", "C:/Rtools45/mingw_64/lib", "C:/Rtools45/mingw_32/lib",
	    "C:/Rtools44/mingw_64/lib", "C:/Rtools44/mingw_32/lib"};
	const char *system_root = getenv("SystemRoot");
	char *system32 = NULL;
	char *syswow64 = NULL;
	if (!system_root || system_root[0] == '\0') {
		system_root = "C:/Windows";
	}
	system32 = tcc_path_join(system_root, "System32");
	syswow64 = tcc_path_join(system_root, "SysWOW64");
	if (system32) {
		if (!tcc_string_list_append_unique(paths, system32)) {
			duckdb_free(system32);
			if (syswow64) {
				duckdb_free(syswow64);
			}
			return false;
		}
		duckdb_free(system32);
	}
	if (syswow64) {
		if (!tcc_string_list_append_unique(paths, syswow64)) {
			duckdb_free(syswow64);
			return false;
		}
		duckdb_free(syswow64);
	}
#elif defined(__APPLE__)
	const char *candidates[] = {"/usr/lib", "/usr/local/lib", "/opt/homebrew/lib", "/opt/local/lib",
	                            "/System/Library/Frameworks", "/Library/Frameworks"};
#else
	const char *candidates[] = {"/usr/lib",          "/usr/lib64",            "/usr/local/lib",       "/lib",
	                            "/lib64",            "/lib32",                "/usr/local/lib64",     "/usr/lib/x86_64-linux-gnu",
	                            "/usr/lib/i386-linux-gnu", "/lib/x86_64-linux-gnu", "/lib32/x86_64-linux-gnu",
	                            "/usr/lib/x86_64-linux-musl", "/usr/lib/i386-linux-musl", "/lib/x86_64-linux-musl",
	                            "/lib32/x86_64-linux-musl",   "/usr/lib/amd64-linux-gnu", "/usr/lib/aarch64-linux-gnu"};
#endif
	for (i = 0; i < (idx_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
		if (!tcc_string_list_append_unique(paths, candidates[i])) {
			return false;
		}
	}
	return true;
}

/* tcc_collect_library_search_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_collect_library_search_paths(const char *runtime_path, const char *extra_paths,
                                             tcc_string_list_t *out_paths) {
	char *p = NULL;
	if (!out_paths) {
		return false;
	}
	if (runtime_path && runtime_path[0] != '\0') {
		if (!tcc_string_list_append_unique(out_paths, runtime_path)) {
			return false;
		}
		p = tcc_path_join(runtime_path, "lib");
		if (p) {
			if (!tcc_string_list_append_unique(out_paths, p)) {
				duckdb_free(p);
				return false;
			}
			duckdb_free(p);
		}
		p = tcc_path_join(runtime_path, "lib/tcc");
		if (p) {
			if (!tcc_string_list_append_unique(out_paths, p)) {
				duckdb_free(p);
				return false;
			}
			duckdb_free(p);
		}
#ifdef _WIN32
		p = tcc_path_join(runtime_path, "bin");
		if (p) {
			if (!tcc_string_list_append_unique(out_paths, p)) {
				duckdb_free(p);
				return false;
			}
			duckdb_free(p);
		}
#endif
	}
	if (extra_paths && extra_paths[0] != '\0') {
		if (!tcc_append_env_path_list(out_paths, extra_paths)) {
			return false;
		}
	}
	if (!tcc_add_platform_library_paths(out_paths)) {
		return false;
	}
#ifdef _WIN32
	if (!tcc_append_env_path_list(out_paths, getenv("LIB"))) {
		return false;
	}
	if (!tcc_append_env_path_list(out_paths, getenv("PATH"))) {
		return false;
	}
#elif defined(__APPLE__)
	if (!tcc_append_env_path_list(out_paths, getenv("DYLD_LIBRARY_PATH"))) {
		return false;
	}
	if (!tcc_append_env_path_list(out_paths, getenv("LD_LIBRARY_PATH"))) {
		return false;
	}
	if (!tcc_append_env_path_list(out_paths, getenv("LIBRARY_PATH"))) {
		return false;
	}
#else
	if (!tcc_append_env_path_list(out_paths, getenv("LD_LIBRARY_PATH"))) {
		return false;
	}
	if (!tcc_append_env_path_list(out_paths, getenv("LIBRARY_PATH"))) {
		return false;
	}
#endif
	return true;
}

/* tcc_collect_include_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_collect_include_paths(const char *runtime_path, tcc_string_list_t *out_paths) {
	char *p = NULL;
	if (!out_paths) {
		return false;
	}
	if (runtime_path && runtime_path[0] != '\0') {
		p = tcc_path_join(runtime_path, "include");
		if (p) {
			if (!tcc_string_list_append_unique(out_paths, p)) {
				duckdb_free(p);
				return false;
			}
			duckdb_free(p);
		}
		p = tcc_path_join(runtime_path, "lib/tcc/include");
		if (p) {
			if (!tcc_string_list_append_unique(out_paths, p)) {
				duckdb_free(p);
				return false;
			}
			duckdb_free(p);
		}
#ifdef _WIN32
		p = tcc_path_join(runtime_path, "include/winapi");
		if (p) {
			if (!tcc_string_list_append_unique(out_paths, p)) {
				duckdb_free(p);
				return false;
			}
			duckdb_free(p);
		}
#endif
	}
	return true;
}

/* tcc_build_library_candidates: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_build_library_candidates(const char *library, tcc_string_list_t *out_candidates) {
	char candidate[512];
	if (!library || library[0] == '\0' || !out_candidates) {
		return false;
	}
	if (tcc_is_path_like(library) || tcc_has_library_suffix(library)) {
		return tcc_string_list_append_unique(out_candidates, library);
	}
	if (!tcc_string_list_append_unique(out_candidates, library)) {
		return false;
	}
#ifdef _WIN32
	snprintf(candidate, sizeof(candidate), "%s.dll", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "lib%s.dll", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "%s.lib", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "lib%s.lib", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "lib%s.a", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
#elif defined(__APPLE__)
	snprintf(candidate, sizeof(candidate), "lib%s.dylib", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "lib%s.so", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "lib%s.a", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
#else
	snprintf(candidate, sizeof(candidate), "lib%s.so", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
	snprintf(candidate, sizeof(candidate), "lib%s.a", library);
	if (!tcc_string_list_append_unique(out_candidates, candidate)) {
		return false;
	}
#endif
	return true;
}

static const char *tcc_basename_ptr(const char *path) {
	const char *s1;
	const char *s2;
	const char *best;
	if (!path) {
		return "";
	}
	s1 = strrchr(path, '/');
	s2 = strrchr(path, '\\');
	best = s1;
	if (!best || (s2 && s2 > best)) {
		best = s2;
	}
	return best ? best + 1 : path;
}

static char *tcc_library_link_name_from_path(const char *path) {
	const char *base;
	char *name;
	char *so_pos;
	size_t len;
	if (!path || path[0] == '\0') {
		return NULL;
	}
	base = tcc_basename_ptr(path);
	name = tcc_strdup(base);
	if (!name) {
		return NULL;
	}
	len = strlen(name);
#ifdef _WIN32
	if (len > 4 && tcc_equals_ci(name + len - 4, ".dll")) {
		name[len - 4] = '\0';
	} else if (len > 4 && tcc_equals_ci(name + len - 4, ".lib")) {
		name[len - 4] = '\0';
	} else if (len > 2 && tcc_equals_ci(name + len - 2, ".a")) {
		name[len - 2] = '\0';
	}
#else
	so_pos = strstr(name, ".so");
	if (so_pos) {
		*so_pos = '\0';
	} else if (len > 6 && tcc_equals_ci(name + len - 6, ".dylib")) {
		name[len - 6] = '\0';
	} else if (len > 2 && tcc_equals_ci(name + len - 2, ".a")) {
		name[len - 2] = '\0';
	}
#endif
	if (strncmp(name, "lib", 3) == 0 && strlen(name) > 3) {
		memmove(name, name + 3, strlen(name + 3) + 1);
	}
	return name;
}

/* tcc_session_clear_bind: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_session_clear_bind(tcc_session_t *session) {
	if (!session) {
		return;
	}
	if (session->bound_symbol) {
		duckdb_free(session->bound_symbol);
		session->bound_symbol = NULL;
	}
	if (session->bound_sql_name) {
		duckdb_free(session->bound_sql_name);
		session->bound_sql_name = NULL;
	}
}

/* tcc_session_clear_build_state: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_session_clear_build_state(tcc_session_t *session) {
	if (!session) {
		return;
	}
	tcc_string_list_destroy(&session->include_paths);
	tcc_string_list_destroy(&session->sysinclude_paths);
	tcc_string_list_destroy(&session->library_paths);
	tcc_string_list_destroy(&session->libraries);
	tcc_string_list_destroy(&session->options);
	tcc_string_list_destroy(&session->headers);
	tcc_string_list_destroy(&session->sources);
	tcc_string_list_destroy(&session->define_names);
	tcc_string_list_destroy(&session->define_values);
	tcc_session_clear_bind(session);
	session->state_id++;
	session->config_version++;
}

/* tcc_session_set_runtime_path: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_session_set_runtime_path(tcc_session_t *session, const char *runtime_path) {
	if (!session) {
		return;
	}
	if (session->runtime_path) {
		duckdb_free(session->runtime_path);
		session->runtime_path = NULL;
	}
	if (runtime_path && runtime_path[0] != '\0') {
		session->runtime_path = tcc_strdup(runtime_path);
	}
	session->config_version++;
}

static const char *tcc_session_runtime_path(tcc_module_state_t *state, const char *override_path) {
	if (override_path && override_path[0] != '\0') {
		return override_path;
	}
	if (state->session.runtime_path && state->session.runtime_path[0] != '\0') {
		return state->session.runtime_path;
	}
	return tcc_default_runtime_path();
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* tcc_configure_runtime_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_configure_runtime_paths(TCCState *s, const char *runtime_path) {
	char include_path[1024];
	char include_path2[1024];
	char lib_path[1024];
	char lib_tcc_path[1024];
	if (!runtime_path || runtime_path[0] == '\0') {
		return;
	}
	(void)tcc_set_lib_path(s, runtime_path);
	snprintf(include_path, sizeof(include_path), "%s/include", runtime_path);
	snprintf(include_path2, sizeof(include_path2), "%s/lib/tcc/include", runtime_path);
	snprintf(lib_path, sizeof(lib_path), "%s/lib", runtime_path);
	snprintf(lib_tcc_path, sizeof(lib_tcc_path), "%s/lib/tcc", runtime_path);
	(void)tcc_add_library_path(s, runtime_path);
	(void)tcc_add_include_path(s, include_path);
	(void)tcc_add_sysinclude_path(s, include_path);
	(void)tcc_add_include_path(s, include_path2);
	(void)tcc_add_sysinclude_path(s, include_path2);
	(void)tcc_add_library_path(s, lib_path);
	(void)tcc_add_library_path(s, lib_tcc_path);
}
#endif

/* Destructor for per-UDF host signature context.
 * All members are treated as owned by `ctx` once attached via
 * `duckdb_scalar_function_set_extra_info`.
 */
static void tcc_host_sig_ctx_destroy(void *ptr) {
	tcc_host_sig_ctx_t *ctx = (tcc_host_sig_ctx_t *)ptr;
	if (!ctx) {
		return;
	}
	if (ctx->arg_types) {
		duckdb_free(ctx->arg_types);
	}
	if (ctx->arg_sizes) {
		duckdb_free(ctx->arg_sizes);
	}
	if (ctx->arg_array_sizes) {
		duckdb_free(ctx->arg_array_sizes);
	}
	if (ctx->arg_struct_metas && ctx->arg_count > 0) {
		tcc_struct_meta_array_destroy(ctx->arg_struct_metas, ctx->arg_count);
	}
	if (ctx->arg_map_metas && ctx->arg_count > 0) {
		tcc_map_meta_array_destroy(ctx->arg_map_metas, ctx->arg_count);
	}
	if (ctx->arg_union_metas && ctx->arg_count > 0) {
		tcc_union_meta_array_destroy(ctx->arg_union_metas, ctx->arg_count);
	}
	if (ctx->arg_descs && ctx->arg_count > 0) {
		int i;
		for (i = 0; i < ctx->arg_count; i++) {
			if (ctx->arg_descs[i]) {
				tcc_typedesc_destroy(ctx->arg_descs[i]);
			}
		}
		duckdb_free(ctx->arg_descs);
	}
	if (ctx->return_desc) {
		tcc_typedesc_destroy(ctx->return_desc);
	}
	tcc_struct_meta_destroy(&ctx->return_struct_meta);
	tcc_map_meta_destroy(&ctx->return_map_meta);
	tcc_union_meta_destroy(&ctx->return_union_meta);
	duckdb_free(ctx);
}

/* tcc_ffi_type_is_list: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_list(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_LIST:
	case TCC_FFI_LIST_BOOL:
	case TCC_FFI_LIST_I8:
	case TCC_FFI_LIST_U8:
	case TCC_FFI_LIST_I16:
	case TCC_FFI_LIST_U16:
	case TCC_FFI_LIST_I32:
	case TCC_FFI_LIST_U32:
	case TCC_FFI_LIST_I64:
	case TCC_FFI_LIST_U64:
	case TCC_FFI_LIST_F32:
	case TCC_FFI_LIST_F64:
	case TCC_FFI_LIST_UUID:
	case TCC_FFI_LIST_DATE:
	case TCC_FFI_LIST_TIME:
	case TCC_FFI_LIST_TIMESTAMP:
	case TCC_FFI_LIST_INTERVAL:
	case TCC_FFI_LIST_DECIMAL:
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_type_is_fixed_width_scalar: Type-system conversion/parsing helper. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static bool tcc_ffi_type_is_fixed_width_scalar(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_BOOL:
	case TCC_FFI_I8:
	case TCC_FFI_U8:
	case TCC_FFI_I16:
	case TCC_FFI_U16:
	case TCC_FFI_I32:
	case TCC_FFI_U32:
	case TCC_FFI_I64:
	case TCC_FFI_U64:
	case TCC_FFI_F32:
	case TCC_FFI_F64:
	case TCC_FFI_UUID:
	case TCC_FFI_DATE:
	case TCC_FFI_TIME:
	case TCC_FFI_TIMESTAMP:
	case TCC_FFI_INTERVAL:
	case TCC_FFI_DECIMAL:
	case TCC_FFI_PTR:
		return true;
	default:
		return false;
	}
}

/* tcc_struct_meta_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_struct_meta_destroy(tcc_ffi_struct_meta_t *meta) {
	int i;
	if (!meta) {
		return;
	}
	if (meta->field_names) {
		for (i = 0; i < meta->field_count; i++) {
			if (meta->field_names[i]) {
				duckdb_free(meta->field_names[i]);
			}
		}
		duckdb_free(meta->field_names);
	}
	if (meta->field_tokens) {
		for (i = 0; i < meta->field_count; i++) {
			if (meta->field_tokens[i]) {
				duckdb_free(meta->field_tokens[i]);
			}
		}
		duckdb_free(meta->field_tokens);
	}
	if (meta->field_types) {
		duckdb_free(meta->field_types);
	}
	if (meta->field_sizes) {
		duckdb_free(meta->field_sizes);
	}
	memset(meta, 0, sizeof(*meta));
}

/* tcc_struct_meta_array_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_struct_meta_array_destroy(tcc_ffi_struct_meta_t *metas, int count) {
	int i;
	if (!metas || count <= 0) {
		if (metas) {
			duckdb_free(metas);
		}
		return;
	}
	for (i = 0; i < count; i++) {
		tcc_struct_meta_destroy(&metas[i]);
	}
	duckdb_free(metas);
}

/* tcc_map_meta_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_map_meta_destroy(tcc_ffi_map_meta_t *meta) {
	if (!meta) {
		return;
	}
	if (meta->key_token) {
		duckdb_free(meta->key_token);
	}
	if (meta->value_token) {
		duckdb_free(meta->value_token);
	}
	memset(meta, 0, sizeof(*meta));
}

/* tcc_map_meta_array_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_map_meta_array_destroy(tcc_ffi_map_meta_t *metas, int count) {
	int i;
	if (!metas || count <= 0) {
		if (metas) {
			duckdb_free(metas);
		}
		return;
	}
	for (i = 0; i < count; i++) {
		tcc_map_meta_destroy(&metas[i]);
	}
	duckdb_free(metas);
}

/* tcc_union_meta_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_union_meta_destroy(tcc_ffi_union_meta_t *meta) {
	int i;
	if (!meta) {
		return;
	}
	if (meta->member_names) {
		for (i = 0; i < meta->member_count; i++) {
			if (meta->member_names[i]) {
				duckdb_free(meta->member_names[i]);
			}
		}
		duckdb_free(meta->member_names);
	}
	if (meta->member_tokens) {
		for (i = 0; i < meta->member_count; i++) {
			if (meta->member_tokens[i]) {
				duckdb_free(meta->member_tokens[i]);
			}
		}
		duckdb_free(meta->member_tokens);
	}
	if (meta->member_types) {
		duckdb_free(meta->member_types);
	}
	if (meta->member_sizes) {
		duckdb_free(meta->member_sizes);
	}
	memset(meta, 0, sizeof(*meta));
}

/* tcc_union_meta_array_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_union_meta_array_destroy(tcc_ffi_union_meta_t *metas, int count) {
	int i;
	if (!metas) {
		return;
	}
	for (i = 0; i < count; i++) {
		tcc_union_meta_destroy(&metas[i]);
	}
	duckdb_free(metas);
}

/* tcc_ffi_list_child_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_list_child_type(tcc_ffi_type_t list_type, tcc_ffi_type_t *out_child) {
	if (!out_child) {
		return false;
	}
	switch (list_type) {
	case TCC_FFI_LIST_BOOL:
		*out_child = TCC_FFI_BOOL;
		return true;
	case TCC_FFI_LIST_I8:
		*out_child = TCC_FFI_I8;
		return true;
	case TCC_FFI_LIST_U8:
		*out_child = TCC_FFI_U8;
		return true;
	case TCC_FFI_LIST_I16:
		*out_child = TCC_FFI_I16;
		return true;
	case TCC_FFI_LIST_U16:
		*out_child = TCC_FFI_U16;
		return true;
	case TCC_FFI_LIST_I32:
		*out_child = TCC_FFI_I32;
		return true;
	case TCC_FFI_LIST_U32:
		*out_child = TCC_FFI_U32;
		return true;
	case TCC_FFI_LIST_I64:
		*out_child = TCC_FFI_I64;
		return true;
	case TCC_FFI_LIST_U64:
		*out_child = TCC_FFI_U64;
		return true;
	case TCC_FFI_LIST_F32:
		*out_child = TCC_FFI_F32;
		return true;
	case TCC_FFI_LIST_F64:
		*out_child = TCC_FFI_F64;
		return true;
	case TCC_FFI_LIST_UUID:
		*out_child = TCC_FFI_UUID;
		return true;
	case TCC_FFI_LIST_DATE:
		*out_child = TCC_FFI_DATE;
		return true;
	case TCC_FFI_LIST_TIME:
		*out_child = TCC_FFI_TIME;
		return true;
	case TCC_FFI_LIST_TIMESTAMP:
		*out_child = TCC_FFI_TIMESTAMP;
		return true;
	case TCC_FFI_LIST_INTERVAL:
		*out_child = TCC_FFI_INTERVAL;
		return true;
	case TCC_FFI_LIST_DECIMAL:
		*out_child = TCC_FFI_DECIMAL;
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_list_type_from_child: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_list_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_list_type) {
	if (!out_list_type) {
		return false;
	}
	switch (child_type) {
	case TCC_FFI_BOOL:
		*out_list_type = TCC_FFI_LIST_BOOL;
		return true;
	case TCC_FFI_I8:
		*out_list_type = TCC_FFI_LIST_I8;
		return true;
	case TCC_FFI_U8:
		*out_list_type = TCC_FFI_LIST_U8;
		return true;
	case TCC_FFI_I16:
		*out_list_type = TCC_FFI_LIST_I16;
		return true;
	case TCC_FFI_U16:
		*out_list_type = TCC_FFI_LIST_U16;
		return true;
	case TCC_FFI_I32:
		*out_list_type = TCC_FFI_LIST_I32;
		return true;
	case TCC_FFI_U32:
		*out_list_type = TCC_FFI_LIST_U32;
		return true;
	case TCC_FFI_I64:
		*out_list_type = TCC_FFI_LIST_I64;
		return true;
	case TCC_FFI_U64:
		*out_list_type = TCC_FFI_LIST_U64;
		return true;
	case TCC_FFI_F32:
		*out_list_type = TCC_FFI_LIST_F32;
		return true;
	case TCC_FFI_F64:
		*out_list_type = TCC_FFI_LIST_F64;
		return true;
	case TCC_FFI_UUID:
		*out_list_type = TCC_FFI_LIST_UUID;
		return true;
	case TCC_FFI_DATE:
		*out_list_type = TCC_FFI_LIST_DATE;
		return true;
	case TCC_FFI_TIME:
		*out_list_type = TCC_FFI_LIST_TIME;
		return true;
	case TCC_FFI_TIMESTAMP:
		*out_list_type = TCC_FFI_LIST_TIMESTAMP;
		return true;
	case TCC_FFI_INTERVAL:
		*out_list_type = TCC_FFI_LIST_INTERVAL;
		return true;
	case TCC_FFI_DECIMAL:
		*out_list_type = TCC_FFI_LIST_DECIMAL;
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_type_is_array: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_array(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_ARRAY:
	case TCC_FFI_ARRAY_BOOL:
	case TCC_FFI_ARRAY_I8:
	case TCC_FFI_ARRAY_U8:
	case TCC_FFI_ARRAY_I16:
	case TCC_FFI_ARRAY_U16:
	case TCC_FFI_ARRAY_I32:
	case TCC_FFI_ARRAY_U32:
	case TCC_FFI_ARRAY_I64:
	case TCC_FFI_ARRAY_U64:
	case TCC_FFI_ARRAY_F32:
	case TCC_FFI_ARRAY_F64:
	case TCC_FFI_ARRAY_UUID:
	case TCC_FFI_ARRAY_DATE:
	case TCC_FFI_ARRAY_TIME:
	case TCC_FFI_ARRAY_TIMESTAMP:
	case TCC_FFI_ARRAY_INTERVAL:
	case TCC_FFI_ARRAY_DECIMAL:
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_type_is_struct: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_struct(tcc_ffi_type_t type) {
	return type == TCC_FFI_STRUCT;
}

/* tcc_ffi_type_is_map: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_map(tcc_ffi_type_t type) {
	return type == TCC_FFI_MAP;
}

/* tcc_ffi_type_is_union: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_union(tcc_ffi_type_t type) {
	return type == TCC_FFI_UNION;
}

/* tcc_ffi_array_child_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_array_child_type(tcc_ffi_type_t array_type, tcc_ffi_type_t *out_child) {
	if (!out_child) {
		return false;
	}
	switch (array_type) {
	case TCC_FFI_ARRAY_BOOL:
		*out_child = TCC_FFI_BOOL;
		return true;
	case TCC_FFI_ARRAY_I8:
		*out_child = TCC_FFI_I8;
		return true;
	case TCC_FFI_ARRAY_U8:
		*out_child = TCC_FFI_U8;
		return true;
	case TCC_FFI_ARRAY_I16:
		*out_child = TCC_FFI_I16;
		return true;
	case TCC_FFI_ARRAY_U16:
		*out_child = TCC_FFI_U16;
		return true;
	case TCC_FFI_ARRAY_I32:
		*out_child = TCC_FFI_I32;
		return true;
	case TCC_FFI_ARRAY_U32:
		*out_child = TCC_FFI_U32;
		return true;
	case TCC_FFI_ARRAY_I64:
		*out_child = TCC_FFI_I64;
		return true;
	case TCC_FFI_ARRAY_U64:
		*out_child = TCC_FFI_U64;
		return true;
	case TCC_FFI_ARRAY_F32:
		*out_child = TCC_FFI_F32;
		return true;
	case TCC_FFI_ARRAY_F64:
		*out_child = TCC_FFI_F64;
		return true;
	case TCC_FFI_ARRAY_UUID:
		*out_child = TCC_FFI_UUID;
		return true;
	case TCC_FFI_ARRAY_DATE:
		*out_child = TCC_FFI_DATE;
		return true;
	case TCC_FFI_ARRAY_TIME:
		*out_child = TCC_FFI_TIME;
		return true;
	case TCC_FFI_ARRAY_TIMESTAMP:
		*out_child = TCC_FFI_TIMESTAMP;
		return true;
	case TCC_FFI_ARRAY_INTERVAL:
		*out_child = TCC_FFI_INTERVAL;
		return true;
	case TCC_FFI_ARRAY_DECIMAL:
		*out_child = TCC_FFI_DECIMAL;
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_array_type_from_child: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_array_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_array_type) {
	if (!out_array_type) {
		return false;
	}
	switch (child_type) {
	case TCC_FFI_BOOL:
		*out_array_type = TCC_FFI_ARRAY_BOOL;
		return true;
	case TCC_FFI_I8:
		*out_array_type = TCC_FFI_ARRAY_I8;
		return true;
	case TCC_FFI_U8:
		*out_array_type = TCC_FFI_ARRAY_U8;
		return true;
	case TCC_FFI_I16:
		*out_array_type = TCC_FFI_ARRAY_I16;
		return true;
	case TCC_FFI_U16:
		*out_array_type = TCC_FFI_ARRAY_U16;
		return true;
	case TCC_FFI_I32:
		*out_array_type = TCC_FFI_ARRAY_I32;
		return true;
	case TCC_FFI_U32:
		*out_array_type = TCC_FFI_ARRAY_U32;
		return true;
	case TCC_FFI_I64:
		*out_array_type = TCC_FFI_ARRAY_I64;
		return true;
	case TCC_FFI_U64:
		*out_array_type = TCC_FFI_ARRAY_U64;
		return true;
	case TCC_FFI_F32:
		*out_array_type = TCC_FFI_ARRAY_F32;
		return true;
	case TCC_FFI_F64:
		*out_array_type = TCC_FFI_ARRAY_F64;
		return true;
	case TCC_FFI_UUID:
		*out_array_type = TCC_FFI_ARRAY_UUID;
		return true;
	case TCC_FFI_DATE:
		*out_array_type = TCC_FFI_ARRAY_DATE;
		return true;
	case TCC_FFI_TIME:
		*out_array_type = TCC_FFI_ARRAY_TIME;
		return true;
	case TCC_FFI_TIMESTAMP:
		*out_array_type = TCC_FFI_ARRAY_TIMESTAMP;
		return true;
	case TCC_FFI_INTERVAL:
		*out_array_type = TCC_FFI_ARRAY_INTERVAL;
		return true;
	case TCC_FFI_DECIMAL:
		*out_array_type = TCC_FFI_ARRAY_DECIMAL;
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_type_size: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static size_t tcc_ffi_type_size(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_BOOL:
	case TCC_FFI_I8:
	case TCC_FFI_U8:
		return 1;
	case TCC_FFI_I16:
	case TCC_FFI_U16:
		return 2;
	case TCC_FFI_I32:
	case TCC_FFI_U32:
	case TCC_FFI_F32:
		return 4;
	case TCC_FFI_I64:
	case TCC_FFI_U64:
	case TCC_FFI_F64:
	case TCC_FFI_PTR:
		return 8;
	case TCC_FFI_VARCHAR:
		return sizeof(duckdb_string_t);
	case TCC_FFI_BLOB:
		return sizeof(ducktinycc_blob_t);
	case TCC_FFI_UUID:
		return sizeof(ducktinycc_hugeint_t);
	case TCC_FFI_DATE:
		return sizeof(ducktinycc_date_t);
	case TCC_FFI_TIME:
		return sizeof(ducktinycc_time_t);
	case TCC_FFI_TIMESTAMP:
		return sizeof(ducktinycc_timestamp_t);
	case TCC_FFI_INTERVAL:
		return sizeof(ducktinycc_interval_t);
	case TCC_FFI_DECIMAL:
		return sizeof(ducktinycc_decimal_t);
	case TCC_FFI_STRUCT:
		return sizeof(ducktinycc_struct_t);
	case TCC_FFI_MAP:
		return sizeof(ducktinycc_map_t);
	case TCC_FFI_UNION:
		return sizeof(ducktinycc_union_t);
	case TCC_FFI_LIST:
	case TCC_FFI_LIST_BOOL:
	case TCC_FFI_LIST_I8:
	case TCC_FFI_LIST_U8:
	case TCC_FFI_LIST_I16:
	case TCC_FFI_LIST_U16:
	case TCC_FFI_LIST_I32:
	case TCC_FFI_LIST_U32:
	case TCC_FFI_LIST_I64:
	case TCC_FFI_LIST_U64:
	case TCC_FFI_LIST_F32:
	case TCC_FFI_LIST_F64:
	case TCC_FFI_LIST_UUID:
	case TCC_FFI_LIST_DATE:
	case TCC_FFI_LIST_TIME:
	case TCC_FFI_LIST_TIMESTAMP:
	case TCC_FFI_LIST_INTERVAL:
	case TCC_FFI_LIST_DECIMAL:
		return sizeof(ducktinycc_list_t);
	case TCC_FFI_ARRAY:
	case TCC_FFI_ARRAY_BOOL:
	case TCC_FFI_ARRAY_I8:
	case TCC_FFI_ARRAY_U8:
	case TCC_FFI_ARRAY_I16:
	case TCC_FFI_ARRAY_U16:
	case TCC_FFI_ARRAY_I32:
	case TCC_FFI_ARRAY_U32:
	case TCC_FFI_ARRAY_I64:
	case TCC_FFI_ARRAY_U64:
	case TCC_FFI_ARRAY_F32:
	case TCC_FFI_ARRAY_F64:
	case TCC_FFI_ARRAY_UUID:
	case TCC_FFI_ARRAY_DATE:
	case TCC_FFI_ARRAY_TIME:
	case TCC_FFI_ARRAY_TIMESTAMP:
	case TCC_FFI_ARRAY_INTERVAL:
	case TCC_FFI_ARRAY_DECIMAL:
		return sizeof(ducktinycc_array_t);
	case TCC_FFI_VOID:
	default:
		return 0;
	}
}

/* tcc_ffi_type_to_duckdb_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static duckdb_type tcc_ffi_type_to_duckdb_type(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_VOID:
		/* Scalar UDFs need a concrete type; void returns are emitted as NULL BIGINT. */
		return DUCKDB_TYPE_BIGINT;
	case TCC_FFI_BOOL:
		return DUCKDB_TYPE_BOOLEAN;
	case TCC_FFI_I8:
		return DUCKDB_TYPE_TINYINT;
	case TCC_FFI_U8:
		return DUCKDB_TYPE_UTINYINT;
	case TCC_FFI_I16:
		return DUCKDB_TYPE_SMALLINT;
	case TCC_FFI_U16:
		return DUCKDB_TYPE_USMALLINT;
	case TCC_FFI_I32:
		return DUCKDB_TYPE_INTEGER;
	case TCC_FFI_U32:
		return DUCKDB_TYPE_UINTEGER;
	case TCC_FFI_I64:
		return DUCKDB_TYPE_BIGINT;
	case TCC_FFI_U64:
		return DUCKDB_TYPE_UBIGINT;
	case TCC_FFI_PTR:
		return DUCKDB_TYPE_UBIGINT;
	case TCC_FFI_F32:
		return DUCKDB_TYPE_FLOAT;
	case TCC_FFI_F64:
		return DUCKDB_TYPE_DOUBLE;
	case TCC_FFI_VARCHAR:
		return DUCKDB_TYPE_VARCHAR;
	case TCC_FFI_BLOB:
		return DUCKDB_TYPE_BLOB;
	case TCC_FFI_UUID:
		return DUCKDB_TYPE_UUID;
	case TCC_FFI_DATE:
		return DUCKDB_TYPE_DATE;
	case TCC_FFI_TIME:
		return DUCKDB_TYPE_TIME;
	case TCC_FFI_TIMESTAMP:
		return DUCKDB_TYPE_TIMESTAMP;
	case TCC_FFI_INTERVAL:
		return DUCKDB_TYPE_INTERVAL;
	case TCC_FFI_DECIMAL:
		return DUCKDB_TYPE_DECIMAL;
	case TCC_FFI_STRUCT:
		return DUCKDB_TYPE_STRUCT;
	case TCC_FFI_MAP:
		return DUCKDB_TYPE_MAP;
	case TCC_FFI_UNION:
		return DUCKDB_TYPE_UNION;
	case TCC_FFI_LIST:
	case TCC_FFI_LIST_BOOL:
	case TCC_FFI_LIST_I8:
	case TCC_FFI_LIST_U8:
	case TCC_FFI_LIST_I16:
	case TCC_FFI_LIST_U16:
	case TCC_FFI_LIST_I32:
	case TCC_FFI_LIST_U32:
	case TCC_FFI_LIST_I64:
	case TCC_FFI_LIST_U64:
	case TCC_FFI_LIST_F32:
	case TCC_FFI_LIST_F64:
	case TCC_FFI_LIST_UUID:
	case TCC_FFI_LIST_DATE:
	case TCC_FFI_LIST_TIME:
	case TCC_FFI_LIST_TIMESTAMP:
	case TCC_FFI_LIST_INTERVAL:
	case TCC_FFI_LIST_DECIMAL:
		return DUCKDB_TYPE_LIST;
	case TCC_FFI_ARRAY:
	case TCC_FFI_ARRAY_BOOL:
	case TCC_FFI_ARRAY_I8:
	case TCC_FFI_ARRAY_U8:
	case TCC_FFI_ARRAY_I16:
	case TCC_FFI_ARRAY_U16:
	case TCC_FFI_ARRAY_I32:
	case TCC_FFI_ARRAY_U32:
	case TCC_FFI_ARRAY_I64:
	case TCC_FFI_ARRAY_U64:
	case TCC_FFI_ARRAY_F32:
	case TCC_FFI_ARRAY_F64:
	case TCC_FFI_ARRAY_UUID:
	case TCC_FFI_ARRAY_DATE:
	case TCC_FFI_ARRAY_TIME:
	case TCC_FFI_ARRAY_TIMESTAMP:
	case TCC_FFI_ARRAY_INTERVAL:
	case TCC_FFI_ARRAY_DECIMAL:
		return DUCKDB_TYPE_ARRAY;
	default:
		return DUCKDB_TYPE_INVALID;
	}
}

/* tcc_ffi_type_create_logical_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static duckdb_logical_type tcc_ffi_type_create_logical_type(tcc_ffi_type_t type, size_t array_size,
                                                             const tcc_ffi_struct_meta_t *struct_meta,
                                                             const tcc_ffi_map_meta_t *map_meta,
                                                             const tcc_ffi_union_meta_t *union_meta) {
	duckdb_type base_type;
	if (tcc_ffi_type_is_list(type)) {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		duckdb_logical_type child_logical = NULL;
		duckdb_logical_type list_logical = NULL;
		if (!tcc_ffi_list_child_type(type, &child_type)) {
			return NULL;
		}
		child_logical = tcc_ffi_type_create_logical_type(child_type, 0, NULL, NULL, NULL);
		if (!child_logical) {
			return NULL;
		}
		list_logical = duckdb_create_list_type(child_logical);
		duckdb_destroy_logical_type(&child_logical);
		return list_logical;
	}
	if (tcc_ffi_type_is_array(type)) {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		duckdb_logical_type child_logical = NULL;
		duckdb_logical_type array_logical = NULL;
		if (array_size == 0 || !tcc_ffi_array_child_type(type, &child_type)) {
			return NULL;
		}
		child_logical = tcc_ffi_type_create_logical_type(child_type, 0, NULL, NULL, NULL);
		if (!child_logical) {
			return NULL;
		}
		array_logical = duckdb_create_array_type(child_logical, (idx_t)array_size);
		duckdb_destroy_logical_type(&child_logical);
		return array_logical;
	}
	if (type == TCC_FFI_STRUCT) {
		duckdb_logical_type struct_type = NULL;
		duckdb_logical_type *child_types = NULL;
		const char **child_names = NULL;
		int i;
		if (!struct_meta || struct_meta->field_count <= 0 || !struct_meta->field_names || !struct_meta->field_types) {
			return NULL;
		}
		child_types = (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)struct_meta->field_count);
		child_names = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)struct_meta->field_count);
		if (!child_types || !child_names) {
			if (child_types) {
				duckdb_free(child_types);
			}
			if (child_names) {
				duckdb_free((void *)child_names);
			}
			return NULL;
		}
		for (i = 0; i < struct_meta->field_count; i++) {
			tcc_ffi_type_t child_type = struct_meta->field_types[i];
			size_t child_array_size = 0;
			tcc_ffi_struct_meta_t child_struct_meta;
			tcc_ffi_map_meta_t child_map_meta;
			tcc_ffi_union_meta_t child_union_meta;
			child_types[i] = NULL;
			child_names[i] = struct_meta->field_names[i];
			memset(&child_struct_meta, 0, sizeof(child_struct_meta));
			memset(&child_map_meta, 0, sizeof(child_map_meta));
			memset(&child_union_meta, 0, sizeof(child_union_meta));
			if (struct_meta->field_tokens && struct_meta->field_tokens[i] && struct_meta->field_tokens[i][0] != '\0') {
				if (!tcc_parse_type_token(struct_meta->field_tokens[i], false, &child_type, &child_array_size)) {
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
				if (child_type == TCC_FFI_STRUCT &&
				    !tcc_parse_struct_meta_token(struct_meta->field_tokens[i], &child_struct_meta, NULL)) {
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
				if (child_type == TCC_FFI_MAP &&
				    !tcc_parse_map_meta_token(struct_meta->field_tokens[i], &child_map_meta, NULL)) {
					tcc_struct_meta_destroy(&child_struct_meta);
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
				if (child_type == TCC_FFI_UNION &&
				    !tcc_parse_union_meta_token(struct_meta->field_tokens[i], &child_union_meta, NULL)) {
					tcc_struct_meta_destroy(&child_struct_meta);
					tcc_map_meta_destroy(&child_map_meta);
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
			}
			child_types[i] =
			    tcc_ffi_type_create_logical_type(child_type, child_array_size, &child_struct_meta, &child_map_meta,
			                                     &child_union_meta);
			tcc_struct_meta_destroy(&child_struct_meta);
			tcc_map_meta_destroy(&child_map_meta);
			tcc_union_meta_destroy(&child_union_meta);
			if (!child_types[i]) {
				int j;
				for (j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&child_types[j]);
				}
				duckdb_free(child_types);
				duckdb_free((void *)child_names);
				return NULL;
			}
		}
		struct_type = duckdb_create_struct_type(child_types, child_names, (idx_t)struct_meta->field_count);
		for (i = 0; i < struct_meta->field_count; i++) {
			duckdb_destroy_logical_type(&child_types[i]);
		}
		duckdb_free(child_types);
		duckdb_free((void *)child_names);
		return struct_type;
	}
	if (type == TCC_FFI_MAP) {
		duckdb_logical_type key_type = NULL;
		duckdb_logical_type value_type = NULL;
		duckdb_logical_type map_type = NULL;
		tcc_ffi_type_t key_ffi_type;
		tcc_ffi_type_t value_ffi_type;
		size_t key_array_size = 0;
		size_t value_array_size = 0;
		tcc_ffi_struct_meta_t key_struct_meta;
		tcc_ffi_struct_meta_t value_struct_meta;
		tcc_ffi_map_meta_t key_map_meta;
		tcc_ffi_map_meta_t value_map_meta;
		tcc_ffi_union_meta_t key_union_meta;
		tcc_ffi_union_meta_t value_union_meta;
		if (!map_meta) {
			return NULL;
		}
		memset(&key_struct_meta, 0, sizeof(key_struct_meta));
		memset(&value_struct_meta, 0, sizeof(value_struct_meta));
		memset(&key_map_meta, 0, sizeof(key_map_meta));
		memset(&value_map_meta, 0, sizeof(value_map_meta));
		memset(&key_union_meta, 0, sizeof(key_union_meta));
		memset(&value_union_meta, 0, sizeof(value_union_meta));
		key_ffi_type = map_meta->key_type;
		value_ffi_type = map_meta->value_type;
		if (map_meta->key_token && map_meta->key_token[0] != '\0') {
			if (!tcc_parse_type_token(map_meta->key_token, false, &key_ffi_type, &key_array_size)) {
				return NULL;
			}
			if (key_ffi_type == TCC_FFI_STRUCT &&
			    !tcc_parse_struct_meta_token(map_meta->key_token, &key_struct_meta, NULL)) {
				return NULL;
			}
			if (key_ffi_type == TCC_FFI_MAP && !tcc_parse_map_meta_token(map_meta->key_token, &key_map_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				return NULL;
			}
			if (key_ffi_type == TCC_FFI_UNION &&
			    !tcc_parse_union_meta_token(map_meta->key_token, &key_union_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				return NULL;
			}
		}
		if (map_meta->value_token && map_meta->value_token[0] != '\0') {
			if (!tcc_parse_type_token(map_meta->value_token, false, &value_ffi_type, &value_array_size)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				return NULL;
			}
			if (value_ffi_type == TCC_FFI_STRUCT &&
			    !tcc_parse_struct_meta_token(map_meta->value_token, &value_struct_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				return NULL;
			}
			if (value_ffi_type == TCC_FFI_MAP &&
			    !tcc_parse_map_meta_token(map_meta->value_token, &value_map_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				tcc_struct_meta_destroy(&value_struct_meta);
				return NULL;
			}
			if (value_ffi_type == TCC_FFI_UNION &&
			    !tcc_parse_union_meta_token(map_meta->value_token, &value_union_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				tcc_struct_meta_destroy(&value_struct_meta);
				tcc_map_meta_destroy(&value_map_meta);
				return NULL;
			}
		}
		key_type = tcc_ffi_type_create_logical_type(key_ffi_type, key_array_size, &key_struct_meta, &key_map_meta,
		                                            &key_union_meta);
		value_type = tcc_ffi_type_create_logical_type(value_ffi_type, value_array_size, &value_struct_meta,
		                                              &value_map_meta, &value_union_meta);
		tcc_struct_meta_destroy(&key_struct_meta);
		tcc_struct_meta_destroy(&value_struct_meta);
		tcc_map_meta_destroy(&key_map_meta);
		tcc_map_meta_destroy(&value_map_meta);
		tcc_union_meta_destroy(&key_union_meta);
		tcc_union_meta_destroy(&value_union_meta);
		if (!key_type || !value_type) {
			if (key_type) {
				duckdb_destroy_logical_type(&key_type);
			}
			if (value_type) {
				duckdb_destroy_logical_type(&value_type);
			}
			return NULL;
		}
		map_type = duckdb_create_map_type(key_type, value_type);
		duckdb_destroy_logical_type(&key_type);
		duckdb_destroy_logical_type(&value_type);
		return map_type;
	}
	if (type == TCC_FFI_UNION) {
		duckdb_logical_type union_type = NULL;
		duckdb_logical_type *member_types = NULL;
		const char **member_names = NULL;
		int i;
		if (!union_meta || union_meta->member_count <= 0 || !union_meta->member_names || !union_meta->member_types) {
			return NULL;
		}
		member_types = (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)union_meta->member_count);
		member_names = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)union_meta->member_count);
		if (!member_types || !member_names) {
			if (member_types) {
				duckdb_free(member_types);
			}
			if (member_names) {
				duckdb_free((void *)member_names);
			}
			return NULL;
		}
		for (i = 0; i < union_meta->member_count; i++) {
			member_types[i] = NULL;
			member_names[i] = union_meta->member_names[i];
			member_types[i] = tcc_ffi_type_create_logical_type(union_meta->member_types[i], 0, NULL, NULL, NULL);
			if (!member_types[i]) {
				int j;
				for (j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&member_types[j]);
				}
				duckdb_free(member_types);
				duckdb_free((void *)member_names);
				return NULL;
			}
		}
		union_type = duckdb_create_union_type(member_types, member_names, (idx_t)union_meta->member_count);
		for (i = 0; i < union_meta->member_count; i++) {
			duckdb_destroy_logical_type(&member_types[i]);
		}
		duckdb_free(member_types);
		duckdb_free((void *)member_names);
		return union_type;
	}
	if (type == TCC_FFI_DECIMAL) {
		/* Keep a stable default until typed signatures accept precision/scale parameters. */
		return duckdb_create_decimal_type(18, 3);
	}
	base_type = tcc_ffi_type_to_duckdb_type(type);
	if (base_type == DUCKDB_TYPE_INVALID) {
		return NULL;
	}
	return duckdb_create_logical_type(base_type);
}

/* tcc_typedesc_create_logical_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static duckdb_logical_type tcc_typedesc_create_logical_type(const tcc_typedesc_t *desc) {
	duckdb_type base_type;
	if (!desc) {
		return NULL;
	}
	switch (desc->kind) {
	case TCC_TYPEDESC_LIST: {
		duckdb_logical_type child = tcc_typedesc_create_logical_type(desc->as.list_like.child);
		duckdb_logical_type out = NULL;
		if (!child) {
			return NULL;
		}
		out = duckdb_create_list_type(child);
		duckdb_destroy_logical_type(&child);
		return out;
	}
	case TCC_TYPEDESC_ARRAY: {
		duckdb_logical_type child = tcc_typedesc_create_logical_type(desc->as.list_like.child);
		duckdb_logical_type out = NULL;
		if (!child || desc->array_size == 0) {
			if (child) {
				duckdb_destroy_logical_type(&child);
			}
			return NULL;
		}
		out = duckdb_create_array_type(child, (idx_t)desc->array_size);
		duckdb_destroy_logical_type(&child);
		return out;
	}
	case TCC_TYPEDESC_STRUCT: {
		idx_t i;
		duckdb_logical_type *child_types = NULL;
		const char **child_names = NULL;
		duckdb_logical_type out = NULL;
		if (!desc->as.struct_like.fields || desc->as.struct_like.count <= 0) {
			return NULL;
		}
		child_types =
		    (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)desc->as.struct_like.count);
		child_names = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)desc->as.struct_like.count);
		if (!child_types || !child_names) {
			if (child_types) {
				duckdb_free(child_types);
			}
			if (child_names) {
				duckdb_free((void *)child_names);
			}
			return NULL;
		}
		for (i = 0; i < desc->as.struct_like.count; i++) {
			child_types[i] = tcc_typedesc_create_logical_type(desc->as.struct_like.fields[i].type);
			child_names[i] = desc->as.struct_like.fields[i].name;
			if (!child_types[i]) {
				idx_t j;
				for (j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&child_types[j]);
				}
				duckdb_free(child_types);
				duckdb_free((void *)child_names);
				return NULL;
			}
		}
		out = duckdb_create_struct_type(child_types, child_names, desc->as.struct_like.count);
		for (i = 0; i < desc->as.struct_like.count; i++) {
			duckdb_destroy_logical_type(&child_types[i]);
		}
		duckdb_free(child_types);
		duckdb_free((void *)child_names);
		return out;
	}
	case TCC_TYPEDESC_MAP: {
		duckdb_logical_type key_type = tcc_typedesc_create_logical_type(desc->as.map_like.key);
		duckdb_logical_type value_type = tcc_typedesc_create_logical_type(desc->as.map_like.value);
		duckdb_logical_type out = NULL;
		if (!key_type || !value_type) {
			if (key_type) {
				duckdb_destroy_logical_type(&key_type);
			}
			if (value_type) {
				duckdb_destroy_logical_type(&value_type);
			}
			return NULL;
		}
		out = duckdb_create_map_type(key_type, value_type);
		duckdb_destroy_logical_type(&key_type);
		duckdb_destroy_logical_type(&value_type);
		return out;
	}
	case TCC_TYPEDESC_UNION: {
		idx_t i;
		duckdb_logical_type *member_types = NULL;
		const char **member_names = NULL;
		duckdb_logical_type out = NULL;
		if (!desc->as.union_like.members || desc->as.union_like.count <= 0) {
			return NULL;
		}
		member_types =
		    (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)desc->as.union_like.count);
		member_names = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)desc->as.union_like.count);
		if (!member_types || !member_names) {
			if (member_types) {
				duckdb_free(member_types);
			}
			if (member_names) {
				duckdb_free((void *)member_names);
			}
			return NULL;
		}
		for (i = 0; i < desc->as.union_like.count; i++) {
			member_types[i] = tcc_typedesc_create_logical_type(desc->as.union_like.members[i].type);
			member_names[i] = desc->as.union_like.members[i].name;
			if (!member_types[i]) {
				idx_t j;
				for (j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&member_types[j]);
				}
				duckdb_free(member_types);
				duckdb_free((void *)member_names);
				return NULL;
			}
		}
		out = duckdb_create_union_type(member_types, member_names, desc->as.union_like.count);
		for (i = 0; i < desc->as.union_like.count; i++) {
			duckdb_destroy_logical_type(&member_types[i]);
		}
		duckdb_free(member_types);
		duckdb_free((void *)member_names);
		return out;
	}
	case TCC_TYPEDESC_PRIMITIVE:
	default:
		if (desc->ffi_type == TCC_FFI_DECIMAL) {
			return duckdb_create_decimal_type(18, 3);
		}
		base_type = tcc_ffi_type_to_duckdb_type(desc->ffi_type);
		if (base_type == DUCKDB_TYPE_INVALID) {
			return NULL;
		}
		return duckdb_create_logical_type(base_type);
	}
}

/* tcc_validity_set_all: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_validity_set_all(uint64_t *validity, idx_t count, bool valid) {
	idx_t word_count;
	idx_t rem_bits;
	if (!validity || count == 0) {
		return;
	}
	word_count = (count + 63) / 64;
	memset(validity, valid ? 0xFF : 0x00, sizeof(uint64_t) * (size_t)word_count);
	if (valid) {
		rem_bits = count % 64;
		if (rem_bits > 0) {
			validity[word_count - 1] = (1ULL << rem_bits) - 1ULL;
		}
	}
}

/* Copies DuckDB varchar payload into `duckdb_malloc` memory.
 * Caller owns and must `duckdb_free` the returned buffer.
 */
static char *tcc_copy_duckdb_string_as_cstr(duckdb_string_t *value) {
	const char *src;
	uint32_t len;
	char *copy;
	if (!value) {
		return NULL;
	}
	src = duckdb_string_t_data(value);
	len = duckdb_string_t_length(*value);
	copy = (char *)duckdb_malloc((size_t)len + 1);
	if (!copy) {
		return NULL;
	}
	if (len > 0 && src) {
		memcpy(copy, src, (size_t)len);
	}
	copy[len] = '\0';
	return copy;
}

/* Returns a borrowed view over a DuckDB varchar payload as blob bytes.
 * Lifetime is limited to the current vector/chunk scope.
 */
static ducktinycc_blob_t tcc_duckdb_string_to_blob(duckdb_string_t *value) {
	ducktinycc_blob_t out;
	out.ptr = NULL;
	out.len = 0;
	if (!value) {
		return out;
	}
	out.ptr = (const void *)duckdb_string_t_data(value);
	out.len = (uint64_t)duckdb_string_t_length(*value);
	return out;
}

/* tcc_nested_struct_bridge_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_nested_struct_bridge_destroy(tcc_nested_struct_bridge_t *bridge) {
	idx_t i;
	if (!bridge) {
		return;
	}
	if (bridge->field_bridges && bridge->field_count > 0) {
		for (i = 0; i < bridge->field_count; i++) {
			if (bridge->field_bridges[i]) {
				tcc_nested_struct_bridge_destroy(bridge->field_bridges[i]);
			}
		}
		duckdb_free((void *)bridge->field_bridges);
	}
	if (bridge->field_ptrs) {
		duckdb_free((void *)bridge->field_ptrs);
	}
	if (bridge->field_validity) {
		duckdb_free((void *)bridge->field_validity);
	}
	if (bridge->rows) {
		duckdb_free((void *)bridge->rows);
	}
	if (bridge->row_validity_mask) {
		duckdb_free((void *)bridge->row_validity_mask);
	}
	duckdb_free((void *)bridge);
}

static tcc_nested_struct_bridge_t *tcc_build_struct_bridge_from_vector(duckdb_vector struct_vector,
                                                                       const tcc_ffi_struct_meta_t *meta, idx_t n,
                                                                       const char **out_error) {
	tcc_nested_struct_bridge_t *bridge = NULL;
	ducktinycc_struct_t *rows = NULL;
	const void **field_ptrs = NULL;
	const uint64_t **field_validity = NULL;
	tcc_nested_struct_bridge_t **field_bridges = NULL;
	uint64_t *row_validity_mask = NULL;
	const uint64_t *struct_validity;
	idx_t field_idx;
	idx_t row;
	if (out_error) {
		*out_error = NULL;
	}
	if (!struct_vector || !meta || meta->field_count <= 0 || !meta->field_types || !meta->field_sizes) {
		if (out_error) {
			*out_error = "ducktinycc invalid struct metadata";
		}
		return NULL;
	}
	bridge = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
	if (!bridge) {
		if (out_error) {
			*out_error = "ducktinycc out of memory";
		}
		return NULL;
	}
	memset(bridge, 0, sizeof(*bridge));
	bridge->kind = TCC_NESTED_BRIDGE_STRUCT;
	bridge->field_count = (idx_t)meta->field_count;
	if (n > 0) {
		rows = (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)n);
	}
	field_ptrs = (const void **)duckdb_malloc(sizeof(const void *) * (size_t)meta->field_count);
	field_validity = (const uint64_t **)duckdb_malloc(sizeof(const uint64_t *) * (size_t)meta->field_count);
	field_bridges =
	    (tcc_nested_struct_bridge_t **)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t *) * (size_t)meta->field_count);
	if ((n > 0 && !rows) || !field_ptrs || !field_validity || !field_bridges) {
		if (rows) {
			duckdb_free((void *)rows);
		}
		if (field_ptrs) {
			duckdb_free((void *)field_ptrs);
		}
		if (field_validity) {
			duckdb_free((void *)field_validity);
		}
		if (field_bridges) {
			duckdb_free((void *)field_bridges);
		}
		duckdb_free((void *)bridge);
		if (out_error) {
			*out_error = "ducktinycc out of memory";
		}
		return NULL;
	}
	memset(field_ptrs, 0, sizeof(const void *) * (size_t)meta->field_count);
	memset(field_validity, 0, sizeof(const uint64_t *) * (size_t)meta->field_count);
	memset(field_bridges, 0, sizeof(tcc_nested_struct_bridge_t *) * (size_t)meta->field_count);
	for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
		duckdb_vector child_vector = duckdb_struct_vector_get_child(struct_vector, field_idx);
		const uint64_t *child_row_validity = NULL;
		if (!child_vector) {
			if (out_error) {
				*out_error = "ducktinycc struct child vector missing";
			}
			goto fail;
		}
		if (meta->field_types[field_idx] == TCC_FFI_STRUCT) {
			tcc_ffi_struct_meta_t nested_meta;
			tcc_nested_struct_bridge_t *nested = NULL;
			memset(&nested_meta, 0, sizeof(nested_meta));
			if (!meta->field_tokens || !meta->field_tokens[field_idx] ||
			    !tcc_parse_struct_meta_token(meta->field_tokens[field_idx], &nested_meta, NULL)) {
				if (out_error) {
					*out_error = "ducktinycc nested struct metadata parse failed";
				}
				goto fail;
			}
			nested = tcc_build_struct_bridge_from_vector(child_vector, &nested_meta, n, out_error);
			tcc_struct_meta_destroy(&nested_meta);
			if (!nested) {
				if (out_error && !*out_error) {
					*out_error = "ducktinycc nested struct bridge failed";
				}
				goto fail;
			}
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)nested->rows;
			child_row_validity = nested->row_validity_mask ? (const uint64_t *)nested->row_validity_mask
			                                               : (const uint64_t *)duckdb_vector_get_validity(child_vector);
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		if (tcc_ffi_type_is_list(meta->field_types[field_idx])) {
			tcc_nested_struct_bridge_t *nested = NULL;
			ducktinycc_list_t *list_rows = NULL;
			duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(child_vector);
			duckdb_vector list_child_vector = duckdb_list_vector_get_child(child_vector);
			uint8_t *list_child_data;
			const uint64_t *list_child_validity;
			tcc_ffi_type_t list_child_type = TCC_FFI_VOID;
			size_t list_child_size = 0;
			if (!list_child_vector || !tcc_ffi_list_child_type(meta->field_types[field_idx], &list_child_type)) {
				if (out_error) {
					*out_error = "ducktinycc invalid list child type";
				}
				goto fail;
			}
			list_child_size = tcc_ffi_type_size(list_child_type);
			if (list_child_size == 0) {
				if (out_error) {
					*out_error = "ducktinycc invalid list child type size";
				}
				goto fail;
			}
			nested = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
			if (!nested) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			memset(nested, 0, sizeof(*nested));
			nested->kind = TCC_NESTED_BRIDGE_LIST;
			if (n > 0) {
				list_rows = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)n);
			}
			if (n > 0 && !list_rows) {
				duckdb_free((void *)nested);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			list_child_data = (uint8_t *)duckdb_vector_get_data(list_child_vector);
			list_child_validity = (const uint64_t *)duckdb_vector_get_validity(list_child_vector);
			child_row_validity = (const uint64_t *)duckdb_vector_get_validity(child_vector);
			for (row = 0; row < n; row++) {
				if (child_row_validity && !duckdb_validity_row_is_valid((uint64_t *)child_row_validity, row)) {
					list_rows[row].ptr = NULL;
					list_rows[row].validity = NULL;
					list_rows[row].offset = 0;
					list_rows[row].len = 0;
				} else {
					duckdb_list_entry entry = entries[row];
					list_rows[row].ptr =
					    list_child_data
					        ? (const void *)(list_child_data + ((size_t)entry.offset * list_child_size))
					        : NULL;
					list_rows[row].validity = list_child_validity;
					list_rows[row].offset = (uint64_t)entry.offset;
					list_rows[row].len = (uint64_t)entry.length;
				}
			}
			nested->rows = (ducktinycc_struct_t *)list_rows;
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)list_rows;
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		if (tcc_ffi_type_is_array(meta->field_types[field_idx])) {
			tcc_nested_struct_bridge_t *nested = NULL;
			ducktinycc_array_t *array_rows = NULL;
			duckdb_vector array_child_vector = duckdb_array_vector_get_child(child_vector);
			uint8_t *array_child_data;
			const uint64_t *array_child_validity;
			tcc_ffi_type_t array_child_type = TCC_FFI_VOID;
			size_t array_child_size = 0;
			size_t array_len = 0;
			tcc_ffi_type_t parsed_type = TCC_FFI_VOID;
			if (!meta->field_tokens || !meta->field_tokens[field_idx] ||
			    !tcc_parse_type_token(meta->field_tokens[field_idx], false, &parsed_type, &array_len) || array_len == 0 ||
			    !tcc_ffi_array_child_type(meta->field_types[field_idx], &array_child_type) || !array_child_vector) {
				if (out_error) {
					*out_error = "ducktinycc invalid array child type";
				}
				goto fail;
			}
			array_child_size = tcc_ffi_type_size(array_child_type);
			if (array_child_size == 0) {
				if (out_error) {
					*out_error = "ducktinycc invalid array child type size";
				}
				goto fail;
			}
			nested = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
			if (!nested) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			memset(nested, 0, sizeof(*nested));
			nested->kind = TCC_NESTED_BRIDGE_ARRAY;
			if (n > 0) {
				array_rows = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)n);
			}
			if (n > 0 && !array_rows) {
				duckdb_free((void *)nested);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			array_child_data = (uint8_t *)duckdb_vector_get_data(array_child_vector);
			array_child_validity = (const uint64_t *)duckdb_vector_get_validity(array_child_vector);
			child_row_validity = (const uint64_t *)duckdb_vector_get_validity(child_vector);
			for (row = 0; row < n; row++) {
				if (child_row_validity && !duckdb_validity_row_is_valid((uint64_t *)child_row_validity, row)) {
					array_rows[row].ptr = NULL;
					array_rows[row].validity = NULL;
					array_rows[row].offset = 0;
					array_rows[row].len = 0;
				} else {
					uint64_t off = (uint64_t)row * (uint64_t)array_len;
					array_rows[row].ptr =
					    array_child_data
					        ? (const void *)(array_child_data + ((size_t)off * array_child_size))
					        : NULL;
					array_rows[row].validity = array_child_validity;
					array_rows[row].offset = off;
					array_rows[row].len = (uint64_t)array_len;
				}
			}
			nested->rows = (ducktinycc_struct_t *)array_rows;
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)array_rows;
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		if (tcc_ffi_type_is_map(meta->field_types[field_idx])) {
			tcc_nested_struct_bridge_t *nested = NULL;
			ducktinycc_map_t *map_rows = NULL;
			tcc_ffi_map_meta_t map_meta;
			duckdb_list_entry *entries;
			duckdb_vector map_struct_vector;
			duckdb_vector map_key_vector;
			duckdb_vector map_value_vector;
			uint8_t *key_data;
			uint8_t *value_data;
			const uint64_t *key_validity;
			const uint64_t *value_validity;
			memset(&map_meta, 0, sizeof(map_meta));
			if (!meta->field_tokens || !meta->field_tokens[field_idx] ||
			    !tcc_parse_map_meta_token(meta->field_tokens[field_idx], &map_meta, NULL)) {
				if (out_error) {
					*out_error = "ducktinycc map metadata parse failed";
				}
				goto fail;
			}
			if (map_meta.key_size == 0 || map_meta.value_size == 0) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc invalid map metadata";
				}
				goto fail;
			}
			entries = (duckdb_list_entry *)duckdb_vector_get_data(child_vector);
			map_struct_vector = duckdb_list_vector_get_child(child_vector);
			if (!map_struct_vector) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc map child vector missing";
				}
				goto fail;
			}
			map_key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
			map_value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
			if (!map_key_vector || !map_value_vector) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc map key/value child vector missing";
				}
				goto fail;
			}
			nested = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
			if (!nested) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			memset(nested, 0, sizeof(*nested));
			nested->kind = TCC_NESTED_BRIDGE_MAP;
			if (n > 0) {
				map_rows = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)n);
			}
			if (n > 0 && !map_rows) {
				tcc_map_meta_destroy(&map_meta);
				duckdb_free((void *)nested);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			key_data = (uint8_t *)duckdb_vector_get_data(map_key_vector);
			value_data = (uint8_t *)duckdb_vector_get_data(map_value_vector);
			key_validity = (const uint64_t *)duckdb_vector_get_validity(map_key_vector);
			value_validity = (const uint64_t *)duckdb_vector_get_validity(map_value_vector);
			child_row_validity = (const uint64_t *)duckdb_vector_get_validity(child_vector);
			for (row = 0; row < n; row++) {
				if (child_row_validity && !duckdb_validity_row_is_valid((uint64_t *)child_row_validity, row)) {
					map_rows[row].key_ptr = NULL;
					map_rows[row].key_validity = NULL;
					map_rows[row].value_ptr = NULL;
					map_rows[row].value_validity = NULL;
					map_rows[row].offset = 0;
					map_rows[row].len = 0;
				} else {
					duckdb_list_entry entry = entries[row];
					map_rows[row].key_ptr =
					    key_data ? (const void *)(key_data + ((size_t)entry.offset * map_meta.key_size)) : NULL;
					map_rows[row].key_validity = key_validity;
					map_rows[row].value_ptr =
					    value_data ? (const void *)(value_data + ((size_t)entry.offset * map_meta.value_size)) : NULL;
					map_rows[row].value_validity = value_validity;
					map_rows[row].offset = (uint64_t)entry.offset;
					map_rows[row].len = (uint64_t)entry.length;
				}
			}
			tcc_map_meta_destroy(&map_meta);
			nested->rows = (ducktinycc_struct_t *)map_rows;
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)map_rows;
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		field_ptrs[field_idx] = (const void *)duckdb_vector_get_data(child_vector);
		field_validity[field_idx] = (const uint64_t *)duckdb_vector_get_validity(child_vector);
	}
	struct_validity = (const uint64_t *)duckdb_vector_get_validity(struct_vector);
	if (!struct_validity && n > 0) {
		idx_t words = (n + 63) / 64;
		row_validity_mask = (uint64_t *)duckdb_malloc(sizeof(uint64_t) * (size_t)words);
		if (!row_validity_mask) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			goto fail;
		}
		tcc_validity_set_all(row_validity_mask, n, true);
		for (row = 0; row < n; row++) {
			bool any_valid = false;
			for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
				if (!field_validity[field_idx] ||
				    duckdb_validity_row_is_valid((uint64_t *)field_validity[field_idx], row)) {
					any_valid = true;
					break;
				}
			}
			if (!any_valid) {
				duckdb_validity_set_row_validity(row_validity_mask, row, false);
			}
		}
		struct_validity = (const uint64_t *)row_validity_mask;
	}
	for (row = 0; row < n; row++) {
		if (struct_validity && !duckdb_validity_row_is_valid((uint64_t *)struct_validity, row)) {
			rows[row].field_ptrs = NULL;
			rows[row].field_validity = NULL;
			rows[row].field_count = 0;
			rows[row].offset = 0;
		} else {
			rows[row].field_ptrs = field_ptrs;
			rows[row].field_validity = field_validity;
			rows[row].field_count = (uint64_t)meta->field_count;
			rows[row].offset = (uint64_t)row;
		}
	}
	bridge->rows = rows;
	bridge->field_ptrs = field_ptrs;
	bridge->field_validity = field_validity;
	bridge->field_bridges = field_bridges;
	bridge->row_validity_mask = row_validity_mask;
	return bridge;
fail:
	if (row_validity_mask) {
		duckdb_free((void *)row_validity_mask);
	}
	if (field_bridges) {
		for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
			if (field_bridges[field_idx]) {
				tcc_nested_struct_bridge_destroy(field_bridges[field_idx]);
			}
		}
		duckdb_free((void *)field_bridges);
	}
	if (field_ptrs) {
		duckdb_free((void *)field_ptrs);
	}
	if (field_validity) {
		duckdb_free((void *)field_validity);
	}
	if (rows) {
		duckdb_free((void *)rows);
	}
	if (bridge) {
		duckdb_free((void *)bridge);
	}
	return NULL;
}

/* tcc_typedesc_is_composite: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_typedesc_is_composite(const tcc_typedesc_t *desc) {
	if (!desc) {
		return false;
	}
	return tcc_ffi_type_is_list(desc->ffi_type) || tcc_ffi_type_is_array(desc->ffi_type) ||
	       tcc_ffi_type_is_struct(desc->ffi_type) || tcc_ffi_type_is_map(desc->ffi_type) ||
	       tcc_ffi_type_is_union(desc->ffi_type);
}

/* tcc_value_bridge_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_value_bridge_destroy(tcc_value_bridge_t *bridge) {
	idx_t i;
	if (!bridge) {
		return;
	}
	if (bridge->children) {
		for (i = 0; i < bridge->child_count; i++) {
			if (bridge->children[i]) {
				tcc_value_bridge_destroy(bridge->children[i]);
			}
		}
		duckdb_free(bridge->children);
	}
	if (bridge->child_ptrs) {
		duckdb_free((void *)bridge->child_ptrs);
	}
	if (bridge->child_validity_ptrs) {
		duckdb_free((void *)bridge->child_validity_ptrs);
	}
	if (bridge->owned_validity) {
		duckdb_free(bridge->owned_validity);
	}
	if (bridge->owns_rows && bridge->rows) {
		duckdb_free(bridge->rows);
	}
	duckdb_free(bridge);
}

static tcc_value_bridge_t *tcc_build_value_bridge(duckdb_vector vector, const tcc_typedesc_t *desc, idx_t count,
                                                  const char **out_error) {
	tcc_value_bridge_t *bridge = NULL;
	idx_t row;
	if (out_error) {
		*out_error = NULL;
	}
	if (!vector || !desc) {
		if (out_error) {
			*out_error = "ducktinycc invalid bridge input";
		}
		return NULL;
	}
	bridge = (tcc_value_bridge_t *)duckdb_malloc(sizeof(tcc_value_bridge_t));
	if (!bridge) {
		if (out_error) {
			*out_error = "ducktinycc out of memory";
		}
		return NULL;
	}
	memset(bridge, 0, sizeof(*bridge));
	bridge->desc = desc;
	bridge->count = count;
	bridge->elem_size = tcc_ffi_type_size(desc->ffi_type);
	bridge->rows = duckdb_vector_get_data(vector);
	bridge->validity = (const uint64_t *)duckdb_vector_get_validity(vector);
	if (bridge->elem_size == 0 && desc->ffi_type != TCC_FFI_VOID) {
		if (out_error) {
			*out_error = "ducktinycc unsupported bridge type";
		}
		goto fail;
	}
	if (tcc_ffi_type_is_list(desc->ffi_type)) {
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector child_vector = duckdb_list_vector_get_child(vector);
		tcc_value_bridge_t *child_bridge = NULL;
		ducktinycc_list_t *rows = NULL;
		idx_t child_count = duckdb_list_vector_get_size(vector);
		if (!entries || !child_vector || !desc->as.list_like.child) {
			if (out_error) {
				*out_error = "ducktinycc invalid list bridge shape";
			}
			goto fail;
		}
		child_bridge = tcc_build_value_bridge(child_vector, desc->as.list_like.child, child_count, out_error);
		if (!child_bridge) {
			goto fail;
		}
		if (count > 0) {
			rows = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				tcc_value_bridge_destroy(child_bridge);
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			duckdb_list_entry entry = entries[row];
			rows[row].ptr = child_bridge->rows
			                    ? (const void *)((const uint8_t *)child_bridge->rows +
			                                     ((size_t)entry.offset * child_bridge->elem_size))
			                    : NULL;
			rows[row].validity = child_bridge->validity;
			rows[row].offset = (uint64_t)entry.offset;
			rows[row].len = (uint64_t)entry.length;
		}
		bridge->children = (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *));
		if (!bridge->children) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			if (rows) {
				duckdb_free(rows);
			}
			tcc_value_bridge_destroy(child_bridge);
			goto fail;
		}
		bridge->children[0] = child_bridge;
		bridge->child_count = 1;
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->elem_size = sizeof(ducktinycc_list_t);
		return bridge;
	}
	if (tcc_ffi_type_is_array(desc->ffi_type)) {
		duckdb_vector child_vector = duckdb_array_vector_get_child(vector);
		tcc_value_bridge_t *child_bridge = NULL;
		ducktinycc_array_t *rows = NULL;
		size_t array_len = desc->array_size;
		uint64_t child_count_u64;
		if (!child_vector || !desc->as.list_like.child || array_len == 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid array bridge shape";
			}
			goto fail;
		}
		child_count_u64 = (uint64_t)count * (uint64_t)array_len;
		if (array_len > 0 && count > (idx_t)(UINT64_MAX / array_len)) {
			if (out_error) {
				*out_error = "ducktinycc array bridge overflow";
			}
			goto fail;
		}
		child_bridge = tcc_build_value_bridge(child_vector, desc->as.list_like.child, (idx_t)child_count_u64, out_error);
		if (!child_bridge) {
			goto fail;
		}
		if (count > 0) {
			rows = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				tcc_value_bridge_destroy(child_bridge);
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			uint64_t off = (uint64_t)row * (uint64_t)array_len;
			rows[row].ptr = child_bridge->rows
			                    ? (const void *)((const uint8_t *)child_bridge->rows +
			                                     ((size_t)off * child_bridge->elem_size))
			                    : NULL;
			rows[row].validity = child_bridge->validity;
			rows[row].offset = off;
			rows[row].len = (uint64_t)array_len;
		}
		bridge->children = (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *));
		if (!bridge->children) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			if (rows) {
				duckdb_free(rows);
			}
			tcc_value_bridge_destroy(child_bridge);
			goto fail;
		}
		bridge->children[0] = child_bridge;
		bridge->child_count = 1;
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->elem_size = sizeof(ducktinycc_array_t);
		return bridge;
	}
	if (tcc_ffi_type_is_struct(desc->ffi_type)) {
		idx_t i;
		ducktinycc_struct_t *rows = NULL;
		idx_t field_count = desc->as.struct_like.count;
		if (!desc->as.struct_like.fields || field_count <= 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid struct bridge metadata";
			}
			goto fail;
		}
		bridge->children =
		    (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * (size_t)field_count);
		bridge->child_ptrs = (const void **)duckdb_malloc(sizeof(const void *) * (size_t)field_count);
		bridge->child_validity_ptrs = (const uint64_t **)duckdb_malloc(sizeof(const uint64_t *) * (size_t)field_count);
		if (!bridge->children || !bridge->child_ptrs || !bridge->child_validity_ptrs) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			goto fail;
		}
		memset(bridge->children, 0, sizeof(tcc_value_bridge_t *) * (size_t)field_count);
		for (i = 0; i < field_count; i++) {
			duckdb_vector child_vector = duckdb_struct_vector_get_child(vector, i);
			tcc_value_bridge_t *child_bridge;
			if (!child_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing struct child vector";
				}
				goto fail;
			}
			child_bridge = tcc_build_value_bridge(child_vector, desc->as.struct_like.fields[i].type, count, out_error);
			if (!child_bridge) {
				goto fail;
			}
			bridge->children[i] = child_bridge;
			bridge->child_ptrs[i] = child_bridge->rows;
			bridge->child_validity_ptrs[i] = child_bridge->validity;
		}
		if (count > 0) {
			rows = (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			rows[row].field_ptrs = bridge->child_ptrs;
			rows[row].field_validity = bridge->child_validity_ptrs;
			rows[row].field_count = (uint64_t)field_count;
			rows[row].offset = (uint64_t)row;
		}
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->child_count = field_count;
		bridge->elem_size = sizeof(ducktinycc_struct_t);
		return bridge;
	}
	if (tcc_ffi_type_is_map(desc->ffi_type)) {
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector map_struct_vector = duckdb_list_vector_get_child(vector);
		duckdb_vector key_vector;
		duckdb_vector value_vector;
		tcc_value_bridge_t *key_bridge = NULL;
		tcc_value_bridge_t *value_bridge = NULL;
		ducktinycc_map_t *rows = NULL;
		idx_t child_count = duckdb_list_vector_get_size(vector);
		if (!entries || !map_struct_vector || !desc->as.map_like.key || !desc->as.map_like.value) {
			if (out_error) {
				*out_error = "ducktinycc invalid map bridge shape";
			}
			goto fail;
		}
		key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
		value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
		if (!key_vector || !value_vector) {
			if (out_error) {
				*out_error = "ducktinycc invalid map key/value vector";
			}
			goto fail;
		}
		key_bridge = tcc_build_value_bridge(key_vector, desc->as.map_like.key, child_count, out_error);
		if (!key_bridge) {
			goto fail;
		}
		value_bridge = tcc_build_value_bridge(value_vector, desc->as.map_like.value, child_count, out_error);
		if (!value_bridge) {
			tcc_value_bridge_destroy(key_bridge);
			goto fail;
		}
		if (count > 0) {
			rows = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				tcc_value_bridge_destroy(key_bridge);
				tcc_value_bridge_destroy(value_bridge);
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			duckdb_list_entry entry = entries[row];
			rows[row].key_ptr = key_bridge->rows
			                        ? (const void *)((const uint8_t *)key_bridge->rows +
			                                         ((size_t)entry.offset * key_bridge->elem_size))
			                        : NULL;
			rows[row].key_validity = key_bridge->validity;
			rows[row].value_ptr = value_bridge->rows
			                          ? (const void *)((const uint8_t *)value_bridge->rows +
			                                           ((size_t)entry.offset * value_bridge->elem_size))
			                          : NULL;
			rows[row].value_validity = value_bridge->validity;
			rows[row].offset = (uint64_t)entry.offset;
			rows[row].len = (uint64_t)entry.length;
		}
		bridge->children = (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * 2);
		if (!bridge->children) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			if (rows) {
				duckdb_free(rows);
			}
			tcc_value_bridge_destroy(key_bridge);
			tcc_value_bridge_destroy(value_bridge);
			goto fail;
		}
		bridge->children[0] = key_bridge;
		bridge->children[1] = value_bridge;
		bridge->child_count = 2;
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->elem_size = sizeof(ducktinycc_map_t);
		return bridge;
	}
	if (tcc_ffi_type_is_union(desc->ffi_type)) {
		idx_t i;
		uint8_t *tags = (uint8_t *)duckdb_vector_get_data(vector);
		ducktinycc_union_t *rows = NULL;
		idx_t member_count = desc->as.union_like.count;
		if (!tags || !desc->as.union_like.members || member_count <= 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid union bridge shape";
			}
			goto fail;
		}
		bridge->children =
		    (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * (size_t)member_count);
		bridge->child_ptrs = (const void **)duckdb_malloc(sizeof(const void *) * (size_t)member_count);
		bridge->child_validity_ptrs =
		    (const uint64_t **)duckdb_malloc(sizeof(const uint64_t *) * (size_t)member_count);
		if (!bridge->children || !bridge->child_ptrs || !bridge->child_validity_ptrs) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			goto fail;
		}
		memset(bridge->children, 0, sizeof(tcc_value_bridge_t *) * (size_t)member_count);
		for (i = 0; i < member_count; i++) {
			duckdb_vector member_vector = duckdb_struct_vector_get_child(vector, i);
			tcc_value_bridge_t *member_bridge;
			if (!member_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing union member vector";
				}
				goto fail;
			}
			member_bridge =
			    tcc_build_value_bridge(member_vector, desc->as.union_like.members[i].type, count, out_error);
			if (!member_bridge) {
				goto fail;
			}
			bridge->children[i] = member_bridge;
			bridge->child_ptrs[i] = member_bridge->rows;
			bridge->child_validity_ptrs[i] = member_bridge->validity;
		}
		if (!bridge->validity && count > 0) {
			idx_t words = (count + 63) / 64;
			uint64_t *mask = (uint64_t *)duckdb_malloc(sizeof(uint64_t) * (size_t)words);
			if (!mask) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			tcc_validity_set_all(mask, count, true);
			for (row = 0; row < count; row++) {
				uint8_t tag = tags[row];
				bool row_valid = true;
				if ((idx_t)tag >= member_count) {
					row_valid = false;
				} else if (bridge->child_validity_ptrs[tag] &&
				           !duckdb_validity_row_is_valid((uint64_t *)bridge->child_validity_ptrs[tag], row)) {
					row_valid = false;
				}
				duckdb_validity_set_row_validity(mask, row, row_valid);
			}
			bridge->owned_validity = mask;
			bridge->validity = mask;
		}
		if (count > 0) {
			rows = (ducktinycc_union_t *)duckdb_malloc(sizeof(ducktinycc_union_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			rows[row].tag_ptr = tags;
			rows[row].member_ptrs = bridge->child_ptrs;
			rows[row].member_validity = bridge->child_validity_ptrs;
			rows[row].member_count = (uint64_t)member_count;
			rows[row].offset = (uint64_t)row;
		}
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->child_count = member_count;
		bridge->elem_size = sizeof(ducktinycc_union_t);
		return bridge;
	}
	return bridge;
fail:
	tcc_value_bridge_destroy(bridge);
	return NULL;
}

/* tcc_set_vector_row_validity: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_set_vector_row_validity(duckdb_vector vector, idx_t row, bool valid) {
	uint64_t *validity;
	if (!vector) {
		return false;
	}
	duckdb_vector_ensure_validity_writable(vector);
	validity = duckdb_vector_get_validity(vector);
	if (!validity) {
		return false;
	}
	duckdb_validity_set_row_validity(validity, row, valid);
	return true;
}

/* tcc_write_value_to_vector: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_write_value_to_vector(duckdb_vector vector, const tcc_typedesc_t *desc, idx_t row,
                                      const void *src_base, uint64_t src_offset, const uint64_t *src_validity,
                                      const char **out_error) {
	const uint8_t *src_ptr = NULL;
	size_t src_size;
	bool row_valid = true;
	if (!vector || !desc) {
		if (out_error) {
			*out_error = "ducktinycc invalid return bridge arguments";
		}
		return false;
	}
	if (src_validity) {
		row_valid = (src_validity[src_offset >> 6] & (1ULL << (src_offset & 63))) != 0;
	}
	if (!tcc_set_vector_row_validity(vector, row, row_valid)) {
		if (out_error) {
			*out_error = "ducktinycc failed to set output validity";
		}
		return false;
	}
	if (!row_valid) {
		return true;
	}
	src_size = tcc_ffi_type_size(desc->ffi_type);
	if (src_size == 0 && desc->ffi_type != TCC_FFI_VOID) {
		if (out_error) {
			*out_error = "ducktinycc unsupported output type size";
		}
		return false;
	}
	if (src_size > 0) {
		if (!src_base) {
			if (!tcc_set_vector_row_validity(vector, row, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set output validity";
				}
				return false;
			}
			return true;
		}
		src_ptr = (const uint8_t *)src_base + ((size_t)src_offset * src_size);
	}
	if (desc->ffi_type == TCC_FFI_VARCHAR) {
		const duckdb_string_t *str = (const duckdb_string_t *)src_ptr;
		const char *s = duckdb_string_t_data((duckdb_string_t *)str);
		uint32_t len = duckdb_string_t_length(*str);
		duckdb_vector_assign_string_element_len(vector, row, s ? s : "", (idx_t)len);
		return true;
	}
	if (desc->ffi_type == TCC_FFI_BLOB) {
		const ducktinycc_blob_t *blob = (const ducktinycc_blob_t *)src_ptr;
		if (!blob->ptr && blob->len > 0) {
			if (!tcc_set_vector_row_validity(vector, row, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set output validity";
				}
				return false;
			}
			return true;
		}
		duckdb_vector_assign_string_element_len(vector, row, (const char *)blob->ptr, (idx_t)blob->len);
		return true;
	}
	if (tcc_ffi_type_is_list(desc->ffi_type)) {
		const ducktinycc_list_t *list = (const ducktinycc_list_t *)src_ptr;
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector child_vector = duckdb_list_vector_get_child(vector);
		idx_t child_offset = duckdb_list_vector_get_size(vector);
		idx_t i;
		if (!entries || !child_vector || !desc->as.list_like.child) {
			if (out_error) {
				*out_error = "ducktinycc invalid list return bridge";
			}
			return false;
		}
		if (list->len > 0 && !list->ptr) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		if (duckdb_list_vector_reserve(vector, child_offset + (idx_t)list->len) != DuckDBSuccess ||
		    duckdb_list_vector_set_size(vector, child_offset + (idx_t)list->len) != DuckDBSuccess) {
			if (out_error) {
				*out_error = "ducktinycc list return reserve/set_size failed";
			}
			return false;
		}
		for (i = 0; i < (idx_t)list->len; i++) {
			uint64_t src_idx = list->offset + (uint64_t)i;
			bool child_valid = true;
			if (list->validity) {
				child_valid = (list->validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (!tcc_write_value_to_vector(child_vector, desc->as.list_like.child, child_offset + i, list->ptr,
			                               (uint64_t)i, NULL, out_error)) {
				return false;
			}
			if (!tcc_set_vector_row_validity(child_vector, child_offset + i, child_valid)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set list child validity";
				}
				return false;
			}
		}
		entries[row].offset = child_offset;
		entries[row].length = (idx_t)list->len;
		return true;
	}
	if (tcc_ffi_type_is_array(desc->ffi_type)) {
		const ducktinycc_array_t *arr = (const ducktinycc_array_t *)src_ptr;
		duckdb_vector child_vector = duckdb_array_vector_get_child(vector);
		size_t array_len = desc->array_size;
		idx_t i;
		if (!child_vector || !desc->as.list_like.child || array_len == 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid array return bridge";
			}
			return false;
		}
		if ((size_t)arr->len != array_len || (arr->len > 0 && !arr->ptr)) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		for (i = 0; i < (idx_t)array_len; i++) {
			uint64_t src_idx = arr->offset + (uint64_t)i;
			bool child_valid = true;
			if (arr->validity) {
				child_valid = (arr->validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (!tcc_write_value_to_vector(child_vector, desc->as.list_like.child, (idx_t)((size_t)row * array_len) + i,
			                               arr->ptr, (uint64_t)i, NULL, out_error)) {
				return false;
			}
			if (!tcc_set_vector_row_validity(child_vector, (idx_t)((size_t)row * array_len) + i, child_valid)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set array child validity";
				}
				return false;
			}
		}
		return true;
	}
	if (tcc_ffi_type_is_struct(desc->ffi_type)) {
		const ducktinycc_struct_t *st = (const ducktinycc_struct_t *)src_ptr;
		idx_t field_idx;
		if (!st->field_ptrs || !desc->as.struct_like.fields || st->field_count != (uint64_t)desc->as.struct_like.count) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		for (field_idx = 0; field_idx < desc->as.struct_like.count; field_idx++) {
			duckdb_vector field_vector = duckdb_struct_vector_get_child(vector, field_idx);
			const uint64_t *field_validity =
			    (st->field_validity && st->field_validity[field_idx]) ? st->field_validity[field_idx] : NULL;
			if (!field_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing struct output child vector";
				}
				return false;
			}
			if (!tcc_write_value_to_vector(field_vector, desc->as.struct_like.fields[field_idx].type, row,
			                               st->field_ptrs[field_idx], st->offset, field_validity, out_error)) {
				return false;
			}
		}
		return true;
	}
	if (tcc_ffi_type_is_map(desc->ffi_type)) {
		const ducktinycc_map_t *m = (const ducktinycc_map_t *)src_ptr;
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector map_struct_vector = duckdb_list_vector_get_child(vector);
		duckdb_vector key_vector;
		duckdb_vector value_vector;
		idx_t child_offset;
		idx_t i;
		if (!entries || !map_struct_vector || !desc->as.map_like.key || !desc->as.map_like.value) {
			if (out_error) {
				*out_error = "ducktinycc invalid map return bridge";
			}
			return false;
		}
		key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
		value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
		if (!key_vector || !value_vector) {
			if (out_error) {
				*out_error = "ducktinycc invalid map output key/value vector";
			}
			return false;
		}
		if (m->len > 0 && (!m->key_ptr || !m->value_ptr)) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		child_offset = duckdb_list_vector_get_size(vector);
		if (duckdb_list_vector_reserve(vector, child_offset + (idx_t)m->len) != DuckDBSuccess ||
		    duckdb_list_vector_set_size(vector, child_offset + (idx_t)m->len) != DuckDBSuccess) {
			if (out_error) {
				*out_error = "ducktinycc map return reserve/set_size failed";
			}
			return false;
		}
		for (i = 0; i < (idx_t)m->len; i++) {
			uint64_t src_idx = m->offset + (uint64_t)i;
			bool key_valid = true;
			bool value_valid = true;
			if (m->key_validity) {
				key_valid = (m->key_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (m->value_validity) {
				value_valid = (m->value_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (!tcc_write_value_to_vector(key_vector, desc->as.map_like.key, child_offset + i, m->key_ptr, (uint64_t)i, NULL,
			                               out_error) ||
			    !tcc_write_value_to_vector(value_vector, desc->as.map_like.value, child_offset + i, m->value_ptr,
			                               (uint64_t)i, NULL, out_error)) {
				return false;
			}
			if (!tcc_set_vector_row_validity(key_vector, child_offset + i, key_valid) ||
			    !tcc_set_vector_row_validity(value_vector, child_offset + i, value_valid)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set map child validity";
				}
				return false;
			}
		}
		entries[row].offset = child_offset;
		entries[row].length = (idx_t)m->len;
		return true;
	}
	if (tcc_ffi_type_is_union(desc->ffi_type)) {
		const ducktinycc_union_t *u = (const ducktinycc_union_t *)src_ptr;
		uint8_t *tags = (uint8_t *)duckdb_vector_get_data(vector);
		idx_t member_count = desc->as.union_like.count;
		idx_t member_idx;
		uint8_t tag;
		if (!tags || !u->tag_ptr || !u->member_ptrs || !desc->as.union_like.members ||
		    u->member_count != (uint64_t)member_count) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		tag = u->tag_ptr[u->offset];
		if ((idx_t)tag >= member_count) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		tags[row] = tag;
		for (member_idx = 0; member_idx < member_count; member_idx++) {
			duckdb_vector member_vector = duckdb_struct_vector_get_child(vector, member_idx);
			if (!member_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing union output member vector";
				}
				return false;
			}
			if (member_idx == (idx_t)tag) {
				const uint64_t *member_validity =
				    (u->member_validity && u->member_validity[member_idx]) ? u->member_validity[member_idx] : NULL;
				if (!tcc_write_value_to_vector(member_vector, desc->as.union_like.members[member_idx].type, row,
				                               u->member_ptrs[member_idx], u->offset, member_validity, out_error)) {
					return false;
				}
			} else if (!tcc_set_vector_row_validity(member_vector, row, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set union member validity";
				}
				return false;
			}
		}
		return true;
	}
	if (desc->ffi_type != TCC_FFI_VOID && src_size > 0) {
		uint8_t *dst = (uint8_t *)duckdb_vector_get_data(vector);
		if (!dst || !src_ptr) {
			if (out_error) {
				*out_error = "ducktinycc output copy failed";
			}
			return false;
		}
		memcpy(dst + ((size_t)row * src_size), src_ptr, src_size);
	}
	return true;
}

/* tcc_execute_compiled_scalar_udf: Runtime bridge executing compiled row/batch wrappers. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_execute_compiled_scalar_udf(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_host_sig_ctx_t *ctx = (tcc_host_sig_ctx_t *)duckdb_scalar_function_get_extra_info(info);
	idx_t n = duckdb_data_chunk_get_size(input);
	uint8_t *out_data = (uint8_t *)duckdb_vector_get_data(output);
	uint64_t *out_validity;
	uint8_t **in_data = NULL;
	uint64_t **in_validity = NULL;
	uint64_t **synthetic_validity_columns = NULL;
	size_t ret_size;
	void **arg_ptrs = NULL;
	void **batch_arg_data = NULL;
	const char **row_varchar_values = NULL;
	char **row_varchar_allocations = NULL;
	idx_t row_varchar_alloc_count = 0;
	idx_t row_varchar_alloc_capacity = 0;
	ducktinycc_blob_t *row_blob_values = NULL;
	ducktinycc_list_t *row_list_values = NULL;
	ducktinycc_array_t *row_array_values = NULL;
	ducktinycc_struct_t *row_struct_values = NULL;
	ducktinycc_map_t *row_map_values = NULL;
	ducktinycc_union_t *row_union_values = NULL;
	ducktinycc_blob_t **batch_blob_columns = NULL;
	ducktinycc_list_t **batch_list_columns = NULL;
	ducktinycc_array_t **batch_array_columns = NULL;
	ducktinycc_struct_t **batch_struct_columns = NULL;
	ducktinycc_map_t **batch_map_columns = NULL;
	ducktinycc_union_t **batch_union_columns = NULL;
	const char ***batch_varchar_columns = NULL;
	char ***batch_varchar_owned = NULL;
	duckdb_list_entry **list_entries_columns = NULL;
	uint8_t **list_child_data_columns = NULL;
	uint64_t **list_child_validity_columns = NULL;
	size_t *list_child_sizes = NULL;
	uint8_t **array_child_data_columns = NULL;
	uint64_t **array_child_validity_columns = NULL;
	size_t *array_child_sizes = NULL;
	tcc_value_bridge_t **arg_value_bridges = NULL;
	uint8_t ***struct_child_data_columns = NULL;
	uint64_t ***struct_child_validity_columns = NULL;
	size_t **struct_child_sizes = NULL;
	idx_t *struct_field_counts = NULL;
	tcc_nested_struct_bridge_t ***arg_struct_nested_bridges = NULL;
	duckdb_list_entry **map_entries_columns = NULL;
	uint8_t **map_key_data_columns = NULL;
	uint64_t **map_key_validity_columns = NULL;
	size_t *map_key_sizes = NULL;
	uint8_t **map_value_data_columns = NULL;
	uint64_t **map_value_validity_columns = NULL;
	size_t *map_value_sizes = NULL;
	uint8_t **union_tag_columns = NULL;
	uint8_t ***union_member_data_columns = NULL;
	uint64_t ***union_member_validity_columns = NULL;
	size_t **union_member_sizes = NULL;
	idx_t *union_member_counts = NULL;
	const char **batch_out_varchar = NULL;
	ducktinycc_blob_t *batch_out_blob = NULL;
	ducktinycc_list_t *batch_out_list = NULL;
	ducktinycc_array_t *batch_out_array = NULL;
	ducktinycc_struct_t *batch_out_struct = NULL;
	ducktinycc_map_t *batch_out_map = NULL;
	ducktinycc_union_t *batch_out_union = NULL;
	duckdb_list_entry *out_list_entries = NULL;
	duckdb_vector out_list_child_vector = NULL;
	size_t out_list_child_size = 0;
	idx_t out_list_child_count = 0;
	duckdb_vector out_array_child_vector = NULL;
	size_t out_array_child_size = 0;
	size_t out_array_len = 0;
	uint8_t **out_struct_child_data = NULL;
	uint64_t **out_struct_child_validity = NULL;
	size_t *out_struct_child_sizes = NULL;
	idx_t out_struct_field_count = 0;
	duckdb_list_entry *out_map_entries = NULL;
	duckdb_vector out_map_struct_vector = NULL;
	duckdb_vector out_map_key_vector = NULL;
	duckdb_vector out_map_value_vector = NULL;
	size_t out_map_key_size = 0;
	size_t out_map_value_size = 0;
	idx_t out_map_child_count = 0;
	uint8_t *out_union_tags = NULL;
	uint8_t **out_union_member_data = NULL;
	uint64_t **out_union_member_validity = NULL;
	size_t *out_union_member_sizes = NULL;
	idx_t out_union_member_count = 0;
	uint8_t out_value[64];
	const char *out_varchar_value = NULL;
	ducktinycc_blob_t out_blob_value;
	ducktinycc_list_t out_list_value;
	ducktinycc_array_t out_array_value;
	ducktinycc_struct_t out_struct_value;
	ducktinycc_map_t out_map_value;
	ducktinycc_union_t out_union_value;
	void *batch_out_ptr = NULL;
	idx_t row;
	int col;
	const char *error = NULL;
	const tcc_typedesc_t *return_desc = NULL;
	if (!ctx || ctx->arg_count < 0) {
		duckdb_scalar_function_set_error(info, "ducktinycc signature ctx missing");
		return;
	}
	if (ctx->wrapper_mode == TCC_WRAPPER_MODE_ROW && !ctx->row_wrapper) {
		duckdb_scalar_function_set_error(info, "ducktinycc row wrapper missing");
		return;
	}
	if (ctx->wrapper_mode == TCC_WRAPPER_MODE_BATCH && !ctx->batch_wrapper) {
		duckdb_scalar_function_set_error(info, "ducktinycc batch wrapper missing");
		return;
	}
	if (ctx->wrapper_mode != TCC_WRAPPER_MODE_ROW && ctx->wrapper_mode != TCC_WRAPPER_MODE_BATCH) {
		duckdb_scalar_function_set_error(info, "ducktinycc signature ctx missing");
		return;
	}
	return_desc = ctx->return_desc;
	if (!return_desc) {
		duckdb_scalar_function_set_error(info, "ducktinycc typed signature is missing");
		return;
	}
	if ((size_t)ctx->arg_count > (SIZE_MAX / sizeof(uint8_t *))) {
		duckdb_scalar_function_set_error(info, "ducktinycc arg count too large");
		return;
	}
		if (ctx->arg_count > 0) {
			in_data = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			in_validity = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			synthetic_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			if (ctx->wrapper_mode == TCC_WRAPPER_MODE_ROW) {
				arg_ptrs = (void **)duckdb_malloc(sizeof(void *) * (size_t)ctx->arg_count);
				row_varchar_values = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)ctx->arg_count);
				row_blob_values = (ducktinycc_blob_t *)duckdb_malloc(sizeof(ducktinycc_blob_t) * (size_t)ctx->arg_count);
				row_list_values = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)ctx->arg_count);
				row_array_values = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)ctx->arg_count);
				row_struct_values =
				    (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)ctx->arg_count);
				row_map_values = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)ctx->arg_count);
				row_union_values =
				    (ducktinycc_union_t *)duckdb_malloc(sizeof(ducktinycc_union_t) * (size_t)ctx->arg_count);
			} else {
				batch_arg_data = (void **)duckdb_malloc(sizeof(void *) * (size_t)ctx->arg_count);
				batch_varchar_columns = (const char ***)duckdb_malloc(sizeof(const char **) * (size_t)ctx->arg_count);
				batch_varchar_owned = (char ***)duckdb_malloc(sizeof(char **) * (size_t)ctx->arg_count);
				batch_blob_columns =
				    (ducktinycc_blob_t **)duckdb_malloc(sizeof(ducktinycc_blob_t *) * (size_t)ctx->arg_count);
				batch_list_columns =
				    (ducktinycc_list_t **)duckdb_malloc(sizeof(ducktinycc_list_t *) * (size_t)ctx->arg_count);
				batch_array_columns =
				    (ducktinycc_array_t **)duckdb_malloc(sizeof(ducktinycc_array_t *) * (size_t)ctx->arg_count);
				batch_struct_columns =
				    (ducktinycc_struct_t **)duckdb_malloc(sizeof(ducktinycc_struct_t *) * (size_t)ctx->arg_count);
				batch_map_columns =
				    (ducktinycc_map_t **)duckdb_malloc(sizeof(ducktinycc_map_t *) * (size_t)ctx->arg_count);
				batch_union_columns =
				    (ducktinycc_union_t **)duckdb_malloc(sizeof(ducktinycc_union_t *) * (size_t)ctx->arg_count);
			}
			list_entries_columns =
			    (duckdb_list_entry **)duckdb_malloc(sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			list_child_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			list_child_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			list_child_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			array_child_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			array_child_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			array_child_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			arg_value_bridges =
			    (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * (size_t)ctx->arg_count);
			struct_child_data_columns = (uint8_t ***)duckdb_malloc(sizeof(uint8_t **) * (size_t)ctx->arg_count);
			struct_child_validity_columns =
			    (uint64_t ***)duckdb_malloc(sizeof(uint64_t **) * (size_t)ctx->arg_count);
			struct_child_sizes = (size_t **)duckdb_malloc(sizeof(size_t *) * (size_t)ctx->arg_count);
			struct_field_counts = (idx_t *)duckdb_malloc(sizeof(idx_t) * (size_t)ctx->arg_count);
			arg_struct_nested_bridges =
			    (tcc_nested_struct_bridge_t ***)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t **) * (size_t)ctx->arg_count);
			map_entries_columns = (duckdb_list_entry **)duckdb_malloc(sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			map_key_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			map_key_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			map_key_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			map_value_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			map_value_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			map_value_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			union_tag_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			union_member_data_columns = (uint8_t ***)duckdb_malloc(sizeof(uint8_t **) * (size_t)ctx->arg_count);
			union_member_validity_columns =
			    (uint64_t ***)duckdb_malloc(sizeof(uint64_t **) * (size_t)ctx->arg_count);
			union_member_sizes = (size_t **)duckdb_malloc(sizeof(size_t *) * (size_t)ctx->arg_count);
			union_member_counts = (idx_t *)duckdb_malloc(sizeof(idx_t) * (size_t)ctx->arg_count);
			if (!in_data || !in_validity || !synthetic_validity_columns ||
			    (ctx->wrapper_mode == TCC_WRAPPER_MODE_ROW &&
			     (!arg_ptrs || !row_varchar_values || !row_blob_values || !row_list_values || !row_array_values ||
			      !row_struct_values || !row_map_values || !row_union_values)) ||
			    (ctx->wrapper_mode == TCC_WRAPPER_MODE_BATCH &&
			     (!batch_arg_data || !batch_varchar_columns || !batch_varchar_owned || !batch_blob_columns ||
			      !batch_list_columns || !batch_array_columns || !batch_struct_columns || !batch_map_columns ||
			      !batch_union_columns)) ||
			    !list_entries_columns || !list_child_data_columns || !list_child_validity_columns || !list_child_sizes ||
			    !array_child_data_columns || !array_child_validity_columns || !array_child_sizes ||
			    !arg_value_bridges ||
			    !struct_child_data_columns || !struct_child_validity_columns || !struct_child_sizes ||
			    !struct_field_counts || !arg_struct_nested_bridges || !map_entries_columns || !map_key_data_columns ||
			    !map_key_validity_columns ||
			    !map_key_sizes || !map_value_data_columns || !map_value_validity_columns || !map_value_sizes ||
			    !union_tag_columns || !union_member_data_columns || !union_member_validity_columns ||
			    !union_member_sizes || !union_member_counts) {
				error = "ducktinycc out of memory";
				goto cleanup;
			}
		if (batch_varchar_columns) {
			memset(batch_varchar_columns, 0, sizeof(const char **) * (size_t)ctx->arg_count);
		}
		if (batch_varchar_owned) {
			memset(batch_varchar_owned, 0, sizeof(char **) * (size_t)ctx->arg_count);
		}
			if (batch_blob_columns) {
				memset(batch_blob_columns, 0, sizeof(ducktinycc_blob_t *) * (size_t)ctx->arg_count);
			}
			if (batch_list_columns) {
				memset(batch_list_columns, 0, sizeof(ducktinycc_list_t *) * (size_t)ctx->arg_count);
			}
			if (batch_array_columns) {
				memset(batch_array_columns, 0, sizeof(ducktinycc_array_t *) * (size_t)ctx->arg_count);
			}
			if (batch_struct_columns) {
				memset(batch_struct_columns, 0, sizeof(ducktinycc_struct_t *) * (size_t)ctx->arg_count);
			}
			if (batch_map_columns) {
				memset(batch_map_columns, 0, sizeof(ducktinycc_map_t *) * (size_t)ctx->arg_count);
			}
			if (batch_union_columns) {
				memset(batch_union_columns, 0, sizeof(ducktinycc_union_t *) * (size_t)ctx->arg_count);
			}
			memset(list_entries_columns, 0, sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			memset(list_child_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(list_child_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(list_child_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(array_child_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(array_child_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(array_child_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(arg_value_bridges, 0, sizeof(tcc_value_bridge_t *) * (size_t)ctx->arg_count);
			memset(synthetic_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(struct_child_data_columns, 0, sizeof(uint8_t **) * (size_t)ctx->arg_count);
			memset(struct_child_validity_columns, 0, sizeof(uint64_t **) * (size_t)ctx->arg_count);
			memset(struct_child_sizes, 0, sizeof(size_t *) * (size_t)ctx->arg_count);
			memset(struct_field_counts, 0, sizeof(idx_t) * (size_t)ctx->arg_count);
			memset(arg_struct_nested_bridges, 0, sizeof(tcc_nested_struct_bridge_t **) * (size_t)ctx->arg_count);
			memset(map_entries_columns, 0, sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			memset(map_key_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(map_key_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(map_key_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(map_value_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(map_value_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(map_value_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(union_tag_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(union_member_data_columns, 0, sizeof(uint8_t **) * (size_t)ctx->arg_count);
			memset(union_member_validity_columns, 0, sizeof(uint64_t **) * (size_t)ctx->arg_count);
			memset(union_member_sizes, 0, sizeof(size_t *) * (size_t)ctx->arg_count);
			memset(union_member_counts, 0, sizeof(idx_t) * (size_t)ctx->arg_count);
		}
	ret_size = tcc_ffi_type_size(ctx->return_type);

		for (col = 0; col < ctx->arg_count; col++) {
			duckdb_vector v = duckdb_data_chunk_get_vector(input, (idx_t)col);
			const tcc_typedesc_t *arg_desc = (ctx->arg_descs && col < ctx->arg_count) ? ctx->arg_descs[col] : NULL;
			in_data[col] = (uint8_t *)duckdb_vector_get_data(v);
			in_validity[col] = duckdb_vector_get_validity(v);
			if (!ctx->arg_sizes || ctx->arg_sizes[col] == 0) {
				error = "ducktinycc invalid arg type size";
				goto cleanup;
			}
			if (arg_desc && tcc_typedesc_is_composite(arg_desc)) {
				const char *bridge_error = NULL;
				tcc_value_bridge_t *bridge = tcc_build_value_bridge(v, arg_desc, n, &bridge_error);
				if (!bridge) {
					error = bridge_error ? bridge_error : "ducktinycc composite bridge failed";
					goto cleanup;
				}
				arg_value_bridges[col] = bridge;
				in_data[col] = (uint8_t *)bridge->rows;
				if (bridge->validity) {
					in_validity[col] = (uint64_t *)bridge->validity;
				}
				continue;
			}
			if (tcc_ffi_type_is_list(ctx->arg_types[col])) {
				tcc_ffi_type_t child_type = TCC_FFI_VOID;
				duckdb_vector child_vector;
				if (!tcc_ffi_list_child_type(ctx->arg_types[col], &child_type)) {
					error = "ducktinycc invalid list child type";
					goto cleanup;
				}
				list_child_sizes[col] = tcc_ffi_type_size(child_type);
				if (list_child_sizes[col] == 0) {
					error = "ducktinycc invalid list child type size";
					goto cleanup;
				}
				child_vector = duckdb_list_vector_get_child(v);
				list_entries_columns[col] = (duckdb_list_entry *)in_data[col];
				list_child_data_columns[col] = (uint8_t *)duckdb_vector_get_data(child_vector);
				list_child_validity_columns[col] = duckdb_vector_get_validity(child_vector);
			} else if (tcc_ffi_type_is_array(ctx->arg_types[col])) {
				tcc_ffi_type_t child_type = TCC_FFI_VOID;
				duckdb_vector child_vector;
				size_t array_len = ctx->arg_array_sizes ? ctx->arg_array_sizes[col] : 0;
				if (array_len == 0 || !tcc_ffi_array_child_type(ctx->arg_types[col], &child_type)) {
					error = "ducktinycc invalid array child type";
					goto cleanup;
				}
				array_child_sizes[col] = tcc_ffi_type_size(child_type);
				if (array_child_sizes[col] == 0) {
					error = "ducktinycc invalid array child type size";
					goto cleanup;
				}
				child_vector = duckdb_array_vector_get_child(v);
				array_child_data_columns[col] = (uint8_t *)duckdb_vector_get_data(child_vector);
				array_child_validity_columns[col] = duckdb_vector_get_validity(child_vector);
			} else if (tcc_ffi_type_is_struct(ctx->arg_types[col])) {
				const tcc_ffi_struct_meta_t *meta = ctx->arg_struct_metas ? &ctx->arg_struct_metas[col] : NULL;
				tcc_nested_struct_bridge_t *bridge = NULL;
				const char *bridge_error = NULL;
				idx_t field_idx;
				if (!meta || meta->field_count <= 0 || !meta->field_sizes) {
					error = "ducktinycc invalid struct metadata";
					goto cleanup;
				}
				struct_field_counts[col] = (idx_t)meta->field_count;
				struct_child_data_columns[col] =
				    (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)meta->field_count);
				struct_child_validity_columns[col] =
				    (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)meta->field_count);
				struct_child_sizes[col] = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)meta->field_count);
				arg_struct_nested_bridges[col] =
				    (tcc_nested_struct_bridge_t **)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t *) * (size_t)meta->field_count);
				if (!struct_child_data_columns[col] || !struct_child_validity_columns[col] || !struct_child_sizes[col] ||
				    !arg_struct_nested_bridges[col]) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				memset(arg_struct_nested_bridges[col], 0, sizeof(tcc_nested_struct_bridge_t *) * (size_t)meta->field_count);
				bridge = tcc_build_struct_bridge_from_vector(v, meta, n, &bridge_error);
				if (!bridge) {
					error = bridge_error ? bridge_error : "ducktinycc struct bridge failed";
					goto cleanup;
				}
				arg_struct_nested_bridges[col][0] = bridge;
				for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
					struct_child_data_columns[col][field_idx] = (uint8_t *)bridge->field_ptrs[field_idx];
					struct_child_validity_columns[col][field_idx] = (uint64_t *)bridge->field_validity[field_idx];
					struct_child_sizes[col][field_idx] = meta->field_sizes[field_idx];
					if (struct_child_sizes[col][field_idx] == 0) {
						error = "ducktinycc invalid struct child type size";
						goto cleanup;
					}
				}
				if (!in_validity[col] && bridge->row_validity_mask) {
					in_validity[col] = bridge->row_validity_mask;
				}
			} else if (tcc_ffi_type_is_map(ctx->arg_types[col])) {
				const tcc_ffi_map_meta_t *meta = ctx->arg_map_metas ? &ctx->arg_map_metas[col] : NULL;
				duckdb_vector map_struct_vector;
				duckdb_vector map_key_vector;
				duckdb_vector map_value_vector;
				if (!meta || meta->key_size == 0 || meta->value_size == 0) {
					error = "ducktinycc invalid map metadata";
					goto cleanup;
				}
				map_entries_columns[col] = (duckdb_list_entry *)in_data[col];
				map_key_sizes[col] = meta->key_size;
				map_value_sizes[col] = meta->value_size;
				map_struct_vector = duckdb_list_vector_get_child(v);
				if (!map_struct_vector) {
					error = "ducktinycc map child vector missing";
					goto cleanup;
				}
				map_key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
				map_value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
				if (!map_key_vector || !map_value_vector) {
					error = "ducktinycc map key/value child vector missing";
					goto cleanup;
				}
				map_key_data_columns[col] = (uint8_t *)duckdb_vector_get_data(map_key_vector);
				map_key_validity_columns[col] = duckdb_vector_get_validity(map_key_vector);
				map_value_data_columns[col] = (uint8_t *)duckdb_vector_get_data(map_value_vector);
				map_value_validity_columns[col] = duckdb_vector_get_validity(map_value_vector);
			} else if (tcc_ffi_type_is_union(ctx->arg_types[col])) {
				const tcc_ffi_union_meta_t *meta = ctx->arg_union_metas ? &ctx->arg_union_metas[col] : NULL;
				idx_t member_idx;
				if (!meta || meta->member_count <= 0 || !meta->member_sizes) {
					error = "ducktinycc invalid union metadata";
					goto cleanup;
				}
				union_member_counts[col] = (idx_t)meta->member_count;
				union_tag_columns[col] = (uint8_t *)in_data[col];
				union_member_data_columns[col] =
				    (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)meta->member_count);
				union_member_validity_columns[col] =
				    (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)meta->member_count);
				union_member_sizes[col] = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)meta->member_count);
				if (!union_member_data_columns[col] || !union_member_validity_columns[col] || !union_member_sizes[col]) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				for (member_idx = 0; member_idx < (idx_t)meta->member_count; member_idx++) {
					duckdb_vector child_vector = duckdb_struct_vector_get_child(v, member_idx);
					if (!child_vector) {
						error = "ducktinycc union member vector missing";
						goto cleanup;
					}
					union_member_data_columns[col][member_idx] = (uint8_t *)duckdb_vector_get_data(child_vector);
					union_member_validity_columns[col][member_idx] = duckdb_vector_get_validity(child_vector);
					union_member_sizes[col][member_idx] = meta->member_sizes[member_idx];
					if (union_member_sizes[col][member_idx] == 0) {
						error = "ducktinycc invalid union member size";
						goto cleanup;
					}
				}
				if (!in_validity[col] && n > 0) {
					idx_t words = (n + 63) / 64;
					uint64_t *mask = (uint64_t *)duckdb_malloc(sizeof(uint64_t) * (size_t)words);
					if (!mask) {
						error = "ducktinycc out of memory";
						goto cleanup;
					}
					tcc_validity_set_all(mask, n, true);
					for (row = 0; row < n; row++) {
						bool any_valid = false;
						for (member_idx = 0; member_idx < (idx_t)meta->member_count; member_idx++) {
							if (!union_member_validity_columns[col][member_idx] ||
							    duckdb_validity_row_is_valid(union_member_validity_columns[col][member_idx], row)) {
								any_valid = true;
								break;
							}
						}
						if (!any_valid) {
							duckdb_validity_set_row_validity(mask, row, false);
						}
					}
					synthetic_validity_columns[col] = mask;
					in_validity[col] = mask;
				}
			}
		}

	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	if (!out_validity) {
		error = "ducktinycc output validity missing";
		goto cleanup;
	}
	if (tcc_ffi_type_is_list(ctx->return_type)) {
		tcc_ffi_type_t out_child_type = TCC_FFI_VOID;
		if (!tcc_ffi_list_child_type(ctx->return_type, &out_child_type)) {
			error = "ducktinycc invalid list return type";
			goto cleanup;
		}
		out_list_child_size = tcc_ffi_type_size(out_child_type);
		if (out_list_child_size == 0) {
			error = "ducktinycc invalid list child type size";
			goto cleanup;
		}
		out_list_entries = (duckdb_list_entry *)out_data;
		out_list_child_vector = duckdb_list_vector_get_child(output);
		if (!out_list_child_vector) {
			error = "ducktinycc list return child vector missing";
			goto cleanup;
		}
		if (duckdb_list_vector_set_size(output, 0) != DuckDBSuccess) {
			error = "ducktinycc failed to initialize list return vector";
			goto cleanup;
		}
		out_list_child_count = 0;
	}
	if (tcc_ffi_type_is_array(ctx->return_type)) {
		tcc_ffi_type_t out_child_type = TCC_FFI_VOID;
		out_array_len = ctx->return_array_size;
		if (out_array_len == 0 || !tcc_ffi_array_child_type(ctx->return_type, &out_child_type)) {
			error = "ducktinycc invalid array return type";
			goto cleanup;
		}
		out_array_child_size = tcc_ffi_type_size(out_child_type);
		if (out_array_child_size == 0) {
			error = "ducktinycc invalid array child type size";
			goto cleanup;
		}
		out_array_child_vector = duckdb_array_vector_get_child(output);
		if (!out_array_child_vector) {
			error = "ducktinycc array return child vector missing";
			goto cleanup;
		}
	}
	if (tcc_ffi_type_is_struct(ctx->return_type)) {
		const tcc_ffi_struct_meta_t *meta = &ctx->return_struct_meta;
		idx_t field_idx;
		if (!meta || meta->field_count <= 0 || !meta->field_sizes) {
			error = "ducktinycc invalid struct return metadata";
			goto cleanup;
		}
		out_struct_field_count = (idx_t)meta->field_count;
		out_struct_child_data = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)out_struct_field_count);
		out_struct_child_validity =
		    (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)out_struct_field_count);
		out_struct_child_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)out_struct_field_count);
		if (!out_struct_child_data || !out_struct_child_validity || !out_struct_child_sizes) {
			error = "ducktinycc out of memory";
			goto cleanup;
		}
		for (field_idx = 0; field_idx < out_struct_field_count; field_idx++) {
			duckdb_vector child_vector = duckdb_struct_vector_get_child(output, field_idx);
			if (!child_vector) {
				error = "ducktinycc struct return child vector missing";
				goto cleanup;
			}
			out_struct_child_data[field_idx] = (uint8_t *)duckdb_vector_get_data(child_vector);
			out_struct_child_validity[field_idx] = duckdb_vector_get_validity(child_vector);
			out_struct_child_sizes[field_idx] = meta->field_sizes[field_idx];
			if (out_struct_child_sizes[field_idx] == 0) {
				error = "ducktinycc invalid struct return child size";
				goto cleanup;
			}
		}
	}
	if (tcc_ffi_type_is_map(ctx->return_type)) {
		const tcc_ffi_map_meta_t *meta = &ctx->return_map_meta;
		if (!meta || meta->key_size == 0 || meta->value_size == 0) {
			error = "ducktinycc invalid map return metadata";
			goto cleanup;
		}
		out_map_entries = (duckdb_list_entry *)out_data;
		out_map_struct_vector = duckdb_list_vector_get_child(output);
		if (!out_map_struct_vector) {
			error = "ducktinycc map return child vector missing";
			goto cleanup;
		}
		out_map_key_vector = duckdb_struct_vector_get_child(out_map_struct_vector, 0);
		out_map_value_vector = duckdb_struct_vector_get_child(out_map_struct_vector, 1);
		if (!out_map_key_vector || !out_map_value_vector) {
			error = "ducktinycc map return key/value child vector missing";
			goto cleanup;
		}
		out_map_key_size = meta->key_size;
		out_map_value_size = meta->value_size;
		if (duckdb_list_vector_set_size(output, 0) != DuckDBSuccess) {
			error = "ducktinycc failed to initialize map return vector";
			goto cleanup;
		}
		out_map_child_count = 0;
	}
	if (tcc_ffi_type_is_union(ctx->return_type)) {
		const tcc_ffi_union_meta_t *meta = &ctx->return_union_meta;
		idx_t member_idx;
		if (!meta || meta->member_count <= 0 || !meta->member_sizes) {
			error = "ducktinycc invalid union return metadata";
			goto cleanup;
		}
		out_union_member_count = (idx_t)meta->member_count;
		out_union_tags = (uint8_t *)out_data;
		out_union_member_data = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)out_union_member_count);
		out_union_member_validity =
		    (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)out_union_member_count);
		out_union_member_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)out_union_member_count);
		if (!out_union_member_data || !out_union_member_validity || !out_union_member_sizes) {
			error = "ducktinycc out of memory";
			goto cleanup;
		}
		for (member_idx = 0; member_idx < out_union_member_count; member_idx++) {
			duckdb_vector child_vector = duckdb_struct_vector_get_child(output, member_idx);
			if (!child_vector) {
				error = "ducktinycc union return child vector missing";
				goto cleanup;
			}
			out_union_member_data[member_idx] = (uint8_t *)duckdb_vector_get_data(child_vector);
			out_union_member_validity[member_idx] = duckdb_vector_get_validity(child_vector);
			out_union_member_sizes[member_idx] = meta->member_sizes[member_idx];
			if (out_union_member_sizes[member_idx] == 0) {
				error = "ducktinycc invalid union return member size";
				goto cleanup;
			}
		}
	}

	if (ctx->wrapper_mode == TCC_WRAPPER_MODE_BATCH) {
		for (col = 0; col < ctx->arg_count; col++) {
			if (ctx->arg_types[col] == TCC_FFI_VARCHAR) {
				duckdb_string_t *strings = (duckdb_string_t *)in_data[col];
				const char **decoded = NULL;
				char **owned = NULL;
				if (n > 0) {
					decoded = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)n);
					owned = (char **)duckdb_malloc(sizeof(char *) * (size_t)n);
					if (!decoded || !owned) {
						if (decoded) {
							duckdb_free((void *)decoded);
						}
						if (owned) {
							duckdb_free((void *)owned);
						}
						error = "ducktinycc out of memory";
						goto cleanup;
					}
					memset(owned, 0, sizeof(char *) * (size_t)n);
					batch_varchar_columns[col] = decoded;
					batch_varchar_owned[col] = owned;
					for (row = 0; row < n; row++) {
						if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
							decoded[row] = NULL;
						} else {
							owned[row] = tcc_copy_duckdb_string_as_cstr(&strings[row]);
							if (!owned[row]) {
								error = "ducktinycc out of memory";
								goto cleanup;
							}
							decoded[row] = owned[row];
						}
					}
				} else {
					batch_varchar_columns[col] = NULL;
					batch_varchar_owned[col] = NULL;
				}
				batch_arg_data[col] = (void *)decoded;
				} else if (ctx->arg_types[col] == TCC_FFI_BLOB) {
					duckdb_string_t *strings = (duckdb_string_t *)in_data[col];
					ducktinycc_blob_t *decoded = NULL;
					if (n > 0) {
					decoded = (ducktinycc_blob_t *)duckdb_malloc(sizeof(ducktinycc_blob_t) * (size_t)n);
					if (!decoded) {
						error = "ducktinycc out of memory";
						goto cleanup;
					}
					for (row = 0; row < n; row++) {
						if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
							decoded[row].ptr = NULL;
							decoded[row].len = 0;
						} else {
							decoded[row] = tcc_duckdb_string_to_blob(&strings[row]);
						}
					}
					}
					batch_blob_columns[col] = decoded;
					batch_arg_data[col] = (void *)decoded;
				} else if (arg_value_bridges && arg_value_bridges[col]) {
					batch_arg_data[col] = (void *)in_data[col];
				} else if (tcc_ffi_type_is_list(ctx->arg_types[col])) {
					ducktinycc_list_t *decoded = NULL;
					if (n > 0) {
						decoded = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)n);
						if (!decoded) {
							error = "ducktinycc out of memory";
							goto cleanup;
						}
						for (row = 0; row < n; row++) {
							if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
								decoded[row].ptr = NULL;
								decoded[row].validity = NULL;
								decoded[row].offset = 0;
								decoded[row].len = 0;
							} else {
								duckdb_list_entry entry = list_entries_columns[col][row];
								decoded[row].ptr =
								    list_child_data_columns[col]
								        ? (const void *)(list_child_data_columns[col] +
								                         ((size_t)entry.offset * list_child_sizes[col]))
								        : NULL;
								decoded[row].validity = list_child_validity_columns[col];
								decoded[row].offset = (uint64_t)entry.offset;
								decoded[row].len = (uint64_t)entry.length;
							}
						}
					}
					batch_list_columns[col] = decoded;
					batch_arg_data[col] = (void *)decoded;
				} else if (tcc_ffi_type_is_array(ctx->arg_types[col])) {
					ducktinycc_array_t *decoded = NULL;
					size_t array_len = ctx->arg_array_sizes ? ctx->arg_array_sizes[col] : 0;
					if (n > 0) {
						decoded = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)n);
						if (!decoded) {
							error = "ducktinycc out of memory";
							goto cleanup;
						}
						for (row = 0; row < n; row++) {
							if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
								decoded[row].ptr = NULL;
								decoded[row].validity = NULL;
								decoded[row].offset = 0;
								decoded[row].len = 0;
							} else {
								uint64_t off = (uint64_t)row * (uint64_t)array_len;
								decoded[row].ptr =
								    array_child_data_columns[col]
								        ? (const void *)(array_child_data_columns[col] +
								                         ((size_t)off * array_child_sizes[col]))
								        : NULL;
								decoded[row].validity = array_child_validity_columns[col];
								decoded[row].offset = off;
								decoded[row].len = (uint64_t)array_len;
							}
						}
					}
					batch_array_columns[col] = decoded;
					batch_arg_data[col] = (void *)decoded;
				} else if (tcc_ffi_type_is_struct(ctx->arg_types[col])) {
					ducktinycc_struct_t *decoded = NULL;
					idx_t field_count = struct_field_counts[col];
					if (n > 0) {
						decoded = (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)n);
						if (!decoded) {
							error = "ducktinycc out of memory";
							goto cleanup;
						}
						for (row = 0; row < n; row++) {
							if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
								decoded[row].field_ptrs = NULL;
								decoded[row].field_validity = NULL;
								decoded[row].field_count = 0;
								decoded[row].offset = 0;
							} else {
								decoded[row].field_ptrs = (const void *const *)struct_child_data_columns[col];
								decoded[row].field_validity =
								    (const uint64_t *const *)struct_child_validity_columns[col];
								decoded[row].field_count = (uint64_t)field_count;
								decoded[row].offset = (uint64_t)row;
							}
						}
					}
					batch_struct_columns[col] = decoded;
					batch_arg_data[col] = (void *)decoded;
				} else if (tcc_ffi_type_is_map(ctx->arg_types[col])) {
					ducktinycc_map_t *decoded = NULL;
					if (n > 0) {
						decoded = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)n);
						if (!decoded) {
							error = "ducktinycc out of memory";
							goto cleanup;
						}
						for (row = 0; row < n; row++) {
							if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
								decoded[row].key_ptr = NULL;
								decoded[row].key_validity = NULL;
								decoded[row].value_ptr = NULL;
								decoded[row].value_validity = NULL;
								decoded[row].offset = 0;
								decoded[row].len = 0;
							} else {
								duckdb_list_entry entry = map_entries_columns[col][row];
								decoded[row].key_ptr =
								    map_key_data_columns[col]
								        ? (const void *)(map_key_data_columns[col] +
								                         ((size_t)entry.offset * map_key_sizes[col]))
								        : NULL;
								decoded[row].key_validity = map_key_validity_columns[col];
								decoded[row].value_ptr =
								    map_value_data_columns[col]
								        ? (const void *)(map_value_data_columns[col] +
								                         ((size_t)entry.offset * map_value_sizes[col]))
								        : NULL;
								decoded[row].value_validity = map_value_validity_columns[col];
								decoded[row].offset = (uint64_t)entry.offset;
								decoded[row].len = (uint64_t)entry.length;
							}
						}
					}
					batch_map_columns[col] = decoded;
					batch_arg_data[col] = (void *)decoded;
				} else if (tcc_ffi_type_is_union(ctx->arg_types[col])) {
					ducktinycc_union_t *decoded = NULL;
					idx_t member_count = union_member_counts[col];
					if (n > 0) {
						decoded = (ducktinycc_union_t *)duckdb_malloc(sizeof(ducktinycc_union_t) * (size_t)n);
						if (!decoded) {
							error = "ducktinycc out of memory";
							goto cleanup;
						}
						for (row = 0; row < n; row++) {
							if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
								decoded[row].tag_ptr = NULL;
								decoded[row].member_ptrs = NULL;
								decoded[row].member_validity = NULL;
								decoded[row].member_count = 0;
								decoded[row].offset = 0;
							} else {
								decoded[row].tag_ptr = union_tag_columns[col];
								decoded[row].member_ptrs = (const void *const *)union_member_data_columns[col];
								decoded[row].member_validity =
								    (const uint64_t *const *)union_member_validity_columns[col];
								decoded[row].member_count = (uint64_t)member_count;
								decoded[row].offset = (uint64_t)row;
							}
						}
					}
					batch_union_columns[col] = decoded;
					batch_arg_data[col] = (void *)decoded;
				} else {
					batch_arg_data[col] = (void *)in_data[col];
				}
			}
			if (ctx->return_type == TCC_FFI_VARCHAR && n > 0) {
				batch_out_varchar = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)n);
				if (!batch_out_varchar) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				memset((void *)batch_out_varchar, 0, sizeof(const char *) * (size_t)n);
			} else if (ctx->return_type == TCC_FFI_BLOB && n > 0) {
				batch_out_blob = (ducktinycc_blob_t *)duckdb_malloc(sizeof(ducktinycc_blob_t) * (size_t)n);
				if (!batch_out_blob) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				memset((void *)batch_out_blob, 0, sizeof(ducktinycc_blob_t) * (size_t)n);
			} else if (tcc_ffi_type_is_list(ctx->return_type) && n > 0) {
				batch_out_list = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)n);
				if (!batch_out_list) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				memset((void *)batch_out_list, 0, sizeof(ducktinycc_list_t) * (size_t)n);
			} else if (tcc_ffi_type_is_array(ctx->return_type) && n > 0) {
				batch_out_array = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)n);
				if (!batch_out_array) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				memset((void *)batch_out_array, 0, sizeof(ducktinycc_array_t) * (size_t)n);
			} else if (tcc_ffi_type_is_struct(ctx->return_type) && n > 0) {
				batch_out_struct = (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)n);
				if (!batch_out_struct) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				memset((void *)batch_out_struct, 0, sizeof(ducktinycc_struct_t) * (size_t)n);
				} else if (tcc_ffi_type_is_map(ctx->return_type) && n > 0) {
					batch_out_map = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)n);
					if (!batch_out_map) {
						error = "ducktinycc out of memory";
						goto cleanup;
					}
					memset((void *)batch_out_map, 0, sizeof(ducktinycc_map_t) * (size_t)n);
				} else if (tcc_ffi_type_is_union(ctx->return_type) && n > 0) {
					batch_out_union = (ducktinycc_union_t *)duckdb_malloc(sizeof(ducktinycc_union_t) * (size_t)n);
					if (!batch_out_union) {
						error = "ducktinycc out of memory";
						goto cleanup;
					}
					memset((void *)batch_out_union, 0, sizeof(ducktinycc_union_t) * (size_t)n);
				}
				tcc_validity_set_all(out_validity, n, ctx->return_type != TCC_FFI_VOID);
				batch_out_ptr = out_data;
				if (ctx->return_type == TCC_FFI_VARCHAR) {
					batch_out_ptr = (void *)batch_out_varchar;
				} else if (ctx->return_type == TCC_FFI_BLOB) {
					batch_out_ptr = (void *)batch_out_blob;
				} else if (tcc_ffi_type_is_list(ctx->return_type)) {
					batch_out_ptr = (void *)batch_out_list;
				} else if (tcc_ffi_type_is_array(ctx->return_type)) {
					batch_out_ptr = (void *)batch_out_array;
				} else if (tcc_ffi_type_is_struct(ctx->return_type)) {
					batch_out_ptr = (void *)batch_out_struct;
				} else if (tcc_ffi_type_is_map(ctx->return_type)) {
					batch_out_ptr = (void *)batch_out_map;
				} else if (tcc_ffi_type_is_union(ctx->return_type)) {
					batch_out_ptr = (void *)batch_out_union;
				}
				if (!ctx->batch_wrapper(batch_arg_data, in_validity, (uint64_t)n, batch_out_ptr, out_validity)) {
					error = "ducktinycc invoke failed";
					goto cleanup;
				}
		if (ctx->return_type == TCC_FFI_VOID) {
			tcc_validity_set_all(out_validity, n, false);
		} else if (ctx->return_type == TCC_FFI_VARCHAR) {
			for (row = 0; row < n; row++) {
				if (!duckdb_validity_row_is_valid(out_validity, row)) {
					continue;
				}
				if (!batch_out_varchar || !batch_out_varchar[row]) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				duckdb_vector_assign_string_element(output, row, batch_out_varchar[row]);
			}
			} else if (ctx->return_type == TCC_FFI_BLOB) {
				for (row = 0; row < n; row++) {
					if (!duckdb_validity_row_is_valid(out_validity, row)) {
						continue;
					}
				if (!batch_out_blob || !batch_out_blob[row].ptr) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
					duckdb_vector_assign_string_element_len(output, row, (const char *)batch_out_blob[row].ptr,
					                                        (idx_t)batch_out_blob[row].len);
				}
			} else if (tcc_typedesc_is_composite(return_desc)) {
				for (row = 0; row < n; row++) {
					if (!duckdb_validity_row_is_valid(out_validity, row)) {
						continue;
					}
					if (!tcc_write_value_to_vector(output, return_desc, row, batch_out_ptr, (uint64_t)row, NULL, &error)) {
						goto cleanup;
					}
				}
			} else if (tcc_ffi_type_is_list(ctx->return_type)) {
				for (row = 0; row < n; row++) {
					idx_t elem_idx;
					ducktinycc_list_t desc;
					uint8_t *child_data;
					uint64_t *child_validity;
					if (!duckdb_validity_row_is_valid(out_validity, row)) {
						continue;
					}
					if (!batch_out_list || !batch_out_list[row].ptr) {
						duckdb_validity_set_row_validity(out_validity, row, false);
						continue;
					}
					desc = batch_out_list[row];
					if (duckdb_list_vector_reserve(output, out_list_child_count + (idx_t)desc.len) != DuckDBSuccess) {
						error = "ducktinycc list return reserve failed";
						goto cleanup;
					}
					if (duckdb_list_vector_set_size(output, out_list_child_count + (idx_t)desc.len) != DuckDBSuccess) {
						error = "ducktinycc list return set_size failed";
						goto cleanup;
					}
					child_data = (uint8_t *)duckdb_vector_get_data(out_list_child_vector);
					if (desc.len > 0 && child_data) {
						memcpy(child_data + ((size_t)out_list_child_count * out_list_child_size), desc.ptr,
						       (size_t)desc.len * out_list_child_size);
					}
					duckdb_vector_ensure_validity_writable(out_list_child_vector);
					child_validity = duckdb_vector_get_validity(out_list_child_vector);
					for (elem_idx = 0; elem_idx < (idx_t)desc.len; elem_idx++) {
						bool child_is_valid = true;
						if (desc.validity) {
							uint64_t src_idx = desc.offset + (uint64_t)elem_idx;
							child_is_valid =
							    (desc.validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
						}
						duckdb_validity_set_row_validity(child_validity, out_list_child_count + elem_idx, child_is_valid);
					}
					out_list_entries[row].offset = out_list_child_count;
					out_list_entries[row].length = (idx_t)desc.len;
					out_list_child_count += (idx_t)desc.len;
				}
			} else if (tcc_ffi_type_is_array(ctx->return_type)) {
				for (row = 0; row < n; row++) {
					idx_t elem_idx;
					ducktinycc_array_t desc;
					uint8_t *child_data;
					uint64_t *child_validity;
					size_t expected_len = out_array_len;
					if (!duckdb_validity_row_is_valid(out_validity, row)) {
						continue;
					}
					if (!batch_out_array || !batch_out_array[row].ptr) {
						duckdb_validity_set_row_validity(out_validity, row, false);
						continue;
					}
					desc = batch_out_array[row];
					if ((size_t)desc.len != expected_len) {
						error = "ducktinycc array return length mismatch";
						goto cleanup;
					}
					child_data = (uint8_t *)duckdb_vector_get_data(out_array_child_vector);
					if (expected_len > 0 && child_data) {
						memcpy(child_data + (((size_t)row * expected_len) * out_array_child_size), desc.ptr,
						       expected_len * out_array_child_size);
					}
					duckdb_vector_ensure_validity_writable(out_array_child_vector);
					child_validity = duckdb_vector_get_validity(out_array_child_vector);
					for (elem_idx = 0; elem_idx < (idx_t)expected_len; elem_idx++) {
						bool child_is_valid = true;
						if (desc.validity) {
							uint64_t src_idx = desc.offset + (uint64_t)elem_idx;
							child_is_valid =
							    (desc.validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
						}
						duckdb_validity_set_row_validity(child_validity, ((idx_t)row * (idx_t)expected_len) + elem_idx,
						                                 child_is_valid);
					}
				}
			} else if (tcc_ffi_type_is_struct(ctx->return_type)) {
				for (row = 0; row < n; row++) {
					ducktinycc_struct_t desc;
					idx_t field_idx;
					if (!duckdb_validity_row_is_valid(out_validity, row)) {
						continue;
					}
					if (!batch_out_struct || !batch_out_struct[row].field_ptrs ||
					    batch_out_struct[row].field_count != (uint64_t)out_struct_field_count) {
						duckdb_validity_set_row_validity(out_validity, row, false);
						continue;
					}
					desc = batch_out_struct[row];
					for (field_idx = 0; field_idx < out_struct_field_count; field_idx++) {
						uint8_t *dst;
						uint64_t *field_validity;
						const uint8_t *src_base = (const uint8_t *)desc.field_ptrs[field_idx];
						if (!src_base) {
							duckdb_validity_set_row_validity(out_validity, row, false);
							break;
						}
						dst = out_struct_child_data[field_idx] + ((size_t)row * out_struct_child_sizes[field_idx]);
						memcpy(dst, src_base + ((size_t)desc.offset * out_struct_child_sizes[field_idx]),
						       out_struct_child_sizes[field_idx]);
						duckdb_vector_ensure_validity_writable(duckdb_struct_vector_get_child(output, field_idx));
						field_validity =
						    duckdb_vector_get_validity(duckdb_struct_vector_get_child(output, field_idx));
						{
							bool field_is_valid = true;
							if (desc.field_validity && desc.field_validity[field_idx]) {
								field_is_valid = (desc.field_validity[field_idx][desc.offset >> 6] &
								                  (1ULL << (desc.offset & 63))) != 0;
							}
							duckdb_validity_set_row_validity(field_validity, row, field_is_valid);
						}
					}
				}
				} else if (tcc_ffi_type_is_map(ctx->return_type)) {
					for (row = 0; row < n; row++) {
					ducktinycc_map_t desc;
					idx_t elem_idx;
					uint8_t *key_data;
					uint8_t *value_data;
					uint64_t *key_validity;
					uint64_t *value_validity;
					if (!duckdb_validity_row_is_valid(out_validity, row)) {
						continue;
					}
					if (!batch_out_map) {
						duckdb_validity_set_row_validity(out_validity, row, false);
						continue;
					}
					desc = batch_out_map[row];
					if (desc.len > 0 && (!desc.key_ptr || !desc.value_ptr)) {
						duckdb_validity_set_row_validity(out_validity, row, false);
						continue;
					}
					if (duckdb_list_vector_reserve(output, out_map_child_count + (idx_t)desc.len) != DuckDBSuccess) {
						error = "ducktinycc map return reserve failed";
						goto cleanup;
					}
					if (duckdb_list_vector_set_size(output, out_map_child_count + (idx_t)desc.len) != DuckDBSuccess) {
						error = "ducktinycc map return set_size failed";
						goto cleanup;
					}
					key_data = (uint8_t *)duckdb_vector_get_data(out_map_key_vector);
					value_data = (uint8_t *)duckdb_vector_get_data(out_map_value_vector);
					if (desc.len > 0) {
						memcpy(key_data + ((size_t)out_map_child_count * out_map_key_size), desc.key_ptr,
						       (size_t)desc.len * out_map_key_size);
						memcpy(value_data + ((size_t)out_map_child_count * out_map_value_size), desc.value_ptr,
						       (size_t)desc.len * out_map_value_size);
					}
					duckdb_vector_ensure_validity_writable(out_map_key_vector);
					duckdb_vector_ensure_validity_writable(out_map_value_vector);
					key_validity = duckdb_vector_get_validity(out_map_key_vector);
					value_validity = duckdb_vector_get_validity(out_map_value_vector);
					for (elem_idx = 0; elem_idx < (idx_t)desc.len; elem_idx++) {
						bool key_ok = true;
						bool value_ok = true;
						uint64_t src_idx = desc.offset + (uint64_t)elem_idx;
						if (desc.key_validity) {
							key_ok = (desc.key_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
						}
						if (desc.value_validity) {
							value_ok = (desc.value_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
						}
						duckdb_validity_set_row_validity(key_validity, out_map_child_count + elem_idx, key_ok);
						duckdb_validity_set_row_validity(value_validity, out_map_child_count + elem_idx, value_ok);
					}
					out_map_entries[row].offset = out_map_child_count;
						out_map_entries[row].length = (idx_t)desc.len;
						out_map_child_count += (idx_t)desc.len;
					}
				} else if (tcc_ffi_type_is_union(ctx->return_type)) {
					for (row = 0; row < n; row++) {
						ducktinycc_union_t desc;
						idx_t member_idx;
						uint8_t tag;
						if (!duckdb_validity_row_is_valid(out_validity, row)) {
							continue;
						}
						if (!batch_out_union) {
							duckdb_validity_set_row_validity(out_validity, row, false);
							continue;
						}
						desc = batch_out_union[row];
						if (!desc.tag_ptr || !desc.member_ptrs || desc.member_count != (uint64_t)out_union_member_count) {
							duckdb_validity_set_row_validity(out_validity, row, false);
							continue;
						}
						tag = desc.tag_ptr[desc.offset];
						if ((idx_t)tag >= out_union_member_count) {
							duckdb_validity_set_row_validity(out_validity, row, false);
							continue;
						}
						out_union_tags[row] = tag;
						for (member_idx = 0; member_idx < out_union_member_count; member_idx++) {
							duckdb_vector member_vector = duckdb_struct_vector_get_child(output, member_idx);
							uint64_t *member_validity;
							duckdb_vector_ensure_validity_writable(member_vector);
							member_validity = duckdb_vector_get_validity(member_vector);
							if (member_idx == (idx_t)tag) {
								const uint8_t *src_base = (const uint8_t *)desc.member_ptrs[member_idx];
								bool member_is_valid = true;
								if (!src_base) {
									duckdb_validity_set_row_validity(out_validity, row, false);
									break;
								}
								memcpy(out_union_member_data[member_idx] + ((size_t)row * out_union_member_sizes[member_idx]),
								       src_base + ((size_t)desc.offset * out_union_member_sizes[member_idx]),
								       out_union_member_sizes[member_idx]);
								if (desc.member_validity && desc.member_validity[member_idx]) {
									member_is_valid = (desc.member_validity[member_idx][desc.offset >> 6] &
									                   (1ULL << (desc.offset & 63))) != 0;
								}
								duckdb_validity_set_row_validity(member_validity, row, member_is_valid);
							} else {
								duckdb_validity_set_row_validity(member_validity, row, false);
							}
						}
					}
				}
				goto cleanup;
			}

	for (row = 0; row < n; row++) {
		bool valid = true;
		bool out_is_null = false;
		const void *row_result_base = (const void *)out_value;
		for (col = 0; col < ctx->arg_count; col++) {
			if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
				valid = false;
				break;
			}
			if (ctx->arg_types[col] == TCC_FFI_VARCHAR) {
				duckdb_string_t *sv = (duckdb_string_t *)(in_data[col] + ((size_t)row * ctx->arg_sizes[col]));
				char *owned_cstr = tcc_copy_duckdb_string_as_cstr(sv);
				if (!owned_cstr) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				if (row_varchar_alloc_count >= row_varchar_alloc_capacity) {
					idx_t new_cap = row_varchar_alloc_capacity == 0 ? 64 : row_varchar_alloc_capacity * 2;
					char **new_allocs = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
					if (!new_allocs) {
						duckdb_free(owned_cstr);
						error = "ducktinycc out of memory";
						goto cleanup;
					}
					if (row_varchar_allocations && row_varchar_alloc_count > 0) {
						memcpy(new_allocs, row_varchar_allocations, sizeof(char *) * (size_t)row_varchar_alloc_count);
						duckdb_free(row_varchar_allocations);
					}
					row_varchar_allocations = new_allocs;
					row_varchar_alloc_capacity = new_cap;
				}
				row_varchar_allocations[row_varchar_alloc_count++] = owned_cstr;
				row_varchar_values[col] = owned_cstr;
				arg_ptrs[col] = (void *)&row_varchar_values[col];
				} else if (ctx->arg_types[col] == TCC_FFI_BLOB) {
					duckdb_string_t *sv = (duckdb_string_t *)(in_data[col] + ((size_t)row * ctx->arg_sizes[col]));
					row_blob_values[col] = tcc_duckdb_string_to_blob(sv);
					arg_ptrs[col] = (void *)&row_blob_values[col];
				} else if (arg_value_bridges && arg_value_bridges[col]) {
					arg_ptrs[col] = (void *)(in_data[col] + ((size_t)row * ctx->arg_sizes[col]));
				} else if (tcc_ffi_type_is_list(ctx->arg_types[col])) {
					duckdb_list_entry entry = list_entries_columns[col][row];
					row_list_values[col].ptr =
					    list_child_data_columns[col]
					        ? (const void *)(list_child_data_columns[col] + ((size_t)entry.offset * list_child_sizes[col]))
					        : NULL;
					row_list_values[col].validity = list_child_validity_columns[col];
					row_list_values[col].offset = (uint64_t)entry.offset;
					row_list_values[col].len = (uint64_t)entry.length;
					arg_ptrs[col] = (void *)&row_list_values[col];
				} else if (tcc_ffi_type_is_array(ctx->arg_types[col])) {
					size_t array_len = ctx->arg_array_sizes ? ctx->arg_array_sizes[col] : 0;
					uint64_t off = (uint64_t)row * (uint64_t)array_len;
					row_array_values[col].ptr =
					    array_child_data_columns[col]
					        ? (const void *)(array_child_data_columns[col] + ((size_t)off * array_child_sizes[col]))
					        : NULL;
					row_array_values[col].validity = array_child_validity_columns[col];
					row_array_values[col].offset = off;
					row_array_values[col].len = (uint64_t)array_len;
					arg_ptrs[col] = (void *)&row_array_values[col];
				} else if (tcc_ffi_type_is_struct(ctx->arg_types[col])) {
					row_struct_values[col].field_ptrs = (const void *const *)struct_child_data_columns[col];
					row_struct_values[col].field_validity = (const uint64_t *const *)struct_child_validity_columns[col];
					row_struct_values[col].field_count = (uint64_t)struct_field_counts[col];
					row_struct_values[col].offset = (uint64_t)row;
					arg_ptrs[col] = (void *)&row_struct_values[col];
				} else if (tcc_ffi_type_is_map(ctx->arg_types[col])) {
					duckdb_list_entry entry = map_entries_columns[col][row];
					row_map_values[col].key_ptr =
					    map_key_data_columns[col]
					        ? (const void *)(map_key_data_columns[col] + ((size_t)entry.offset * map_key_sizes[col]))
					        : NULL;
					row_map_values[col].key_validity = map_key_validity_columns[col];
					row_map_values[col].value_ptr =
					    map_value_data_columns[col]
					        ? (const void *)(map_value_data_columns[col] + ((size_t)entry.offset * map_value_sizes[col]))
					        : NULL;
					row_map_values[col].value_validity = map_value_validity_columns[col];
					row_map_values[col].offset = (uint64_t)entry.offset;
					row_map_values[col].len = (uint64_t)entry.length;
					arg_ptrs[col] = (void *)&row_map_values[col];
				} else if (tcc_ffi_type_is_union(ctx->arg_types[col])) {
					row_union_values[col].tag_ptr = union_tag_columns[col];
					row_union_values[col].member_ptrs = (const void *const *)union_member_data_columns[col];
					row_union_values[col].member_validity = (const uint64_t *const *)union_member_validity_columns[col];
					row_union_values[col].member_count = (uint64_t)union_member_counts[col];
					row_union_values[col].offset = (uint64_t)row;
					arg_ptrs[col] = (void *)&row_union_values[col];
				} else {
					arg_ptrs[col] = (void *)(in_data[col] + ((size_t)row * ctx->arg_sizes[col]));
				}
			}
		if (!valid) {
			duckdb_validity_set_row_validity(out_validity, row, false);
			continue;
		}
			out_varchar_value = NULL;
			out_blob_value.ptr = NULL;
			out_blob_value.len = 0;
			out_list_value.ptr = NULL;
			out_list_value.validity = NULL;
			out_list_value.offset = 0;
			out_list_value.len = 0;
			out_array_value.ptr = NULL;
			out_array_value.validity = NULL;
			out_array_value.offset = 0;
			out_array_value.len = 0;
			out_struct_value.field_ptrs = NULL;
			out_struct_value.field_validity = NULL;
			out_struct_value.field_count = 0;
			out_struct_value.offset = 0;
			out_map_value.key_ptr = NULL;
			out_map_value.key_validity = NULL;
			out_map_value.value_ptr = NULL;
			out_map_value.value_validity = NULL;
			out_map_value.offset = 0;
			out_map_value.len = 0;
			out_union_value.tag_ptr = NULL;
			out_union_value.member_ptrs = NULL;
			out_union_value.member_validity = NULL;
			out_union_value.member_count = 0;
			out_union_value.offset = 0;
			{
				void *row_out_ptr = (void *)out_value;
				if (ctx->return_type == TCC_FFI_VARCHAR) {
					row_out_ptr = (void *)&out_varchar_value;
				} else if (ctx->return_type == TCC_FFI_BLOB) {
					row_out_ptr = (void *)&out_blob_value;
				} else if (tcc_ffi_type_is_list(ctx->return_type)) {
					row_out_ptr = (void *)&out_list_value;
				} else if (tcc_ffi_type_is_array(ctx->return_type)) {
					row_out_ptr = (void *)&out_array_value;
				} else if (tcc_ffi_type_is_struct(ctx->return_type)) {
					row_out_ptr = (void *)&out_struct_value;
				} else if (tcc_ffi_type_is_map(ctx->return_type)) {
					row_out_ptr = (void *)&out_map_value;
				} else if (tcc_ffi_type_is_union(ctx->return_type)) {
					row_out_ptr = (void *)&out_union_value;
				}
				row_result_base = row_out_ptr;
				if (!ctx->row_wrapper(arg_ptrs, row_out_ptr, &out_is_null)) {
					error = "ducktinycc invoke failed";
					goto cleanup;
				}
			}
		if (ctx->return_type == TCC_FFI_VOID || out_is_null) {
			duckdb_validity_set_row_validity(out_validity, row, false);
			continue;
		}
		if (ctx->return_type == TCC_FFI_VARCHAR) {
			if (!out_varchar_value) {
				duckdb_validity_set_row_validity(out_validity, row, false);
				continue;
			}
			duckdb_validity_set_row_validity(out_validity, row, true);
			duckdb_vector_assign_string_element(output, row, out_varchar_value);
			continue;
		}
			if (ctx->return_type == TCC_FFI_BLOB) {
				if (!out_blob_value.ptr) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
			}
			duckdb_validity_set_row_validity(out_validity, row, true);
				duckdb_vector_assign_string_element_len(output, row, (const char *)out_blob_value.ptr,
				                                        (idx_t)out_blob_value.len);
				continue;
			}
			if (tcc_typedesc_is_composite(return_desc)) {
				if (!tcc_write_value_to_vector(output, return_desc, row, row_result_base, 0, NULL, &error)) {
					goto cleanup;
				}
				continue;
			}
			if (tcc_ffi_type_is_list(ctx->return_type)) {
				idx_t elem_idx;
				uint8_t *child_data;
				uint64_t *child_validity;
				if (!out_list_value.ptr) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				if (duckdb_list_vector_reserve(output, out_list_child_count + (idx_t)out_list_value.len) != DuckDBSuccess) {
					error = "ducktinycc list return reserve failed";
					goto cleanup;
				}
				if (duckdb_list_vector_set_size(output, out_list_child_count + (idx_t)out_list_value.len) != DuckDBSuccess) {
					error = "ducktinycc list return set_size failed";
					goto cleanup;
				}
				child_data = (uint8_t *)duckdb_vector_get_data(out_list_child_vector);
				if (out_list_value.len > 0 && child_data) {
					memcpy(child_data + ((size_t)out_list_child_count * out_list_child_size), out_list_value.ptr,
					       (size_t)out_list_value.len * out_list_child_size);
				}
				duckdb_vector_ensure_validity_writable(out_list_child_vector);
				child_validity = duckdb_vector_get_validity(out_list_child_vector);
				for (elem_idx = 0; elem_idx < (idx_t)out_list_value.len; elem_idx++) {
					bool child_is_valid = true;
					if (out_list_value.validity) {
						uint64_t src_idx = out_list_value.offset + (uint64_t)elem_idx;
						child_is_valid =
						    (out_list_value.validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
					}
					duckdb_validity_set_row_validity(child_validity, out_list_child_count + elem_idx, child_is_valid);
				}
				duckdb_validity_set_row_validity(out_validity, row, true);
				out_list_entries[row].offset = out_list_child_count;
				out_list_entries[row].length = (idx_t)out_list_value.len;
				out_list_child_count += (idx_t)out_list_value.len;
				continue;
			}
			if (tcc_ffi_type_is_array(ctx->return_type)) {
				idx_t elem_idx;
				uint8_t *child_data;
				uint64_t *child_validity;
				size_t expected_len = out_array_len;
				if (!out_array_value.ptr) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				if ((size_t)out_array_value.len != expected_len) {
					error = "ducktinycc array return length mismatch";
					goto cleanup;
				}
				child_data = (uint8_t *)duckdb_vector_get_data(out_array_child_vector);
				if (expected_len > 0 && child_data) {
					memcpy(child_data + (((size_t)row * expected_len) * out_array_child_size), out_array_value.ptr,
					       expected_len * out_array_child_size);
				}
				duckdb_vector_ensure_validity_writable(out_array_child_vector);
				child_validity = duckdb_vector_get_validity(out_array_child_vector);
				for (elem_idx = 0; elem_idx < (idx_t)expected_len; elem_idx++) {
					bool child_is_valid = true;
					if (out_array_value.validity) {
						uint64_t src_idx = out_array_value.offset + (uint64_t)elem_idx;
						child_is_valid =
						    (out_array_value.validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
					}
					duckdb_validity_set_row_validity(child_validity, ((idx_t)row * (idx_t)expected_len) + elem_idx,
					                                 child_is_valid);
				}
				duckdb_validity_set_row_validity(out_validity, row, true);
				continue;
			}
			if (tcc_ffi_type_is_struct(ctx->return_type)) {
				idx_t field_idx;
				if (!out_struct_value.field_ptrs || out_struct_value.field_count != (uint64_t)out_struct_field_count) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				for (field_idx = 0; field_idx < out_struct_field_count; field_idx++) {
					const uint8_t *src_base = (const uint8_t *)out_struct_value.field_ptrs[field_idx];
					uint8_t *dst;
					uint64_t *field_validity;
					bool field_is_valid = true;
					if (!src_base) {
						duckdb_validity_set_row_validity(out_validity, row, false);
						break;
					}
					dst = out_struct_child_data[field_idx] + ((size_t)row * out_struct_child_sizes[field_idx]);
					memcpy(dst, src_base + ((size_t)out_struct_value.offset * out_struct_child_sizes[field_idx]),
					       out_struct_child_sizes[field_idx]);
					duckdb_vector_ensure_validity_writable(duckdb_struct_vector_get_child(output, field_idx));
					field_validity = duckdb_vector_get_validity(duckdb_struct_vector_get_child(output, field_idx));
					if (out_struct_value.field_validity && out_struct_value.field_validity[field_idx]) {
						field_is_valid = (out_struct_value.field_validity[field_idx][out_struct_value.offset >> 6] &
						                  (1ULL << (out_struct_value.offset & 63))) != 0;
					}
					duckdb_validity_set_row_validity(field_validity, row, field_is_valid);
				}
				if (!duckdb_validity_row_is_valid(out_validity, row)) {
					continue;
				}
				duckdb_validity_set_row_validity(out_validity, row, true);
				continue;
			}
			if (tcc_ffi_type_is_map(ctx->return_type)) {
				idx_t elem_idx;
				uint8_t *key_data;
				uint8_t *value_data;
				uint64_t *key_validity;
				uint64_t *value_validity;
				if (out_map_value.len > 0 && (!out_map_value.key_ptr || !out_map_value.value_ptr)) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				if (duckdb_list_vector_reserve(output, out_map_child_count + (idx_t)out_map_value.len) != DuckDBSuccess) {
					error = "ducktinycc map return reserve failed";
					goto cleanup;
				}
				if (duckdb_list_vector_set_size(output, out_map_child_count + (idx_t)out_map_value.len) != DuckDBSuccess) {
					error = "ducktinycc map return set_size failed";
					goto cleanup;
				}
				key_data = (uint8_t *)duckdb_vector_get_data(out_map_key_vector);
				value_data = (uint8_t *)duckdb_vector_get_data(out_map_value_vector);
				if (out_map_value.len > 0) {
					memcpy(key_data + ((size_t)out_map_child_count * out_map_key_size), out_map_value.key_ptr,
					       (size_t)out_map_value.len * out_map_key_size);
					memcpy(value_data + ((size_t)out_map_child_count * out_map_value_size), out_map_value.value_ptr,
					       (size_t)out_map_value.len * out_map_value_size);
				}
				duckdb_vector_ensure_validity_writable(out_map_key_vector);
				duckdb_vector_ensure_validity_writable(out_map_value_vector);
				key_validity = duckdb_vector_get_validity(out_map_key_vector);
				value_validity = duckdb_vector_get_validity(out_map_value_vector);
				for (elem_idx = 0; elem_idx < (idx_t)out_map_value.len; elem_idx++) {
					bool key_ok = true;
					bool value_ok = true;
					uint64_t src_idx = out_map_value.offset + (uint64_t)elem_idx;
					if (out_map_value.key_validity) {
						key_ok = (out_map_value.key_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
					}
					if (out_map_value.value_validity) {
						value_ok = (out_map_value.value_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
					}
					duckdb_validity_set_row_validity(key_validity, out_map_child_count + elem_idx, key_ok);
					duckdb_validity_set_row_validity(value_validity, out_map_child_count + elem_idx, value_ok);
				}
				out_map_entries[row].offset = out_map_child_count;
				out_map_entries[row].length = (idx_t)out_map_value.len;
				out_map_child_count += (idx_t)out_map_value.len;
				duckdb_validity_set_row_validity(out_validity, row, true);
				continue;
			}
			if (tcc_ffi_type_is_union(ctx->return_type)) {
				idx_t member_idx;
				uint8_t tag;
				if (!out_union_value.tag_ptr || !out_union_value.member_ptrs ||
				    out_union_value.member_count != (uint64_t)out_union_member_count) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				tag = out_union_value.tag_ptr[out_union_value.offset];
				if ((idx_t)tag >= out_union_member_count) {
					duckdb_validity_set_row_validity(out_validity, row, false);
					continue;
				}
				out_union_tags[row] = tag;
				for (member_idx = 0; member_idx < out_union_member_count; member_idx++) {
					duckdb_vector member_vector = duckdb_struct_vector_get_child(output, member_idx);
					uint64_t *member_validity;
					duckdb_vector_ensure_validity_writable(member_vector);
					member_validity = duckdb_vector_get_validity(member_vector);
					if (member_idx == (idx_t)tag) {
						const uint8_t *src_base = (const uint8_t *)out_union_value.member_ptrs[member_idx];
						bool member_is_valid = true;
						if (!src_base) {
							duckdb_validity_set_row_validity(out_validity, row, false);
							break;
						}
						memcpy(out_union_member_data[member_idx] + ((size_t)row * out_union_member_sizes[member_idx]),
						       src_base + ((size_t)out_union_value.offset * out_union_member_sizes[member_idx]),
						       out_union_member_sizes[member_idx]);
						if (out_union_value.member_validity && out_union_value.member_validity[member_idx]) {
							member_is_valid = (out_union_value.member_validity[member_idx][out_union_value.offset >> 6] &
							                   (1ULL << (out_union_value.offset & 63))) != 0;
						}
						duckdb_validity_set_row_validity(member_validity, row, member_is_valid);
					} else {
						duckdb_validity_set_row_validity(member_validity, row, false);
					}
				}
				if (!duckdb_validity_row_is_valid(out_validity, row)) {
					continue;
				}
				duckdb_validity_set_row_validity(out_validity, row, true);
				continue;
			}
			duckdb_validity_set_row_validity(out_validity, row, true);
			if (ret_size > 0) {
				memcpy(out_data + ((size_t)row * ret_size), out_value, ret_size);
			}
	}
cleanup:
	if (row_varchar_allocations) {
		for (row = 0; row < row_varchar_alloc_count; row++) {
			if (row_varchar_allocations[row]) {
				duckdb_free(row_varchar_allocations[row]);
			}
		}
	}
	if (batch_varchar_owned) {
		for (col = 0; col < ctx->arg_count; col++) {
			if (batch_varchar_owned[col]) {
				for (row = 0; row < n; row++) {
					if (batch_varchar_owned[col][row]) {
						duckdb_free(batch_varchar_owned[col][row]);
					}
				}
				duckdb_free(batch_varchar_owned[col]);
			}
		}
	}
	if (batch_varchar_columns) {
		for (col = 0; col < ctx->arg_count; col++) {
			if (batch_varchar_columns[col]) {
				duckdb_free((void *)batch_varchar_columns[col]);
			}
		}
	}
		if (batch_blob_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (batch_blob_columns[col]) {
					duckdb_free((void *)batch_blob_columns[col]);
				}
			}
		}
		if (batch_list_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (batch_list_columns[col]) {
					duckdb_free((void *)batch_list_columns[col]);
				}
			}
		}
		if (batch_array_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (batch_array_columns[col]) {
					duckdb_free((void *)batch_array_columns[col]);
				}
			}
		}
		if (batch_struct_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (batch_struct_columns[col]) {
					duckdb_free((void *)batch_struct_columns[col]);
				}
			}
		}
		if (batch_map_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (batch_map_columns[col]) {
					duckdb_free((void *)batch_map_columns[col]);
				}
			}
		}
		if (batch_union_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (batch_union_columns[col]) {
					duckdb_free((void *)batch_union_columns[col]);
				}
			}
		}
	if (batch_out_varchar) {
		duckdb_free((void *)batch_out_varchar);
	}
		if (batch_out_blob) {
			duckdb_free((void *)batch_out_blob);
		}
		if (batch_out_list) {
			duckdb_free((void *)batch_out_list);
		}
		if (batch_out_array) {
			duckdb_free((void *)batch_out_array);
		}
		if (batch_out_struct) {
			duckdb_free((void *)batch_out_struct);
		}
		if (batch_out_map) {
			duckdb_free((void *)batch_out_map);
		}
		if (batch_out_union) {
			duckdb_free((void *)batch_out_union);
		}
	if (in_data) {
		duckdb_free(in_data);
	}
	if (in_validity) {
		duckdb_free(in_validity);
	}
	if (synthetic_validity_columns) {
		for (col = 0; col < ctx->arg_count; col++) {
			if (synthetic_validity_columns[col]) {
				duckdb_free((void *)synthetic_validity_columns[col]);
			}
		}
		duckdb_free((void *)synthetic_validity_columns);
	}
	if (arg_ptrs) {
		duckdb_free(arg_ptrs);
	}
	if (batch_arg_data) {
		duckdb_free(batch_arg_data);
	}
	if (row_varchar_values) {
		duckdb_free((void *)row_varchar_values);
	}
		if (row_blob_values) {
			duckdb_free((void *)row_blob_values);
		}
		if (row_list_values) {
			duckdb_free((void *)row_list_values);
		}
		if (row_array_values) {
			duckdb_free((void *)row_array_values);
		}
		if (row_struct_values) {
			duckdb_free((void *)row_struct_values);
		}
		if (row_map_values) {
			duckdb_free((void *)row_map_values);
		}
		if (row_union_values) {
			duckdb_free((void *)row_union_values);
		}
		if (row_varchar_allocations) {
			duckdb_free((void *)row_varchar_allocations);
		}
	if (batch_varchar_columns) {
		duckdb_free((void *)batch_varchar_columns);
	}
		if (batch_blob_columns) {
			duckdb_free((void *)batch_blob_columns);
		}
		if (batch_list_columns) {
			duckdb_free((void *)batch_list_columns);
		}
		if (batch_array_columns) {
			duckdb_free((void *)batch_array_columns);
		}
		if (batch_struct_columns) {
			duckdb_free((void *)batch_struct_columns);
		}
		if (batch_map_columns) {
			duckdb_free((void *)batch_map_columns);
		}
		if (batch_union_columns) {
			duckdb_free((void *)batch_union_columns);
		}
		if (batch_varchar_owned) {
			duckdb_free((void *)batch_varchar_owned);
		}
		if (list_entries_columns) {
			duckdb_free((void *)list_entries_columns);
		}
		if (list_child_data_columns) {
			duckdb_free((void *)list_child_data_columns);
		}
		if (list_child_validity_columns) {
			duckdb_free((void *)list_child_validity_columns);
		}
		if (list_child_sizes) {
			duckdb_free((void *)list_child_sizes);
		}
		if (array_child_data_columns) {
			duckdb_free((void *)array_child_data_columns);
		}
		if (array_child_validity_columns) {
			duckdb_free((void *)array_child_validity_columns);
		}
		if (array_child_sizes) {
			duckdb_free((void *)array_child_sizes);
		}
		if (arg_value_bridges) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (arg_value_bridges[col]) {
					tcc_value_bridge_destroy(arg_value_bridges[col]);
				}
			}
			duckdb_free((void *)arg_value_bridges);
		}
		if (arg_struct_nested_bridges) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (arg_struct_nested_bridges[col]) {
					idx_t field_count = struct_field_counts ? struct_field_counts[col] : 0;
					idx_t field_idx;
					for (field_idx = 0; field_idx < field_count; field_idx++) {
						tcc_nested_struct_bridge_t *bridge = arg_struct_nested_bridges[col][field_idx];
						if (bridge) {
							tcc_nested_struct_bridge_destroy(bridge);
						}
					}
					duckdb_free((void *)arg_struct_nested_bridges[col]);
				}
			}
			duckdb_free((void *)arg_struct_nested_bridges);
		}
		if (struct_child_data_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (struct_child_data_columns[col]) {
					duckdb_free((void *)struct_child_data_columns[col]);
				}
			}
			duckdb_free((void *)struct_child_data_columns);
		}
		if (struct_child_validity_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (struct_child_validity_columns[col]) {
					duckdb_free((void *)struct_child_validity_columns[col]);
				}
			}
			duckdb_free((void *)struct_child_validity_columns);
		}
		if (struct_child_sizes) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (struct_child_sizes[col]) {
					duckdb_free((void *)struct_child_sizes[col]);
				}
			}
			duckdb_free((void *)struct_child_sizes);
		}
		if (struct_field_counts) {
			duckdb_free((void *)struct_field_counts);
		}
		if (map_entries_columns) {
			duckdb_free((void *)map_entries_columns);
		}
		if (map_key_data_columns) {
			duckdb_free((void *)map_key_data_columns);
		}
		if (map_key_validity_columns) {
			duckdb_free((void *)map_key_validity_columns);
		}
		if (map_key_sizes) {
			duckdb_free((void *)map_key_sizes);
		}
		if (map_value_data_columns) {
			duckdb_free((void *)map_value_data_columns);
		}
		if (map_value_validity_columns) {
			duckdb_free((void *)map_value_validity_columns);
		}
		if (map_value_sizes) {
			duckdb_free((void *)map_value_sizes);
		}
		if (union_tag_columns) {
			duckdb_free((void *)union_tag_columns);
		}
		if (union_member_data_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (union_member_data_columns[col]) {
					duckdb_free((void *)union_member_data_columns[col]);
				}
			}
			duckdb_free((void *)union_member_data_columns);
		}
		if (union_member_validity_columns) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (union_member_validity_columns[col]) {
					duckdb_free((void *)union_member_validity_columns[col]);
				}
			}
			duckdb_free((void *)union_member_validity_columns);
		}
		if (union_member_sizes) {
			for (col = 0; col < ctx->arg_count; col++) {
				if (union_member_sizes[col]) {
					duckdb_free((void *)union_member_sizes[col]);
				}
			}
			duckdb_free((void *)union_member_sizes);
		}
		if (union_member_counts) {
			duckdb_free((void *)union_member_counts);
		}
		if (out_struct_child_data) {
			duckdb_free((void *)out_struct_child_data);
		}
		if (out_struct_child_validity) {
			duckdb_free((void *)out_struct_child_validity);
		}
		if (out_struct_child_sizes) {
			duckdb_free((void *)out_struct_child_sizes);
		}
		if (out_union_member_data) {
			duckdb_free((void *)out_union_member_data);
		}
		if (out_union_member_validity) {
			duckdb_free((void *)out_union_member_validity);
		}
		if (out_union_member_sizes) {
			duckdb_free((void *)out_union_member_sizes);
		}
	if (error) {
		duckdb_scalar_function_set_error(info, error);
	}
}

/* Registers a generated wrapper against DuckDB.
 * Ownership transfer:
 * - Parsed signature metadata is owned locally until assigned into `ctx`.
 * - After assignment, `ctx` owns it and `tcc_host_sig_ctx_destroy` releases it.
 */
static bool ducktinycc_register_signature(duckdb_connection con, const char *name, void *fn_ptr,
                                          const char *return_type, const char *arg_types_csv,
                                          const char *wrapper_mode) {
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
		return false;
	}
	if (!tcc_typedesc_parse_token(return_type, true, &return_desc, &err)) {
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
		return false;
	}
	if (!tcc_split_csv_tokens(arg_types_csv, &arg_tokens, &err)) {
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
		tcc_typedesc_destroy(return_desc);
		return false;
	}
	if ((int)arg_tokens.count != arg_count) {
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
		tcc_typedesc_destroy(return_desc);
		tcc_string_list_destroy(&arg_tokens);
		return false;
	}
	if (arg_count > 0) {
		arg_descs = (tcc_typedesc_t **)duckdb_malloc(sizeof(tcc_typedesc_t *) * (size_t)arg_count);
		if (!arg_descs) {
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
			tcc_typedesc_destroy(return_desc);
			tcc_string_list_destroy(&arg_tokens);
			return false;
		}
		memset(arg_descs, 0, sizeof(tcc_typedesc_t *) * (size_t)arg_count);
		for (i = 0; i < arg_count; i++) {
			if (!tcc_typedesc_parse_token(arg_tokens.items[i], false, &arg_descs[i], &err)) {
				int j;
				for (j = 0; j < i; j++) {
					if (arg_descs[j]) {
						tcc_typedesc_destroy(arg_descs[j]);
					}
				}
				duckdb_free(arg_descs);
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
				tcc_typedesc_destroy(return_desc);
				tcc_string_list_destroy(&arg_tokens);
				return false;
			}
		}
	}
	tcc_string_list_destroy(&arg_tokens);

	fn = duckdb_create_scalar_function();
	if (!fn) {
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
	ctx = (tcc_host_sig_ctx_t *)duckdb_malloc(sizeof(tcc_host_sig_ctx_t));
	if (!ctx) {
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
		duckdb_destroy_scalar_function(&fn);
		return false;
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
	duckdb_scalar_function_set_function(fn, tcc_execute_compiled_scalar_udf);
	duckdb_scalar_function_set_extra_info(fn, ctx, tcc_host_sig_ctx_destroy);
	rc = duckdb_register_scalar_function(con, fn);
	if (rc != DuckDBSuccess) {
		duckdb_destroy_scalar_function(&fn);
		return false;
	}
	return true;
}

/* ducktinycc_valid_is_set: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_valid_is_set(const uint64_t *validity, uint64_t idx) {
	if (!validity) {
		return 1;
	}
	return (validity[idx >> 6] & (1ULL << (idx & 63))) != 0;
}

/* ducktinycc_valid_set: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void ducktinycc_valid_set(uint64_t *validity, uint64_t idx, int valid) {
	uint64_t bit;
	if (!validity) {
		return;
	}
	bit = 1ULL << (idx & 63);
	if (valid) {
		validity[idx >> 6] |= bit;
	} else {
		validity[idx >> 6] &= ~bit;
	}
}

/* ducktinycc_span_contains: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_span_contains(uint64_t len, uint64_t idx) {
	return idx < len;
}

static const void *ducktinycc_ptr_add(const void *base, uint64_t byte_offset) {
	const uint8_t *p = (const uint8_t *)base;
	if (!p) {
		return NULL;
	}
	return (const void *)(p + byte_offset);
}

static void *ducktinycc_ptr_add_mut(void *base, uint64_t byte_offset) {
	uint8_t *p = (uint8_t *)base;
	if (!p) {
		return NULL;
	}
	return (void *)(p + byte_offset);
}

/* ducktinycc_span_fits: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_span_fits(uint64_t len, uint64_t offset, uint64_t width) {
	if (offset > len) {
		return 0;
	}
	return width <= (len - offset);
}

static const void *ducktinycc_buf_ptr_at(const void *base, uint64_t len, uint64_t offset, uint64_t width) {
	if (!base || !ducktinycc_span_fits(len, offset, width)) {
		return NULL;
	}
	return ducktinycc_ptr_add(base, offset);
}

static void *ducktinycc_buf_ptr_at_mut(void *base, uint64_t len, uint64_t offset, uint64_t width) {
	if (!base || !ducktinycc_span_fits(len, offset, width)) {
		return NULL;
	}
	return ducktinycc_ptr_add_mut(base, offset);
}

/* ducktinycc_read_bytes: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_read_bytes(const void *base, uint64_t len, uint64_t offset, void *out, uint64_t width) {
	const void *src;
	if (!out && width > 0) {
		return 0;
	}
	if (width == 0) {
		return 1;
	}
	src = ducktinycc_buf_ptr_at(base, len, offset, width);
	if (!src) {
		return 0;
	}
	memcpy(out, src, (size_t)width);
	return 1;
}

/* ducktinycc_write_bytes: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_write_bytes(void *base, uint64_t len, uint64_t offset, const void *in, uint64_t width) {
	void *dst;
	if (!in && width > 0) {
		return 0;
	}
	if (width == 0) {
		return 1;
	}
	dst = ducktinycc_buf_ptr_at_mut(base, len, offset, width);
	if (!dst) {
		return 0;
	}
	memcpy(dst, in, (size_t)width);
	return 1;
}

#define DUCKTINYCC_DEFINE_BUF_RW(name, type)                                                                                \
	static int ducktinycc_read_##name(const void *base, uint64_t len, uint64_t offset, type *out) {                     \
		return ducktinycc_read_bytes(base, len, offset, out, (uint64_t)sizeof(type));                               \
	}                                                                                                                      \
	static int ducktinycc_write_##name(void *base, uint64_t len, uint64_t offset, type value) {                        \
		return ducktinycc_write_bytes(base, len, offset, &value, (uint64_t)sizeof(type));                          \
	}

DUCKTINYCC_DEFINE_BUF_RW(i8, int8_t)
DUCKTINYCC_DEFINE_BUF_RW(u8, uint8_t)
DUCKTINYCC_DEFINE_BUF_RW(i16, int16_t)
DUCKTINYCC_DEFINE_BUF_RW(u16, uint16_t)
DUCKTINYCC_DEFINE_BUF_RW(i32, int32_t)
DUCKTINYCC_DEFINE_BUF_RW(u32, uint32_t)
DUCKTINYCC_DEFINE_BUF_RW(i64, int64_t)
DUCKTINYCC_DEFINE_BUF_RW(u64, uint64_t)
DUCKTINYCC_DEFINE_BUF_RW(f32, float)
DUCKTINYCC_DEFINE_BUF_RW(f64, double)

#undef DUCKTINYCC_DEFINE_BUF_RW

/* ducktinycc_read_ptr: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_read_ptr(const void *base, uint64_t len, uint64_t offset, const void **out) {
	uintptr_t tmp = 0;
	if (!out) {
		return 0;
	}
	if (!ducktinycc_read_bytes(base, len, offset, &tmp, (uint64_t)sizeof(uintptr_t))) {
		return 0;
	}
	*out = (const void *)tmp;
	return 1;
}

/* ducktinycc_write_ptr: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_write_ptr(void *base, uint64_t len, uint64_t offset, const void *value) {
	uintptr_t tmp = (uintptr_t)value;
	return ducktinycc_write_bytes(base, len, offset, &tmp, (uint64_t)sizeof(uintptr_t));
}

/* ducktinycc_list_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_list_is_valid(const ducktinycc_list_t *list, uint64_t idx) {
	uint64_t global_idx;
	if (!list || idx >= list->len) {
		return 0;
	}
	if (!list->validity) {
		return 1;
	}
	global_idx = list->offset + idx;
	return ducktinycc_valid_is_set(list->validity, global_idx);
}

static const void *ducktinycc_list_elem_ptr(const ducktinycc_list_t *list, uint64_t idx, uint64_t elem_size) {
	uint64_t global_idx;
	if (!list || !list->ptr || idx >= list->len || elem_size == 0) {
		return NULL;
	}
	global_idx = list->offset + idx;
	return ducktinycc_ptr_add(list->ptr, global_idx * elem_size);
}

/* ducktinycc_array_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_array_is_valid(const ducktinycc_array_t *arr, uint64_t idx) {
	uint64_t global_idx;
	if (!arr || idx >= arr->len) {
		return 0;
	}
	if (!arr->validity) {
		return 1;
	}
	global_idx = arr->offset + idx;
	return ducktinycc_valid_is_set(arr->validity, global_idx);
}

static const void *ducktinycc_array_elem_ptr(const ducktinycc_array_t *arr, uint64_t idx, uint64_t elem_size) {
	uint64_t global_idx;
	if (!arr || !arr->ptr || idx >= arr->len || elem_size == 0) {
		return NULL;
	}
	global_idx = arr->offset + idx;
	return ducktinycc_ptr_add(arr->ptr, global_idx * elem_size);
}

static const void *ducktinycc_struct_field_ptr(const ducktinycc_struct_t *st, uint64_t idx) {
	if (!st || !st->field_ptrs || idx >= st->field_count) {
		return NULL;
	}
	return st->field_ptrs[idx];
}

/* ducktinycc_struct_field_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_struct_field_is_valid(const ducktinycc_struct_t *st, uint64_t field_idx) {
	if (!st || !st->field_ptrs || field_idx >= st->field_count) {
		return 0;
	}
	if (!st->field_validity || !st->field_validity[field_idx]) {
		return 1;
	}
	return ducktinycc_valid_is_set(st->field_validity[field_idx], st->offset);
}

static const void *ducktinycc_map_key_ptr(const ducktinycc_map_t *m, uint64_t idx, uint64_t key_size) {
	uint64_t global_idx;
	if (!m || !m->key_ptr || idx >= m->len || key_size == 0) {
		return NULL;
	}
	global_idx = m->offset + idx;
	return ducktinycc_ptr_add(m->key_ptr, global_idx * key_size);
}

static const void *ducktinycc_map_value_ptr(const ducktinycc_map_t *m, uint64_t idx, uint64_t value_size) {
	uint64_t global_idx;
	if (!m || !m->value_ptr || idx >= m->len || value_size == 0) {
		return NULL;
	}
	global_idx = m->offset + idx;
	return ducktinycc_ptr_add(m->value_ptr, global_idx * value_size);
}

/* ducktinycc_map_key_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_map_key_is_valid(const ducktinycc_map_t *m, uint64_t idx) {
	uint64_t global_idx;
	if (!m || idx >= m->len) {
		return 0;
	}
	if (!m->key_validity) {
		return 1;
	}
	global_idx = m->offset + idx;
	return ducktinycc_valid_is_set(m->key_validity, global_idx);
}

/* ducktinycc_map_value_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_map_value_is_valid(const ducktinycc_map_t *m, uint64_t idx) {
	uint64_t global_idx;
	if (!m || idx >= m->len) {
		return 0;
	}
	if (!m->value_validity) {
		return 1;
	}
	global_idx = m->offset + idx;
	return ducktinycc_valid_is_set(m->value_validity, global_idx);
}

#define TCC_HOST_SYMBOL_TABLE(X)                                                                                          \
	X("duckdb_ext_api", &duckdb_ext_api)                                                                                 \
	X("ducktinycc_register_signature", ducktinycc_register_signature)                                                    \
	X("ducktinycc_valid_is_set", ducktinycc_valid_is_set)                                                                \
	X("ducktinycc_valid_set", ducktinycc_valid_set)                                                                      \
	X("ducktinycc_span_contains", ducktinycc_span_contains)                                                              \
	X("ducktinycc_ptr_add", ducktinycc_ptr_add)                                                                          \
	X("ducktinycc_ptr_add_mut", ducktinycc_ptr_add_mut)                                                                  \
	X("ducktinycc_span_fits", ducktinycc_span_fits)                                                                      \
	X("ducktinycc_buf_ptr_at", ducktinycc_buf_ptr_at)                                                                    \
	X("ducktinycc_buf_ptr_at_mut", ducktinycc_buf_ptr_at_mut)                                                            \
	X("ducktinycc_read_bytes", ducktinycc_read_bytes)                                                                    \
	X("ducktinycc_write_bytes", ducktinycc_write_bytes)                                                                  \
	X("ducktinycc_read_i8", ducktinycc_read_i8)                                                                          \
	X("ducktinycc_write_i8", ducktinycc_write_i8)                                                                        \
	X("ducktinycc_read_u8", ducktinycc_read_u8)                                                                          \
	X("ducktinycc_write_u8", ducktinycc_write_u8)                                                                        \
	X("ducktinycc_read_i16", ducktinycc_read_i16)                                                                        \
	X("ducktinycc_write_i16", ducktinycc_write_i16)                                                                      \
	X("ducktinycc_read_u16", ducktinycc_read_u16)                                                                        \
	X("ducktinycc_write_u16", ducktinycc_write_u16)                                                                      \
	X("ducktinycc_read_i32", ducktinycc_read_i32)                                                                        \
	X("ducktinycc_write_i32", ducktinycc_write_i32)                                                                      \
	X("ducktinycc_read_u32", ducktinycc_read_u32)                                                                        \
	X("ducktinycc_write_u32", ducktinycc_write_u32)                                                                      \
	X("ducktinycc_read_i64", ducktinycc_read_i64)                                                                        \
	X("ducktinycc_write_i64", ducktinycc_write_i64)                                                                      \
	X("ducktinycc_read_u64", ducktinycc_read_u64)                                                                        \
	X("ducktinycc_write_u64", ducktinycc_write_u64)                                                                      \
	X("ducktinycc_read_f32", ducktinycc_read_f32)                                                                        \
	X("ducktinycc_write_f32", ducktinycc_write_f32)                                                                      \
	X("ducktinycc_read_f64", ducktinycc_read_f64)                                                                        \
	X("ducktinycc_write_f64", ducktinycc_write_f64)                                                                      \
	X("ducktinycc_read_ptr", ducktinycc_read_ptr)                                                                        \
	X("ducktinycc_write_ptr", ducktinycc_write_ptr)                                                                      \
	X("ducktinycc_list_is_valid", ducktinycc_list_is_valid)                                                              \
	X("ducktinycc_list_elem_ptr", ducktinycc_list_elem_ptr)                                                              \
	X("ducktinycc_array_is_valid", ducktinycc_array_is_valid)                                                            \
	X("ducktinycc_array_elem_ptr", ducktinycc_array_elem_ptr)                                                            \
	X("ducktinycc_struct_field_ptr", ducktinycc_struct_field_ptr)                                                        \
	X("ducktinycc_struct_field_is_valid", ducktinycc_struct_field_is_valid)                                              \
	X("ducktinycc_map_key_ptr", ducktinycc_map_key_ptr)                                                                  \
	X("ducktinycc_map_value_ptr", ducktinycc_map_value_ptr)                                                              \
	X("ducktinycc_map_key_is_valid", ducktinycc_map_key_is_valid)                                                        \
	X("ducktinycc_map_value_is_valid", ducktinycc_map_value_is_valid)

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* tcc_add_host_symbols: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_add_host_symbols(TCCState *s) {
	if (!s) {
		return;
	}
#define TCC_ADD_HOST_SYMBOL(name, ptr) (void)tcc_add_symbol(s, name, ptr);
	TCC_HOST_SYMBOL_TABLE(TCC_ADD_HOST_SYMBOL)
#undef TCC_ADD_HOST_SYMBOL
}
#undef TCC_HOST_SYMBOL_TABLE

/* tcc_artifact_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_artifact_destroy(void *ptr) {
	tcc_registered_artifact_t *artifact = (tcc_registered_artifact_t *)ptr;
	if (!artifact) {
		return;
	}
	if (artifact->tcc) {
		tcc_delete(artifact->tcc);
	}
	if (artifact->sql_name) {
		duckdb_free(artifact->sql_name);
	}
	if (artifact->symbol) {
		duckdb_free(artifact->symbol);
	}
	duckdb_free(artifact);
}

/* tcc_apply_session_to_state: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static int tcc_apply_session_to_state(TCCState *s, const tcc_session_t *session, tcc_error_buffer_t *error_buf) {
	idx_t i;
	for (i = 0; i < session->include_paths.count; i++) {
		if (tcc_add_include_path(s, session->include_paths.items[i]) != 0) {
			tcc_set_error(error_buf, "tcc_add_include_path failed");
			return -1;
		}
	}
	for (i = 0; i < session->sysinclude_paths.count; i++) {
		if (tcc_add_sysinclude_path(s, session->sysinclude_paths.items[i]) != 0) {
			tcc_set_error(error_buf, "tcc_add_sysinclude_path failed");
			return -1;
		}
	}
	for (i = 0; i < session->library_paths.count; i++) {
		if (tcc_add_library_path(s, session->library_paths.items[i]) != 0) {
			tcc_set_error(error_buf, "tcc_add_library_path failed");
			return -1;
		}
	}
	for (i = 0; i < session->options.count; i++) {
		tcc_set_options(s, session->options.items[i]);
	}
	for (i = 0; i < session->define_names.count; i++) {
		tcc_define_symbol(s, session->define_names.items[i], session->define_values.items[i]);
	}
	for (i = 0; i < session->headers.count; i++) {
		if (tcc_compile_string(s, session->headers.items[i]) != 0) {
			if (error_buf->message[0] == '\0') {
				tcc_set_error(error_buf, "header compile failed");
			}
			return -1;
		}
	}
	for (i = 0; i < session->sources.count; i++) {
		if (tcc_compile_string(s, session->sources.items[i]) != 0) {
			if (error_buf->message[0] == '\0') {
				tcc_set_error(error_buf, "source compile failed");
			}
			return -1;
		}
	}
	for (i = 0; i < session->libraries.count; i++) {
		if (tcc_add_library(s, session->libraries.items[i]) != 0) {
			tcc_set_error(error_buf, "tcc_add_library failed");
			return -1;
		}
	}
	return 0;
}

/* tcc_apply_bind_overrides_to_state: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static int tcc_apply_bind_overrides_to_state(TCCState *s, const tcc_module_bind_data_t *bind,
                                             tcc_error_buffer_t *error_buf) {
	const char *define_value;
	if (!s || !bind) {
		return 0;
	}
	if (bind->include_path && bind->include_path[0] != '\0') {
		if (tcc_add_include_path(s, bind->include_path) != 0) {
			tcc_set_error(error_buf, "tcc_add_include_path failed");
			return -1;
		}
	}
	if (bind->sysinclude_path && bind->sysinclude_path[0] != '\0') {
		if (tcc_add_sysinclude_path(s, bind->sysinclude_path) != 0) {
			tcc_set_error(error_buf, "tcc_add_sysinclude_path failed");
			return -1;
		}
	}
	if (bind->library_path && bind->library_path[0] != '\0') {
		if (tcc_add_library_path(s, bind->library_path) != 0) {
			tcc_set_error(error_buf, "tcc_add_library_path failed");
			return -1;
		}
	}
	if (bind->option && bind->option[0] != '\0') {
		tcc_set_options(s, bind->option);
	}
	if (bind->define_name && bind->define_name[0] != '\0') {
		define_value = bind->define_value ? bind->define_value : "1";
		tcc_define_symbol(s, bind->define_name, define_value);
	}
	if (bind->header && bind->header[0] != '\0') {
		if (tcc_compile_string(s, bind->header) != 0) {
			if (error_buf->message[0] == '\0') {
				tcc_set_error(error_buf, "header compile failed");
			}
			return -1;
		}
	}
	if (bind->library && bind->library[0] != '\0') {
		if (tcc_add_library(s, bind->library) != 0) {
			tcc_set_error(error_buf, "tcc_add_library failed");
			return -1;
		}
	}
	return 0;
}

/* Builds and relocates one TinyCC module artifact, returning its init symbol wrapper. */
static int tcc_build_module_artifact(const char *runtime_path, tcc_module_state_t *state,
                                     const tcc_module_bind_data_t *bind, const char *module_symbol,
                                     const char *module_name, tcc_registered_artifact_t **out_artifact,
                                     tcc_error_buffer_t *error_buf) {
	TCCState *s;
	void *sym;
	tcc_registered_artifact_t *artifact;
	if (!module_symbol || module_symbol[0] == '\0') {
		tcc_set_error(error_buf, "module symbol is required");
		return -1;
	}
	if (!module_name || module_name[0] == '\0') {
		tcc_set_error(error_buf, "module name is required");
		return -1;
	}
	if (state->session.sources.count == 0 && (!bind->source || bind->source[0] == '\0')) {
		tcc_set_error(error_buf, "no source provided (use add_source/source)");
		return -1;
	}

	s = tcc_new();
	if (!s) {
		tcc_set_error(error_buf, "tcc_new failed");
		return -1;
	}
	tcc_set_error_func(s, error_buf, tcc_append_error);
	if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) != 0) {
		tcc_set_error(error_buf, "tcc_set_output_type failed");
		tcc_delete(s);
		return -1;
	}
	tcc_configure_runtime_paths(s, runtime_path);
	tcc_add_host_symbols(s);
	if (tcc_apply_session_to_state(s, &state->session, error_buf) != 0) {
		tcc_delete(s);
		return -1;
	}
	if (tcc_apply_bind_overrides_to_state(s, bind, error_buf) != 0) {
		tcc_delete(s);
		return -1;
	}
	if (bind->source && bind->source[0] != '\0') {
		if (tcc_compile_string(s, bind->source) != 0) {
			if (error_buf->message[0] == '\0') {
				tcc_set_error(error_buf, "source compile failed");
			}
			tcc_delete(s);
			return -1;
		}
	}
	if (tcc_relocate(s) != 0) {
		if (error_buf->message[0] == '\0') {
			tcc_set_error(error_buf, "tcc_relocate failed");
		}
		tcc_delete(s);
		return -1;
	}
	sym = tcc_get_symbol(s, module_symbol);
	if (!sym) {
		tcc_set_error(error_buf, "module symbol not found after relocation");
		tcc_delete(s);
		return -1;
	}

	artifact = (tcc_registered_artifact_t *)duckdb_malloc(sizeof(tcc_registered_artifact_t));
	if (!artifact) {
		tcc_set_error(error_buf, "out of memory");
		tcc_delete(s);
		return -1;
	}
	memset(artifact, 0, sizeof(tcc_registered_artifact_t));
	artifact->tcc = s;
	artifact->is_module = true;
	artifact->module_init = (tcc_dynamic_init_fn_t)sym;
	artifact->sql_name = tcc_strdup(module_name);
	artifact->symbol = tcc_strdup(module_symbol);
	artifact->state_id = state->session.state_id;
	if (!artifact->module_init || !artifact->sql_name || !artifact->symbol) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "invalid module artifact or out of memory");
		return -1;
	}
	*out_artifact = artifact;
	return 0;
}
#endif

/* Finds compiled-artifact registry index by SQL function name. */
static idx_t tcc_registry_find_sql_name(tcc_module_state_t *state, const char *sql_name) {
	idx_t i;
	if (!state || !sql_name) {
		return (idx_t)-1;
	}
	for (i = 0; i < state->entry_count; i++) {
		if (state->entries[i].sql_name && strcmp(state->entries[i].sql_name, sql_name) == 0) {
			return i;
		}
	}
	return (idx_t)-1;
}

/* Ensures registry storage capacity for compiled artifact metadata. */
static bool tcc_registry_reserve(tcc_module_state_t *state, idx_t wanted) {
	tcc_registered_entry_t *new_entries;
	idx_t new_capacity;
	if (state->entry_capacity >= wanted) {
		return true;
	}
	new_capacity = state->entry_capacity == 0 ? 8 : state->entry_capacity * 2;
	while (new_capacity < wanted) {
		new_capacity *= 2;
	}
	new_entries = (tcc_registered_entry_t *)duckdb_malloc(sizeof(tcc_registered_entry_t) * new_capacity);
	if (!new_entries) {
		return false;
	}
	memset(new_entries, 0, sizeof(tcc_registered_entry_t) * new_capacity);
	if (state->entries && state->entry_count > 0) {
		memcpy(new_entries, state->entries, sizeof(tcc_registered_entry_t) * state->entry_count);
		duckdb_free(state->entries);
	}
	state->entries = new_entries;
	state->entry_capacity = new_capacity;
	return true;
}

/* Releases metadata (and underlying artifacts) for one registry entry. */
static void tcc_registry_entry_destroy_metadata(tcc_registered_entry_t *entry) {
	if (!entry) {
		return;
	}
	if (entry->sql_name) {
		duckdb_free(entry->sql_name);
		entry->sql_name = NULL;
	}
	if (entry->symbol) {
		duckdb_free(entry->symbol);
		entry->symbol = NULL;
	}
	entry->state_id = 0;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	if (entry->artifact) {
		tcc_artifact_destroy(entry->artifact);
	}
	entry->artifact = NULL;
#endif
}

/* Inserts/replaces compiled artifact metadata for one SQL function name. */
static bool tcc_registry_store_metadata(tcc_module_state_t *state, const char *sql_name, const char *symbol,
                                        uint64_t state_id
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
                                        , tcc_registered_artifact_t *artifact
#endif
) {
	idx_t idx = tcc_registry_find_sql_name(state, sql_name);
	tcc_registered_entry_t *entry;
	if (idx == (idx_t)-1) {
		if (!tcc_registry_reserve(state, state->entry_count + 1)) {
			return false;
		}
		idx = state->entry_count++;
	}
	entry = &state->entries[idx];
	tcc_registry_entry_destroy_metadata(entry);
	entry->sql_name = tcc_strdup(sql_name);
	entry->symbol = tcc_strdup(symbol);
	entry->state_id = state_id;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	entry->artifact = artifact;
#endif
	if (!entry->sql_name || !entry->symbol) {
		tcc_registry_entry_destroy_metadata(entry);
		return false;
	}
	return true;
}

/* Destructor for table-function extra info (`tcc_module_state_t`). */
static void destroy_tcc_module_state(void *ptr) {
	tcc_module_state_t *state = (tcc_module_state_t *)ptr;
	idx_t i;
	if (!state) {
		return;
	}
	for (i = 0; i < state->entry_count; i++) {
		tcc_registry_entry_destroy_metadata(&state->entries[i]);
	}
	if (state->entries) {
		duckdb_free(state->entries);
	}
	if (state->ptr_registry) {
		tcc_ptr_registry_unref(state->ptr_registry);
		state->ptr_registry = NULL;
	}
	if (state->session.runtime_path) {
		duckdb_free(state->session.runtime_path);
	}
	tcc_session_clear_build_state(&state->session);
	duckdb_free(state);
}

/* Destructor for per-invocation bind payload of `tcc_module(...)`. */
static void destroy_tcc_module_bind_data(void *ptr) {
	tcc_module_bind_data_t *bind = (tcc_module_bind_data_t *)ptr;
	if (!bind) {
		return;
	}
	if (bind->mode) {
		duckdb_free(bind->mode);
	}
	if (bind->runtime_path) {
		duckdb_free(bind->runtime_path);
	}
	if (bind->source) {
		duckdb_free(bind->source);
	}
	if (bind->symbol) {
		duckdb_free(bind->symbol);
	}
	if (bind->sql_name) {
		duckdb_free(bind->sql_name);
	}
	if (bind->arg_types) {
		duckdb_free(bind->arg_types);
	}
	if (bind->return_type) {
		duckdb_free(bind->return_type);
	}
	if (bind->wrapper_mode) {
		duckdb_free(bind->wrapper_mode);
	}
	if (bind->include_path) {
		duckdb_free(bind->include_path);
	}
	if (bind->sysinclude_path) {
		duckdb_free(bind->sysinclude_path);
	}
	if (bind->library_path) {
		duckdb_free(bind->library_path);
	}
	if (bind->library) {
		duckdb_free(bind->library);
	}
	if (bind->option) {
		duckdb_free(bind->option);
	}
	if (bind->header) {
		duckdb_free(bind->header);
	}
	if (bind->define_name) {
		duckdb_free(bind->define_name);
	}
	if (bind->define_value) {
		duckdb_free(bind->define_value);
	}
	duckdb_free(bind);
}

/* Destructor for per-scan init payload of `tcc_module(...)`. */
static void destroy_tcc_module_init_data(void *ptr) {
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)ptr;
	if (!init) {
		return;
	}
	duckdb_free(init);
}

/* Reads optional named VARCHAR bind parameter as owned C string. */
static void tcc_bind_read_named_varchar(duckdb_bind_info info, const char *name, char **out_value) {
	duckdb_value value = duckdb_bind_get_named_parameter(info, name);
	if (value && !duckdb_is_null_value(value)) {
		*out_value = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}
}

/* Reads `arg_types` named parameter, normalizing LIST/ARRAY inputs into CSV token string. */
static void tcc_bind_read_named_arg_types(duckdb_bind_info info, char **out_csv) {
	duckdb_value value = duckdb_bind_get_named_parameter(info, "arg_types");
	if (!value || duckdb_is_null_value(value)) {
		if (value) {
			duckdb_destroy_value(&value);
		}
		return;
	}
	duckdb_logical_type vtype = duckdb_get_value_type(value);
	duckdb_type type_id = duckdb_get_type_id(vtype);
	if (type_id == DUCKDB_TYPE_LIST || type_id == DUCKDB_TYPE_ARRAY) {
		idx_t n = duckdb_get_list_size(value);
		idx_t i;
		size_t total = 0;
		char *buf;
		size_t off = 0;
		for (i = 0; i < n; i++) {
			duckdb_value child = duckdb_get_list_child(value, i);
			char *txt = duckdb_get_varchar(child);
			size_t len = txt ? strlen(txt) : 0;
			total += len + (i > 0 ? 1 : 0);
			if (txt) {
				duckdb_free(txt);
			}
			duckdb_destroy_value(&child);
		}
		buf = (char *)duckdb_malloc(total + 1);
		if (!buf) {
			duckdb_destroy_value(&value);
			return;
		}
		buf[0] = '\0';
		for (i = 0; i < n; i++) {
			duckdb_value child = duckdb_get_list_child(value, i);
			char *txt = duckdb_get_varchar(child);
			if (i > 0) {
				buf[off++] = ',';
			}
			if (txt) {
				size_t len = strlen(txt);
				memcpy(buf + off, txt, len);
				off += len;
				duckdb_free(txt);
			}
			duckdb_destroy_value(&child);
		}
		buf[off] = '\0';
		*out_csv = buf;
	}
	duckdb_destroy_value(&value);
}

/* Bind callback: parses named parameters into immutable bind data for one call. */
static void tcc_module_bind(duckdb_bind_info info) {
	tcc_module_bind_data_t *bind;
	duckdb_logical_type varchar_type;
	duckdb_logical_type bool_type;

	bind = (tcc_module_bind_data_t *)duckdb_malloc(sizeof(tcc_module_bind_data_t));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(tcc_module_bind_data_t));

	tcc_bind_read_named_varchar(info, "mode", &bind->mode);
	if (!bind->mode || bind->mode[0] == '\0') {
		bind->mode = tcc_strdup("config_get");
	}
	tcc_bind_read_named_varchar(info, "runtime_path", &bind->runtime_path);
	tcc_bind_read_named_varchar(info, "source", &bind->source);
	tcc_bind_read_named_varchar(info, "symbol", &bind->symbol);
	tcc_bind_read_named_varchar(info, "sql_name", &bind->sql_name);
	tcc_bind_read_named_arg_types(info, &bind->arg_types);
	tcc_bind_read_named_varchar(info, "return_type", &bind->return_type);
	tcc_bind_read_named_varchar(info, "wrapper_mode", &bind->wrapper_mode);
	if (!bind->wrapper_mode || bind->wrapper_mode[0] == '\0') {
		if (bind->wrapper_mode) {
			duckdb_free(bind->wrapper_mode);
		}
		bind->wrapper_mode = tcc_strdup("row");
	}
	tcc_bind_read_named_varchar(info, "include_path", &bind->include_path);
	tcc_bind_read_named_varchar(info, "sysinclude_path", &bind->sysinclude_path);
	tcc_bind_read_named_varchar(info, "library_path", &bind->library_path);
	tcc_bind_read_named_varchar(info, "library", &bind->library);
	tcc_bind_read_named_varchar(info, "option", &bind->option);
	tcc_bind_read_named_varchar(info, "header", &bind->header);
	tcc_bind_read_named_varchar(info, "define_name", &bind->define_name);
	tcc_bind_read_named_varchar(info, "define_value", &bind->define_value);

	bool_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
	varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

	duckdb_bind_add_result_column(info, "ok", bool_type);
	duckdb_bind_add_result_column(info, "mode", varchar_type);
	duckdb_bind_add_result_column(info, "phase", varchar_type);
	duckdb_bind_add_result_column(info, "code", varchar_type);
	duckdb_bind_add_result_column(info, "message", varchar_type);
	duckdb_bind_add_result_column(info, "detail", varchar_type);
	duckdb_bind_add_result_column(info, "sql_name", varchar_type);
	duckdb_bind_add_result_column(info, "symbol", varchar_type);
	duckdb_bind_add_result_column(info, "artifact_id", varchar_type);
	duckdb_bind_add_result_column(info, "connection_scope", varchar_type);

	duckdb_destroy_logical_type(&bool_type);
	duckdb_destroy_logical_type(&varchar_type);

	duckdb_bind_set_cardinality(info, 1, true);
	duckdb_bind_set_bind_data(info, bind, destroy_tcc_module_bind_data);
}

/* Init callback: stores one-shot emission state for table-function execution. */
static void tcc_module_init(duckdb_init_info info) {
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)duckdb_malloc(sizeof(tcc_module_init_data_t));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	atomic_store_explicit(&init->emitted, false, memory_order_relaxed);
	duckdb_init_set_init_data(info, init, destroy_tcc_module_init_data);
}

/* Writes nullable VARCHAR value into an output vector cell. */
static void tcc_set_varchar_col(duckdb_vector vector, idx_t row, const char *value) {
	uint64_t *validity;
	if (!value) {
		duckdb_vector_ensure_validity_writable(vector);
		validity = duckdb_vector_get_validity(vector);
		duckdb_validity_set_row_invalid(validity, row);
		return;
	}
	duckdb_vector_assign_string_element(vector, row, value);
}

/* Emits one diagnostics/status row in the `tcc_module(...)` table output schema. */
static void tcc_write_row(duckdb_data_chunk output, bool ok, const char *mode, const char *phase, const char *code,
                          const char *message, const char *detail, const char *sql_name, const char *symbol,
                          const char *artifact_id, const char *connection_scope) {
	duckdb_vector v_ok = duckdb_data_chunk_get_vector(output, 0);
	bool *ok_data = (bool *)duckdb_vector_get_data(v_ok);
	ok_data[0] = ok;

	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 1), 0, mode);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 2), 0, phase);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 3), 0, code);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 4), 0, message);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 5), 0, detail);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 6), 0, sql_name);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 7), 0, symbol);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 8), 0, artifact_id);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 9), 0, connection_scope);
	duckdb_data_chunk_set_size(output, 1);
}

static const char *tcc_skip_space(const char *p) {
	while (p && *p && isspace((unsigned char)*p)) {
		p++;
	}
	return p;
}

/* tcc_equals_ci: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_equals_ci(const char *a, const char *b) {
	unsigned char ca;
	unsigned char cb;
	if (!a || !b) {
		return false;
	}
	while (*a && *b) {
		ca = (unsigned char)tolower((unsigned char)*a++);
		cb = (unsigned char)tolower((unsigned char)*b++);
		if (ca != cb) {
			return false;
		}
	}
	return *a == '\0' && *b == '\0';
}

static const char *tcc_wrapper_mode_token(tcc_wrapper_mode_t mode) {
	switch (mode) {
	case TCC_WRAPPER_MODE_ROW:
		return "row";
	case TCC_WRAPPER_MODE_BATCH:
		return "batch";
	default:
		return NULL;
	}
}

/* tcc_parse_wrapper_mode: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_wrapper_mode(const char *wrapper_mode, tcc_wrapper_mode_t *out_mode,
                                   tcc_error_buffer_t *error_buf) {
	const char *mode = wrapper_mode;
	char token[32];
	size_t len;
	if (!out_mode) {
		tcc_set_error(error_buf, "wrapper_mode output is required");
		return false;
	}
	if (!mode || mode[0] == '\0') {
		*out_mode = TCC_WRAPPER_MODE_ROW;
		return true;
	}
	mode = tcc_skip_space(mode);
	len = strlen(mode);
	while (len > 0 && isspace((unsigned char)mode[len - 1])) {
		len--;
	}
	if (len == 0 || len >= sizeof(token)) {
		tcc_set_error(error_buf, "wrapper_mode contains unsupported token");
		return false;
	}
	memcpy(token, mode, len);
	token[len] = '\0';
	if (tcc_equals_ci(token, "row")) {
		*out_mode = TCC_WRAPPER_MODE_ROW;
		return true;
	}
	if (tcc_equals_ci(token, "batch")) {
		*out_mode = TCC_WRAPPER_MODE_BATCH;
		return true;
	}
	tcc_set_error(error_buf, "wrapper_mode contains unsupported token");
	return false;
}

static char *tcc_next_top_level_part(char **cursor, char sep) {
	char *start;
	char *p;
	int angle_depth = 0;
	int square_depth = 0;
	if (!cursor || !*cursor) {
		return NULL;
	}
	start = *cursor;
	p = start;
	while (*p) {
		if (*p == '<') {
			angle_depth++;
		} else if (*p == '>') {
			if (angle_depth > 0) {
				angle_depth--;
			}
		} else if (*p == '[') {
			square_depth++;
		} else if (*p == ']') {
			if (square_depth > 0) {
				square_depth--;
			}
		} else if (*p == sep && angle_depth == 0 && square_depth == 0) {
			*p = '\0';
			*cursor = p + 1;
			return start;
		}
		p++;
	}
	*cursor = NULL;
	return start;
}

static char *tcc_find_top_level_char(char *input, char target) {
	char *p;
	int angle_depth = 0;
	int square_depth = 0;
	if (!input) {
		return NULL;
	}
	p = input;
	while (*p) {
		if (*p == '<') {
			angle_depth++;
		} else if (*p == '>') {
			if (angle_depth > 0) {
				angle_depth--;
			}
		} else if (*p == '[') {
			square_depth++;
		} else if (*p == ']') {
			if (square_depth > 0) {
				square_depth--;
			}
		} else if (*p == target && angle_depth == 0 && square_depth == 0) {
			return p;
		}
		p++;
	}
	return NULL;
}

/* tcc_parse_type_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_type_token(const char *token, bool allow_void, tcc_ffi_type_t *out_type, size_t *out_array_size) {
	size_t token_len;
	if (!token || token[0] == '\0' || !out_type) {
		return false;
	}
	token_len = strlen(token);
	if (out_array_size) {
		*out_array_size = 0;
	}
	if (allow_void && tcc_equals_ci(token, "void")) {
		*out_type = TCC_FFI_VOID;
		return true;
	}
	if (tcc_equals_ci(token, "bool") || tcc_equals_ci(token, "boolean")) {
		*out_type = TCC_FFI_BOOL;
		return true;
	}
	if (tcc_equals_ci(token, "i8") || tcc_equals_ci(token, "int8") || tcc_equals_ci(token, "tinyint")) {
		*out_type = TCC_FFI_I8;
		return true;
	}
	if (tcc_equals_ci(token, "u8") || tcc_equals_ci(token, "uint8") || tcc_equals_ci(token, "utinyint")) {
		*out_type = TCC_FFI_U8;
		return true;
	}
	if (tcc_equals_ci(token, "i16") || tcc_equals_ci(token, "int16") || tcc_equals_ci(token, "smallint")) {
		*out_type = TCC_FFI_I16;
		return true;
	}
	if (tcc_equals_ci(token, "u16") || tcc_equals_ci(token, "uint16") || tcc_equals_ci(token, "usmallint")) {
		*out_type = TCC_FFI_U16;
		return true;
	}
	if (tcc_equals_ci(token, "i32") || tcc_equals_ci(token, "int32") || tcc_equals_ci(token, "integer")) {
		*out_type = TCC_FFI_I32;
		return true;
	}
	if (tcc_equals_ci(token, "u32") || tcc_equals_ci(token, "uint32") || tcc_equals_ci(token, "uinteger")) {
		*out_type = TCC_FFI_U32;
		return true;
	}
	if (tcc_equals_ci(token, "i64") || tcc_equals_ci(token, "int64") || tcc_equals_ci(token, "bigint") ||
	    tcc_equals_ci(token, "longlong")) {
		*out_type = TCC_FFI_I64;
		return true;
	}
	if (tcc_equals_ci(token, "u64") || tcc_equals_ci(token, "uint64") || tcc_equals_ci(token, "ubigint") ||
	    tcc_equals_ci(token, "ulonglong")) {
		*out_type = TCC_FFI_U64;
		return true;
	}
	if (tcc_equals_ci(token, "ptr") || tcc_equals_ci(token, "pointer") || tcc_equals_ci(token, "c_ptr")) {
		*out_type = TCC_FFI_PTR;
		return true;
	}
	if (tcc_equals_ci(token, "f32") || tcc_equals_ci(token, "float") || tcc_equals_ci(token, "real")) {
		*out_type = TCC_FFI_F32;
		return true;
	}
	if (tcc_equals_ci(token, "f64") || tcc_equals_ci(token, "double")) {
		*out_type = TCC_FFI_F64;
		return true;
	}
	if (tcc_equals_ci(token, "varchar") || tcc_equals_ci(token, "text") || tcc_equals_ci(token, "string") ||
	    tcc_equals_ci(token, "cstring")) {
		*out_type = TCC_FFI_VARCHAR;
		return true;
	}
	if (tcc_equals_ci(token, "blob") || tcc_equals_ci(token, "bytea") || tcc_equals_ci(token, "binary") ||
	    tcc_equals_ci(token, "varbinary") || tcc_equals_ci(token, "buffer") || tcc_equals_ci(token, "bytes")) {
		*out_type = TCC_FFI_BLOB;
		return true;
	}
	if (tcc_equals_ci(token, "uuid")) {
		*out_type = TCC_FFI_UUID;
		return true;
	}
	if (tcc_equals_ci(token, "date")) {
		*out_type = TCC_FFI_DATE;
		return true;
	}
	if (tcc_equals_ci(token, "time")) {
		*out_type = TCC_FFI_TIME;
		return true;
	}
	if (tcc_equals_ci(token, "timestamp") || tcc_equals_ci(token, "datetime")) {
		*out_type = TCC_FFI_TIMESTAMP;
		return true;
	}
	if (tcc_equals_ci(token, "interval")) {
		*out_type = TCC_FFI_INTERVAL;
		return true;
	}
	if (tcc_equals_ci(token, "decimal") || tcc_equals_ci(token, "numeric")) {
		*out_type = TCC_FFI_DECIMAL;
		return true;
	}
	if (token_len > 8 && (token[0] == 's' || token[0] == 'S') && (token[1] == 't' || token[1] == 'T') &&
	    (token[2] == 'r' || token[2] == 'R') && (token[3] == 'u' || token[3] == 'U') &&
	    (token[4] == 'c' || token[4] == 'C') && (token[5] == 't' || token[5] == 'T') && token[6] == '<' &&
	    token[token_len - 1] == '>') {
		*out_type = TCC_FFI_STRUCT;
		return true;
	}
	if (token_len > 5 && (token[0] == 'm' || token[0] == 'M') && (token[1] == 'a' || token[1] == 'A') &&
	    (token[2] == 'p' || token[2] == 'P') && token[3] == '<' && token[token_len - 1] == '>') {
		*out_type = TCC_FFI_MAP;
		return true;
	}
	if (token_len > 7 && (token[0] == 'u' || token[0] == 'U') && (token[1] == 'n' || token[1] == 'N') &&
	    (token[2] == 'i' || token[2] == 'I') && (token[3] == 'o' || token[3] == 'O') &&
	    (token[4] == 'n' || token[4] == 'N') && token[5] == '<' && token[token_len - 1] == '>') {
		*out_type = TCC_FFI_UNION;
		return true;
	}
	if (token_len > 6 && (token[0] == 'l' || token[0] == 'L') && (token[1] == 'i' || token[1] == 'I') &&
	    (token[2] == 's' || token[2] == 'S') && (token[3] == 't' || token[3] == 'T') && token[4] == '<' &&
	    token[token_len - 1] == '>') {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		size_t child_array_size = 0;
		char *inner = (char *)duckdb_malloc(token_len - 4);
		if (!inner) {
			return false;
		}
		memcpy(inner, token + 5, token_len - 6);
		inner[token_len - 6] = '\0';
		{
			char *trimmed = tcc_trim_inplace(inner);
			bool ok = tcc_parse_type_token(trimmed, false, &child_type, &child_array_size) && child_type != TCC_FFI_VOID;
			duckdb_free(inner);
			if (!ok) {
				return false;
			}
		}
		if (tcc_ffi_list_type_from_child(child_type, out_type)) {
			return true;
		}
		*out_type = TCC_FFI_LIST;
		return true;
	}
	if (token_len >= 5 && (token[0] == 'l' || token[0] == 'L') && (token[1] == 'i' || token[1] == 'I') &&
	    (token[2] == 's' || token[2] == 'S') && (token[3] == 't' || token[3] == 'T') && token[4] == '_') {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		size_t child_array_size = 0;
		if (!tcc_parse_type_token(token + 5, false, &child_type, &child_array_size) || child_type == TCC_FFI_VOID) {
			return false;
		}
		if (tcc_ffi_list_type_from_child(child_type, out_type)) {
			return true;
		}
		*out_type = TCC_FFI_LIST;
		return true;
	}
	if (token_len > 3 && token[token_len - 1] == ']') {
		const char *lb = strrchr(token, '[');
		if (lb && lb < token + token_len - 1 && lb[1] != ']') {
			uint64_t arr_n = 0;
			const char *p = lb + 1;
			bool ok_digits = true;
			tcc_ffi_type_t child_type = TCC_FFI_VOID;
			size_t child_array_size = 0;
			char child_token[64];
			size_t child_len = (size_t)(lb - token);
			while (p < token + token_len - 1) {
				if (!isdigit((unsigned char)*p)) {
					ok_digits = false;
					break;
				}
				arr_n = arr_n * 10 + (uint64_t)(*p - '0');
				p++;
			}
			if (ok_digits && arr_n > 0 && arr_n <= (uint64_t)SIZE_MAX && child_len > 0 && child_len < sizeof(child_token)) {
				memcpy(child_token, token, child_len);
				child_token[child_len] = '\0';
				if (tcc_parse_type_token(child_token, false, &child_type, &child_array_size) &&
				    child_type != TCC_FFI_VOID) {
					if (!tcc_ffi_array_type_from_child(child_type, out_type)) {
						*out_type = TCC_FFI_ARRAY;
					}
					if (out_array_size) {
						*out_array_size = (size_t)arr_n;
					}
					return true;
				}
			}
		}
	}
	if (token_len > 2 && token[token_len - 2] == '[' && token[token_len - 1] == ']') {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		size_t child_array_size = 0;
		char child_token[64];
		size_t child_len = token_len - 2;
		if (child_len > 0 && child_len < sizeof(child_token)) {
			memcpy(child_token, token, child_len);
			child_token[child_len] = '\0';
			if (tcc_parse_type_token(child_token, false, &child_type, &child_array_size) &&
			    child_type != TCC_FFI_VOID) {
				if (!tcc_ffi_list_type_from_child(child_type, out_type)) {
					*out_type = TCC_FFI_LIST;
				}
				return true;
			}
		}
	}
	return false;
}

/* tcc_is_identifier_token: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_is_identifier_token(const char *value) {
	size_t i;
	if (!value || value[0] == '\0') {
		return false;
	}
	if (!(isalpha((unsigned char)value[0]) || value[0] == '_')) {
		return false;
	}
	for (i = 1; value[i] != '\0'; i++) {
		if (!(isalnum((unsigned char)value[i]) || value[i] == '_')) {
			return false;
		}
	}
	return true;
}

/* tcc_split_csv_tokens: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_split_csv_tokens(const char *csv, tcc_string_list_t *out_tokens, tcc_error_buffer_t *error_buf) {
	char *copy;
	char *cursor;
	if (!out_tokens) {
		tcc_set_error(error_buf, "token output is required");
		return false;
	}
	memset(out_tokens, 0, sizeof(*out_tokens));
	if (!csv || csv[0] == '\0') {
		return true;
	}
	copy = tcc_strdup(csv);
	if (!copy) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	cursor = copy;
	while (cursor) {
		char *token;
		char *part = tcc_next_top_level_part(&cursor, ',');
		token = tcc_trim_inplace(part);
		if (token[0] == '\0') {
			duckdb_free(copy);
			tcc_string_list_destroy(out_tokens);
			tcc_set_error(error_buf, "arg_types contains an empty token");
			return false;
		}
		if (!tcc_string_list_append(out_tokens, token)) {
			duckdb_free(copy);
			tcc_string_list_destroy(out_tokens);
			tcc_set_error(error_buf, "out of memory");
			return false;
		}
	}
	duckdb_free(copy);
	return true;
}

/* tcc_parse_c_field_spec_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_c_field_spec_token(const char *token, bool force_bitfield, tcc_c_field_spec_t *out_field,
                                         tcc_error_buffer_t *error_buf) {
	char *copy;
	char *type_part;
	char *opts_part = NULL;
	char *sep;
	char *name;
	char *type_token;
	tcc_ffi_type_t parsed_type = TCC_FFI_VOID;
	tcc_ffi_type_t child_type = TCC_FFI_VOID;
	size_t array_size = 0;
	bool is_bitfield = force_bitfield;
	if (!token || !out_field) {
		tcc_set_error(error_buf, "invalid c field specification");
		return false;
	}
	memset(out_field, 0, sizeof(*out_field));
	copy = tcc_strdup(token);
	if (!copy) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	sep = strchr(copy, ':');
	if (!sep) {
		duckdb_free(copy);
		tcc_set_error(error_buf, "field spec must use name:type or name:type:bitfield");
		return false;
	}
	*sep = '\0';
	type_part = sep + 1;
	sep = strchr(type_part, ':');
	if (sep) {
		*sep = '\0';
		opts_part = sep + 1;
	}
	name = tcc_trim_inplace(copy);
	type_token = tcc_trim_inplace(type_part);
	if (!tcc_is_identifier_token(name)) {
		duckdb_free(copy);
		tcc_set_error(error_buf, "field name must be a valid identifier");
		return false;
	}
	if (!tcc_parse_type_token(type_token, false, &parsed_type, &array_size)) {
		duckdb_free(copy);
		tcc_set_error(error_buf, "field type token is unsupported");
		return false;
	}
	if (opts_part) {
		char *opt = tcc_trim_inplace(opts_part);
		if (opt[0] != '\0') {
			if (tcc_equals_ci(opt, "bitfield")) {
				is_bitfield = true;
			} else {
				duckdb_free(copy);
				tcc_set_error(error_buf, "field option is unsupported (expected bitfield)");
				return false;
			}
		}
	}
	if (tcc_ffi_type_is_array(parsed_type)) {
		if (!tcc_ffi_array_child_type(parsed_type, &child_type) || array_size == 0) {
			duckdb_free(copy);
			tcc_set_error(error_buf, "array field type is invalid");
			return false;
		}
		if (is_bitfield) {
			duckdb_free(copy);
			tcc_set_error(error_buf, "bitfield option cannot be used with array fields");
			return false;
		}
		parsed_type = child_type;
	} else {
		array_size = 0;
	}
	if (!(tcc_ffi_type_is_fixed_width_scalar(parsed_type) || parsed_type == TCC_FFI_PTR)) {
		duckdb_free(copy);
		tcc_set_error(error_buf,
		              "fields currently support fixed-width scalar/pointer tokens only (no varchar/blob/nested)");
		return false;
	}
	out_field->name = tcc_strdup(name);
	out_field->type = parsed_type;
	out_field->array_size = array_size;
	out_field->is_bitfield = is_bitfield;
	duckdb_free(copy);
	if (!out_field->name) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	return true;
}

/* tcc_parse_c_field_specs: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_c_field_specs(const char *arg_types_csv, bool force_bitfield, tcc_c_field_list_t *out_fields,
                                    tcc_error_buffer_t *error_buf) {
	tcc_string_list_t tokens;
	idx_t i;
	if (!out_fields) {
		tcc_set_error(error_buf, "field output is required");
		return false;
	}
	memset(out_fields, 0, sizeof(*out_fields));
	if (!tcc_split_csv_tokens(arg_types_csv, &tokens, error_buf)) {
		return false;
	}
	if (tokens.count == 0) {
		tcc_string_list_destroy(&tokens);
		tcc_set_error(error_buf, "arg_types is required for c_struct/c_union/c_bitfield");
		return false;
	}
	for (i = 0; i < tokens.count; i++) {
		tcc_c_field_spec_t field;
		memset(&field, 0, sizeof(field));
		if (!tcc_parse_c_field_spec_token(tokens.items[i], force_bitfield, &field, error_buf) ||
		    !tcc_c_field_list_append(out_fields, &field)) {
			if (field.name) {
				duckdb_free(field.name);
			}
			tcc_string_list_destroy(&tokens);
			tcc_c_field_list_destroy(out_fields);
			if (!error_buf || error_buf->message[0] == '\0') {
				tcc_set_error(error_buf, "out of memory");
			}
			return false;
		}
	}
	tcc_string_list_destroy(&tokens);
	return true;
}

/* tcc_parse_c_enum_constants: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_c_enum_constants(const char *arg_types_csv, tcc_string_list_t *out_constants,
                                       tcc_error_buffer_t *error_buf) {
	idx_t i;
	if (!tcc_split_csv_tokens(arg_types_csv, out_constants, error_buf)) {
		return false;
	}
	if (out_constants->count == 0) {
		tcc_set_error(error_buf, "arg_types must list at least one enum constant");
		tcc_string_list_destroy(out_constants);
		return false;
	}
	for (i = 0; i < out_constants->count; i++) {
		if (!tcc_is_identifier_token(out_constants->items[i])) {
			tcc_set_error(error_buf, "enum constants must be valid identifiers");
			tcc_string_list_destroy(out_constants);
			return false;
		}
	}
	return true;
}

/* tcc_parse_struct_meta_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_struct_meta_token(const char *token, tcc_ffi_struct_meta_t *out_meta,
                                        tcc_error_buffer_t *error_buf) {
	size_t token_len;
	char *inner = NULL;
	char *cur;
	int field_count = 0;
	int cap = 0;
	char **field_names = NULL;
	char **field_tokens = NULL;
	tcc_ffi_type_t *field_types = NULL;
	size_t *field_sizes = NULL;
	if (!token || !out_meta) {
		tcc_set_error(error_buf, "invalid struct token");
		return false;
	}
	memset(out_meta, 0, sizeof(*out_meta));
	token_len = strlen(token);
	if (token_len <= 8 || !((token[0] == 's' || token[0] == 'S') && (token[1] == 't' || token[1] == 'T') &&
	                        (token[2] == 'r' || token[2] == 'R') && (token[3] == 'u' || token[3] == 'U') &&
	                        (token[4] == 'c' || token[4] == 'C') && (token[5] == 't' || token[5] == 'T') &&
	                        token[6] == '<' && token[token_len - 1] == '>')) {
		tcc_set_error(error_buf, "struct token must use struct<...>");
		return false;
	}
	inner = (char *)duckdb_malloc(token_len - 6);
	if (!inner) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	memcpy(inner, token + 7, token_len - 8);
	inner[token_len - 8] = '\0';
	cur = inner;
	while (cur && *cur) {
		char *part = tcc_next_top_level_part(&cur, ';');
		char *colon;
		char *name_part;
		char *type_part;
		size_t part_len;
		tcc_ffi_type_t field_type = TCC_FFI_VOID;
		size_t field_array_size = 0;
		size_t field_size;
		char auto_name[32];
		char **new_names;
		char **new_tokens;
		tcc_ffi_type_t *new_types;
		size_t *new_sizes;
		while (*part && isspace((unsigned char)*part)) {
			part++;
		}
		part_len = strlen(part);
		while (part_len > 0 && isspace((unsigned char)part[part_len - 1])) {
			part[--part_len] = '\0';
		}
		if (part_len == 0) {
			tcc_set_error(error_buf, "struct token contains empty field");
			goto fail;
		}
		colon = tcc_find_top_level_char(part, ':');
		if (colon) {
			*colon = '\0';
			name_part = part;
			type_part = colon + 1;
			while (*name_part && isspace((unsigned char)*name_part)) {
				name_part++;
			}
			part_len = strlen(name_part);
			while (part_len > 0 && isspace((unsigned char)name_part[part_len - 1])) {
				name_part[--part_len] = '\0';
			}
			while (*type_part && isspace((unsigned char)*type_part)) {
				type_part++;
			}
			part_len = strlen(type_part);
			while (part_len > 0 && isspace((unsigned char)type_part[part_len - 1])) {
				type_part[--part_len] = '\0';
			}
			if (!tcc_is_identifier_token(name_part)) {
				tcc_set_error(error_buf, "struct field names must be identifiers");
				goto fail;
			}
			if (type_part[0] == '\0') {
				tcc_set_error(error_buf, "struct field type is missing");
				goto fail;
			}
		} else {
			snprintf(auto_name, sizeof(auto_name), "f%d", field_count + 1);
			name_part = auto_name;
			type_part = part;
		}
		if (!tcc_parse_type_token(type_part, false, &field_type, &field_array_size)) {
			tcc_set_error(error_buf, "struct field type token is unsupported");
			goto fail;
		}
		if (field_type == TCC_FFI_VOID) {
			tcc_set_error(error_buf, "struct fields cannot use void");
			goto fail;
		}
		field_size = tcc_ffi_type_size(field_type);
		if (field_size == 0) {
			tcc_set_error(error_buf, "struct field has unsupported storage type");
			goto fail;
		}
		if (field_count >= cap) {
			int new_cap = cap == 0 ? 4 : cap * 2;
			new_names = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
			new_tokens = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
			new_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)new_cap);
			new_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)new_cap);
			if (!new_names || !new_tokens || !new_types || !new_sizes) {
				if (new_names) {
					duckdb_free(new_names);
				}
				if (new_tokens) {
					duckdb_free(new_tokens);
				}
				if (new_types) {
					duckdb_free(new_types);
				}
				if (new_sizes) {
					duckdb_free(new_sizes);
				}
				tcc_set_error(error_buf, "out of memory");
				goto fail;
			}
			memset(new_names, 0, sizeof(char *) * (size_t)new_cap);
			memset(new_tokens, 0, sizeof(char *) * (size_t)new_cap);
			if (field_names && field_count > 0) {
				memcpy(new_names, field_names, sizeof(char *) * (size_t)field_count);
				memcpy(new_tokens, field_tokens, sizeof(char *) * (size_t)field_count);
				memcpy(new_types, field_types, sizeof(tcc_ffi_type_t) * (size_t)field_count);
				memcpy(new_sizes, field_sizes, sizeof(size_t) * (size_t)field_count);
				duckdb_free(field_names);
				duckdb_free(field_tokens);
				duckdb_free(field_types);
				duckdb_free(field_sizes);
			}
			field_names = new_names;
			field_tokens = new_tokens;
			field_types = new_types;
			field_sizes = new_sizes;
			cap = new_cap;
		}
		field_names[field_count] = tcc_strdup(name_part);
		if (!field_names[field_count]) {
			tcc_set_error(error_buf, "out of memory");
			goto fail;
		}
		field_tokens[field_count] = tcc_strdup(type_part);
		if (!field_tokens[field_count]) {
			tcc_set_error(error_buf, "out of memory");
			goto fail;
		}
		field_types[field_count] = field_type;
		field_sizes[field_count] = field_size;
		field_count++;
	}
	if (field_count <= 0) {
		tcc_set_error(error_buf, "struct token requires at least one field");
		goto fail;
	}
	duckdb_free(inner);
	out_meta->field_count = field_count;
	out_meta->field_names = field_names;
	out_meta->field_tokens = field_tokens;
	out_meta->field_types = field_types;
	out_meta->field_sizes = field_sizes;
	return true;
fail:
	if (inner) {
		duckdb_free(inner);
	}
	if (field_names) {
		int i;
		for (i = 0; i < field_count; i++) {
			if (field_names[i]) {
				duckdb_free(field_names[i]);
			}
		}
		duckdb_free(field_names);
	}
	if (field_tokens) {
		int i;
		for (i = 0; i < field_count; i++) {
			if (field_tokens[i]) {
				duckdb_free(field_tokens[i]);
			}
		}
		duckdb_free(field_tokens);
	}
	if (field_types) {
		duckdb_free(field_types);
	}
	if (field_sizes) {
		duckdb_free(field_sizes);
	}
	memset(out_meta, 0, sizeof(*out_meta));
	return false;
}

/* tcc_parse_map_meta_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_map_meta_token(const char *token, tcc_ffi_map_meta_t *out_meta, tcc_error_buffer_t *error_buf) {
	size_t token_len;
	char *inner = NULL;
	char *sep;
	char *key_token;
	char *value_token;
	tcc_ffi_type_t key_type = TCC_FFI_VOID;
	tcc_ffi_type_t value_type = TCC_FFI_VOID;
	size_t key_array_size = 0;
	size_t value_array_size = 0;
	size_t key_size = 0;
	size_t value_size = 0;
	char *key_token_copy = NULL;
	char *value_token_copy = NULL;
	if (!token || !out_meta) {
		tcc_set_error(error_buf, "invalid map token");
		return false;
	}
	memset(out_meta, 0, sizeof(*out_meta));
	token_len = strlen(token);
	if (token_len <= 5 || !((token[0] == 'm' || token[0] == 'M') && (token[1] == 'a' || token[1] == 'A') &&
	                        (token[2] == 'p' || token[2] == 'P') && token[3] == '<' &&
	                        token[token_len - 1] == '>')) {
		tcc_set_error(error_buf, "map token must use map<key_type;value_type>");
		return false;
	}
	inner = (char *)duckdb_malloc(token_len - 3);
	if (!inner) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	memcpy(inner, token + 4, token_len - 5);
	inner[token_len - 5] = '\0';
	sep = tcc_find_top_level_char(inner, ';');
	if (!sep || tcc_find_top_level_char(sep + 1, ';')) {
		duckdb_free(inner);
		tcc_set_error(error_buf, "map token must use exactly one ';' separator");
		return false;
	}
	*sep = '\0';
	key_token = inner;
	value_token = sep + 1;
	while (*key_token && isspace((unsigned char)*key_token)) {
		key_token++;
	}
	while (*value_token && isspace((unsigned char)*value_token)) {
		value_token++;
	}
	{
		size_t len = strlen(key_token);
		while (len > 0 && isspace((unsigned char)key_token[len - 1])) {
			key_token[--len] = '\0';
		}
	}
	{
		size_t len = strlen(value_token);
		while (len > 0 && isspace((unsigned char)value_token[len - 1])) {
			value_token[--len] = '\0';
		}
	}
	if (key_token[0] == '\0' || value_token[0] == '\0') {
		duckdb_free(inner);
		tcc_set_error(error_buf, "map token key/value type is missing");
		return false;
	}
	key_token_copy = tcc_strdup(key_token);
	value_token_copy = tcc_strdup(value_token);
	if (!key_token_copy || !value_token_copy) {
		if (key_token_copy) {
			duckdb_free(key_token_copy);
		}
		if (value_token_copy) {
			duckdb_free(value_token_copy);
		}
		duckdb_free(inner);
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	if (!tcc_parse_type_token(key_token, false, &key_type, &key_array_size) ||
	    !tcc_parse_type_token(value_token, false, &value_type, &value_array_size) || key_type == TCC_FFI_VOID ||
	    value_type == TCC_FFI_VOID) {
		duckdb_free(key_token_copy);
		duckdb_free(value_token_copy);
		duckdb_free(inner);
		tcc_set_error(error_buf, "map key/value contains unsupported type token");
		return false;
	}
	key_size = tcc_ffi_type_size(key_type);
	value_size = tcc_ffi_type_size(value_type);
	duckdb_free(inner);
	if (key_size == 0 || value_size == 0) {
		duckdb_free(key_token_copy);
		duckdb_free(value_token_copy);
		tcc_set_error(error_buf, "map key/value has unsupported storage type");
		return false;
	}
	out_meta->key_token = key_token_copy;
	out_meta->value_token = value_token_copy;
	out_meta->key_type = key_type;
	out_meta->value_type = value_type;
	out_meta->key_size = key_size;
	out_meta->value_size = value_size;
	return true;
}

/* tcc_parse_union_meta_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_union_meta_token(const char *token, tcc_ffi_union_meta_t *out_meta,
                                       tcc_error_buffer_t *error_buf) {
	size_t token_len;
	char *inner = NULL;
	char *cur;
	int member_count = 0;
	int cap = 0;
	char **member_names = NULL;
	char **member_tokens = NULL;
	tcc_ffi_type_t *member_types = NULL;
	size_t *member_sizes = NULL;
	if (!token || !out_meta) {
		tcc_set_error(error_buf, "invalid union token");
		return false;
	}
	memset(out_meta, 0, sizeof(*out_meta));
	token_len = strlen(token);
	if (token_len <= 7 || !((token[0] == 'u' || token[0] == 'U') && (token[1] == 'n' || token[1] == 'N') &&
	                        (token[2] == 'i' || token[2] == 'I') && (token[3] == 'o' || token[3] == 'O') &&
	                        (token[4] == 'n' || token[4] == 'N') && token[5] == '<' && token[token_len - 1] == '>')) {
		tcc_set_error(error_buf, "union token must use union<name:type;...>");
		return false;
	}
	inner = (char *)duckdb_malloc(token_len - 4);
	if (!inner) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	memcpy(inner, token + 6, token_len - 7);
	inner[token_len - 7] = '\0';
	cur = inner;
	while (cur && *cur) {
		char *part = tcc_next_top_level_part(&cur, ';');
		char *colon;
		char *name_part;
		char *type_part;
		size_t part_len;
		tcc_ffi_type_t member_type = TCC_FFI_VOID;
		size_t member_array_size = 0;
		size_t member_size = 0;
		while (*part && isspace((unsigned char)*part)) {
			part++;
		}
		part_len = strlen(part);
		while (part_len > 0 && isspace((unsigned char)part[part_len - 1])) {
			part[--part_len] = '\0';
		}
		if (part_len == 0) {
			tcc_set_error(error_buf, "union token contains empty member");
			goto fail;
		}
		colon = tcc_find_top_level_char(part, ':');
		if (!colon) {
			tcc_set_error(error_buf, "union member must use name:type");
			goto fail;
		}
		*colon = '\0';
		name_part = part;
		type_part = colon + 1;
		while (*name_part && isspace((unsigned char)*name_part)) {
			name_part++;
		}
		part_len = strlen(name_part);
		while (part_len > 0 && isspace((unsigned char)name_part[part_len - 1])) {
			name_part[--part_len] = '\0';
		}
		while (*type_part && isspace((unsigned char)*type_part)) {
			type_part++;
		}
		part_len = strlen(type_part);
		while (part_len > 0 && isspace((unsigned char)type_part[part_len - 1])) {
			type_part[--part_len] = '\0';
		}
		if (!tcc_is_identifier_token(name_part)) {
			tcc_set_error(error_buf, "union member names must be identifiers");
			goto fail;
		}
		if (type_part[0] == '\0') {
			tcc_set_error(error_buf, "union member type is missing");
			goto fail;
		}
		if (!tcc_parse_type_token(type_part, false, &member_type, &member_array_size) || member_type == TCC_FFI_VOID) {
			tcc_set_error(error_buf, "union member type token is unsupported");
			goto fail;
		}
		member_size = tcc_ffi_type_size(member_type);
		if (member_size == 0) {
			tcc_set_error(error_buf, "union member has unsupported storage type");
			goto fail;
		}
		if (member_count >= cap) {
			int new_cap = cap == 0 ? 4 : cap * 2;
			char **new_names = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
			char **new_tokens = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
			tcc_ffi_type_t *new_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)new_cap);
			size_t *new_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)new_cap);
			if (!new_names || !new_tokens || !new_types || !new_sizes) {
				if (new_names) {
					duckdb_free(new_names);
				}
				if (new_tokens) {
					duckdb_free(new_tokens);
				}
				if (new_types) {
					duckdb_free(new_types);
				}
				if (new_sizes) {
					duckdb_free(new_sizes);
				}
				tcc_set_error(error_buf, "out of memory");
				goto fail;
			}
			memset(new_names, 0, sizeof(char *) * (size_t)new_cap);
			memset(new_tokens, 0, sizeof(char *) * (size_t)new_cap);
			if (member_names && member_count > 0) {
				memcpy(new_names, member_names, sizeof(char *) * (size_t)member_count);
				memcpy(new_tokens, member_tokens, sizeof(char *) * (size_t)member_count);
				memcpy(new_types, member_types, sizeof(tcc_ffi_type_t) * (size_t)member_count);
				memcpy(new_sizes, member_sizes, sizeof(size_t) * (size_t)member_count);
				duckdb_free(member_names);
				duckdb_free(member_tokens);
				duckdb_free(member_types);
				duckdb_free(member_sizes);
			}
			member_names = new_names;
			member_tokens = new_tokens;
			member_types = new_types;
			member_sizes = new_sizes;
			cap = new_cap;
		}
		member_names[member_count] = tcc_strdup(name_part);
		member_tokens[member_count] = tcc_strdup(type_part);
		if (!member_names[member_count] || !member_tokens[member_count]) {
			tcc_set_error(error_buf, "out of memory");
			goto fail;
		}
		member_types[member_count] = member_type;
		member_sizes[member_count] = member_size;
		member_count++;
	}
	if (member_count <= 0) {
		tcc_set_error(error_buf, "union token requires at least one member");
		goto fail;
	}
	duckdb_free(inner);
	out_meta->member_count = member_count;
	out_meta->member_names = member_names;
	out_meta->member_tokens = member_tokens;
	out_meta->member_types = member_types;
	out_meta->member_sizes = member_sizes;
	return true;
fail:
	if (inner) {
		duckdb_free(inner);
	}
	if (member_names) {
		int i;
		for (i = 0; i < member_count; i++) {
			if (member_names[i]) {
				duckdb_free(member_names[i]);
			}
		}
		duckdb_free(member_names);
	}
	if (member_tokens) {
		int i;
		for (i = 0; i < member_count; i++) {
			if (member_tokens[i]) {
				duckdb_free(member_tokens[i]);
			}
		}
		duckdb_free(member_tokens);
	}
	if (member_types) {
		duckdb_free(member_types);
	}
	if (member_sizes) {
		duckdb_free(member_sizes);
	}
	memset(out_meta, 0, sizeof(*out_meta));
	return false;
}

/* tcc_typedesc_destroy: Type-system conversion/parsing helper. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_typedesc_destroy(tcc_typedesc_t *desc) {
	idx_t i;
	if (!desc) {
		return;
	}
	if (desc->token) {
		duckdb_free(desc->token);
	}
	if (desc->kind == TCC_TYPEDESC_LIST || desc->kind == TCC_TYPEDESC_ARRAY) {
		tcc_typedesc_destroy(desc->as.list_like.child);
	} else if (desc->kind == TCC_TYPEDESC_STRUCT) {
		if (desc->as.struct_like.fields) {
			for (i = 0; i < desc->as.struct_like.count; i++) {
				if (desc->as.struct_like.fields[i].name) {
					duckdb_free(desc->as.struct_like.fields[i].name);
				}
				tcc_typedesc_destroy(desc->as.struct_like.fields[i].type);
			}
			duckdb_free(desc->as.struct_like.fields);
		}
	} else if (desc->kind == TCC_TYPEDESC_MAP) {
		tcc_typedesc_destroy(desc->as.map_like.key);
		tcc_typedesc_destroy(desc->as.map_like.value);
	} else if (desc->kind == TCC_TYPEDESC_UNION) {
		if (desc->as.union_like.members) {
			for (i = 0; i < desc->as.union_like.count; i++) {
				if (desc->as.union_like.members[i].name) {
					duckdb_free(desc->as.union_like.members[i].name);
				}
				tcc_typedesc_destroy(desc->as.union_like.members[i].type);
			}
			duckdb_free(desc->as.union_like.members);
		}
	}
	duckdb_free(desc);
}

/* tcc_typedesc_parse_token: Type-system conversion/parsing helper. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_typedesc_parse_token(const char *token, bool allow_void, tcc_typedesc_t **out_desc,
                                     tcc_error_buffer_t *error_buf) {
	tcc_typedesc_t *desc = NULL;
	tcc_ffi_type_t parsed = TCC_FFI_VOID;
	size_t array_size = 0;
	if (!token || !out_desc) {
		tcc_set_error(error_buf, "type token is required");
		return false;
	}
	*out_desc = NULL;
	if (!tcc_parse_type_token(token, allow_void, &parsed, &array_size)) {
		tcc_set_error(error_buf, "type token is unsupported");
		return false;
	}
	desc = (tcc_typedesc_t *)duckdb_malloc(sizeof(tcc_typedesc_t));
	if (!desc) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	memset(desc, 0, sizeof(*desc));
	desc->ffi_type = parsed;
	desc->array_size = array_size;
	desc->token = tcc_strdup(token);
	if (!desc->token) {
		duckdb_free(desc);
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	if (tcc_ffi_type_is_list(parsed)) {
		tcc_ffi_type_t child_ffi = TCC_FFI_VOID;
		const char *child_token = NULL;
		char *owned_child_token = NULL;
		desc->kind = TCC_TYPEDESC_LIST;
		if (tcc_ffi_list_child_type(parsed, &child_ffi)) {
			child_token = tcc_ffi_type_to_token(child_ffi);
		} else {
			size_t token_len_local = strlen(token);
			if (token_len_local > 6 && token[4] == '<' && token[token_len_local - 1] == '>') {
				owned_child_token = (char *)duckdb_malloc(token_len_local - 4);
				if (!owned_child_token) {
					tcc_typedesc_destroy(desc);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memcpy(owned_child_token, token + 5, token_len_local - 6);
				owned_child_token[token_len_local - 6] = '\0';
				child_token = tcc_trim_inplace(owned_child_token);
			} else if (token_len_local >= 5 && (token[0] == 'l' || token[0] == 'L') && (token[1] == 'i' || token[1] == 'I') &&
			           (token[2] == 's' || token[2] == 'S') && (token[3] == 't' || token[3] == 'T') && token[4] == '_') {
				child_token = token + 5;
			} else if (token_len_local > 2 && token[token_len_local - 2] == '[' && token[token_len_local - 1] == ']') {
				owned_child_token = (char *)duckdb_malloc(token_len_local - 1);
				if (!owned_child_token) {
					tcc_typedesc_destroy(desc);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memcpy(owned_child_token, token, token_len_local - 2);
				owned_child_token[token_len_local - 2] = '\0';
				child_token = tcc_trim_inplace(owned_child_token);
			}
		}
		if (!child_token || !tcc_typedesc_parse_token(child_token, false, &desc->as.list_like.child, error_buf)) {
			if (owned_child_token) {
				duckdb_free(owned_child_token);
			}
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (owned_child_token) {
			duckdb_free(owned_child_token);
		}
	} else if (tcc_ffi_type_is_array(parsed)) {
		tcc_ffi_type_t child_ffi = TCC_FFI_VOID;
		const char *child_token = NULL;
		char *owned_child_token = NULL;
		desc->kind = TCC_TYPEDESC_ARRAY;
		if (tcc_ffi_array_child_type(parsed, &child_ffi)) {
			child_token = tcc_ffi_type_to_token(child_ffi);
		} else {
			size_t token_len_local = strlen(token);
			const char *lb = strrchr(token, '[');
			if (lb && token_len_local > 3 && token[token_len_local - 1] == ']' && lb < token + token_len_local - 1) {
				size_t child_len = (size_t)(lb - token);
				owned_child_token = (char *)duckdb_malloc(child_len + 1);
				if (!owned_child_token) {
					tcc_typedesc_destroy(desc);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memcpy(owned_child_token, token, child_len);
				owned_child_token[child_len] = '\0';
				child_token = tcc_trim_inplace(owned_child_token);
			}
		}
		if (!child_token || !tcc_typedesc_parse_token(child_token, false, &desc->as.list_like.child, error_buf)) {
			if (owned_child_token) {
				duckdb_free(owned_child_token);
			}
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (owned_child_token) {
			duckdb_free(owned_child_token);
		}
	} else if (parsed == TCC_FFI_STRUCT) {
		tcc_ffi_struct_meta_t meta;
		idx_t i;
		desc->kind = TCC_TYPEDESC_STRUCT;
		memset(&meta, 0, sizeof(meta));
		if (!tcc_parse_struct_meta_token(token, &meta, error_buf)) {
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (meta.field_count > 0) {
			desc->as.struct_like.fields =
			    (tcc_typedesc_field_t *)duckdb_malloc(sizeof(tcc_typedesc_field_t) * (size_t)meta.field_count);
			if (!desc->as.struct_like.fields) {
				tcc_struct_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			memset(desc->as.struct_like.fields, 0, sizeof(tcc_typedesc_field_t) * (size_t)meta.field_count);
		}
		desc->as.struct_like.count = (idx_t)meta.field_count;
		for (i = 0; i < (idx_t)meta.field_count; i++) {
			desc->as.struct_like.fields[i].name = tcc_strdup(meta.field_names[i]);
			if (!desc->as.struct_like.fields[i].name ||
			    !tcc_typedesc_parse_token(meta.field_tokens[i], false, &desc->as.struct_like.fields[i].type, error_buf)) {
				tcc_struct_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				if (!error_buf || error_buf->message[0] == '\0') {
					tcc_set_error(error_buf, "out of memory");
				}
				return false;
			}
		}
		tcc_struct_meta_destroy(&meta);
	} else if (parsed == TCC_FFI_MAP) {
		tcc_ffi_map_meta_t meta;
		desc->kind = TCC_TYPEDESC_MAP;
		memset(&meta, 0, sizeof(meta));
		if (!tcc_parse_map_meta_token(token, &meta, error_buf) ||
		    !tcc_typedesc_parse_token(meta.key_token, false, &desc->as.map_like.key, error_buf) ||
		    !tcc_typedesc_parse_token(meta.value_token, false, &desc->as.map_like.value, error_buf)) {
			tcc_map_meta_destroy(&meta);
			tcc_typedesc_destroy(desc);
			return false;
		}
		tcc_map_meta_destroy(&meta);
	} else if (parsed == TCC_FFI_UNION) {
		tcc_ffi_union_meta_t meta;
		idx_t i;
		desc->kind = TCC_TYPEDESC_UNION;
		memset(&meta, 0, sizeof(meta));
		if (!tcc_parse_union_meta_token(token, &meta, error_buf)) {
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (meta.member_count > 0) {
			desc->as.union_like.members =
			    (tcc_typedesc_field_t *)duckdb_malloc(sizeof(tcc_typedesc_field_t) * (size_t)meta.member_count);
			if (!desc->as.union_like.members) {
				tcc_union_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			memset(desc->as.union_like.members, 0, sizeof(tcc_typedesc_field_t) * (size_t)meta.member_count);
		}
		desc->as.union_like.count = (idx_t)meta.member_count;
		for (i = 0; i < (idx_t)meta.member_count; i++) {
			const char *member_token = meta.member_tokens ? meta.member_tokens[i] : NULL;
			desc->as.union_like.members[i].name = tcc_strdup(meta.member_names[i]);
			if (!member_token || !desc->as.union_like.members[i].name ||
			    !tcc_typedesc_parse_token(member_token, false, &desc->as.union_like.members[i].type, error_buf)) {
				tcc_union_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				if (!error_buf || error_buf->message[0] == '\0') {
					tcc_set_error(error_buf, "out of memory");
				}
				return false;
			}
		}
		tcc_union_meta_destroy(&meta);
	} else {
		desc->kind = TCC_TYPEDESC_PRIMITIVE;
	}
	*out_desc = desc;
	return true;
}

/* tcc_parse_signature: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_parse_signature(const char *return_type, const char *arg_types_csv, tcc_ffi_type_t *out_return_type,
		                                size_t *out_return_array_size, tcc_ffi_type_t **out_arg_types,
		                                size_t **out_arg_array_sizes, tcc_ffi_struct_meta_t *out_return_struct_meta,
		                                tcc_ffi_map_meta_t *out_return_map_meta,
		                                tcc_ffi_union_meta_t *out_return_union_meta,
		                                tcc_ffi_struct_meta_t **out_arg_struct_metas,
		                                tcc_ffi_map_meta_t **out_arg_map_metas,
		                                tcc_ffi_union_meta_t **out_arg_union_metas, int *out_arg_count,
		                                tcc_error_buffer_t *error_buf) {
	tcc_typedesc_t *return_desc = NULL;
	tcc_typedesc_t *arg_desc = NULL;
	tcc_string_list_t arg_tokens;
	tcc_ffi_struct_meta_t return_struct_meta;
	tcc_ffi_map_meta_t return_map_meta;
	tcc_ffi_union_meta_t return_union_meta;
	tcc_ffi_type_t *arg_types = NULL;
	size_t *arg_array_sizes = NULL;
	tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
	tcc_ffi_map_meta_t *arg_map_metas = NULL;
	tcc_ffi_union_meta_t *arg_union_metas = NULL;
	int argc = 0;
	idx_t i;
	memset(&arg_tokens, 0, sizeof(arg_tokens));
	memset(&return_struct_meta, 0, sizeof(return_struct_meta));
	memset(&return_map_meta, 0, sizeof(return_map_meta));
	memset(&return_union_meta, 0, sizeof(return_union_meta));
	if (!return_type || return_type[0] == '\0' || !out_return_type || !out_return_array_size || !out_arg_types ||
	    !out_arg_array_sizes || !out_return_struct_meta || !out_return_map_meta || !out_return_union_meta ||
	    !out_arg_struct_metas || !out_arg_map_metas || !out_arg_union_metas || !out_arg_count) {
		tcc_set_error(error_buf, "return_type is required");
		return false;
	}
	if (!arg_types_csv) {
		tcc_set_error(error_buf, "arg_types is required (use [] for no args)");
		return false;
	}
	if (!tcc_typedesc_parse_token(return_type, true, &return_desc, error_buf)) {
		tcc_set_error(error_buf, "return_type contains unsupported type token");
		return false;
	}
	if (return_desc->ffi_type == TCC_FFI_STRUCT &&
	    !tcc_parse_struct_meta_token(return_desc->token, &return_struct_meta, error_buf)) {
		goto fail;
	}
	if (return_desc->ffi_type == TCC_FFI_MAP &&
	    !tcc_parse_map_meta_token(return_desc->token, &return_map_meta, error_buf)) {
		goto fail;
	}
	if (return_desc->ffi_type == TCC_FFI_UNION &&
	    !tcc_parse_union_meta_token(return_desc->token, &return_union_meta, error_buf)) {
		goto fail;
	}
	if (!tcc_split_csv_tokens(arg_types_csv, &arg_tokens, error_buf)) {
		goto fail;
	}
	argc = (int)arg_tokens.count;
	if (argc > 0) {
		arg_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)argc);
		arg_array_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)argc);
		arg_struct_metas = (tcc_ffi_struct_meta_t *)duckdb_malloc(sizeof(tcc_ffi_struct_meta_t) * (size_t)argc);
		arg_map_metas = (tcc_ffi_map_meta_t *)duckdb_malloc(sizeof(tcc_ffi_map_meta_t) * (size_t)argc);
		arg_union_metas = (tcc_ffi_union_meta_t *)duckdb_malloc(sizeof(tcc_ffi_union_meta_t) * (size_t)argc);
		if (!arg_types || !arg_array_sizes || !arg_struct_metas || !arg_map_metas || !arg_union_metas) {
			tcc_set_error(error_buf, "out of memory");
			goto fail;
		}
		memset(arg_struct_metas, 0, sizeof(tcc_ffi_struct_meta_t) * (size_t)argc);
		memset(arg_map_metas, 0, sizeof(tcc_ffi_map_meta_t) * (size_t)argc);
		memset(arg_union_metas, 0, sizeof(tcc_ffi_union_meta_t) * (size_t)argc);
	}
	for (i = 0; i < (idx_t)argc; i++) {
		if (!tcc_typedesc_parse_token(arg_tokens.items[i], false, &arg_desc, error_buf)) {
			tcc_set_error(error_buf, "arg_types contains unsupported type token");
			goto fail;
		}
		arg_types[i] = arg_desc->ffi_type;
		arg_array_sizes[i] = arg_desc->array_size;
		if (arg_types[i] == TCC_FFI_STRUCT && !tcc_parse_struct_meta_token(arg_desc->token, &arg_struct_metas[i], error_buf)) {
			goto fail;
		}
		if (arg_types[i] == TCC_FFI_MAP && !tcc_parse_map_meta_token(arg_desc->token, &arg_map_metas[i], error_buf)) {
			goto fail;
		}
		if (arg_types[i] == TCC_FFI_UNION && !tcc_parse_union_meta_token(arg_desc->token, &arg_union_metas[i], error_buf)) {
			goto fail;
		}
		tcc_typedesc_destroy(arg_desc);
		arg_desc = NULL;
	}
	*out_return_type = return_desc->ffi_type;
	*out_return_array_size = return_desc->array_size;
	*out_arg_types = arg_types;
	*out_arg_array_sizes = arg_array_sizes;
	*out_return_struct_meta = return_struct_meta;
	*out_return_map_meta = return_map_meta;
	*out_return_union_meta = return_union_meta;
	*out_arg_struct_metas = arg_struct_metas;
	*out_arg_map_metas = arg_map_metas;
	*out_arg_union_metas = arg_union_metas;
	*out_arg_count = argc;
	tcc_string_list_destroy(&arg_tokens);
	tcc_typedesc_destroy(return_desc);
	return true;
fail:
	if (arg_desc) {
		tcc_typedesc_destroy(arg_desc);
	}
	tcc_typedesc_destroy(return_desc);
	tcc_string_list_destroy(&arg_tokens);
	if (arg_types) {
		duckdb_free(arg_types);
	}
	if (arg_array_sizes) {
		duckdb_free(arg_array_sizes);
	}
	if (arg_struct_metas) {
		tcc_struct_meta_array_destroy(arg_struct_metas, argc);
	}
	if (arg_map_metas) {
		tcc_map_meta_array_destroy(arg_map_metas, argc);
	}
	if (arg_union_metas) {
		tcc_union_meta_array_destroy(arg_union_metas, argc);
	}
	tcc_struct_meta_destroy(&return_struct_meta);
	tcc_map_meta_destroy(&return_map_meta);
	tcc_union_meta_destroy(&return_union_meta);
	return false;
}

/* tcc_codegen_signature_ctx_init: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_codegen_signature_ctx_init(tcc_codegen_signature_ctx_t *ctx) {
	if (!ctx) {
		return;
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->return_type = TCC_FFI_I64;
	ctx->wrapper_mode = TCC_WRAPPER_MODE_ROW;
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
	snprintf(ctx->module_symbol, sizeof(ctx->module_symbol), "__ducktinycc_ffi_init_%llu_%llu",
	         (unsigned long long)state->session.state_id, (unsigned long long)state->session.config_version);
	ctx->wrapper_loader_source =
	    tcc_codegen_generate_wrapper_source(ctx->module_symbol, target_symbol, sql_name,
	                                        bind->return_type ? bind->return_type : "i64",
	                                        bind->arg_types ? bind->arg_types : "",
	                                        ctx->signature.wrapper_mode_token, ctx->signature.wrapper_mode,
	                                        ctx->signature.return_type, ctx->signature.arg_types,
	                                        ctx->signature.arg_count);
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
static const char *tcc_effective_symbol(tcc_module_state_t *state, tcc_module_bind_data_t *bind) {
	if (bind->symbol && bind->symbol[0] != '\0') {
		return bind->symbol;
	}
	if (state->session.bound_symbol && state->session.bound_symbol[0] != '\0') {
		return state->session.bound_symbol;
	}
	return NULL;
}

/* Resolves SQL function name from explicit args or session-level fallback. */
static const char *tcc_effective_sql_name(tcc_module_state_t *state, tcc_module_bind_data_t *bind,
                                          const char *effective_symbol) {
	if (bind->sql_name && bind->sql_name[0] != '\0') {
		return bind->sql_name;
	}
	if (state->session.bound_sql_name && state->session.bound_sql_name[0] != '\0') {
		return state->session.bound_sql_name;
	}
	return effective_symbol;
}

static const char *tcc_ffi_type_to_token(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_BOOL:
		return "bool";
	case TCC_FFI_I8:
		return "i8";
	case TCC_FFI_U8:
		return "u8";
	case TCC_FFI_I16:
		return "i16";
	case TCC_FFI_U16:
		return "u16";
	case TCC_FFI_I32:
		return "i32";
	case TCC_FFI_U32:
		return "u32";
	case TCC_FFI_I64:
		return "i64";
	case TCC_FFI_U64:
		return "u64";
	case TCC_FFI_PTR:
		return "ptr";
	case TCC_FFI_F32:
		return "f32";
	case TCC_FFI_F64:
		return "f64";
	case TCC_FFI_UUID:
		return "uuid";
	case TCC_FFI_DATE:
		return "date";
	case TCC_FFI_TIME:
		return "time";
	case TCC_FFI_TIMESTAMP:
		return "timestamp";
	case TCC_FFI_INTERVAL:
		return "interval";
	case TCC_FFI_DECIMAL:
		return "decimal";
	case TCC_FFI_LIST:
		return "list";
	case TCC_FFI_ARRAY:
		return "array";
	case TCC_FFI_UNION:
		return "union";
	default:
		return NULL;
	}
}

static const char *tcc_ffi_type_to_c_type_name(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_VOID:
		return "void";
	case TCC_FFI_BOOL:
		return "_Bool";
	case TCC_FFI_I8:
		return "signed char";
	case TCC_FFI_U8:
		return "unsigned char";
	case TCC_FFI_I16:
		return "short";
	case TCC_FFI_U16:
		return "unsigned short";
	case TCC_FFI_I32:
		return "int";
	case TCC_FFI_U32:
		return "unsigned int";
	case TCC_FFI_I64:
		return "long long";
	case TCC_FFI_U64:
		return "unsigned long long";
	case TCC_FFI_PTR:
		return "void *";
	case TCC_FFI_F32:
		return "float";
	case TCC_FFI_F64:
		return "double";
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
	case TCC_FFI_LIST:
	case TCC_FFI_LIST_BOOL:
	case TCC_FFI_LIST_I8:
	case TCC_FFI_LIST_U8:
	case TCC_FFI_LIST_I16:
	case TCC_FFI_LIST_U16:
	case TCC_FFI_LIST_I32:
	case TCC_FFI_LIST_U32:
	case TCC_FFI_LIST_I64:
	case TCC_FFI_LIST_U64:
	case TCC_FFI_LIST_F32:
	case TCC_FFI_LIST_F64:
	case TCC_FFI_LIST_UUID:
	case TCC_FFI_LIST_DATE:
	case TCC_FFI_LIST_TIME:
	case TCC_FFI_LIST_TIMESTAMP:
	case TCC_FFI_LIST_INTERVAL:
	case TCC_FFI_LIST_DECIMAL:
		return "ducktinycc_list_t";
	case TCC_FFI_ARRAY:
	case TCC_FFI_ARRAY_BOOL:
	case TCC_FFI_ARRAY_I8:
	case TCC_FFI_ARRAY_U8:
	case TCC_FFI_ARRAY_I16:
	case TCC_FFI_ARRAY_U16:
	case TCC_FFI_ARRAY_I32:
	case TCC_FFI_ARRAY_U32:
	case TCC_FFI_ARRAY_I64:
	case TCC_FFI_ARRAY_U64:
	case TCC_FFI_ARRAY_F32:
	case TCC_FFI_ARRAY_F64:
	case TCC_FFI_ARRAY_UUID:
	case TCC_FFI_ARRAY_DATE:
	case TCC_FFI_ARRAY_TIME:
	case TCC_FFI_ARRAY_TIMESTAMP:
	case TCC_FFI_ARRAY_INTERVAL:
	case TCC_FFI_ARRAY_DECIMAL:
		return "ducktinycc_array_t";
	default:
		return NULL;
	}
}

static char *tcc_codegen_generate_wrapper_source(const char *module_symbol, const char *target_symbol,
                                                 const char *sql_name, const char *return_type,
                                                 const char *arg_types_csv, const char *wrapper_mode_token,
                                                 tcc_wrapper_mode_t wrapper_mode, tcc_ffi_type_t ret_type,
                                                 const tcc_ffi_type_t *arg_types, int arg_count) {
	tcc_text_buf_t args_decl = {0};
	tcc_text_buf_t row_unpack_lines = {0};
	tcc_text_buf_t row_call_args = {0};
	tcc_text_buf_t batch_col_decls = {0};
	tcc_text_buf_t batch_call_args = {0};
	tcc_text_buf_t batch_null_checks = {0};
	tcc_text_buf_t src = {0};
	const char *ret_c_type = tcc_ffi_type_to_c_type_name(ret_type);
	const char *resolved_wrapper_mode = wrapper_mode_token ? wrapper_mode_token : tcc_wrapper_mode_token(wrapper_mode);
	char *wrapper_name = NULL;
	char *out_src = NULL;
	size_t wrapper_len;
	int i;
	bool ok = true;
	if (!ret_c_type) {
		return NULL;
	}
	if (!module_symbol || !target_symbol || !sql_name || !return_type || !arg_types_csv || !resolved_wrapper_mode ||
	    arg_count < 0) {
		return NULL;
	}
	wrapper_len = strlen("__ducktinycc_wrapper_") + strlen(module_symbol) + 1;
	wrapper_name = (char *)duckdb_malloc(wrapper_len);
	if (!wrapper_name) {
		return NULL;
	}
	snprintf(wrapper_name, wrapper_len, "__ducktinycc_wrapper_%s", module_symbol);
	for (i = 0; i < arg_count; i++) {
		const char *arg_c_type = tcc_ffi_type_to_c_type_name(arg_types[i]);
		if (!arg_c_type) {
			ok = false;
			break;
		}
		if (!tcc_text_buf_appendf(&args_decl, "%s%s a%d", i == 0 ? "" : ", ", arg_c_type, i)) {
			ok = false;
			break;
		}
		if (arg_types[i] == TCC_FFI_PTR) {
			if (!tcc_text_buf_appendf(&row_unpack_lines,
			                          "  void *a%d = (void *)(uintptr_t)(*(unsigned long long *)args[%d]);\n", i, i) ||
			    !tcc_text_buf_appendf(&row_call_args, "%sa%d", i == 0 ? "" : ", ", i) ||
			    !tcc_text_buf_appendf(&batch_col_decls, "  unsigned long long *col%d_ptr = (unsigned long long *)arg_data[%d];\n",
			                          i, i) ||
			    !tcc_text_buf_appendf(&batch_call_args, "%s(void *)(uintptr_t)col%d_ptr[row]", i == 0 ? "" : ", ", i) ||
			    !tcc_text_buf_appendf(
			        &batch_null_checks,
			        "%s(arg_validity[%d] && ((arg_validity[%d][row >> 6] & (1ULL << (row & 63))) == 0))",
			        i == 0 ? "" : " || ", i, i)) {
				ok = false;
				break;
			}
		} else {
			if (!tcc_text_buf_appendf(&row_unpack_lines, "  %s a%d = *(%s *)args[%d];\n", arg_c_type, i, arg_c_type, i) ||
			    !tcc_text_buf_appendf(&row_call_args, "%sa%d", i == 0 ? "" : ", ", i) ||
			    !tcc_text_buf_appendf(&batch_col_decls, "  %s *col%d = (%s *)arg_data[%d];\n", arg_c_type, i, arg_c_type,
			                          i) ||
			    !tcc_text_buf_appendf(&batch_call_args, "%scol%d[row]", i == 0 ? "" : ", ", i) ||
			    !tcc_text_buf_appendf(
			        &batch_null_checks,
			        "%s(arg_validity[%d] && ((arg_validity[%d][row >> 6] & (1ULL << (row & 63))) == 0))",
			        i == 0 ? "" : " || ", i, i)) {
				ok = false;
				break;
			}
		}
	}
	if (ok && arg_count == 0) {
		ok = tcc_text_buf_appendf(&args_decl, "void");
	}
	if (ok && wrapper_mode == TCC_WRAPPER_MODE_ROW) {
		ok = tcc_text_buf_appendf(
		    &src,
		    "#include <stdint.h>\n"
		    "typedef struct _duckdb_connection *duckdb_connection;\n"
		    "extern _Bool ducktinycc_register_signature(duckdb_connection con, const char *name, void *fn_ptr, "
		    "const char *return_type, const char *arg_types_csv, const char *wrapper_mode);\n"
		    "extern %s %s(%s);\n"
		    "static _Bool %s(void **args, void *out_value, _Bool *out_is_null) {\n%s",
		    ret_c_type, target_symbol, args_decl.data ? args_decl.data : "", wrapper_name,
		    row_unpack_lines.data ? row_unpack_lines.data : "");
		if (ok && ret_type == TCC_FFI_VOID) {
			ok = tcc_text_buf_appendf(&src,
			                          "  %s(%s);\n"
			                          "  if (out_is_null) { *out_is_null = 1; }\n"
			                          "  (void)out_value;\n"
			                          "  return 1;\n"
			                          "}\n",
			                          target_symbol, row_call_args.data ? row_call_args.data : "");
		} else if (ok && ret_type == TCC_FFI_VARCHAR) {
			ok = tcc_text_buf_appendf(&src,
			                          "  %s result = %s(%s);\n"
			                          "  if (!result) {\n"
			                          "    if (out_is_null) { *out_is_null = 1; }\n"
			                          "    return 1;\n"
			                          "  }\n"
			                          "  *(%s *)out_value = result;\n"
			                          "  if (out_is_null) { *out_is_null = 0; }\n"
			                          "  return 1;\n"
			                          "}\n",
			                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
			                          ret_c_type);
			} else if (ok && ret_type == TCC_FFI_BLOB) {
				ok = tcc_text_buf_appendf(&src,
				                          "  %s result = %s(%s);\n"
				                          "  if (!result.ptr) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n"
				                          "  *(%s *)out_value = result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n",
				                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
				                          ret_c_type);
			} else if (ok && ret_type == TCC_FFI_PTR) {
				ok = tcc_text_buf_appendf(&src,
				                          "  void *result = (void *)%s(%s);\n"
				                          "  if (!result) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n"
				                          "  *(unsigned long long *)out_value = (unsigned long long)(uintptr_t)result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n",
				                          target_symbol, row_call_args.data ? row_call_args.data : "");
			} else if (ok && (tcc_ffi_type_is_list(ret_type) || tcc_ffi_type_is_array(ret_type))) {
				ok = tcc_text_buf_appendf(&src,
				                          "  %s result = %s(%s);\n"
				                          "  if (!result.ptr) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n"
				                          "  *(%s *)out_value = result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n",
				                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
				                          ret_c_type);
			} else if (ok && tcc_ffi_type_is_struct(ret_type)) {
				ok = tcc_text_buf_appendf(&src,
				                          "  %s result = %s(%s);\n"
				                          "  if (!result.field_ptrs || result.field_count == 0) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n"
				                          "  *(%s *)out_value = result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n",
				                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
				                          ret_c_type);
			} else if (ok && tcc_ffi_type_is_map(ret_type)) {
				ok = tcc_text_buf_appendf(&src,
				                          "  %s result = %s(%s);\n"
				                          "  if (result.len > 0 && (!result.key_ptr || !result.value_ptr)) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n"
				                          "  *(%s *)out_value = result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n",
				                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
				                          ret_c_type);
			} else if (ok && tcc_ffi_type_is_union(ret_type)) {
				ok = tcc_text_buf_appendf(&src,
				                          "  %s result = %s(%s);\n"
				                          "  if (!result.tag_ptr || !result.member_ptrs || result.member_count == 0) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n"
				                          "  *(%s *)out_value = result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n",
				                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
				                          ret_c_type);
			} else if (ok) {
				ok = tcc_text_buf_appendf(&src,
				                          "  %s result = %s(%s);\n"
				                          "  *(%s *)out_value = result;\n"
			                          "  if (out_is_null) { *out_is_null = 0; }\n"
			                          "  return 1;\n"
			                          "}\n",
			                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "",
			                          ret_c_type);
		}
	} else if (ok && wrapper_mode == TCC_WRAPPER_MODE_BATCH) {
		ok = tcc_text_buf_appendf(
		    &src,
		    "#include <stdint.h>\n"
		    "typedef struct _duckdb_connection *duckdb_connection;\n"
		    "extern _Bool ducktinycc_register_signature(duckdb_connection con, const char *name, void *fn_ptr, "
		    "const char *return_type, const char *arg_types_csv, const char *wrapper_mode);\n"
		    "extern %s %s(%s);\n"
		    "static _Bool %s(void **arg_data, uint64_t **arg_validity, uint64_t count, void *out_data, uint64_t "
		    "*out_validity) {\n%s",
		    ret_c_type, target_symbol, args_decl.data ? args_decl.data : "", wrapper_name,
		    batch_col_decls.data ? batch_col_decls.data : "");
		if (ok && ret_type == TCC_FFI_VOID) {
			ok = tcc_text_buf_appendf(&src, "  (void)out_data;\n");
		} else if (ok && ret_type == TCC_FFI_PTR) {
			ok = tcc_text_buf_appendf(&src, "  unsigned long long *out = (unsigned long long *)out_data;\n");
		} else if (ok) {
			ok = tcc_text_buf_appendf(&src, "  %s *out = (%s *)out_data;\n", ret_c_type, ret_c_type);
		}
		if (ok) {
			ok = tcc_text_buf_appendf(&src, "  for (uint64_t row = 0; row < count; row++) {\n");
		}
		if (ok && arg_count > 0) {
			ok = tcc_text_buf_appendf(&src,
			                          "    if (%s) {\n"
			                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
			                          "      continue;\n"
			                          "    }\n",
			                          batch_null_checks.data ? batch_null_checks.data : "");
		}
		if (ok && ret_type == TCC_FFI_VOID) {
			ok = tcc_text_buf_appendf(&src,
			                          "    %s(%s);\n"
			                          "    if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n",
			                          target_symbol, batch_call_args.data ? batch_call_args.data : "");
		} else if (ok && ret_type == TCC_FFI_VARCHAR) {
			ok = tcc_text_buf_appendf(&src,
			                          "    %s result = %s(%s);\n"
			                          "    if (!result) {\n"
			                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
			                          "      continue;\n"
			                          "    }\n"
			                          "    out[row] = result;\n",
			                          ret_c_type, target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok && ret_type == TCC_FFI_BLOB) {
				ok = tcc_text_buf_appendf(&src,
				                          "    %s result = %s(%s);\n"
				                          "    if (!result.ptr) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = result;\n",
				                          ret_c_type, target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok && ret_type == TCC_FFI_PTR) {
				ok = tcc_text_buf_appendf(&src,
				                          "    void *result = (void *)%s(%s);\n"
				                          "    if (!result) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = (unsigned long long)(uintptr_t)result;\n",
				                          target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok && (tcc_ffi_type_is_list(ret_type) || tcc_ffi_type_is_array(ret_type))) {
				ok = tcc_text_buf_appendf(&src,
				                          "    %s result = %s(%s);\n"
				                          "    if (!result.ptr) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = result;\n",
				                          ret_c_type, target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok && tcc_ffi_type_is_struct(ret_type)) {
				ok = tcc_text_buf_appendf(&src,
				                          "    %s result = %s(%s);\n"
				                          "    if (!result.field_ptrs || result.field_count == 0) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = result;\n",
				                          ret_c_type, target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok && tcc_ffi_type_is_map(ret_type)) {
				ok = tcc_text_buf_appendf(&src,
				                          "    %s result = %s(%s);\n"
				                          "    if (result.len > 0 && (!result.key_ptr || !result.value_ptr)) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = result;\n",
				                          ret_c_type, target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok && tcc_ffi_type_is_union(ret_type)) {
				ok = tcc_text_buf_appendf(&src,
				                          "    %s result = %s(%s);\n"
				                          "    if (!result.tag_ptr || !result.member_ptrs || result.member_count == 0) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = result;\n",
				                          ret_c_type, target_symbol, batch_call_args.data ? batch_call_args.data : "");
			} else if (ok) {
				ok = tcc_text_buf_appendf(&src, "    out[row] = %s(%s);\n", target_symbol,
				                          batch_call_args.data ? batch_call_args.data : "");
			}
		if (ok) {
			ok = tcc_text_buf_appendf(&src,
			                          "  }\n"
			                          "  return 1;\n"
			                          "}\n");
		}
	} else {
		ok = false;
	}
	if (ok) {
		ok = tcc_text_buf_appendf(&src,
		                          "_Bool %s(duckdb_connection con) {\n"
		                          "  return ducktinycc_register_signature(con, \"%s\", (void *)%s, \"%s\", \"%s\", "
		                          "\"%s\");\n"
		                          "}\n",
		                          module_symbol, sql_name, wrapper_name, return_type, arg_types_csv,
		                          resolved_wrapper_mode);
	}
	if (ok && src.data) {
		out_src = tcc_strdup(src.data);
	}
	if (wrapper_name) {
		duckdb_free(wrapper_name);
	}
	tcc_text_buf_destroy(&args_decl);
	tcc_text_buf_destroy(&row_unpack_lines);
	tcc_text_buf_destroy(&row_call_args);
	tcc_text_buf_destroy(&batch_col_decls);
	tcc_text_buf_destroy(&batch_call_args);
	tcc_text_buf_destroy(&batch_null_checks);
	tcc_text_buf_destroy(&src);
	return out_src;
}

static char *tcc_codegen_build_compilation_unit(const char *user_source, const char *wrapper_loader_source) {
	char *compilation_unit_source;
	const char *prelude = "#include <stdint.h>\n"
	                      "/* Composite descriptors below are borrowed views from DuckDB vectors. */\n"
	                      "/* Wrappers must not free them and must not retain them after invocation. */\n"
	                      "typedef struct {\n"
	                      "  uint64_t lower;\n"
	                      "  int64_t upper;\n"
	                      "} ducktinycc_hugeint_t;\n"
	                      "typedef struct {\n"
	                      "  const void *ptr;\n"
	                      "  uint64_t len;\n"
	                      "} ducktinycc_blob_t;\n"
	                      "typedef struct {\n"
	                      "  int32_t days;\n"
	                      "} ducktinycc_date_t;\n"
	                      "typedef struct {\n"
	                      "  int64_t micros;\n"
	                      "} ducktinycc_time_t;\n"
	                      "typedef struct {\n"
	                      "  int64_t micros;\n"
	                      "} ducktinycc_timestamp_t;\n"
	                      "typedef struct {\n"
	                      "  int32_t months;\n"
	                      "  int32_t days;\n"
	                      "  int64_t micros;\n"
	                      "} ducktinycc_interval_t;\n"
	                      "typedef struct {\n"
	                      "  uint8_t width;\n"
	                      "  uint8_t scale;\n"
	                      "  ducktinycc_hugeint_t value;\n"
	                      "} ducktinycc_decimal_t;\n"
	                      "typedef struct {\n"
	                      "  const void *ptr;\n"
	                      "  const uint64_t *validity;\n"
	                      "  uint64_t offset;\n"
	                      "  uint64_t len;\n"
	                      "} ducktinycc_list_t;\n"
	                      "typedef struct {\n"
	                      "  const void *ptr;\n"
	                      "  const uint64_t *validity;\n"
	                      "  uint64_t offset;\n"
	                      "  uint64_t len;\n"
	                      "} ducktinycc_array_t;\n"
	                      "typedef struct {\n"
	                      "  const void *const *field_ptrs;\n"
	                      "  const uint64_t *const *field_validity;\n"
	                      "  uint64_t field_count;\n"
	                      "  uint64_t offset;\n"
	                      "} ducktinycc_struct_t;\n"
	                      "typedef struct {\n"
	                      "  const void *key_ptr;\n"
	                      "  const uint64_t *key_validity;\n"
	                      "  const void *value_ptr;\n"
		                      "  const uint64_t *value_validity;\n"
		                      "  uint64_t offset;\n"
		                      "  uint64_t len;\n"
		                      "} ducktinycc_map_t;\n"
	                      "typedef struct {\n"
	                      "  const uint8_t *tag_ptr;\n"
	                      "  const void *const *member_ptrs;\n"
	                      "  const uint64_t *const *member_validity;\n"
	                      "  uint64_t member_count;\n"
	                      "  uint64_t offset;\n"
	                      "} ducktinycc_union_t;\n"
	                      "/* Accessor helpers below operate on caller-owned memory spans. */\n"
		                      "extern int ducktinycc_valid_is_set(const uint64_t *validity, uint64_t idx);\n"
		                      "extern void ducktinycc_valid_set(uint64_t *validity, uint64_t idx, int valid);\n"
		                      "extern int ducktinycc_span_contains(uint64_t len, uint64_t idx);\n"
		                      "extern const void *ducktinycc_ptr_add(const void *base, uint64_t byte_offset);\n"
		                      "extern void *ducktinycc_ptr_add_mut(void *base, uint64_t byte_offset);\n"
		                      "extern int ducktinycc_span_fits(uint64_t len, uint64_t offset, uint64_t width);\n"
		                      "extern const void *ducktinycc_buf_ptr_at(const void *base, uint64_t len, uint64_t offset, uint64_t width);\n"
		                      "extern void *ducktinycc_buf_ptr_at_mut(void *base, uint64_t len, uint64_t offset, uint64_t width);\n"
		                      "extern int ducktinycc_read_bytes(const void *base, uint64_t len, uint64_t offset, void *out, uint64_t width);\n"
		                      "extern int ducktinycc_write_bytes(void *base, uint64_t len, uint64_t offset, const void *in, uint64_t width);\n"
		                      "extern int ducktinycc_read_i8(const void *base, uint64_t len, uint64_t offset, int8_t *out);\n"
		                      "extern int ducktinycc_write_i8(void *base, uint64_t len, uint64_t offset, int8_t value);\n"
		                      "extern int ducktinycc_read_u8(const void *base, uint64_t len, uint64_t offset, uint8_t *out);\n"
		                      "extern int ducktinycc_write_u8(void *base, uint64_t len, uint64_t offset, uint8_t value);\n"
		                      "extern int ducktinycc_read_i16(const void *base, uint64_t len, uint64_t offset, int16_t *out);\n"
		                      "extern int ducktinycc_write_i16(void *base, uint64_t len, uint64_t offset, int16_t value);\n"
		                      "extern int ducktinycc_read_u16(const void *base, uint64_t len, uint64_t offset, uint16_t *out);\n"
		                      "extern int ducktinycc_write_u16(void *base, uint64_t len, uint64_t offset, uint16_t value);\n"
		                      "extern int ducktinycc_read_i32(const void *base, uint64_t len, uint64_t offset, int32_t *out);\n"
		                      "extern int ducktinycc_write_i32(void *base, uint64_t len, uint64_t offset, int32_t value);\n"
		                      "extern int ducktinycc_read_u32(const void *base, uint64_t len, uint64_t offset, uint32_t *out);\n"
		                      "extern int ducktinycc_write_u32(void *base, uint64_t len, uint64_t offset, uint32_t value);\n"
		                      "extern int ducktinycc_read_i64(const void *base, uint64_t len, uint64_t offset, int64_t *out);\n"
		                      "extern int ducktinycc_write_i64(void *base, uint64_t len, uint64_t offset, int64_t value);\n"
		                      "extern int ducktinycc_read_u64(const void *base, uint64_t len, uint64_t offset, uint64_t *out);\n"
		                      "extern int ducktinycc_write_u64(void *base, uint64_t len, uint64_t offset, uint64_t value);\n"
		                      "extern int ducktinycc_read_f32(const void *base, uint64_t len, uint64_t offset, float *out);\n"
		                      "extern int ducktinycc_write_f32(void *base, uint64_t len, uint64_t offset, float value);\n"
		                      "extern int ducktinycc_read_f64(const void *base, uint64_t len, uint64_t offset, double *out);\n"
		                      "extern int ducktinycc_write_f64(void *base, uint64_t len, uint64_t offset, double value);\n"
		                      "extern int ducktinycc_read_ptr(const void *base, uint64_t len, uint64_t offset, const void **out);\n"
		                      "extern int ducktinycc_write_ptr(void *base, uint64_t len, uint64_t offset, const void *value);\n"
		                      "extern int ducktinycc_list_is_valid(const ducktinycc_list_t *list, uint64_t idx);\n"
		                      "extern const void *ducktinycc_list_elem_ptr(const ducktinycc_list_t *list, uint64_t idx, uint64_t elem_size);\n"
		                      "extern int ducktinycc_array_is_valid(const ducktinycc_array_t *arr, uint64_t idx);\n"
		                      "extern const void *ducktinycc_array_elem_ptr(const ducktinycc_array_t *arr, uint64_t idx, uint64_t elem_size);\n"
		                      "extern const void *ducktinycc_struct_field_ptr(const ducktinycc_struct_t *st, uint64_t idx);\n"
		                      "extern int ducktinycc_struct_field_is_valid(const ducktinycc_struct_t *st, uint64_t field_idx);\n"
		                      "extern const void *ducktinycc_map_key_ptr(const ducktinycc_map_t *m, uint64_t idx, uint64_t key_size);\n"
		                      "extern const void *ducktinycc_map_value_ptr(const ducktinycc_map_t *m, uint64_t idx, uint64_t value_size);\n"
		                      "extern int ducktinycc_map_key_is_valid(const ducktinycc_map_t *m, uint64_t idx);\n"
		                      "extern int ducktinycc_map_value_is_valid(const ducktinycc_map_t *m, uint64_t idx);\n";
	size_t n0;
	size_t n1;
	size_t n2;
	if (!wrapper_loader_source) {
		return NULL;
	}
	if (!user_source || user_source[0] == '\0') {
		n0 = strlen(prelude);
		n2 = strlen(wrapper_loader_source);
		compilation_unit_source = (char *)duckdb_malloc(n0 + n2 + 2);
		if (!compilation_unit_source) {
			return NULL;
		}
		memcpy(compilation_unit_source, prelude, n0);
		memcpy(compilation_unit_source + n0, wrapper_loader_source, n2);
		compilation_unit_source[n0 + n2] = '\0';
		return compilation_unit_source;
	}
	n0 = strlen(prelude);
	n1 = strlen(user_source);
	n2 = strlen(wrapper_loader_source);
	compilation_unit_source = (char *)duckdb_malloc(n0 + n1 + n2 + 3);
	if (!compilation_unit_source) {
		return NULL;
	}
	memcpy(compilation_unit_source, prelude, n0);
	memcpy(compilation_unit_source + n0, user_source, n1);
	compilation_unit_source[n0 + n1] = '\n';
	memcpy(compilation_unit_source + n0 + n1 + 1, wrapper_loader_source, n2);
	compilation_unit_source[n0 + n1 + 1 + n2] = '\0';
	return compilation_unit_source;
}

/* tcc_helper_binding_list_add_prefixed: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_helper_binding_list_add_prefixed(tcc_helper_binding_list_t *bindings, const char *prefix,
                                                 const char *suffix, const char *return_type,
                                                 const char *arg_types_csv) {
	char symbol[512];
	char sql_name[512];
	int n_symbol;
	int n_sql;
	if (!bindings || !prefix || !suffix || !return_type || !arg_types_csv) {
		return false;
	}
	n_symbol = snprintf(symbol, sizeof(symbol), "%s_%s", prefix, suffix);
	n_sql = snprintf(sql_name, sizeof(sql_name), "%s_%s", prefix, suffix);
	if (n_symbol < 0 || n_sql < 0 || (size_t)n_symbol >= sizeof(symbol) || (size_t)n_sql >= sizeof(sql_name)) {
		return false;
	}
	return tcc_helper_binding_list_add(bindings, symbol, sql_name, return_type, arg_types_csv);
}

/* tcc_format_cstr: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_format_cstr(char *buf, size_t buf_size, const char *fmt, ...) {
	va_list args;
	int n;
	if (!buf || buf_size == 0 || !fmt) {
		return false;
	}
	va_start(args, fmt);
	n = vsnprintf(buf, buf_size, fmt, args);
	va_end(args);
	return n >= 0 && (size_t)n < buf_size;
}

static char *tcc_generate_c_composite_helpers_source(const char *kind_keyword, const char *type_name,
                                                     const char *prefix, const tcc_c_field_list_t *fields,
                                                     tcc_error_buffer_t *error_buf) {
	tcc_text_buf_t src = {0};
	idx_t i;
	bool ok;
	if (!kind_keyword || !type_name || !prefix || !fields) {
		tcc_set_error(error_buf, "invalid composite helper arguments");
		return NULL;
	}
	ok = tcc_text_buf_appendf(
	    &src,
	    "extern void *malloc(unsigned long long);\n"
	    "extern void free(void *);\n"
	    "/* Generated helpers allocate with libc malloc/free. Pair %s_new with %s_free. */\n"
	    "#ifndef DUCKTINYCC_OFFSETOF\n"
	    "#define DUCKTINYCC_OFFSETOF(type, member) ((unsigned long long)((const char *)&(((type *)0)->member) - (const char *)0))\n"
	    "#endif\n"
	    "unsigned long long %s_sizeof(void){ return (unsigned long long)sizeof(%s %s); }\n"
	    "unsigned long long %s_alignof(void){ struct __ducktinycc_align_%s { char c; %s %s v; };"
	    " return (unsigned long long)(sizeof(struct __ducktinycc_align_%s) - sizeof(%s %s)); }\n"
	    "void *%s_new(void){ return malloc(sizeof(%s %s)); }\n"
	    "void %s_free(void *p){ if (p) free(p); }\n",
	    prefix, prefix, prefix, kind_keyword, type_name, prefix, prefix, kind_keyword, type_name, prefix,
	    kind_keyword, type_name, prefix, kind_keyword, type_name, prefix);
	if (!ok) {
		tcc_set_error(error_buf, "out of memory");
		tcc_text_buf_destroy(&src);
		return NULL;
	}
	for (i = 0; i < fields->count; i++) {
		const tcc_c_field_spec_t *field = &fields->items[i];
		const char *c_type = tcc_ffi_type_to_c_type_name(field->type);
		if (!c_type) {
			tcc_set_error(error_buf, "field type is unsupported for helper codegen");
			tcc_text_buf_destroy(&src);
			return NULL;
		}
		if (field->array_size > 0) {
			ok = tcc_text_buf_appendf(
			    &src,
			    "%s %s_get_%s_elt(void *p, unsigned long long idx){ %s out = (%s){0};"
			    " if (!p || idx >= %lluULL) return out; out = ((%s %s *)p)->%s[idx]; return out; }\n"
			    "void *%s_set_%s_elt(void *p, unsigned long long idx, %s value){"
			    " if (!p || idx >= %lluULL) return (void *)0; ((%s %s *)p)->%s[idx] = value; return p; }\n"
			    "unsigned long long %s_off_%s(void){ return DUCKTINYCC_OFFSETOF(%s %s, %s); }\n"
			    "void *%s_%s_addr(void *p){ if (!p) return (void *)0; return (void *)&((%s %s *)p)->%s[0]; }\n",
			    c_type, prefix, field->name, c_type, c_type, (unsigned long long)field->array_size, kind_keyword,
			    type_name, field->name, prefix, field->name, c_type, (unsigned long long)field->array_size,
			    kind_keyword, type_name, field->name, prefix, field->name, kind_keyword, type_name, field->name,
			    prefix, field->name, kind_keyword, type_name, field->name);
		} else {
			ok = tcc_text_buf_appendf(
			    &src,
			    "%s %s_get_%s(void *p){ %s out = (%s){0};"
			    " if (!p) return out; out = ((%s %s *)p)->%s; return out; }\n"
			    "void *%s_set_%s(void *p, %s value){ if (!p) return (void *)0; ((%s %s *)p)->%s = value; return p; }\n",
			    c_type, prefix, field->name, c_type, c_type, kind_keyword, type_name, field->name, prefix, field->name,
			    c_type, kind_keyword, type_name, field->name);
			if (ok && !field->is_bitfield) {
				ok = tcc_text_buf_appendf(
				    &src,
				    "unsigned long long %s_off_%s(void){ return DUCKTINYCC_OFFSETOF(%s %s, %s); }\n"
				    "void *%s_%s_addr(void *p){ if (!p) return (void *)0; return (void *)&((%s %s *)p)->%s; }\n",
				    prefix, field->name, kind_keyword, type_name, field->name, prefix, field->name, kind_keyword,
				    type_name, field->name);
			}
		}
		if (!ok) {
			tcc_set_error(error_buf, "out of memory");
			tcc_text_buf_destroy(&src);
			return NULL;
		}
	}
	if (!src.data) {
		tcc_set_error(error_buf, "out of memory");
		return NULL;
	}
	{
		char *out = tcc_strdup(src.data);
		tcc_text_buf_destroy(&src);
		if (!out) {
			tcc_set_error(error_buf, "out of memory");
		}
		return out;
	}
}

static char *tcc_generate_c_enum_helpers_source(const char *enum_name, const char *prefix,
                                                const tcc_string_list_t *constants, tcc_error_buffer_t *error_buf) {
	tcc_text_buf_t src = {0};
	idx_t i;
	if (!enum_name || !prefix || !constants) {
		tcc_set_error(error_buf, "invalid enum helper arguments");
		return NULL;
	}
	if (!tcc_text_buf_appendf(&src, "unsigned long long %s_sizeof(void){ return (unsigned long long)sizeof(enum %s); }\n",
	                          prefix, enum_name)) {
		tcc_set_error(error_buf, "out of memory");
		tcc_text_buf_destroy(&src);
		return NULL;
	}
	for (i = 0; i < constants->count; i++) {
		if (!tcc_text_buf_appendf(&src, "long long %s_%s(void){ return (long long)(%s); }\n", prefix,
		                          constants->items[i], constants->items[i])) {
			tcc_set_error(error_buf, "out of memory");
			tcc_text_buf_destroy(&src);
			return NULL;
		}
	}
	if (!src.data) {
		tcc_set_error(error_buf, "out of memory");
		return NULL;
	}
	{
		char *out = tcc_strdup(src.data);
		tcc_text_buf_destroy(&src);
		if (!out) {
			tcc_set_error(error_buf, "out of memory");
		}
		return out;
	}
}

/* tcc_build_c_composite_bindings: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_build_c_composite_bindings(const char *prefix, const tcc_c_field_list_t *fields,
                                           tcc_helper_binding_list_t *out_bindings, tcc_error_buffer_t *error_buf) {
	idx_t i;
	if (!prefix || !fields || !out_bindings) {
		tcc_set_error(error_buf, "invalid helper binding arguments");
		return false;
	}
#define TCC_ADD_BINDING_FORMAT(RET_TOKEN, ARG_CSV, SUFFIX_FMT, ...)                                                      \
	do {                                                                                                                  \
		if (!tcc_format_cstr(suffix, sizeof(suffix), SUFFIX_FMT, __VA_ARGS__) ||                                         \
		    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, suffix, RET_TOKEN, ARG_CSV)) {                  \
			tcc_set_error(error_buf, "out of memory");                                                                    \
			return false;                                                                                                 \
		}                                                                                                                 \
	} while (0)
	char suffix[512];
	if (!tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "sizeof", "u64", "") ||
	    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "alignof", "u64", "") ||
	    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "new", "ptr", "") ||
	    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "free", "void", "ptr")) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	for (i = 0; i < fields->count; i++) {
		const tcc_c_field_spec_t *field = &fields->items[i];
		const char *token = tcc_ffi_type_to_token(field->type);
		char arg_csv[128];
		if (!token) {
			tcc_set_error(error_buf, "field type token is unsupported for helper bindings");
			return false;
		}
		if (field->array_size > 0) {
			if (!tcc_format_cstr(arg_csv, sizeof(arg_csv), "ptr,u64")) {
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			TCC_ADD_BINDING_FORMAT(token, arg_csv, "get_%s_elt", field->name);
			if (!tcc_format_cstr(arg_csv, sizeof(arg_csv), "ptr,u64,%s", token)) {
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			TCC_ADD_BINDING_FORMAT("ptr", arg_csv, "set_%s_elt", field->name);
			TCC_ADD_BINDING_FORMAT("u64", "", "off_%s", field->name);
			TCC_ADD_BINDING_FORMAT("ptr", "ptr", "%s_addr", field->name);
		} else {
			TCC_ADD_BINDING_FORMAT(token, "ptr", "get_%s", field->name);
			if (!tcc_format_cstr(arg_csv, sizeof(arg_csv), "ptr,%s", token)) {
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			TCC_ADD_BINDING_FORMAT("ptr", arg_csv, "set_%s", field->name);
			if (!field->is_bitfield) {
				TCC_ADD_BINDING_FORMAT("u64", "", "off_%s", field->name);
				TCC_ADD_BINDING_FORMAT("ptr", "ptr", "%s_addr", field->name);
			}
		}
	}
#undef TCC_ADD_BINDING_FORMAT
	return true;
}

/* tcc_build_c_enum_bindings: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_build_c_enum_bindings(const char *prefix, const tcc_string_list_t *constants,
                                      tcc_helper_binding_list_t *out_bindings, tcc_error_buffer_t *error_buf) {
	idx_t i;
	char suffix[512];
	if (!prefix || !constants || !out_bindings) {
		tcc_set_error(error_buf, "invalid enum binding arguments");
		return false;
	}
	if (!tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "sizeof", "u64", "")) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	for (i = 0; i < constants->count; i++) {
		if (!tcc_format_cstr(suffix, sizeof(suffix), "%s", constants->items[i]) ||
		    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, suffix, "i64", "")) {
			tcc_set_error(error_buf, "out of memory");
			return false;
		}
	}
	return true;
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* tcc_codegen_compile_and_load_module: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static int tcc_codegen_compile_and_load_module(const char *runtime_path, tcc_module_state_t *state,
                                               const tcc_module_bind_data_t *bind, const char *sql_name,
                                               const char *target_symbol, tcc_registered_artifact_t **out_artifact,
                                               tcc_error_buffer_t *error_buf, char *out_module_symbol, size_t symbol_len) {
	tcc_codegen_source_ctx_t source_ctx;
	tcc_module_bind_data_t bind_copy;
	tcc_registered_artifact_t *artifact = NULL;

	if (!state || !bind || !sql_name || !target_symbol || !out_artifact || !error_buf || !out_module_symbol ||
	    symbol_len == 0) {
		tcc_set_error(error_buf, "invalid codegen compile arguments");
		return -1;
	}
	if (!state->connection) {
		tcc_set_error(error_buf, "no persistent extension connection available");
		return -1;
	}
	tcc_codegen_source_ctx_init(&source_ctx);
	if (!tcc_codegen_prepare_sources(state, bind, sql_name, target_symbol, &source_ctx, error_buf)) {
		tcc_codegen_source_ctx_destroy(&source_ctx);
		return -1;
	}
	snprintf(out_module_symbol, symbol_len, "%s", source_ctx.module_symbol);

	memset(&bind_copy, 0, sizeof(bind_copy));
	bind_copy = *bind;
	bind_copy.source = source_ctx.compilation_unit_source;
	if (tcc_build_module_artifact(runtime_path, state, &bind_copy, out_module_symbol, sql_name, &artifact, error_buf) !=
	    0) {
		tcc_codegen_source_ctx_destroy(&source_ctx);
		return -1;
	}
	tcc_codegen_source_ctx_destroy(&source_ctx);

	if (!artifact->module_init(state->connection)) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "generated module init returned false");
		return -1;
	}
	*out_artifact = artifact;
	return 0;
}
#endif

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* tcc_compile_generated_binding: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_compile_generated_binding(const char *runtime_path, tcc_module_state_t *state, const char *source,
                                          const tcc_helper_binding_t *binding, tcc_error_buffer_t *error_buf) {
	tcc_module_bind_data_t generated_bind;
	tcc_registered_artifact_t *artifact = NULL;
	char module_symbol[128];
	if (!state || !binding || !source || !error_buf) {
		tcc_set_error(error_buf, "invalid generated binding arguments");
		return false;
	}
	memset(&generated_bind, 0, sizeof(generated_bind));
	memset(module_symbol, 0, sizeof(module_symbol));
	generated_bind.source = (char *)source;
	generated_bind.symbol = binding->symbol;
	generated_bind.sql_name = binding->sql_name;
	generated_bind.arg_types = binding->arg_types_csv;
	generated_bind.return_type = binding->return_type;
	generated_bind.wrapper_mode = "row";
	if (tcc_codegen_compile_and_load_module(runtime_path, state, &generated_bind, binding->sql_name, binding->symbol,
	                                        &artifact, error_buf, module_symbol, sizeof(module_symbol)) != 0) {
		return false;
	}
	if (!tcc_registry_store_metadata(state, binding->sql_name, module_symbol, artifact->state_id, artifact)) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "failed to store generated helper metadata");
		return false;
	}
	return true;
}
#endif

/* Returns whether a mode mutates shared session/registry state. */
static bool tcc_mode_requires_write_lock(const char *mode) {
	if (!mode) {
		return false;
	}
	return strcmp(mode, "config_set") == 0 || strcmp(mode, "config_reset") == 0 ||
	       strcmp(mode, "tcc_new_state") == 0 || strcmp(mode, "add_include") == 0 ||
	       strcmp(mode, "add_sysinclude") == 0 || strcmp(mode, "add_library_path") == 0 ||
	       strcmp(mode, "add_library") == 0 || strcmp(mode, "add_option") == 0 ||
	       strcmp(mode, "add_header") == 0 || strcmp(mode, "add_source") == 0 ||
	       strcmp(mode, "add_define") == 0 || strcmp(mode, "tinycc_bind") == 0 ||
	       strcmp(mode, "compile") == 0 || strcmp(mode, "quick_compile") == 0 ||
	       strcmp(mode, "c_struct") == 0 || strcmp(mode, "c_union") == 0 || strcmp(mode, "c_bitfield") == 0 ||
	       strcmp(mode, "c_enum") == 0;
}

/* Main dispatcher for all `tcc_module(...)` modes. */
static void tcc_module_function(duckdb_function_info info, duckdb_data_chunk output) {
	tcc_module_state_t *state = (tcc_module_state_t *)duckdb_function_get_extra_info(info);
	tcc_module_bind_data_t *bind = (tcc_module_bind_data_t *)duckdb_function_get_bind_data(info);
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)duckdb_function_get_init_data(info);
	const char *runtime_path;
	int lock_mode = 0; /* 0 none, 1 read, 2 write */
	bool expected_not_emitted = false;

	if (!state || !bind || !init) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	if (!atomic_compare_exchange_strong_explicit(&init->emitted, &expected_not_emitted, true, memory_order_acq_rel,
	                                             memory_order_acquire)) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	if (tcc_mode_requires_write_lock(bind->mode)) {
		tcc_rwlock_write_lock(&state->lock);
		lock_mode = 2;
	} else {
		tcc_rwlock_read_lock(&state->lock);
		lock_mode = 1;
	}

	runtime_path = tcc_session_runtime_path(state, bind->runtime_path);
	if (strcmp(bind->mode, "config_set") == 0) {
		tcc_session_set_runtime_path(&state->session, bind->runtime_path);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session runtime updated",
		              state->session.runtime_path ? state->session.runtime_path : "(empty)", NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "config_get") == 0) {
		char detail[256];
		snprintf(detail, sizeof(detail), "runtime=%s state_id=%llu config_version=%llu",
		         runtime_path ? runtime_path : "(unset)", (unsigned long long)state->session.state_id,
		         (unsigned long long)state->session.config_version);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration", detail, NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "config_reset") == 0) {
		tcc_session_set_runtime_path(&state->session, NULL);
		tcc_session_clear_build_state(&state->session);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session reset", "runtime/build state cleared",
		              NULL, NULL, NULL, "database");
	} else if (strcmp(bind->mode, "tcc_new_state") == 0) {
		char detail[128];
		tcc_session_clear_build_state(&state->session);
		snprintf(detail, sizeof(detail), "state_id=%llu", (unsigned long long)state->session.state_id);
		tcc_write_row(output, true, bind->mode, "state", "OK", "new TinyCC build state prepared", detail, NULL,
		              NULL, NULL, "database");
	} else if (strcmp(bind->mode, "add_include") == 0) {
		if (bind->include_path && tcc_string_list_append(&state->session.include_paths, bind->include_path)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "include path added", bind->include_path, NULL,
			              NULL, NULL, "database");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "include_path is required", NULL,
			              NULL, NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_sysinclude") == 0) {
		if (bind->sysinclude_path && tcc_string_list_append(&state->session.sysinclude_paths, bind->sysinclude_path)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "sysinclude path added", bind->sysinclude_path,
			              NULL, NULL, NULL, "database");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "sysinclude_path is required", NULL,
			              NULL, NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_library_path") == 0) {
		if (bind->library_path && tcc_string_list_append(&state->session.library_paths, bind->library_path)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "library path added", bind->library_path, NULL,
			              NULL, NULL, "database");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "library_path is required", NULL,
			              NULL, NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_library") == 0) {
		if (bind->library && tcc_string_list_append(&state->session.libraries, bind->library)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "library added", bind->library, NULL, NULL, NULL,
			              "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "library is required", NULL, NULL,
			              NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_option") == 0) {
		if (bind->option && tcc_string_list_append(&state->session.options, bind->option)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "compiler option added", bind->option, NULL,
			              NULL, NULL, "database");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "option is required", NULL, NULL,
			              NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_header") == 0) {
		if (bind->header && tcc_string_list_append(&state->session.headers, bind->header)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "header source added", "header appended", NULL,
			              NULL, NULL, "database");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "header is required", NULL, NULL,
			              NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_source") == 0) {
		if (bind->source && tcc_string_list_append(&state->session.sources, bind->source)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "source appended", "source appended", NULL,
			              NULL, NULL, "database");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "source is required", NULL, NULL,
			              NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "add_define") == 0) {
		if (bind->define_name && bind->define_name[0] != '\0') {
			const char *define_value = bind->define_value ? bind->define_value : "1";
			if (tcc_string_list_append(&state->session.define_names, bind->define_name)) {
				if (!tcc_string_list_append(&state->session.define_values, define_value)) {
					(void)tcc_string_list_pop_last(&state->session.define_names);
					tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store define", NULL,
					              NULL, NULL, NULL, "database");
					goto tcc_module_done;
				}
				state->session.config_version++;
				tcc_write_row(output, true, bind->mode, "state", "OK", "define added", bind->define_name, NULL, NULL,
				              NULL, "database");
			} else {
				tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store define", NULL,
				              NULL, NULL, NULL, "database");
			}
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "define_name is required", NULL, NULL,
			              NULL, NULL, "database");
		}
	} else if (strcmp(bind->mode, "tinycc_bind") == 0) {
		if (!bind->symbol || bind->symbol[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required", NULL,
			              bind->sql_name, bind->symbol, NULL, "database");
		} else {
			char *bound_symbol = NULL;
			char *bound_sql_name = NULL;
			bound_symbol = tcc_strdup(bind->symbol);
			bound_sql_name = tcc_strdup(bind->sql_name && bind->sql_name[0] != '\0' ? bind->sql_name : bind->symbol);
			if (!bound_symbol || !bound_sql_name) {
				if (bound_symbol) {
					duckdb_free(bound_symbol);
				}
				if (bound_sql_name) {
					duckdb_free(bound_sql_name);
				}
				tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store symbol binding",
				              NULL, bind->sql_name, bind->symbol, NULL, "database");
				goto tcc_module_done;
			}
			tcc_session_clear_bind(&state->session);
			state->session.bound_symbol = bound_symbol;
			state->session.bound_sql_name = bound_sql_name;
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "bind", "OK", "symbol binding updated",
			              state->session.bound_sql_name, state->session.bound_sql_name, state->session.bound_symbol, NULL,
			              "connection");
		}
	} else if (strcmp(bind->mode, "list") == 0) {
		char detail[256];
		snprintf(detail, sizeof(detail),
		         "registered=%llu sources=%llu headers=%llu includes=%llu libs=%llu state_id=%llu", 
		         (unsigned long long)state->entry_count, (unsigned long long)state->session.sources.count,
		         (unsigned long long)state->session.headers.count, (unsigned long long)state->session.include_paths.count,
		         (unsigned long long)state->session.libraries.count, (unsigned long long)state->session.state_id);
		tcc_write_row(output, true, bind->mode, "registry", "OK", "session summary", detail, NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "c_struct") == 0 || strcmp(bind->mode, "c_union") == 0 ||
	           strcmp(bind->mode, "c_bitfield") == 0 || strcmp(bind->mode, "c_enum") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
		tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
		              "TinyCC compile codegen path not supported for WASM build", NULL, bind->sql_name, bind->symbol,
		              NULL, "database");
#else
		const bool is_enum = strcmp(bind->mode, "c_enum") == 0;
		const bool is_union = strcmp(bind->mode, "c_union") == 0;
		const bool force_bitfield = strcmp(bind->mode, "c_bitfield") == 0;
		const char *kind_keyword = is_union ? "union" : "struct";
		const char *type_name = bind->symbol;
		char prefix_buf[256];
		const char *prefix = NULL;
		tcc_error_buffer_t err;
		tcc_c_field_list_t fields;
		tcc_string_list_t enum_constants;
		tcc_helper_binding_list_t helper_bindings;
		char *helper_source = NULL;
		char *combined_source = NULL;
		idx_t i;
		memset(&err, 0, sizeof(err));
		memset(&fields, 0, sizeof(fields));
		memset(&enum_constants, 0, sizeof(enum_constants));
		memset(&helper_bindings, 0, sizeof(helper_bindings));

		if (!type_name || !tcc_is_identifier_token(type_name)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "symbol must be a valid C identifier",
			              NULL, bind->sql_name, bind->symbol, NULL, "database");
			goto tcc_c_helpers_done;
		}
		if (bind->sql_name && bind->sql_name[0] != '\0') {
			prefix = bind->sql_name;
		} else if (is_enum) {
			int n = snprintf(prefix_buf, sizeof(prefix_buf), "enum_%s", type_name);
			if (n < 0 || (size_t)n >= sizeof(prefix_buf)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "failed to build helper prefix", NULL,
				              bind->sql_name, bind->symbol, NULL, "database");
				goto tcc_c_helpers_done;
			}
			prefix = prefix_buf;
		} else {
			int n = snprintf(prefix_buf, sizeof(prefix_buf), "%s_%s", is_union ? "union" : "struct", type_name);
			if (n < 0 || (size_t)n >= sizeof(prefix_buf)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "failed to build helper prefix", NULL,
				              bind->sql_name, bind->symbol, NULL, "database");
				goto tcc_c_helpers_done;
			}
			prefix = prefix_buf;
		}
		if (!prefix || !tcc_is_identifier_token(prefix)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS",
			              "sql_name must be a valid C/SQL identifier when provided", NULL, bind->sql_name, bind->symbol,
			              NULL, "database");
			goto tcc_c_helpers_done;
		}
		if (is_enum) {
			if (!tcc_parse_c_enum_constants(bind->arg_types, &enum_constants, &err)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "invalid c_enum constants",
				              err.message[0] ? err.message : NULL, prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
			helper_source = tcc_generate_c_enum_helpers_source(type_name, prefix, &enum_constants, &err);
			if (!helper_source) {
				tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED", "failed to generate enum helpers",
				              err.message[0] ? err.message : NULL, prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
			if (!tcc_build_c_enum_bindings(prefix, &enum_constants, &helper_bindings, &err)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "failed to build enum helper signatures",
				              err.message[0] ? err.message : NULL, prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
		} else {
			if (!tcc_parse_c_field_specs(bind->arg_types, force_bitfield, &fields, &err)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "invalid c struct/union field specs",
				              err.message[0] ? err.message : NULL, prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
			helper_source = tcc_generate_c_composite_helpers_source(kind_keyword, type_name, prefix, &fields, &err);
			if (!helper_source) {
				tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED",
				              "failed to generate struct/union helpers", err.message[0] ? err.message : NULL, prefix,
				              type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
			if (!tcc_build_c_composite_bindings(prefix, &fields, &helper_bindings, &err)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS",
				              "failed to build struct/union helper signatures", err.message[0] ? err.message : NULL,
				              prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
		}

		if (bind->source && bind->source[0] != '\0') {
			size_t n0 = strlen(bind->source);
			size_t n1 = strlen(helper_source);
			combined_source = (char *)duckdb_malloc(n0 + n1 + 3);
			if (!combined_source) {
				tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED",
				              "failed to allocate helper source buffer", NULL, prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
			memcpy(combined_source, bind->source, n0);
			combined_source[n0] = '\n';
			memcpy(combined_source + n0 + 1, helper_source, n1);
			combined_source[n0 + n1 + 1] = '\0';
		} else {
			combined_source = tcc_strdup(helper_source);
			if (!combined_source) {
				tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED",
				              "failed to allocate helper source buffer", NULL, prefix, type_name, NULL, "database");
				goto tcc_c_helpers_done;
			}
		}

		for (i = 0; i < helper_bindings.count; i++) {
			const tcc_helper_binding_t *entry = &helper_bindings.items[i];
			memset(&err, 0, sizeof(err));
			if (!tcc_compile_generated_binding(runtime_path, state, combined_source, entry, &err)) {
				const char *phase = "compile";
				const char *code = "E_COMPILE_FAILED";
				const char *message = "generated helper compile failed";
				if (strstr(err.message, "wrapper_mode")) {
					phase = "bind";
					code = "E_BAD_WRAPPER_MODE";
					message = "invalid wrapper_mode";
				} else if (strstr(err.message, "return_type") || strstr(err.message, "arg_types") ||
				           strstr(err.message, "struct token") || strstr(err.message, "map token") ||
				           strstr(err.message, "fixed-width scalar tokens only")) {
					phase = "bind";
					code = "E_BAD_SIGNATURE";
					message = "invalid helper signature";
				} else if (strstr(err.message, "failed to generate codegen wrapper") || strstr(err.message, "out of memory")) {
					phase = "codegen";
					code = "E_CODEGEN_FAILED";
					message = "generated helper codegen failed";
				} else if (strstr(err.message, "no persistent extension connection")) {
					phase = "load";
					code = "E_NO_CONNECTION";
					message = "no persistent extension connection available";
				} else if (strstr(err.message, "generated module init returned false")) {
					phase = "load";
					code = "E_INIT_FAILED";
					message = "generated helper module init returned false";
				}
				tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL,
				              entry->sql_name, entry->symbol, NULL, "database");
				goto tcc_c_helpers_done;
			}
		}
		{
			char detail[256];
			snprintf(detail, sizeof(detail), "generated=%llu prefix=%.96s target=%.96s",
			         (unsigned long long)helper_bindings.count, prefix, type_name);
			tcc_write_row(output, true, bind->mode, "load", "OK", "generated and registered helper UDFs", detail,
			              prefix, type_name, NULL, "database");
		}

tcc_c_helpers_done:
		if (helper_source) {
			duckdb_free(helper_source);
			helper_source = NULL;
		}
		if (combined_source) {
			duckdb_free(combined_source);
			combined_source = NULL;
		}
		tcc_c_field_list_destroy(&fields);
		tcc_string_list_destroy(&enum_constants);
		tcc_helper_binding_list_destroy(&helper_bindings);
	#endif
	} else if (strcmp(bind->mode, "codegen_preview") == 0) {
		const char *target_symbol = tcc_effective_symbol(state, bind);
		const char *sql_name = tcc_effective_sql_name(state, bind, target_symbol);
		tcc_codegen_source_ctx_t source_ctx;
		tcc_error_buffer_t err;
		memset(&err, 0, sizeof(err));
		tcc_codegen_source_ctx_init(&source_ctx);
		if (!target_symbol || target_symbol[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required (bind or argument)",
			              NULL, sql_name, target_symbol, NULL, "database");
			goto tcc_codegen_preview_done;
		}
		if (!tcc_codegen_prepare_sources(state, bind, sql_name, target_symbol, &source_ctx, &err)) {
			const char *phase = "codegen";
			const char *code = "E_CODEGEN_FAILED";
			const char *message = "ffi codegen failed";
			tcc_codegen_classify_error_message(err.message, &phase, &code, &message);
			tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL, sql_name,
			              target_symbol, NULL, "database");
			goto tcc_codegen_preview_done;
		}
		tcc_write_row(output, true, bind->mode, "codegen", "OK", "generated codegen source",
		              source_ctx.compilation_unit_source, sql_name, target_symbol, source_ctx.module_symbol, "database");
		goto tcc_codegen_preview_done;
tcc_codegen_preview_done:
		tcc_codegen_source_ctx_destroy(&source_ctx);
	} else if (strcmp(bind->mode, "compile") == 0 || strcmp(bind->mode, "quick_compile") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
			tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
			              "TinyCC compile codegen path not supported for WASM build", NULL, bind->sql_name,
			              bind->symbol, NULL, "database");
#else
			const char *target_symbol = tcc_effective_symbol(state, bind);
			const char *sql_name = tcc_effective_sql_name(state, bind, target_symbol);
			char module_symbol[128];
			tcc_error_buffer_t err;
			tcc_registered_artifact_t *artifact = NULL;
			char artifact_id[256];
			const char *phase = "compile";
			const char *code = "E_COMPILE_FAILED";
			const char *message = "compile failed";
			memset(&err, 0, sizeof(err));

			if (strcmp(bind->mode, "quick_compile") == 0 && (!bind->source || bind->source[0] == '\0')) {
				tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS",
				              "source is required in quick_compile mode", NULL, sql_name, target_symbol, NULL,
				              "connection");
				goto tcc_module_done;
			}

			if (!target_symbol || target_symbol[0] == '\0') {
				tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required (bind or argument)",
				              NULL, sql_name, target_symbol, NULL, "database");
				goto tcc_module_done;
			}

				if (tcc_codegen_compile_and_load_module(runtime_path, state, bind, sql_name, target_symbol, &artifact, &err,
				                                        module_symbol, sizeof(module_symbol)) != 0) {
					tcc_codegen_classify_error_message(err.message, &phase, &code, &message);
					tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL, sql_name,
					              target_symbol, NULL, "database");
					goto tcc_module_done;
				}
			if (!tcc_registry_store_metadata(state, sql_name, module_symbol, artifact->state_id, artifact)) {
				tcc_artifact_destroy(artifact);
				tcc_write_row(output, false, bind->mode, "register", "E_STORE_FAILED",
				              "failed to store ffi module artifact metadata", NULL, sql_name, target_symbol, NULL,
				              "connection");
				goto tcc_module_done;
			}
			snprintf(artifact_id, sizeof(artifact_id), "%s@ffi_state_%llu", sql_name,
			         (unsigned long long)artifact->state_id);
			tcc_write_row(output, true, bind->mode, "load", "OK", "compiled and registered SQL function via codegen",
			              runtime_path, sql_name, target_symbol, artifact_id, "database");
#endif
	} else {
		tcc_write_row(output, false, bind->mode, "bind", "E_BAD_MODE", "unknown mode", NULL, NULL, NULL, NULL,
		              "connection");
	}

tcc_module_done:
	if (lock_mode == 2) {
		tcc_rwlock_write_unlock(&state->lock);
	} else if (lock_mode == 1) {
		tcc_rwlock_read_unlock(&state->lock);
	}
}

/* destroy_tcc_diag_bind_data: Destructor callback for DuckDB bind/init/extra-info payloads. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void destroy_tcc_diag_bind_data(void *ptr) {
	tcc_diag_bind_data_t *bind = (tcc_diag_bind_data_t *)ptr;
	if (!bind) {
		return;
	}
	tcc_diag_rows_destroy(&bind->rows);
	duckdb_free(bind);
}

/* destroy_tcc_diag_init_data: Destructor callback for DuckDB bind/init/extra-info payloads. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void destroy_tcc_diag_init_data(void *ptr) {
	tcc_diag_init_data_t *init = (tcc_diag_init_data_t *)ptr;
	if (!init) {
		return;
	}
	duckdb_free(init);
}

/* tcc_diag_set_result_schema: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_diag_set_result_schema(duckdb_bind_info info) {
	duckdb_logical_type bool_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_bind_add_result_column(info, "kind", varchar_type);
	duckdb_bind_add_result_column(info, "key", varchar_type);
	duckdb_bind_add_result_column(info, "value", varchar_type);
	duckdb_bind_add_result_column(info, "exists", bool_type);
	duckdb_bind_add_result_column(info, "detail", varchar_type);
	duckdb_destroy_logical_type(&bool_type);
	duckdb_destroy_logical_type(&varchar_type);
}

/* tcc_diag_table_init: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_diag_table_init(duckdb_init_info info) {
	tcc_diag_init_data_t *init = (tcc_diag_init_data_t *)duckdb_malloc(sizeof(tcc_diag_init_data_t));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	atomic_store_explicit(&init->offset, 0, memory_order_relaxed);
	duckdb_init_set_init_data(info, init, destroy_tcc_diag_init_data);
}

/* tcc_diag_table_function: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_diag_table_function(duckdb_function_info info, duckdb_data_chunk output) {
	tcc_diag_bind_data_t *bind = (tcc_diag_bind_data_t *)duckdb_function_get_bind_data(info);
	tcc_diag_init_data_t *init = (tcc_diag_init_data_t *)duckdb_function_get_init_data(info);
	tcc_diag_row_t *row;
	duckdb_vector v_exists;
	bool *exists_data;
	uint64_t row_idx;
	if (!bind || !init) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	row_idx = atomic_fetch_add_explicit(&init->offset, 1, memory_order_acq_rel);
	if ((idx_t)row_idx >= bind->rows.count) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	row = &bind->rows.rows[(idx_t)row_idx];
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 0), 0, row->kind);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 1), 0, row->key);
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 2), 0, row->value);
	v_exists = duckdb_data_chunk_get_vector(output, 3);
	exists_data = (bool *)duckdb_vector_get_data(v_exists);
	exists_data[0] = row->exists;
	tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 4), 0, row->detail);
	duckdb_data_chunk_set_size(output, 1);
}

/* tcc_system_paths_bind: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_system_paths_bind(duckdb_bind_info info) {
	tcc_diag_bind_data_t *bind = (tcc_diag_bind_data_t *)duckdb_malloc(sizeof(tcc_diag_bind_data_t));
	tcc_string_list_t include_paths;
	tcc_string_list_t library_paths;
	char *runtime_path = NULL;
	char *library_path = NULL;
	const char *effective_runtime;
	idx_t i;
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(tcc_diag_bind_data_t));
	memset(&include_paths, 0, sizeof(tcc_string_list_t));
	memset(&library_paths, 0, sizeof(tcc_string_list_t));

	tcc_bind_read_named_varchar(info, "runtime_path", &runtime_path);
	tcc_bind_read_named_varchar(info, "library_path", &library_path);
	effective_runtime = (runtime_path && runtime_path[0] != '\0') ? runtime_path : tcc_default_runtime_path();

	if (!tcc_collect_include_paths(effective_runtime, &include_paths) ||
	    !tcc_collect_library_search_paths(effective_runtime, library_path, &library_paths)) {
		tcc_string_list_destroy(&include_paths);
		tcc_string_list_destroy(&library_paths);
		if (runtime_path) {
			duckdb_free(runtime_path);
		}
		if (library_path) {
			duckdb_free(library_path);
		}
		destroy_tcc_diag_bind_data(bind);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}

	(void)tcc_diag_rows_add(&bind->rows, "runtime", "runtime_path", effective_runtime, tcc_path_exists(effective_runtime),
	                        "effective runtime path");
	for (i = 0; i < include_paths.count; i++) {
		(void)tcc_diag_rows_add(&bind->rows, "include_path", "path", include_paths.items[i],
		                        tcc_path_exists(include_paths.items[i]), "TinyCC include search path");
	}
	for (i = 0; i < library_paths.count; i++) {
		(void)tcc_diag_rows_add(&bind->rows, "library_path", "path", library_paths.items[i],
		                        tcc_path_exists(library_paths.items[i]), "library search path");
	}

	tcc_string_list_destroy(&include_paths);
	tcc_string_list_destroy(&library_paths);
	if (runtime_path) {
		duckdb_free(runtime_path);
	}
	if (library_path) {
		duckdb_free(library_path);
	}

	tcc_diag_set_result_schema(info);
	duckdb_bind_set_cardinality(info, bind->rows.count, true);
	duckdb_bind_set_bind_data(info, bind, destroy_tcc_diag_bind_data);
}

/* tcc_try_resolve_candidate: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_try_resolve_candidate(const char *candidate, const tcc_string_list_t *search_paths, char **out_path) {
	idx_t i;
	if (!candidate || candidate[0] == '\0' || !search_paths || !out_path) {
		return false;
	}
	if (tcc_is_path_like(candidate)) {
		if (tcc_path_exists(candidate)) {
			*out_path = tcc_strdup(candidate);
			return *out_path != NULL;
		}
		return false;
	}
	for (i = 0; i < search_paths->count; i++) {
		char *full_path = tcc_path_join(search_paths->items[i], candidate);
		if (!full_path) {
			return false;
		}
		if (tcc_path_exists(full_path)) {
			*out_path = full_path;
			return true;
		}
		duckdb_free(full_path);
	}
	return false;
}

/* tcc_library_probe_bind: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_library_probe_bind(duckdb_bind_info info) {
	tcc_diag_bind_data_t *bind = (tcc_diag_bind_data_t *)duckdb_malloc(sizeof(tcc_diag_bind_data_t));
	tcc_string_list_t library_paths;
	tcc_string_list_t candidates;
	char *library = NULL;
	char *runtime_path = NULL;
	char *library_path = NULL;
	const char *effective_runtime;
	idx_t i;
	bool found = false;
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(tcc_diag_bind_data_t));
	memset(&library_paths, 0, sizeof(tcc_string_list_t));
	memset(&candidates, 0, sizeof(tcc_string_list_t));

	tcc_bind_read_named_varchar(info, "library", &library);
	tcc_bind_read_named_varchar(info, "runtime_path", &runtime_path);
	tcc_bind_read_named_varchar(info, "library_path", &library_path);
	effective_runtime = (runtime_path && runtime_path[0] != '\0') ? runtime_path : tcc_default_runtime_path();

	if (!library || library[0] == '\0') {
		if (library) {
			duckdb_free(library);
		}
		if (runtime_path) {
			duckdb_free(runtime_path);
		}
		if (library_path) {
			duckdb_free(library_path);
		}
		destroy_tcc_diag_bind_data(bind);
		duckdb_bind_set_error(info, "library is required");
		return;
	}

	if (!tcc_collect_library_search_paths(effective_runtime, library_path, &library_paths) ||
	    !tcc_build_library_candidates(library, &candidates)) {
		tcc_string_list_destroy(&library_paths);
		tcc_string_list_destroy(&candidates);
		duckdb_free(library);
		if (runtime_path) {
			duckdb_free(runtime_path);
		}
		if (library_path) {
			duckdb_free(library_path);
		}
		destroy_tcc_diag_bind_data(bind);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}

	(void)tcc_diag_rows_add(&bind->rows, "input", "library", library, false, "library probe request");
	(void)tcc_diag_rows_add(&bind->rows, "runtime", "runtime_path", effective_runtime, tcc_path_exists(effective_runtime),
	                        "effective runtime path");
	for (i = 0; i < library_paths.count; i++) {
		(void)tcc_diag_rows_add(&bind->rows, "search_path", "path", library_paths.items[i],
		                        tcc_path_exists(library_paths.items[i]), "searched path");
	}
	for (i = 0; i < candidates.count; i++) {
		char *resolved = NULL;
		bool candidate_found = tcc_try_resolve_candidate(candidates.items[i], &library_paths, &resolved);
		if (candidate_found && resolved) {
			char *link_name = tcc_library_link_name_from_path(resolved);
			(void)tcc_diag_rows_add(&bind->rows, "candidate", candidates.items[i], resolved, true, "resolved");
			(void)tcc_diag_rows_add(&bind->rows, "resolved", "path", resolved, true, "resolved library path");
			if (link_name) {
				(void)tcc_diag_rows_add(&bind->rows, "resolved", "link_name", link_name, true,
				                        "normalized tcc_add_library value");
				duckdb_free(link_name);
			}
			found = true;
			duckdb_free(resolved);
			break;
		}
		(void)tcc_diag_rows_add(&bind->rows, "candidate", candidates.items[i], NULL, false, "not found");
	}
	if (!found) {
		(void)tcc_diag_rows_add(&bind->rows, "resolved", "path", NULL, false, "no matching library found");
	}

	tcc_string_list_destroy(&library_paths);
	tcc_string_list_destroy(&candidates);
	duckdb_free(library);
	if (runtime_path) {
		duckdb_free(runtime_path);
	}
	if (library_path) {
		duckdb_free(library_path);
	}

	tcc_diag_set_result_schema(info);
	duckdb_bind_set_cardinality(info, bind->rows.count, true);
	duckdb_bind_set_bind_data(info, bind, destroy_tcc_diag_bind_data);
}

/* Registers `tcc_system_paths(...)` diagnostics table function. */
static bool register_tcc_system_paths_function(duckdb_connection connection) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_state rc;
	duckdb_table_function_set_name(tf, "tcc_system_paths");
	duckdb_table_function_add_named_parameter(tf, "runtime_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "library_path", varchar_type);
	duckdb_table_function_set_bind(tf, tcc_system_paths_bind);
	duckdb_table_function_set_init(tf, tcc_diag_table_init);
	duckdb_table_function_set_function(tf, tcc_diag_table_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);
	rc = duckdb_register_table_function(connection, tf);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
	return rc == DuckDBSuccess;
}

/* Registers `tcc_library_probe(...)` diagnostics table function. */
static bool register_tcc_library_probe_function(duckdb_connection connection) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_state rc;
	duckdb_table_function_set_name(tf, "tcc_library_probe");
	duckdb_table_function_add_named_parameter(tf, "library", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "runtime_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "library_path", varchar_type);
	duckdb_table_function_set_bind(tf, tcc_library_probe_bind);
	duckdb_table_function_set_init(tf, tcc_diag_table_init);
	duckdb_table_function_set_function(tf, tcc_diag_table_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);
	rc = duckdb_register_table_function(connection, tf);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
	return rc == DuckDBSuccess;
}

/* Public extension registration entrypoint for module and helper SQL surfaces. */
bool RegisterTccModuleFunction(duckdb_connection connection, duckdb_database database) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type list_varchar_type = duckdb_create_list_type(varchar_type);
	tcc_module_state_t *state;
	duckdb_state rc;

	state = (tcc_module_state_t *)duckdb_malloc(sizeof(tcc_module_state_t));
	if (!state) {
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
	duckdb_table_function_add_named_parameter(tf, "include_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "sysinclude_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "library_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "library", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "option", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "header", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "define_name", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "define_value", varchar_type);

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
