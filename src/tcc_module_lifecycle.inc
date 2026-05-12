/*
 * DuckTinyCC compile artifact registry, module-state lifecycle, and tcc_module bind callback.
 *
 * Included by tcc_module.c after host symbols and before parser/codegen orchestration.
 */

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
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
	for (i = 0; i < session->symbol_names.count && i < session->symbol_count; i++) {
		if (tcc_add_symbol(s, session->symbol_names.items[i], (void *)(uintptr_t)session->symbol_ptrs[i]) != 0) {
			tcc_set_error(error_buf, "tcc_add_symbol failed for user symbol");
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
	/* Set lib_path BEFORE tcc_set_output_type: tcc_set_output_type eagerly
	 * expands {B} in CONFIG_TCC_SYSINCLUDEPATHS via tcc_split_path().  If we
	 * wait until tcc_configure_runtime_paths (called after set_output_type),
	 * {B} resolves to CONFIG_TCCDIR (the TinyCC install dir used at build
	 * time, which does not exist on end-user machines) instead of our
	 * embedded-runtime extraction directory. */
	if (runtime_path && runtime_path[0] != '\0') {
		tcc_set_lib_path(s, runtime_path);
	}
	tcc_set_error_func(s, error_buf, tcc_append_error);
	/* Use -nostdlib so TinyCC does not attempt to locate and link libc.so at
	 * relocation time.  In TCC_OUTPUT_MEMORY mode all undefined symbols are
	 * resolved from the host process via dlsym(RTLD_DEFAULT, ...), so the
	 * shared-library file is never needed.  Without this flag, compiling on a
	 * machine that has no C development files installed (e.g. a bare Ubuntu
	 * runtime with no libc6-dev) would produce "library 'c' not found" even
	 * for code that uses no libc functions at all. */
	tcc_set_options(s, "-nostdlib");
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
#define TCC_FREE_BIND_FIELD(field)        \
	do {                                 \
		if (bind->field) {            \
			duckdb_free(bind->field); \
		}                            \
	} while (0)
	TCC_FREE_BIND_FIELD(mode);
	TCC_FREE_BIND_FIELD(runtime_path);
	TCC_FREE_BIND_FIELD(source);
	TCC_FREE_BIND_FIELD(symbol);
	TCC_FREE_BIND_FIELD(sql_name);
	TCC_FREE_BIND_FIELD(arg_types);
	TCC_FREE_BIND_FIELD(return_type);
	TCC_FREE_BIND_FIELD(wrapper_mode);
	TCC_FREE_BIND_FIELD(stability);
	TCC_FREE_BIND_FIELD(include_path);
	TCC_FREE_BIND_FIELD(sysinclude_path);
	TCC_FREE_BIND_FIELD(library_path);
	TCC_FREE_BIND_FIELD(library);
	TCC_FREE_BIND_FIELD(option);
	TCC_FREE_BIND_FIELD(header);
	TCC_FREE_BIND_FIELD(define_name);
	TCC_FREE_BIND_FIELD(define_value);
	TCC_FREE_BIND_FIELD(symbol_name);
#undef TCC_FREE_BIND_FIELD
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

static void tcc_module_bind_add_result_schema(duckdb_bind_info info) {
	static const char *const varchar_cols[] = {
	    "mode", "phase", "code", "message", "detail", "sql_name", "symbol", "artifact_id", "connection_scope"};
	duckdb_logical_type bool_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	idx_t i;

	duckdb_bind_add_result_column(info, "ok", bool_type);
	for (i = 0; i < (idx_t)(sizeof(varchar_cols) / sizeof(varchar_cols[0])); i++) {
		duckdb_bind_add_result_column(info, varchar_cols[i], varchar_type);
	}
	duckdb_destroy_logical_type(&bool_type);
	duckdb_destroy_logical_type(&varchar_type);
}

/* Bind callback: parses named parameters into immutable bind data for one call. */
static void tcc_module_bind(duckdb_bind_info info) {
	tcc_module_bind_data_t *bind;

	bind = (tcc_module_bind_data_t *)duckdb_malloc(sizeof(tcc_module_bind_data_t));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(tcc_module_bind_data_t));

#define TCC_BIND_READ_VARCHAR(field) tcc_bind_read_named_varchar(info, #field, &bind->field)
	TCC_BIND_READ_VARCHAR(mode);
	if (!bind->mode || bind->mode[0] == '\0') {
		bind->mode = tcc_strdup("config_get");
	}
	TCC_BIND_READ_VARCHAR(runtime_path);
	TCC_BIND_READ_VARCHAR(source);
	TCC_BIND_READ_VARCHAR(symbol);
	TCC_BIND_READ_VARCHAR(sql_name);
	tcc_bind_read_named_arg_types(info, &bind->arg_types);
	TCC_BIND_READ_VARCHAR(return_type);
	TCC_BIND_READ_VARCHAR(wrapper_mode);
	if (!bind->wrapper_mode || bind->wrapper_mode[0] == '\0') {
		if (bind->wrapper_mode) {
			duckdb_free(bind->wrapper_mode);
		}
		bind->wrapper_mode = tcc_strdup("row");
	}
	TCC_BIND_READ_VARCHAR(stability);
	TCC_BIND_READ_VARCHAR(include_path);
	TCC_BIND_READ_VARCHAR(sysinclude_path);
	TCC_BIND_READ_VARCHAR(library_path);
	TCC_BIND_READ_VARCHAR(library);
	TCC_BIND_READ_VARCHAR(option);
	TCC_BIND_READ_VARCHAR(header);
	TCC_BIND_READ_VARCHAR(define_name);
	TCC_BIND_READ_VARCHAR(define_value);
	TCC_BIND_READ_VARCHAR(symbol_name);
#undef TCC_BIND_READ_VARCHAR
	{
		duckdb_value spval = duckdb_bind_get_named_parameter(info, "symbol_ptr");
		if (spval && !duckdb_is_null_value(spval)) {
			char *ptr_str = duckdb_get_varchar(spval);
			if (ptr_str) {
				bind->symbol_ptr = (uint64_t)strtoull(ptr_str, NULL, 10);
				bind->has_symbol_ptr = true;
				duckdb_free(ptr_str);
			}
		}
		if (spval) {
			duckdb_destroy_value(&spval);
		}
	}

	tcc_module_bind_add_result_schema(info);

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

