/*
 * DuckTinyCC diagnostics/probe table functions.
 *
 * Included by tcc_module.c after core module-mode handlers so the diagnostic code can keep
 * file-local access to shared helpers/types without exporting them through a broad internal ABI.
 */

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

static void tcc_diag_free_owned_varchar(char **value) {
	if (value && *value) {
		duckdb_free(*value);
		*value = NULL;
	}
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

	memset(&include_paths, 0, sizeof(include_paths));
	memset(&library_paths, 0, sizeof(library_paths));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	tcc_bind_read_named_varchar(info, "runtime_path", &runtime_path);
	tcc_bind_read_named_varchar(info, "library_path", &library_path);
	effective_runtime = (runtime_path && runtime_path[0] != '\0') ? runtime_path : tcc_default_runtime_path();

	if (!tcc_collect_include_paths(effective_runtime, &include_paths) ||
	    !tcc_collect_library_search_paths(effective_runtime, library_path, &library_paths)) {
		duckdb_bind_set_error(info, "out of memory");
		goto fail;
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
	tcc_diag_free_owned_varchar(&runtime_path);
	tcc_diag_free_owned_varchar(&library_path);
	tcc_diag_set_result_schema(info);
	duckdb_bind_set_cardinality(info, bind->rows.count, true);
	duckdb_bind_set_bind_data(info, bind, destroy_tcc_diag_bind_data);
	return;

fail:
	tcc_string_list_destroy(&include_paths);
	tcc_string_list_destroy(&library_paths);
	tcc_diag_free_owned_varchar(&runtime_path);
	tcc_diag_free_owned_varchar(&library_path);
	destroy_tcc_diag_bind_data(bind);
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

	memset(&library_paths, 0, sizeof(library_paths));
	memset(&candidates, 0, sizeof(candidates));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	tcc_bind_read_named_varchar(info, "library", &library);
	tcc_bind_read_named_varchar(info, "runtime_path", &runtime_path);
	tcc_bind_read_named_varchar(info, "library_path", &library_path);
	effective_runtime = (runtime_path && runtime_path[0] != '\0') ? runtime_path : tcc_default_runtime_path();

	if (!library || library[0] == '\0') {
		duckdb_bind_set_error(info, "library is required");
		goto fail;
	}
	if (!tcc_collect_library_search_paths(effective_runtime, library_path, &library_paths) ||
	    !tcc_build_library_candidates(library, &candidates)) {
		duckdb_bind_set_error(info, "out of memory");
		goto fail;
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
	tcc_diag_free_owned_varchar(&library);
	tcc_diag_free_owned_varchar(&runtime_path);
	tcc_diag_free_owned_varchar(&library_path);
	tcc_diag_set_result_schema(info);
	duckdb_bind_set_cardinality(info, bind->rows.count, true);
	duckdb_bind_set_bind_data(info, bind, destroy_tcc_diag_bind_data);
	return;

fail:
	tcc_string_list_destroy(&library_paths);
	tcc_string_list_destroy(&candidates);
	tcc_diag_free_owned_varchar(&library);
	tcc_diag_free_owned_varchar(&runtime_path);
	tcc_diag_free_owned_varchar(&library_path);
	destroy_tcc_diag_bind_data(bind);
}

static bool tcc_register_diag_table_function(duckdb_connection connection, const char *name,
                                             duckdb_table_function_bind_t bind_fn,
                                             const char *const *named_params, idx_t named_param_count) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_state rc;
	idx_t i;

	if (!tf || !varchar_type) {
		duckdb_destroy_logical_type(&varchar_type);
		duckdb_destroy_table_function(&tf);
		return false;
	}
	duckdb_table_function_set_name(tf, name);
	for (i = 0; i < named_param_count; i++) {
		duckdb_table_function_add_named_parameter(tf, named_params[i], varchar_type);
	}
	duckdb_table_function_set_bind(tf, bind_fn);
	duckdb_table_function_set_init(tf, tcc_diag_table_init);
	duckdb_table_function_set_function(tf, tcc_diag_table_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);
	rc = duckdb_register_table_function(connection, tf);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
	return rc == DuckDBSuccess;
}

/* Registers `tcc_system_paths(...)` diagnostics table function. */
static bool register_tcc_system_paths_function(duckdb_connection connection) {
	static const char *const params[] = {"runtime_path", "library_path"};
	return tcc_register_diag_table_function(connection, "tcc_system_paths", tcc_system_paths_bind, params, 2);
}

/* Registers `tcc_library_probe(...)` diagnostics table function. */
static bool register_tcc_library_probe_function(duckdb_connection connection) {
	static const char *const params[] = {"library", "runtime_path", "library_path"};
	return tcc_register_diag_table_function(connection, "tcc_library_probe", tcc_library_probe_bind, params, 3);
}

