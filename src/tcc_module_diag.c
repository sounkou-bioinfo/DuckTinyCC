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

/*
 * tcc_help(): the SQL-reachable manual.  The DuckDB C API cannot attach
 * descriptions or parameter names to functions, so duckdb_functions() shows
 * none; agents and humans connected only through SQL find this function
 * there instead.  One row per SQL function, tcc_module mode, signature type
 * token, and C-side helper.  test/sql/tcc_module_help.test fails when a
 * registered tcc_* function has no row here.
 */
typedef struct {
	const char *kind;
	const char *name;
	const char *signature;
	const char *description;
	const char *example;
} tcc_help_row_t;

#define TCC_HELP_READ_WRITE(t, sqlt)                                                                                     \
	{"scalar", "tcc_read_" #t, "tcc_read_" #t "(handle UBIGINT, byte_offset UBIGINT) -> " sqlt,                            \
	 "Bounds-checked read of one " sqlt " from a tcc_alloc buffer; NULL if out of bounds or the handle is unknown.",     \
	 "SELECT tcc_read_" #t "(h, 0) FROM (SELECT tcc_alloc(16) AS h);"},                                                     \
	    {"scalar", "tcc_write_" #t, "tcc_write_" #t "(handle UBIGINT, byte_offset UBIGINT, value " sqlt ") -> BOOLEAN",     \
	     "Bounds-checked write of one " sqlt " into a tcc_alloc buffer; false if out of bounds or the handle is unknown.", \
	     "SELECT tcc_write_" #t "(h, 0, 1) FROM (SELECT tcc_alloc(16) AS h);"}

static const tcc_help_row_t tcc_help_rows[] = {
    /* table functions */
    {"table", "tcc_module",
     "tcc_module(mode := 'quick_compile', source := ..., symbol := ..., sql_name := ..., return_type := ..., "
     "arg_types := [...], wrapper_mode := 'row', stability := 'consistent', library := ...)",
     "Compile C with TinyCC in memory and register it as a SQL scalar function; other modes stage inputs, "
     "configure the session, or generate C helpers. Returns one row: ok, mode, phase, code, message, detail, "
     "sql_name, symbol, artifact_id, connection_scope.",
     "SELECT ok, code FROM tcc_module(mode := 'quick_compile', source := 'int64_t twice(int64_t x){ return 2*x; }', "
     "symbol := 'twice', sql_name := 'twice', return_type := 'i64', arg_types := ['i64']); SELECT twice(21);"},
    {"table", "tcc_help", "tcc_help() -> (kind, name, signature, description, example)",
     "This manual: every DuckTinyCC SQL function, tcc_module mode, signature type token, and C helper.",
     "SELECT name, description FROM tcc_help() WHERE kind = 'mode';"},
    {"table", "tcc_system_paths", "tcc_system_paths(runtime_path := ..., library_path := ...)",
     "Show the effective embedded TinyCC runtime directory and the include and library search paths.",
     "SELECT kind, value, exists FROM tcc_system_paths();"},
    {"table", "tcc_library_probe", "tcc_library_probe(library := ..., runtime_path := ..., library_path := ...)",
     "Show where a library request (bare name, file name, or path) would resolve before compiling against it.",
     "SELECT kind, key, value, exists FROM tcc_library_probe(library := 'm');"},

    /* tcc_module modes */
    {"mode", "quick_compile",
     "tcc_module(mode := 'quick_compile', source, symbol, sql_name, return_type, arg_types, kind := 'scalar', ...)",
     "One call: compile source, generate the wrapper, relocate in memory, and register sql_name.",
     "SELECT ok FROM tcc_module(mode := 'quick_compile', source := 'double sq(double x){ return x*x; }', "
     "symbol := 'sq', sql_name := 'sq', return_type := 'f64', arg_types := ['f64']);"},
    {"mode", "compile", "tcc_module(mode := 'compile', return_type, arg_types, kind := 'scalar', ...)",
     "Compile everything staged with add_* modes plus the symbol bound by tinycc_bind, and register it.",
     "SELECT ok FROM tcc_module(mode := 'add_source', source := 'int64_t half(int64_t x){ return x / 2; }'); "
     "SELECT ok FROM tcc_module(mode := 'tinycc_bind', symbol := 'half', sql_name := 'half'); "
     "SELECT ok FROM tcc_module(mode := 'compile', return_type := 'i64', arg_types := ['i64']); SELECT half(42);"},
    {"mode", "codegen_preview", "tcc_module(mode := 'codegen_preview', symbol, sql_name, return_type, arg_types)",
     "Validate a signature and return the generated wrapper C source in detail without compiling it.",
     "SELECT detail FROM tcc_module(mode := 'codegen_preview', symbol := 'f', sql_name := 'f', return_type := 'i64', "
     "arg_types := ['i64']);"},
    {"mode", "config_get", "tcc_module() or tcc_module(mode := 'config_get')",
     "Report the session runtime path, state_id, and config version. The default mode.",
     "SELECT * FROM tcc_module();"},
    {"mode", "config_set", "tcc_module(mode := 'config_set', runtime_path := ...)",
     "Override the TinyCC runtime directory for this session.", "SELECT ok FROM tcc_module(mode := 'config_set', "
     "runtime_path := '/opt/tcc');"},
    {"mode", "config_reset", "tcc_module(mode := 'config_reset')",
     "Clear staged inputs and the runtime override; registered functions stay registered.",
     "SELECT ok FROM tcc_module(mode := 'config_reset');"},
    {"mode", "list", "tcc_module(mode := 'list')", "List staged build inputs and registered artifacts.",
     "SELECT * FROM tcc_module(mode := 'list');"},
    {"mode", "tcc_new_state", "tcc_module(mode := 'tcc_new_state')",
     "Clear all staged build inputs and start a new logical state.", "SELECT ok FROM tcc_module(mode := 'tcc_new_state');"},
    {"mode", "add_source", "tcc_module(mode := 'add_source', source := ...)", "Stage a C translation unit for compile.",
     "SELECT ok FROM tcc_module(mode := 'add_source', source := 'int64_t f(int64_t x){ return x; }');"},
    {"mode", "add_header", "tcc_module(mode := 'add_header', header := ...)", "Stage C header text prepended to sources.",
     "SELECT ok FROM tcc_module(mode := 'add_header', header := '#define K 3');"},
    {"mode", "add_include", "tcc_module(mode := 'add_include', include_path := ...)", "Stage a user include directory.",
     "SELECT ok FROM tcc_module(mode := 'add_include', include_path := '/usr/local/include');"},
    {"mode", "add_sysinclude", "tcc_module(mode := 'add_sysinclude', sysinclude_path := ...)",
     "Stage a system include directory.",
     "SELECT ok FROM tcc_module(mode := 'add_sysinclude', sysinclude_path := '/usr/include');"},
    {"mode", "add_library_path", "tcc_module(mode := 'add_library_path', library_path := ...)",
     "Stage a library search directory.",
     "SELECT ok FROM tcc_module(mode := 'add_library_path', library_path := '/usr/local/lib');"},
    {"mode", "add_library", "tcc_module(mode := 'add_library', library := ...)",
     "Stage a library to link: bare name (m, z), file name, or path. Needed for any libc/system symbol.",
     "SELECT ok FROM tcc_module(mode := 'add_library', library := 'm');"},
    {"mode", "add_option", "tcc_module(mode := 'add_option', option := ...)", "Stage a TinyCC command-line option.",
     "SELECT ok FROM tcc_module(mode := 'add_option', option := '-O2');"},
    {"mode", "add_define", "tcc_module(mode := 'add_define', define_name := ..., define_value := ...)",
     "Stage a preprocessor definition.",
     "SELECT ok FROM tcc_module(mode := 'add_define', define_name := 'N', define_value := '8');"},
    {"mode", "add_symbol", "tcc_module(mode := 'add_symbol', symbol_name := ..., symbol_ptr := UBIGINT)",
     "Inject a raw host address under a C name, e.g. a function pointer or a tcc_dataptr buffer.",
     "SELECT ok FROM tcc_module(mode := 'add_symbol', symbol_name := 'BUF', symbol_ptr := 42::UBIGINT);"},
    {"mode", "tinycc_bind", "tcc_module(mode := 'tinycc_bind', symbol := ..., sql_name := ..., stability := ...)",
     "Stage which C symbol the next compile registers, and under which SQL name.",
     "SELECT ok FROM tcc_module(mode := 'tinycc_bind', symbol := 'f', sql_name := 'f');"},
    {"mode", "c_struct",
     "tcc_module(mode := 'c_struct', source := <definition>, symbol := <tag>, arg_types := ['field:type', ...])",
     "Generate struct_<tag>_new/free/sizeof/alignof and per-field get_/set_/off_/_addr SQL functions.",
     "SELECT ok FROM tcc_module(mode := 'c_struct', source := 'struct pt { int x; int y; };', symbol := 'pt', "
     "arg_types := ['x:i32', 'y:i32']); SELECT struct_pt_get_x(struct_pt_set_x(struct_pt_new(), 7));"},
    {"mode", "c_union",
     "tcc_module(mode := 'c_union', source := <definition>, symbol := <tag>, arg_types := ['member:type', ...])",
     "Generate union_<tag>_* allocation, metadata, and member accessor SQL functions.",
     "SELECT ok FROM tcc_module(mode := 'c_union', source := 'union num { int i; float f; };', symbol := 'num', "
     "arg_types := ['i:i32', 'f:f32']);"},
    {"mode", "c_bitfield",
     "tcc_module(mode := 'c_bitfield', source := <definition>, symbol := <tag>, arg_types := ['field:type', ...])",
     "Generate struct_<tag>_* allocation and bitfield getter/setter SQL functions.",
     "SELECT ok FROM tcc_module(mode := 'c_bitfield', source := 'struct fl { unsigned a:1; unsigned b:3; };', "
     "symbol := 'fl', arg_types := ['a:u8', 'b:u8']);"},
    {"mode", "c_enum",
     "tcc_module(mode := 'c_enum', source := <definition>, symbol := <tag>, arg_types := ['ENUMERATOR', ...])",
     "Generate enum_<tag>_<ENUMERATOR>() constant functions and enum_<tag>_sizeof().",
     "SELECT ok FROM tcc_module(mode := 'c_enum', source := 'enum color { RED, GREEN };', symbol := 'color', "
     "arg_types := ['RED', 'GREEN']); SELECT enum_color_GREEN();"},

    /* function kinds; S is the symbol argument */
    {"kind", "scalar", "R S(args...)",
     "The default kind. Registers a scalar function that calls S for each row, or loops over each chunk in C with "
     "wrapper_mode := 'chunk_scalar_loop'.",
     "SELECT ok FROM tcc_module(mode := 'quick_compile', source := 'int64_t inc(int64_t x){ return x + 1; }', "
     "symbol := 'inc', sql_name := 'inc', return_type := 'i64', arg_types := ['i64']); SELECT inc(41);"},
    {"kind", "aggregate",
     "S_state; void S_init(S_state *) optional; void S_step(S_state *, args...); "
     "void S_combine(S_state *into, S_state *from); int S_final(S_state *, R *out); void S_destroy(S_state *) optional",
     "kind := 'aggregate' builds an aggregate from C functions named after S; source must be in the same call. Rows "
     "with a NULL argument are skipped, and S_final returning 0 gives NULL. Do not call it with ORDER BY inside the "
     "parentheses or over a whole-partition window frame such as OVER (): DuckDB crashes C-API aggregates there "
     "(duckdb/duckdb#26109).",
     "SELECT ok FROM tcc_module(mode := 'quick_compile', kind := 'aggregate', source := 'typedef struct { int64_t n; } "
     "cnt_state; void cnt_step(cnt_state *s, int64_t x) { (void)x; s->n++; } void cnt_combine(cnt_state *a, "
     "cnt_state *b) { a->n += b->n; } int cnt_final(cnt_state *s, int64_t *out) { *out = s->n; return 1; }', "
     "symbol := 'cnt', sql_name := 'cnt', return_type := 'i64', arg_types := ['i64']); SELECT cnt(range) FROM "
     "range(10);"},
    {"kind", "table",
     "S_state; void S_init(S_state *, args...) optional when there are no args; int S_next(S_state *, C1 *, ..., "
     "Cm *); void S_destroy(S_state *) optional",
     "kind := 'table' builds a table function; source must be in the same call. Columns are the fields of a "
     "struct<...> return_type, or one column named value. Arguments are constant bool, integer, float, or varchar "
     "values. S_next fills one row and returns 1, or returns 0 when done. A scan runs on one thread.",
     "SELECT ok FROM tcc_module(mode := 'quick_compile', kind := 'table', source := 'typedef struct { int64_t i, n; } "
     "upto_state; void upto_init(upto_state *s, int64_t n) { s->n = n; } int upto_next(upto_state *s, int64_t *v) { "
     "if (s->i >= s->n) return 0; *v = s->i++; return 1; }', symbol := 'upto', sql_name := 'upto', "
     "return_type := 'i64', arg_types := ['i64']); SELECT * FROM upto(3);"},

    /* signature tokens */
    {"type", "bool", "bool -> _Bool", "SQL BOOLEAN.", "return_type := 'bool'"},
    {"type", "i8/i16/i32/i64", "i8 -> int8_t ... i64 -> int64_t", "SQL TINYINT, SMALLINT, INTEGER, BIGINT.",
     "arg_types := ['i64']"},
    {"type", "u8/u16/u32/u64", "u8 -> uint8_t ... u64 -> uint64_t",
     "SQL UTINYINT ... UBIGINT. DuckDB does not implicitly cast BIGINT to UBIGINT; prefer i64 for integer columns.",
     "arg_types := ['u64']"},
    {"type", "f32/f64", "f32 -> float, f64 -> double", "SQL FLOAT and DOUBLE.", "arg_types := ['f64', 'f64']"},
    {"type", "varchar", "varchar -> const char *",
     "NUL-terminated text. Returning: a literal, or memory from ducktinycc_result_alloc; NULL pointer -> SQL NULL.",
     "return_type := 'varchar'"},
    {"type", "blob", "blob -> ducktinycc_blob_t { const void *ptr; uint64_t len; }",
     "Binary data. Build returned bytes in ducktinycc_result_alloc memory.", "arg_types := ['blob']"},
    {"type", "ptr", "ptr -> void * (SQL UBIGINT)", "Raw address, e.g. from tcc_dataptr.", "arg_types := ['ptr']"},
    {"type", "uuid", "uuid -> ducktinycc_hugeint_t", "128-bit UUID in DuckDB's internal hugeint layout.",
     "arg_types := ['uuid']"},
    {"type", "date", "date -> ducktinycc_date_t { int32_t days; }", "Days since 1970-01-01.", "arg_types := ['date']"},
    {"type", "time", "time -> ducktinycc_time_t { int64_t micros; }", "Microseconds since midnight.",
     "arg_types := ['time']"},
    {"type", "timestamp", "timestamp -> ducktinycc_timestamp_t { int64_t micros; }", "Microseconds since the epoch.",
     "arg_types := ['timestamp']"},
    {"type", "interval", "interval -> ducktinycc_interval_t { int32_t months; int32_t days; int64_t micros; }",
     "SQL INTERVAL.", "arg_types := ['interval']"},
    {"type", "decimal", "decimal -> ducktinycc_decimal_t { uint8_t width, scale; ducktinycc_hugeint_t value; }",
     "Scaled 128-bit integer with width and scale.", "arg_types := ['decimal']"},
    {"type", "list<T> / T[]", "ducktinycc_list_t { const void *ptr; const uint64_t *validity; uint64_t offset, len; }",
     "Variable-length list. Read with ducktinycc_list_elem_ptr and ducktinycc_list_is_valid.",
     "arg_types := ['i64[]']"},
    {"type", "T[N]", "ducktinycc_array_t (same layout as list)", "Fixed-size ARRAY.", "arg_types := ['f32[3]']"},
    {"type", "struct<name:T;...>", "ducktinycc_struct_t { field_ptrs; field_validity; field_count; offset; }",
     "SQL STRUCT. Field i at ((const T *)ducktinycc_struct_field_ptr(&s, i))[s.offset].",
     "arg_types := ['struct<a:i64;b:f64>']"},
    {"type", "map<K;V>", "ducktinycc_map_t { key_ptr; key_validity; value_ptr; value_validity; offset; len; }",
     "SQL MAP. Read with ducktinycc_map_key_ptr / ducktinycc_map_value_ptr.", "arg_types := ['map<varchar;i64>']"},
    {"type", "union<name:T;...>", "ducktinycc_union_t { tag_ptr; member_ptrs; member_validity; member_count; offset; }",
     "SQL UNION. Active member from ducktinycc_union_tag.", "arg_types := ['union<i:i64;s:varchar>']"},

    /* C helpers visible to compiled code */
    {"c_helper", "ducktinycc_result_alloc", "void *ducktinycc_result_alloc(uint64_t size)",
     "Scratch memory for a returned VARCHAR, BLOB, or LIST/MAP/STRUCT payload, in scalar functions, S_final, and "
     "S_next. Private to the executing chunk and thread, freed after DuckDB copies the results. Returns NULL in "
     "S_step and S_combine, whose states must not keep it. Use it instead of static buffers, which race across "
     "threads.",
     "char *b = ducktinycc_result_alloc(n + 1);"},
    {"c_helper", "ducktinycc_malloc", "void *ducktinycc_malloc(uint64_t size)",
     "Heap memory from the host C runtime for state that outlives a call, such as a growing aggregate state. "
     "Needs no library := 'c'. Release it with ducktinycc_free, typically in S_destroy.",
     "st->v = ducktinycc_malloc(256 * sizeof(int64_t));"},
    {"c_helper", "ducktinycc_realloc", "void *ducktinycc_realloc(void *ptr, uint64_t size)",
     "Resize memory from ducktinycc_malloc (NULL ptr allocates). Returns NULL on failure and leaves ptr valid.",
     "v = ducktinycc_realloc(st->v, cap * sizeof(int64_t));"},
    {"c_helper", "ducktinycc_free", "void ducktinycc_free(void *ptr)", "Release memory from ducktinycc_malloc or "
     "ducktinycc_realloc. NULL is ignored.", "void s_destroy(s_state *st) { ducktinycc_free(st->v); }"},
    {"c_helper", "ducktinycc_list_elem_ptr",
     "const void *ducktinycc_list_elem_ptr(const ducktinycc_list_t *l, uint64_t i, uint64_t elem_size)",
     "Bounds-checked pointer to list element i, or NULL.",
     "const int64_t *p = ducktinycc_list_elem_ptr(&l, i, sizeof(int64_t));"},
    {"c_helper", "ducktinycc_list_is_valid", "int ducktinycc_list_is_valid(const ducktinycc_list_t *l, uint64_t i)",
     "Nonzero if list element i is not NULL.", "if (ducktinycc_list_is_valid(&l, i)) ..."},
    {"c_helper", "ducktinycc_array_elem_ptr",
     "const void *ducktinycc_array_elem_ptr(const ducktinycc_array_t *a, uint64_t i, uint64_t elem_size)",
     "Bounds-checked pointer to array element i, or NULL.",
     "const float *p = ducktinycc_array_elem_ptr(&a, i, sizeof(float));"},
    {"c_helper", "ducktinycc_array_is_valid", "int ducktinycc_array_is_valid(const ducktinycc_array_t *a, uint64_t i)",
     "Nonzero if array element i is not NULL.", "if (ducktinycc_array_is_valid(&a, i)) ..."},
    {"c_helper", "ducktinycc_struct_field_ptr",
     "const void *ducktinycc_struct_field_ptr(const ducktinycc_struct_t *s, uint64_t field)",
     "Base of field column; index it with s.offset.",
     "const int64_t *a = ducktinycc_struct_field_ptr(&s, 0); int64_t v = a[s.offset];"},
    {"c_helper", "ducktinycc_struct_field_is_valid",
     "int ducktinycc_struct_field_is_valid(const ducktinycc_struct_t *s, uint64_t field)",
     "Nonzero if the field is not NULL in this row.", "if (ducktinycc_struct_field_is_valid(&s, 0)) ..."},
    {"c_helper", "ducktinycc_map_key_ptr",
     "const void *ducktinycc_map_key_ptr(const ducktinycc_map_t *m, uint64_t i, uint64_t key_size)",
     "Bounds-checked pointer to key i.", "const int64_t *k = ducktinycc_map_key_ptr(&m, i, sizeof(int64_t));"},
    {"c_helper", "ducktinycc_map_value_ptr",
     "const void *ducktinycc_map_value_ptr(const ducktinycc_map_t *m, uint64_t i, uint64_t value_size)",
     "Bounds-checked pointer to value i.", "const double *v = ducktinycc_map_value_ptr(&m, i, sizeof(double));"},
    {"c_helper", "ducktinycc_union_tag", "int ducktinycc_union_tag(const ducktinycc_union_t *u)",
     "Index of the active union member.", "switch (ducktinycc_union_tag(&u)) { ... }"},
    {"c_helper", "ducktinycc_union_member_ptr",
     "const void *ducktinycc_union_member_ptr(const ducktinycc_union_t *u, uint64_t member)",
     "Base of member column; index it with u.offset.",
     "const int64_t *m = ducktinycc_union_member_ptr(&u, 0); int64_t v = m[u.offset];"},

    /* pointer scalars */
    {"scalar", "tcc_alloc", "tcc_alloc(size UBIGINT) -> UBIGINT",
     "Allocate a zeroed host buffer and return a managed handle (not an address).",
     "SELECT tcc_alloc(64) AS h;"},
    {"scalar", "tcc_free_ptr", "tcc_free_ptr(handle UBIGINT) -> BOOLEAN",
     "Free a tcc_alloc buffer; false for unknown or already freed handles.", "SELECT tcc_free_ptr(tcc_alloc(16));"},
    {"scalar", "tcc_ptr_size", "tcc_ptr_size(handle UBIGINT) -> UBIGINT", "Size in bytes of a tcc_alloc buffer.",
     "SELECT tcc_ptr_size(tcc_alloc(16));"},
    {"scalar", "tcc_dataptr", "tcc_dataptr(handle UBIGINT) -> UBIGINT",
     "Raw address of a tcc_alloc buffer, for add_symbol or ptr arguments. Invalid after tcc_free_ptr.",
     "SELECT tcc_dataptr(tcc_alloc(16));"},
    {"scalar", "tcc_ptr_add", "tcc_ptr_add(address UBIGINT, byte_offset UBIGINT) -> UBIGINT",
     "Unchecked address arithmetic.", "SELECT tcc_ptr_add(tcc_dataptr(tcc_alloc(16)), 8);"},
    {"scalar", "tcc_read_bytes", "tcc_read_bytes(handle UBIGINT, byte_offset UBIGINT, width UBIGINT) -> BLOB",
     "Bounds-checked read of width bytes from a tcc_alloc buffer.", "SELECT tcc_read_bytes(tcc_alloc(16), 0, 8);"},
    {"scalar", "tcc_write_bytes", "tcc_write_bytes(handle UBIGINT, byte_offset UBIGINT, bytes BLOB) -> BOOLEAN",
     "Bounds-checked write of a BLOB into a tcc_alloc buffer.", "SELECT tcc_write_bytes(tcc_alloc(16), 0, '\\x01\\x02'::BLOB);"},
    TCC_HELP_READ_WRITE(i8, "TINYINT"),
    TCC_HELP_READ_WRITE(u8, "UTINYINT"),
    TCC_HELP_READ_WRITE(i16, "SMALLINT"),
    TCC_HELP_READ_WRITE(u16, "USMALLINT"),
    TCC_HELP_READ_WRITE(i32, "INTEGER"),
    TCC_HELP_READ_WRITE(u32, "UINTEGER"),
    TCC_HELP_READ_WRITE(i64, "BIGINT"),
    TCC_HELP_READ_WRITE(u64, "UBIGINT"),
    TCC_HELP_READ_WRITE(f32, "FLOAT"),
    TCC_HELP_READ_WRITE(f64, "DOUBLE"),
};

#undef TCC_HELP_READ_WRITE

#define TCC_HELP_ROW_COUNT (sizeof(tcc_help_rows) / sizeof(tcc_help_rows[0]))

static void tcc_help_bind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

	duckdb_bind_add_result_column(info, "kind", varchar_type);
	duckdb_bind_add_result_column(info, "name", varchar_type);
	duckdb_bind_add_result_column(info, "signature", varchar_type);
	duckdb_bind_add_result_column(info, "description", varchar_type);
	duckdb_bind_add_result_column(info, "example", varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_bind_set_cardinality(info, (idx_t)TCC_HELP_ROW_COUNT, true);
}

static void tcc_help_function(duckdb_function_info info, duckdb_data_chunk output) {
	tcc_diag_init_data_t *init = (tcc_diag_init_data_t *)duckdb_function_get_init_data(info);
	idx_t cap = duckdb_vector_size();
	uint64_t start;
	idx_t n;
	idx_t i;

	if (!init) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	start = atomic_fetch_add_explicit(&init->offset, cap, memory_order_acq_rel);
	if (start >= TCC_HELP_ROW_COUNT) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	n = (idx_t)(TCC_HELP_ROW_COUNT - start) < cap ? (idx_t)(TCC_HELP_ROW_COUNT - start) : cap;
	for (i = 0; i < n; i++) {
		const tcc_help_row_t *r = &tcc_help_rows[start + i];

		tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 0), i, r->kind);
		tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 1), i, r->name);
		tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 2), i, r->signature);
		tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 3), i, r->description);
		tcc_set_varchar_col(duckdb_data_chunk_get_vector(output, 4), i, r->example);
	}
	duckdb_data_chunk_set_size(output, n);
}

/* Registers `tcc_help()`. */
static bool register_tcc_help_function(duckdb_connection connection) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_state rc;

	if (!tf) {
		return false;
	}
	duckdb_table_function_set_name(tf, "tcc_help");
	duckdb_table_function_set_bind(tf, tcc_help_bind);
	duckdb_table_function_set_init(tf, tcc_diag_table_init);
	duckdb_table_function_set_function(tf, tcc_help_function);
	rc = duckdb_register_table_function(connection, tf);
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

