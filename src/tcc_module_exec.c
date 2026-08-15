/*
 * DuckTinyCC generated scalar-UDF execution bridge.
 *
 * Converts DuckDB vectors to the row/chunk-scalar-loop ABI used by TinyCC-generated wrappers,
 * then writes scalar/composite results back to DuckDB vectors.  All storage here is per-call.
 */

typedef struct {
	duckdb_function_info info;
	duckdb_data_chunk input;
	duckdb_vector output;
	tcc_host_sig_ctx_t *sig;
	idx_t n;

	uint8_t *out_data;
	uint64_t *out_validity;
	size_t ret_size;

	uint8_t **in_data;
	uint64_t **in_validity;
	tcc_value_bridge_t **arg_value_bridges;
	void **arg_ptrs;
	void **batch_arg_data;

	const char **row_varchar_values;
	char **row_varchar_allocations;
	idx_t row_varchar_alloc_count;
	idx_t row_varchar_alloc_capacity;
	ducktinycc_blob_t *row_blob_values;

	const char ***batch_varchar_columns;
	char ***batch_varchar_owned;
	ducktinycc_blob_t **batch_blob_columns;

	const char **batch_out_varchar;
	ducktinycc_blob_t *batch_out_blob;
	ducktinycc_list_t *batch_out_list;
	ducktinycc_array_t *batch_out_array;
	ducktinycc_struct_t *batch_out_struct;
	ducktinycc_map_t *batch_out_map;
	ducktinycc_union_t *batch_out_union;
	void *batch_out_ptr;

	uint8_t out_value[64];
	const char *out_varchar_value;
	ducktinycc_blob_t out_blob_value;
	ducktinycc_list_t out_list_value;
	ducktinycc_array_t out_array_value;
	ducktinycc_struct_t out_struct_value;
	ducktinycc_map_t out_map_value;
	ducktinycc_union_t out_union_value;

	const char *error;
} tcc_exec_call_t;

/* True for descriptor-backed values passed through the recursive value bridge. */
static bool tcc_ffi_type_is_any_composite(tcc_ffi_type_t type) {
	return tcc_ffi_type_is_list(type) || tcc_ffi_type_is_array(type) || tcc_ffi_type_is_struct(type) ||
	       tcc_ffi_type_is_map(type) || tcc_ffi_type_is_union(type);
}

/* DuckDB-heap calloc with overflow guard; NULL is a valid zero-length result. */
static void *tcc_duckdb_calloc(idx_t count, size_t size) {
	void *ptr;

	if (count == 0 || size == 0) {
		return NULL;
	}
	if ((size_t)count > SIZE_MAX / size) {
		return NULL;
	}
	ptr = duckdb_malloc((size_t)count * size);
	if (ptr) {
		memset(ptr, 0, (size_t)count * size);
	}
	return ptr;
}

static void tcc_exec_call_init(tcc_exec_call_t *call, duckdb_function_info info, duckdb_data_chunk input,
                               duckdb_vector output) {
	memset(call, 0, sizeof(*call));
	call->info = info;
	call->input = input;
	call->output = output;
	call->sig = (tcc_host_sig_ctx_t *)duckdb_scalar_function_get_extra_info(info);
	call->n = duckdb_data_chunk_get_size(input);
	call->out_data = (uint8_t *)duckdb_vector_get_data(output);
}

static bool tcc_exec_validate(tcc_exec_call_t *call) {
	tcc_host_sig_ctx_t *sig = call->sig;

	if (!sig || sig->arg_count < 0) {
		call->error = "ducktinycc signature ctx missing";
		return false;
	}
	if (sig->wrapper_mode == TCC_WRAPPER_MODE_ROW && !sig->row_wrapper) {
		call->error = "ducktinycc row wrapper missing";
		return false;
	}
	if (sig->wrapper_mode == TCC_WRAPPER_MODE_BATCH && !sig->batch_wrapper) {
		call->error = "ducktinycc batch wrapper missing";
		return false;
	}
	if (sig->wrapper_mode != TCC_WRAPPER_MODE_ROW && sig->wrapper_mode != TCC_WRAPPER_MODE_BATCH) {
		call->error = "ducktinycc signature ctx missing";
		return false;
	}
	if (!sig->return_desc) {
		call->error = "ducktinycc typed signature is missing";
		return false;
	}
	if ((size_t)sig->arg_count > SIZE_MAX / sizeof(uint8_t *)) {
		call->error = "ducktinycc arg count too large";
		return false;
	}
	call->ret_size = tcc_ffi_type_size(sig->return_type);
	return true;
}

static bool tcc_exec_alloc_args(tcc_exec_call_t *call) {
	idx_t count;

	if (call->sig->arg_count == 0) {
		return true;
	}
	count = (idx_t)call->sig->arg_count;
	call->in_data = (uint8_t **)tcc_duckdb_calloc(count, sizeof(uint8_t *));
	call->in_validity = (uint64_t **)tcc_duckdb_calloc(count, sizeof(uint64_t *));
	call->arg_value_bridges = (tcc_value_bridge_t **)tcc_duckdb_calloc(count, sizeof(tcc_value_bridge_t *));

	if (call->sig->wrapper_mode == TCC_WRAPPER_MODE_ROW) {
		call->arg_ptrs = (void **)tcc_duckdb_calloc(count, sizeof(void *));
		call->row_varchar_values = (const char **)tcc_duckdb_calloc(count, sizeof(const char *));
		call->row_blob_values = (ducktinycc_blob_t *)tcc_duckdb_calloc(count, sizeof(ducktinycc_blob_t));
	} else {
		call->batch_arg_data = (void **)tcc_duckdb_calloc(count, sizeof(void *));
		call->batch_varchar_columns = (const char ***)tcc_duckdb_calloc(count, sizeof(const char **));
		call->batch_varchar_owned = (char ***)tcc_duckdb_calloc(count, sizeof(char **));
		call->batch_blob_columns = (ducktinycc_blob_t **)tcc_duckdb_calloc(count, sizeof(ducktinycc_blob_t *));
	}

	if (!call->in_data || !call->in_validity || !call->arg_value_bridges) {
		call->error = "ducktinycc out of memory";
		return false;
	}
	if (call->sig->wrapper_mode == TCC_WRAPPER_MODE_ROW &&
	    (!call->arg_ptrs || !call->row_varchar_values || !call->row_blob_values)) {
		call->error = "ducktinycc out of memory";
		return false;
	}
	if (call->sig->wrapper_mode == TCC_WRAPPER_MODE_BATCH &&
	    (!call->batch_arg_data || !call->batch_varchar_columns || !call->batch_varchar_owned ||
	     !call->batch_blob_columns)) {
		call->error = "ducktinycc out of memory";
		return false;
	}
	return true;
}

/* Cache raw vector data, validity, and recursive descriptor bridges for all arguments. */
static bool tcc_exec_build_input_columns(tcc_exec_call_t *call) {
	int col;

	for (col = 0; col < call->sig->arg_count; col++) {
		duckdb_vector vector = duckdb_data_chunk_get_vector(call->input, (idx_t)col);
		const tcc_typedesc_t *desc = call->sig->arg_descs ? call->sig->arg_descs[col] : NULL;

		call->in_data[col] = (uint8_t *)duckdb_vector_get_data(vector);
		call->in_validity[col] = duckdb_vector_get_validity(vector);
		if (!call->sig->arg_sizes || call->sig->arg_sizes[col] == 0) {
			call->error = "ducktinycc invalid arg type size";
			return false;
		}
		if (desc && tcc_typedesc_is_composite(desc)) {
			const char *bridge_error = NULL;
			tcc_value_bridge_t *bridge = tcc_build_value_bridge(vector, desc, call->n, &bridge_error);

			if (!bridge) {
				call->error = bridge_error ? bridge_error : "ducktinycc composite bridge failed";
				return false;
			}
			call->arg_value_bridges[col] = bridge;
			call->in_data[col] = (uint8_t *)bridge->rows;
			if (bridge->validity) {
				call->in_validity[col] = (uint64_t *)bridge->validity;
			}
		}
	}
	return true;
}

static bool tcc_exec_prepare_output_validity(tcc_exec_call_t *call) {
	duckdb_vector_ensure_validity_writable(call->output);
	call->out_validity = duckdb_vector_get_validity(call->output);
	if (!call->out_validity) {
		call->error = "ducktinycc output validity missing";
		return false;
	}
	return true;
}

static bool tcc_exec_decode_batch_varchar(tcc_exec_call_t *call, int col) {
	duckdb_string_t *strings = (duckdb_string_t *)call->in_data[col];
	const char **decoded = NULL;
	char **owned = NULL;
	idx_t row;

	if (call->n > 0) {
		decoded = (const char **)tcc_duckdb_calloc(call->n, sizeof(const char *));
		owned = (char **)tcc_duckdb_calloc(call->n, sizeof(char *));
		if (!decoded || !owned) {
			if (decoded) {
				duckdb_free((void *)decoded);
			}
			if (owned) {
				duckdb_free((void *)owned);
			}
			call->error = "ducktinycc out of memory";
			return false;
		}
	}
	call->batch_varchar_columns[col] = decoded;
	call->batch_varchar_owned[col] = owned;
	call->batch_arg_data[col] = (void *)decoded;

	for (row = 0; row < call->n; row++) {
		if (call->in_validity[col] && !duckdb_validity_row_is_valid(call->in_validity[col], row)) {
			decoded[row] = NULL;
			continue;
		}
		owned[row] = tcc_copy_duckdb_string_as_cstr(&strings[row]);
		if (!owned[row]) {
			call->error = "ducktinycc out of memory";
			return false;
		}
		decoded[row] = owned[row];
	}
	return true;
}

static bool tcc_exec_decode_batch_blob(tcc_exec_call_t *call, int col) {
	duckdb_string_t *strings = (duckdb_string_t *)call->in_data[col];
	ducktinycc_blob_t *decoded = NULL;
	idx_t row;

	if (call->n > 0) {
		decoded = (ducktinycc_blob_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_blob_t));
		if (!decoded) {
			call->error = "ducktinycc out of memory";
			return false;
		}
	}
	call->batch_blob_columns[col] = decoded;
	call->batch_arg_data[col] = (void *)decoded;

	for (row = 0; row < call->n; row++) {
		if (call->in_validity[col] && !duckdb_validity_row_is_valid(call->in_validity[col], row)) {
			decoded[row].ptr = NULL;
			decoded[row].len = 0;
			continue;
		}
		decoded[row] = tcc_duckdb_string_to_blob(&strings[row]);
	}
	return true;
}

static bool tcc_exec_prepare_batch_args(tcc_exec_call_t *call) {
	int col;

	for (col = 0; col < call->sig->arg_count; col++) {
		if (call->sig->arg_types[col] == TCC_FFI_VARCHAR) {
			if (!tcc_exec_decode_batch_varchar(call, col)) {
				return false;
			}
		} else if (call->sig->arg_types[col] == TCC_FFI_BLOB) {
			if (!tcc_exec_decode_batch_blob(call, col)) {
				return false;
			}
		} else if (call->arg_value_bridges && call->arg_value_bridges[col]) {
			call->batch_arg_data[col] = (void *)call->in_data[col];
		} else if (tcc_ffi_type_is_any_composite(call->sig->arg_types[col]) &&
		           (!call->sig->arg_descs || !call->sig->arg_descs[col])) {
			call->error = "ducktinycc composite arg descriptor is missing";
			return false;
		} else {
			call->batch_arg_data[col] = (void *)call->in_data[col];
		}
	}
	return true;
}

static bool tcc_exec_alloc_batch_output(tcc_exec_call_t *call) {
	tcc_ffi_type_t type = call->sig->return_type;

	call->batch_out_ptr = call->out_data;
	if (type == TCC_FFI_VARCHAR) {
		call->batch_out_varchar = (const char **)tcc_duckdb_calloc(call->n, sizeof(const char *));
		call->batch_out_ptr = (void *)call->batch_out_varchar;
	} else if (type == TCC_FFI_BLOB) {
		call->batch_out_blob = (ducktinycc_blob_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_blob_t));
		call->batch_out_ptr = (void *)call->batch_out_blob;
	} else if (tcc_ffi_type_is_list(type)) {
		call->batch_out_list = (ducktinycc_list_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_list_t));
		call->batch_out_ptr = (void *)call->batch_out_list;
	} else if (tcc_ffi_type_is_array(type)) {
		call->batch_out_array = (ducktinycc_array_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_array_t));
		call->batch_out_ptr = (void *)call->batch_out_array;
	} else if (tcc_ffi_type_is_struct(type)) {
		call->batch_out_struct = (ducktinycc_struct_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_struct_t));
		call->batch_out_ptr = (void *)call->batch_out_struct;
	} else if (tcc_ffi_type_is_map(type)) {
		call->batch_out_map = (ducktinycc_map_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_map_t));
		call->batch_out_ptr = (void *)call->batch_out_map;
	} else if (tcc_ffi_type_is_union(type)) {
		call->batch_out_union = (ducktinycc_union_t *)tcc_duckdb_calloc(call->n, sizeof(ducktinycc_union_t));
		call->batch_out_ptr = (void *)call->batch_out_union;
	}

	if (call->n > 0 && !call->batch_out_ptr && type != TCC_FFI_VOID) {
		call->error = "ducktinycc out of memory";
		return false;
	}
	return true;
}

static bool tcc_exec_write_batch_output(tcc_exec_call_t *call) {
	tcc_ffi_type_t type = call->sig->return_type;
	idx_t row;

	if (type == TCC_FFI_VOID) {
		tcc_validity_set_all(call->out_validity, call->n, false);
		return true;
	}
	if (type == TCC_FFI_VARCHAR) {
		for (row = 0; row < call->n; row++) {
			if (!duckdb_validity_row_is_valid(call->out_validity, row)) {
				continue;
			}
			if (!call->batch_out_varchar || !call->batch_out_varchar[row]) {
				duckdb_validity_set_row_validity(call->out_validity, row, false);
				continue;
			}
			duckdb_vector_assign_string_element(call->output, row, call->batch_out_varchar[row]);
		}
		return true;
	}
	if (type == TCC_FFI_BLOB) {
		for (row = 0; row < call->n; row++) {
			if (!duckdb_validity_row_is_valid(call->out_validity, row)) {
				continue;
			}
			if (!call->batch_out_blob || (call->batch_out_blob[row].len > 0 && !call->batch_out_blob[row].ptr)) {
				duckdb_validity_set_row_validity(call->out_validity, row, false);
				continue;
			}
			duckdb_vector_assign_string_element_len(call->output, row, (const char *)call->batch_out_blob[row].ptr,
			                                        (idx_t)call->batch_out_blob[row].len);
		}
		return true;
	}
	if (tcc_typedesc_is_composite(call->sig->return_desc)) {
		for (row = 0; row < call->n; row++) {
			if (!duckdb_validity_row_is_valid(call->out_validity, row)) {
				continue;
			}
			if (!tcc_write_value_to_vector(call->output, call->sig->return_desc, row, call->batch_out_ptr,
			                               (uint64_t)row, NULL, &call->error)) {
				return false;
			}
		}
	}
	return true;
}

static bool tcc_exec_run_batch(tcc_exec_call_t *call) {
	if (!tcc_exec_prepare_batch_args(call)) {
		return false;
	}
	if (!tcc_exec_alloc_batch_output(call)) {
		return false;
	}
	tcc_validity_set_all(call->out_validity, call->n, call->sig->return_type != TCC_FFI_VOID);
	if (!call->sig->batch_wrapper(call->batch_arg_data, call->in_validity, (uint64_t)call->n, call->batch_out_ptr,
	                              call->out_validity)) {
		call->error = "ducktinycc invoke failed";
		return false;
	}
	return tcc_exec_write_batch_output(call);
}

static bool tcc_exec_keep_row_cstr(tcc_exec_call_t *call, char *value) {
	char **new_allocs;
	idx_t new_cap;

	if (call->row_varchar_alloc_count < call->row_varchar_alloc_capacity) {
		call->row_varchar_allocations[call->row_varchar_alloc_count++] = value;
		return true;
	}
	new_cap = call->row_varchar_alloc_capacity == 0 ? 64 : call->row_varchar_alloc_capacity * 2;
	if (new_cap < call->row_varchar_alloc_capacity) {
		call->error = "ducktinycc out of memory";
		return false;
	}
	new_allocs = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
	if (!new_allocs) {
		call->error = "ducktinycc out of memory";
		return false;
	}
	if (call->row_varchar_allocations && call->row_varchar_alloc_count > 0) {
		memcpy(new_allocs, call->row_varchar_allocations, sizeof(char *) * (size_t)call->row_varchar_alloc_count);
		duckdb_free(call->row_varchar_allocations);
	}
	call->row_varchar_allocations = new_allocs;
	call->row_varchar_alloc_capacity = new_cap;
	call->row_varchar_allocations[call->row_varchar_alloc_count++] = value;
	return true;
}

static bool tcc_exec_prepare_row_arg(tcc_exec_call_t *call, int col, idx_t row) {
	if (call->sig->arg_types[col] == TCC_FFI_VARCHAR) {
		duckdb_string_t *sv = (duckdb_string_t *)(call->in_data[col] + ((size_t)row * call->sig->arg_sizes[col]));
		char *owned_cstr = tcc_copy_duckdb_string_as_cstr(sv);

		if (!owned_cstr) {
			call->error = "ducktinycc out of memory";
			return false;
		}
		if (!tcc_exec_keep_row_cstr(call, owned_cstr)) {
			duckdb_free(owned_cstr);
			return false;
		}
		call->row_varchar_values[col] = owned_cstr;
		call->arg_ptrs[col] = (void *)&call->row_varchar_values[col];
	} else if (call->sig->arg_types[col] == TCC_FFI_BLOB) {
		duckdb_string_t *sv = (duckdb_string_t *)(call->in_data[col] + ((size_t)row * call->sig->arg_sizes[col]));

		call->row_blob_values[col] = tcc_duckdb_string_to_blob(sv);
		call->arg_ptrs[col] = (void *)&call->row_blob_values[col];
	} else if (call->arg_value_bridges && call->arg_value_bridges[col]) {
		call->arg_ptrs[col] = (void *)(call->in_data[col] + ((size_t)row * call->sig->arg_sizes[col]));
	} else if (tcc_ffi_type_is_any_composite(call->sig->arg_types[col]) &&
	           (!call->sig->arg_descs || !call->sig->arg_descs[col])) {
		call->error = "ducktinycc composite arg descriptor is missing";
		return false;
	} else {
		call->arg_ptrs[col] = (void *)(call->in_data[col] + ((size_t)row * call->sig->arg_sizes[col]));
	}
	return true;
}

static void tcc_exec_clear_row_result(tcc_exec_call_t *call) {
	call->out_varchar_value = NULL;
	call->out_blob_value.ptr = NULL;
	call->out_blob_value.len = 0;
	call->out_list_value.ptr = NULL;
	call->out_list_value.validity = NULL;
	call->out_list_value.offset = 0;
	call->out_list_value.len = 0;
	call->out_array_value.ptr = NULL;
	call->out_array_value.validity = NULL;
	call->out_array_value.offset = 0;
	call->out_array_value.len = 0;
	call->out_struct_value.field_ptrs = NULL;
	call->out_struct_value.field_validity = NULL;
	call->out_struct_value.field_count = 0;
	call->out_struct_value.offset = 0;
	call->out_map_value.key_ptr = NULL;
	call->out_map_value.key_validity = NULL;
	call->out_map_value.value_ptr = NULL;
	call->out_map_value.value_validity = NULL;
	call->out_map_value.offset = 0;
	call->out_map_value.len = 0;
	call->out_union_value.tag_ptr = NULL;
	call->out_union_value.member_ptrs = NULL;
	call->out_union_value.member_validity = NULL;
	call->out_union_value.member_count = 0;
	call->out_union_value.offset = 0;
}

static void *tcc_exec_row_output_ptr(tcc_exec_call_t *call) {
	tcc_ffi_type_t type = call->sig->return_type;

	if (type == TCC_FFI_VARCHAR) {
		return (void *)&call->out_varchar_value;
	}
	if (type == TCC_FFI_BLOB) {
		return (void *)&call->out_blob_value;
	}
	if (tcc_ffi_type_is_list(type)) {
		return (void *)&call->out_list_value;
	}
	if (tcc_ffi_type_is_array(type)) {
		return (void *)&call->out_array_value;
	}
	if (tcc_ffi_type_is_struct(type)) {
		return (void *)&call->out_struct_value;
	}
	if (tcc_ffi_type_is_map(type)) {
		return (void *)&call->out_map_value;
	}
	if (tcc_ffi_type_is_union(type)) {
		return (void *)&call->out_union_value;
	}
	return (void *)call->out_value;
}

static bool tcc_exec_write_row_output(tcc_exec_call_t *call, idx_t row, const void *row_result_base, bool out_is_null) {
	tcc_ffi_type_t type = call->sig->return_type;

	if (type == TCC_FFI_VOID || out_is_null) {
		duckdb_validity_set_row_validity(call->out_validity, row, false);
		return true;
	}
	if (type == TCC_FFI_VARCHAR) {
		if (!call->out_varchar_value) {
			duckdb_validity_set_row_validity(call->out_validity, row, false);
			return true;
		}
		duckdb_validity_set_row_validity(call->out_validity, row, true);
		duckdb_vector_assign_string_element(call->output, row, call->out_varchar_value);
		return true;
	}
	if (type == TCC_FFI_BLOB) {
		if (call->out_blob_value.len > 0 && !call->out_blob_value.ptr) {
			duckdb_validity_set_row_validity(call->out_validity, row, false);
			return true;
		}
		duckdb_validity_set_row_validity(call->out_validity, row, true);
		duckdb_vector_assign_string_element_len(call->output, row, (const char *)call->out_blob_value.ptr,
		                                        (idx_t)call->out_blob_value.len);
		return true;
	}
	if (tcc_typedesc_is_composite(call->sig->return_desc)) {
		return tcc_write_value_to_vector(call->output, call->sig->return_desc, row, row_result_base, 0, NULL,
		                                 &call->error);
	}
	duckdb_validity_set_row_validity(call->out_validity, row, true);
	if (call->ret_size > 0) {
		memcpy(call->out_data + ((size_t)row * call->ret_size), call->out_value, call->ret_size);
	}
	return true;
}

static bool tcc_exec_run_rows(tcc_exec_call_t *call) {
	idx_t row;
	int col;

	for (row = 0; row < call->n; row++) {
		bool valid = true;
		bool out_is_null = false;
		void *row_out_ptr;

		for (col = 0; col < call->sig->arg_count; col++) {
			if (call->in_validity[col] && !duckdb_validity_row_is_valid(call->in_validity[col], row)) {
				valid = false;
				break;
			}
			if (!tcc_exec_prepare_row_arg(call, col, row)) {
				return false;
			}
		}
		if (!valid) {
			duckdb_validity_set_row_validity(call->out_validity, row, false);
			continue;
		}

		tcc_exec_clear_row_result(call);
		row_out_ptr = tcc_exec_row_output_ptr(call);
		if (!call->sig->row_wrapper(call->arg_ptrs, row_out_ptr, &out_is_null)) {
			call->error = "ducktinycc invoke failed";
			return false;
		}
		if (!tcc_exec_write_row_output(call, row, row_out_ptr, out_is_null)) {
			return false;
		}
	}
	return true;
}

static void tcc_exec_free_owned_cstrings(char **items, idx_t count) {
	idx_t i;

	if (!items) {
		return;
	}
	for (i = 0; i < count; i++) {
		if (items[i]) {
			duckdb_free(items[i]);
		}
	}
	duckdb_free(items);
}

static void tcc_exec_free_column_ptrs(void **columns, int count) {
	int col;

	if (!columns) {
		return;
	}
	for (col = 0; col < count; col++) {
		if (columns[col]) {
			duckdb_free(columns[col]);
		}
	}
	duckdb_free(columns);
}

static void tcc_exec_free_batch_varchar_owned(char ***owned, int count, idx_t n) {
	int col;

	if (!owned) {
		return;
	}
	for (col = 0; col < count; col++) {
		tcc_exec_free_owned_cstrings(owned[col], n);
	}
	duckdb_free(owned);
}

static void tcc_exec_free_value_bridges(tcc_value_bridge_t **bridges, int count) {
	int col;

	if (!bridges) {
		return;
	}
	for (col = 0; col < count; col++) {
		if (bridges[col]) {
			tcc_value_bridge_destroy(bridges[col]);
		}
	}
	duckdb_free((void *)bridges);
}

static void tcc_exec_cleanup(tcc_exec_call_t *call) {
	int arg_count = call->sig ? call->sig->arg_count : 0;

#define TCC_EXEC_FREE_MEMBER(member)        \
	do {                                   \
		if (call->member) {             \
			duckdb_free((void *)call->member); \
		}                              \
	} while (0)

	tcc_exec_free_owned_cstrings(call->row_varchar_allocations, call->row_varchar_alloc_count);
	tcc_exec_free_batch_varchar_owned(call->batch_varchar_owned, arg_count, call->n);
	tcc_exec_free_column_ptrs((void **)call->batch_varchar_columns, arg_count);
	tcc_exec_free_column_ptrs((void **)call->batch_blob_columns, arg_count);
	TCC_EXEC_FREE_MEMBER(batch_out_varchar);
	TCC_EXEC_FREE_MEMBER(batch_out_blob);
	TCC_EXEC_FREE_MEMBER(batch_out_list);
	TCC_EXEC_FREE_MEMBER(batch_out_array);
	TCC_EXEC_FREE_MEMBER(batch_out_struct);
	TCC_EXEC_FREE_MEMBER(batch_out_map);
	TCC_EXEC_FREE_MEMBER(batch_out_union);
	TCC_EXEC_FREE_MEMBER(in_data);
	TCC_EXEC_FREE_MEMBER(in_validity);
	TCC_EXEC_FREE_MEMBER(arg_ptrs);
	TCC_EXEC_FREE_MEMBER(batch_arg_data);
	TCC_EXEC_FREE_MEMBER(row_varchar_values);
	TCC_EXEC_FREE_MEMBER(row_blob_values);
	tcc_exec_free_value_bridges(call->arg_value_bridges, arg_count);

#undef TCC_EXEC_FREE_MEMBER
}

/**
 * @function tcc_execute_compiled_scalar_udf
 * @brief Execute generated row/chunk-scalar-loop wrappers and marshal DuckDB vectors to/from C bridge descriptors.
 * @param[in] info DuckDB function invocation info.
 * @param[in] input Borrowed input chunk.
 * @param[out] output Borrowed output vector to fill.
 * @ownership borrows(info,input,output), transfers(none)
 * @heap allocates transient per-call bridge buffers and decoded varchar/blob arrays; all released in cleanup path
 * @stack fixed-size locals only (large buffers are heap-backed)
 * @thread_safety relies on immutable signature context + per-call temporaries
 * @locks none (registry/session locking happens at other boundaries)
 * @errors sets duckdb_scalar_function_set_error(info, ...) on bridge/runtime failures
 */
static void tcc_execute_compiled_scalar_udf(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_exec_call_t call;

	tcc_exec_call_init(&call, info, input, output);
	if (!tcc_exec_validate(&call)) {
		goto done;
	}
	if (!tcc_exec_alloc_args(&call)) {
		goto done;
	}
	if (!tcc_exec_build_input_columns(&call)) {
		goto done;
	}
	if (!tcc_exec_prepare_output_validity(&call)) {
		goto done;
	}
	if (call.sig->wrapper_mode == TCC_WRAPPER_MODE_BATCH) {
		(void)tcc_exec_run_batch(&call);
	} else {
		(void)tcc_exec_run_rows(&call);
	}

done:
	tcc_exec_cleanup(&call);
	if (call.error) {
		duckdb_scalar_function_set_error(info, call.error);
	}
}

