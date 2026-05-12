/*
 * DuckTinyCC tcc_module(...) SQL mode handlers and dispatcher.
 *
 * Included by tcc_module.c after codegen/compile helpers.  Handlers keep session mutation,
 * compile/load, and output-row reporting explicit while keeping the root file navigable.
 */

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
	       strcmp(mode, "add_define") == 0 || strcmp(mode, "add_symbol") == 0 ||
	       strcmp(mode, "tinycc_bind") == 0 ||
	       strcmp(mode, "compile") == 0 || strcmp(mode, "quick_compile") == 0 ||
	       strcmp(mode, "c_struct") == 0 || strcmp(mode, "c_union") == 0 || strcmp(mode, "c_bitfield") == 0 ||
	       strcmp(mode, "c_enum") == 0;
}

/* ===== Extracted mode handlers ===== */

/* Handles add_include/add_sysinclude/add_library_path/add_library/add_option/add_header/add_source. */
static void tcc_mode_add_staged(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                duckdb_data_chunk output) {
	const char *mode = bind->mode;
	tcc_string_list_t *target = NULL;
	const char *value = NULL;
	const char *label = NULL;
	const char *arg_name = NULL;
	if (strcmp(mode, "add_include") == 0) {
		target = &state->session.include_paths;
		value = bind->include_path;
		label = "include path added";
		arg_name = "include_path";
	} else if (strcmp(mode, "add_sysinclude") == 0) {
		target = &state->session.sysinclude_paths;
		value = bind->sysinclude_path;
		label = "sysinclude path added";
		arg_name = "sysinclude_path";
	} else if (strcmp(mode, "add_library_path") == 0) {
		target = &state->session.library_paths;
		value = bind->library_path;
		label = "library path added";
		arg_name = "library_path";
	} else if (strcmp(mode, "add_library") == 0) {
		target = &state->session.libraries;
		value = bind->library;
		label = "library added";
		arg_name = "library";
	} else if (strcmp(mode, "add_option") == 0) {
		target = &state->session.options;
		value = bind->option;
		label = "compiler option added";
		arg_name = "option";
	} else if (strcmp(mode, "add_header") == 0) {
		target = &state->session.headers;
		value = bind->header;
		label = "header source added";
		arg_name = "header";
	} else if (strcmp(mode, "add_source") == 0) {
		target = &state->session.sources;
		value = bind->source;
		label = "source appended";
		arg_name = "source";
	}
	if (!target) {
		tcc_write_row(output, false, mode, "bind", "E_BAD_MODE", "unknown add mode", NULL, NULL, NULL, NULL,
		              "database");
		return;
	}
	if (value && tcc_string_list_append(target, value)) {
		state->session.config_version++;
		tcc_write_row(output, true, mode, "state", "OK", label, value, NULL, NULL, NULL, "database");
	} else {
		char msg[128];
		snprintf(msg, sizeof(msg), "%s is required", arg_name);
		tcc_write_row(output, false, mode, "bind", "E_MISSING_ARGS", msg, NULL, NULL, NULL, NULL, "database");
	}
}

/* Handles c_struct/c_union/c_bitfield/c_enum modes. */
static void tcc_mode_c_helpers(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                               const char *runtime_path, duckdb_data_chunk output) {
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
		goto done;
	}
	if (bind->sql_name && bind->sql_name[0] != '\0') {
		prefix = bind->sql_name;
	} else if (is_enum) {
		int n = snprintf(prefix_buf, sizeof(prefix_buf), "enum_%s", type_name);
		if (n < 0 || (size_t)n >= sizeof(prefix_buf)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "failed to build helper prefix", NULL,
			              bind->sql_name, bind->symbol, NULL, "database");
			goto done;
		}
		prefix = prefix_buf;
	} else {
		int n = snprintf(prefix_buf, sizeof(prefix_buf), "%s_%s", is_union ? "union" : "struct", type_name);
		if (n < 0 || (size_t)n >= sizeof(prefix_buf)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "failed to build helper prefix", NULL,
			              bind->sql_name, bind->symbol, NULL, "database");
			goto done;
		}
		prefix = prefix_buf;
	}
	if (!prefix || !tcc_is_identifier_token(prefix)) {
		tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS",
		              "sql_name must be a valid C/SQL identifier when provided", NULL, bind->sql_name, bind->symbol,
		              NULL, "database");
		goto done;
	}
	if (is_enum) {
		if (!tcc_parse_c_enum_constants(bind->arg_types, &enum_constants, &err)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "invalid c_enum constants",
			              err.message[0] ? err.message : NULL, prefix, type_name, NULL, "database");
			goto done;
		}
		helper_source = tcc_generate_c_enum_helpers_source(type_name, prefix, &enum_constants, &err);
		if (!helper_source) {
			tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED",
			              "failed to generate enum helpers", err.message[0] ? err.message : NULL, prefix, type_name,
			              NULL, "database");
			goto done;
		}
		if (!tcc_build_c_enum_bindings(prefix, &enum_constants, &helper_bindings, &err)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS",
			              "failed to build enum helper signatures", err.message[0] ? err.message : NULL, prefix,
			              type_name, NULL, "database");
			goto done;
		}
	} else {
		if (!tcc_parse_c_field_specs(bind->arg_types, force_bitfield, &fields, &err)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS", "invalid c struct/union field specs",
			              err.message[0] ? err.message : NULL, prefix, type_name, NULL, "database");
			goto done;
		}
		helper_source = tcc_generate_c_composite_helpers_source(kind_keyword, type_name, prefix, &fields, &err);
		if (!helper_source) {
			tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED",
			              "failed to generate struct/union helpers", err.message[0] ? err.message : NULL, prefix,
			              type_name, NULL, "database");
			goto done;
		}
		if (!tcc_build_c_composite_bindings(prefix, &fields, &helper_bindings, &err)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_BAD_ARGS",
			              "failed to build struct/union helper signatures", err.message[0] ? err.message : NULL,
			              prefix, type_name, NULL, "database");
			goto done;
		}
	}

	if (bind->source && bind->source[0] != '\0') {
		size_t n0 = strlen(bind->source);
		size_t n1 = strlen(helper_source);
		combined_source = (char *)duckdb_malloc(n0 + n1 + 3);
		if (!combined_source) {
			tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED",
			              "failed to allocate helper source buffer", NULL, prefix, type_name, NULL, "database");
			goto done;
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
			goto done;
		}
	}

	for (i = 0; i < helper_bindings.count; i++) {
		const tcc_helper_binding_t *entry = &helper_bindings.items[i];
		memset(&err, 0, sizeof(err));
		if (!tcc_compile_generated_binding(runtime_path, state, combined_source, entry, &err)) {
			const char *phase = "compile";
			const char *code = "E_COMPILE_FAILED";
			const char *message = "generated helper compile failed";
			tcc_codegen_classify_error_message(err.message, &phase, &code, &message);
			tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL,
			              entry->sql_name, entry->symbol, NULL, "database");
			goto done;
		}
	}
	{
		char detail[256];
		snprintf(detail, sizeof(detail), "generated=%llu prefix=%.96s target=%.96s",
		         (unsigned long long)helper_bindings.count, prefix, type_name);
		tcc_write_row(output, true, bind->mode, "load", "OK", "generated and registered helper UDFs", detail,
		              prefix, type_name, NULL, "database");
	}

done:
	if (helper_source) {
		duckdb_free(helper_source);
	}
	if (combined_source) {
		duckdb_free(combined_source);
	}
	tcc_c_field_list_destroy(&fields);
	tcc_string_list_destroy(&enum_constants);
	tcc_helper_binding_list_destroy(&helper_bindings);
#endif
}

/* Handles codegen_preview mode. */
static void tcc_mode_codegen_preview(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                     duckdb_data_chunk output) {
	const char *target_symbol = tcc_effective_symbol(state, bind);
	const char *sql_name = tcc_effective_sql_name(state, bind, target_symbol);
	tcc_codegen_source_ctx_t source_ctx;
	tcc_error_buffer_t err;
	memset(&err, 0, sizeof(err));
	tcc_codegen_source_ctx_init(&source_ctx);
	if (!target_symbol || target_symbol[0] == '\0') {
		tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS",
		              "symbol is required (bind or argument)", NULL, sql_name, target_symbol, NULL, "database");
		goto done;
	}
	if (!tcc_codegen_prepare_sources(state, bind, sql_name, target_symbol, &source_ctx, &err)) {
		const char *phase = "codegen";
		const char *code = "E_CODEGEN_FAILED";
		const char *message = "ffi codegen failed";
		tcc_codegen_classify_error_message(err.message, &phase, &code, &message);
		tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL,
		              sql_name, target_symbol, NULL, "database");
		goto done;
	}
	tcc_write_row(output, true, bind->mode, "codegen", "OK", "generated codegen source",
	              source_ctx.compilation_unit_source, sql_name, target_symbol, source_ctx.module_symbol,
	              "database");
done:
	tcc_codegen_source_ctx_destroy(&source_ctx);
}

/* Handles compile/quick_compile modes. */
static void tcc_mode_compile(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                             const char *runtime_path, duckdb_data_chunk output) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
	tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
	              "TinyCC compile codegen path not supported for WASM build", NULL, bind->sql_name, bind->symbol,
	              NULL, "database");
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
		return;
	}
	if (!target_symbol || target_symbol[0] == '\0') {
		tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS",
		              "symbol is required (bind or argument)", NULL, sql_name, target_symbol, NULL, "database");
		return;
	}
	/* Reject duplicate SQL name: DuckDB's scalar-function registration behaviour
	 * differs across platforms (Linux fails, macOS silently replaces).  Checking
	 * our own registry first makes the "already registered" error consistent. */
	if (sql_name && tcc_registry_find_sql_name(state, sql_name) != (idx_t)-1) {
		tcc_write_row(output, false, bind->mode, "load", "E_INIT_FAILED",
		              "generated module init returned false",
		              "sql_name already registered; use tcc_new_state to reset",
		              sql_name, target_symbol, NULL, "database");
		return;
	}
	if (tcc_codegen_compile_and_load_module(runtime_path, state, bind, sql_name, target_symbol, &artifact, &err,
	                                        module_symbol, sizeof(module_symbol)) != 0) {
		tcc_codegen_classify_error_message(err.message, &phase, &code, &message);
		tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL,
		              sql_name, target_symbol, NULL, "database");
		return;
	}
	if (!tcc_registry_store_metadata(state, sql_name, module_symbol, artifact->state_id, artifact)) {
		tcc_artifact_destroy(artifact);
		tcc_write_row(output, false, bind->mode, "register", "E_STORE_FAILED",
		              "failed to store ffi module artifact metadata", NULL, sql_name, target_symbol, NULL,
		              "connection");
		return;
	}
	snprintf(artifact_id, sizeof(artifact_id), "%s@ffi_state_%llu", sql_name,
	         (unsigned long long)artifact->state_id);
	tcc_write_row(output, true, bind->mode, "load", "OK", "compiled and registered SQL function via codegen",
	              runtime_path, sql_name, target_symbol, artifact_id, "database");
#endif
}


/* Session/configuration modes are small and deliberately side-effect-explicit. */
static void tcc_mode_config_set(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                duckdb_data_chunk output) {
	tcc_session_set_runtime_path(&state->session, bind->runtime_path);
	tcc_write_row(output, true, bind->mode, "config", "OK", "session runtime updated",
	              state->session.runtime_path ? state->session.runtime_path : "(empty)", NULL, NULL, NULL,
	              "connection");
}

static void tcc_mode_config_get(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                const char *runtime_path, duckdb_data_chunk output) {
	char detail[256];

	snprintf(detail, sizeof(detail), "runtime=%s state_id=%llu config_version=%llu",
	         runtime_path ? runtime_path : "(unset)", (unsigned long long)state->session.state_id,
	         (unsigned long long)state->session.config_version);
	tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration", detail, NULL, NULL, NULL,
	              "connection");
}

static void tcc_mode_config_reset(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                  duckdb_data_chunk output) {
	tcc_session_set_runtime_path(&state->session, NULL);
	tcc_session_clear_build_state(&state->session);
	tcc_write_row(output, true, bind->mode, "config", "OK", "session reset", "runtime/build state cleared",
	              NULL, NULL, NULL, "database");
}

static void tcc_mode_new_state(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                               duckdb_data_chunk output) {
	char detail[128];

	tcc_session_clear_build_state(&state->session);
	snprintf(detail, sizeof(detail), "state_id=%llu", (unsigned long long)state->session.state_id);
	tcc_write_row(output, true, bind->mode, "state", "OK", "new TinyCC build state prepared", detail, NULL,
	              NULL, NULL, "database");
}

static void tcc_mode_add_define(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                duckdb_data_chunk output) {
	const char *define_value;

	if (!bind->define_name || bind->define_name[0] == '\0') {
		tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "define_name is required", NULL, NULL,
		              NULL, NULL, "database");
		return;
	}
	define_value = bind->define_value ? bind->define_value : "1";
	if (!tcc_string_list_append(&state->session.define_names, bind->define_name)) {
		tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store define", NULL,
		              NULL, NULL, NULL, "database");
		return;
	}
	if (!tcc_string_list_append(&state->session.define_values, define_value)) {
		(void)tcc_string_list_pop_last(&state->session.define_names);
		tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store define", NULL,
		              NULL, NULL, NULL, "database");
		return;
	}
	state->session.config_version++;
	tcc_write_row(output, true, bind->mode, "state", "OK", "define added", bind->define_name, NULL, NULL,
	              NULL, "database");
}

static bool tcc_session_reserve_symbols(tcc_session_t *sess, idx_t wanted) {
	uint64_t *new_ptrs;
	idx_t new_cap;

	if (wanted <= sess->symbol_capacity) {
		return true;
	}
	new_cap = sess->symbol_capacity == 0 ? 8 : sess->symbol_capacity * 2;
	if (new_cap < wanted) {
		new_cap = wanted;
	}
	if (new_cap < sess->symbol_capacity || new_cap > (idx_t)(SIZE_MAX / sizeof(uint64_t))) {
		return false;
	}
	new_ptrs = (uint64_t *)duckdb_malloc(sizeof(uint64_t) * (size_t)new_cap);
	if (!new_ptrs) {
		return false;
	}
	if (sess->symbol_ptrs && sess->symbol_count > 0) {
		memcpy(new_ptrs, sess->symbol_ptrs, sizeof(uint64_t) * (size_t)sess->symbol_count);
	}
	if (sess->symbol_ptrs) {
		duckdb_free(sess->symbol_ptrs);
	}
	sess->symbol_ptrs = new_ptrs;
	sess->symbol_capacity = new_cap;
	return true;
}

static void tcc_mode_add_symbol(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                duckdb_data_chunk output) {
	tcc_session_t *sess = &state->session;
	char detail[128];

	if (!bind->symbol_name || bind->symbol_name[0] == '\0') {
		tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol_name is required", NULL,
		              NULL, NULL, NULL, "database");
		return;
	}
	if (!bind->has_symbol_ptr) {
		tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol_ptr is required", NULL,
		              NULL, NULL, NULL, "database");
		return;
	}
	if (!tcc_session_reserve_symbols(sess, sess->symbol_count + 1)) {
		tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED",
		              "failed to grow symbol pointer array", NULL, NULL, NULL, NULL, "database");
		return;
	}
	if (!tcc_string_list_append(&sess->symbol_names, bind->symbol_name)) {
		tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store symbol name", NULL,
		              NULL, NULL, NULL, "database");
		return;
	}
	sess->symbol_ptrs[sess->symbol_count++] = bind->symbol_ptr;
	sess->config_version++;
	snprintf(detail, sizeof(detail), "ptr=0x%llx", (unsigned long long)bind->symbol_ptr);
	tcc_write_row(output, true, bind->mode, "state", "OK", "symbol added", detail, NULL, bind->symbol_name,
	              NULL, "database");
}

static void tcc_mode_tinycc_bind(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                                 duckdb_data_chunk output) {
	char *bound_symbol = NULL;
	char *bound_sql_name = NULL;
	char *bound_stability = NULL;
	tcc_function_stability_t parsed_stability = TCC_FUNCTION_STABILITY_CONSISTENT;
	tcc_error_buffer_t err;

	memset(&err, 0, sizeof(err));
	if (!bind->symbol || bind->symbol[0] == '\0') {
		tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required", NULL,
		              bind->sql_name, bind->symbol, NULL, "database");
		return;
	}
	if (bind->stability && bind->stability[0] != '\0' &&
	    !tcc_parse_function_stability(bind->stability, &parsed_stability, &err)) {
		tcc_write_row(output, false, bind->mode, "bind", "E_BAD_STABILITY", "invalid stability",
		              err.message[0] ? err.message : NULL, bind->sql_name, bind->symbol, NULL, "database");
		return;
	}
	bound_symbol = tcc_strdup(bind->symbol);
	bound_sql_name = tcc_strdup(bind->sql_name && bind->sql_name[0] != '\0' ? bind->sql_name : bind->symbol);
	if (bind->stability && bind->stability[0] != '\0') {
		bound_stability = tcc_strdup(tcc_function_stability_token(parsed_stability));
	}
	if (!bound_symbol || !bound_sql_name || (bind->stability && bind->stability[0] != '\0' && !bound_stability)) {
		if (bound_symbol) {
			duckdb_free(bound_symbol);
		}
		if (bound_sql_name) {
			duckdb_free(bound_sql_name);
		}
		if (bound_stability) {
			duckdb_free(bound_stability);
		}
		tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store symbol binding",
		              NULL, bind->sql_name, bind->symbol, NULL, "database");
		return;
	}
	tcc_session_clear_bind(&state->session);
	state->session.bound_symbol = bound_symbol;
	state->session.bound_sql_name = bound_sql_name;
	state->session.bound_stability = bound_stability;
	state->session.config_version++;
	tcc_write_row(output, true, bind->mode, "bind", "OK", "symbol binding updated",
	              state->session.bound_stability ? state->session.bound_stability : "consistent",
	              state->session.bound_sql_name, state->session.bound_symbol, NULL, "connection");
}

static void tcc_mode_list(tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                          duckdb_data_chunk output) {
	char detail[256];

	snprintf(detail, sizeof(detail),
	         "registered=%llu sources=%llu headers=%llu includes=%llu libs=%llu symbols=%llu state_id=%llu",
	         (unsigned long long)state->entry_count, (unsigned long long)state->session.sources.count,
	         (unsigned long long)state->session.headers.count, (unsigned long long)state->session.include_paths.count,
	         (unsigned long long)state->session.libraries.count,
	         (unsigned long long)state->session.symbol_count, (unsigned long long)state->session.state_id);
	tcc_write_row(output, true, bind->mode, "registry", "OK", "session summary", detail, NULL, NULL, NULL,
	              "connection");
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
		tcc_mode_config_set(state, bind, output);
	} else if (strcmp(bind->mode, "config_get") == 0) {
		tcc_mode_config_get(state, bind, runtime_path, output);
	} else if (strcmp(bind->mode, "config_reset") == 0) {
		tcc_mode_config_reset(state, bind, output);
	} else if (strcmp(bind->mode, "tcc_new_state") == 0) {
		tcc_mode_new_state(state, bind, output);
	} else if (strncmp(bind->mode, "add_", 4) == 0 && strcmp(bind->mode, "add_define") != 0 &&
	           strcmp(bind->mode, "add_symbol") != 0) {
		tcc_mode_add_staged(state, bind, output);
	} else if (strcmp(bind->mode, "add_define") == 0) {
		tcc_mode_add_define(state, bind, output);
	} else if (strcmp(bind->mode, "add_symbol") == 0) {
		tcc_mode_add_symbol(state, bind, output);
	} else if (strcmp(bind->mode, "tinycc_bind") == 0) {
		tcc_mode_tinycc_bind(state, bind, output);
	} else if (strcmp(bind->mode, "list") == 0) {
		tcc_mode_list(state, bind, output);
	} else if (strcmp(bind->mode, "c_struct") == 0 || strcmp(bind->mode, "c_union") == 0 ||
	           strcmp(bind->mode, "c_bitfield") == 0 || strcmp(bind->mode, "c_enum") == 0) {
		tcc_mode_c_helpers(state, bind, runtime_path, output);
	} else if (strcmp(bind->mode, "codegen_preview") == 0) {
		tcc_mode_codegen_preview(state, bind, output);
	} else if (strcmp(bind->mode, "compile") == 0 || strcmp(bind->mode, "quick_compile") == 0) {
		tcc_mode_compile(state, bind, runtime_path, output);
	} else {
		tcc_write_row(output, false, bind->mode, "bind", "E_BAD_MODE", "unknown mode", NULL, NULL, NULL, NULL,
		              "connection");
	}
	if (lock_mode == 2) {
		tcc_rwlock_write_unlock(&state->lock);
	} else if (lock_mode == 1) {
		tcc_rwlock_read_unlock(&state->lock);
	}
}

