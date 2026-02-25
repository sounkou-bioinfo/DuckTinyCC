#include "duckdb_extension.h"
#include "tcc_module.h"

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
#include "libtcc.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

typedef struct {
	char *runtime_path;
	uint64_t config_version;
} tcc_session_t;

typedef struct {
	char *sql_name;
	char *symbol;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	TCCState *tcc;
	int64_t (*fn)(int64_t);
#endif
} tcc_registered_entry_t;

typedef struct {
	duckdb_connection connection;
	tcc_session_t session;
	tcc_registered_entry_t *entries;
	idx_t entry_count;
	idx_t entry_capacity;
} tcc_module_state_t;

typedef struct {
	char *mode;
	char *runtime_path;
	char *source;
	char *symbol;
	char *sql_name;
	char *arg_bigint;
} tcc_module_bind_data_t;

typedef struct {
	bool emitted;
} tcc_module_init_data_t;

typedef struct {
	char message[4096];
} tcc_error_buffer_t;

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

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
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

static void tcc_session_set_runtime_path(tcc_session_t *session, const char *runtime_path) {
	if (session->runtime_path) {
		duckdb_free(session->runtime_path);
		session->runtime_path = NULL;
	}
	if (runtime_path && runtime_path[0] != '\0') {
		session->runtime_path = tcc_strdup(runtime_path);
	}
	session->config_version++;
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
static void tcc_registered_entry_destroy(tcc_registered_entry_t *entry) {
	if (!entry) {
		return;
	}
	if (entry->tcc) {
		tcc_delete(entry->tcc);
		entry->tcc = NULL;
	}
	if (entry->sql_name) {
		duckdb_free(entry->sql_name);
		entry->sql_name = NULL;
	}
	if (entry->symbol) {
		duckdb_free(entry->symbol);
		entry->symbol = NULL;
	}
}

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

static idx_t tcc_registry_find_symbol(tcc_module_state_t *state, const char *symbol) {
	idx_t i;
	if (!state || !symbol) {
		return (idx_t)-1;
	}
	for (i = 0; i < state->entry_count; i++) {
		if (state->entries[i].symbol && strcmp(state->entries[i].symbol, symbol) == 0) {
			return i;
		}
	}
	return (idx_t)-1;
}

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

static bool tcc_registry_store(tcc_module_state_t *state, const char *sql_name, const char *symbol, TCCState *tcc,
                               void *fn_ptr) {
	idx_t idx = tcc_registry_find_sql_name(state, sql_name);
	tcc_registered_entry_t *entry;
	if (idx == (idx_t)-1) {
		if (!tcc_registry_reserve(state, state->entry_count + 1)) {
			return false;
		}
		idx = state->entry_count++;
	}
	entry = &state->entries[idx];
	tcc_registered_entry_destroy(entry);
	entry->sql_name = tcc_strdup(sql_name);
	entry->symbol = tcc_strdup(symbol);
	entry->tcc = tcc;
	entry->fn = (int64_t(*)(int64_t))fn_ptr;
	if (!entry->sql_name || !entry->symbol || !entry->fn || !entry->tcc) {
		tcc_registered_entry_destroy(entry);
		return false;
	}
	return true;
}

static bool tcc_registry_remove(tcc_module_state_t *state, const char *sql_name) {
	idx_t idx = tcc_registry_find_sql_name(state, sql_name);
	if (idx == (idx_t)-1) {
		return false;
	}
	tcc_registered_entry_destroy(&state->entries[idx]);
	if (idx + 1 < state->entry_count) {
		memmove(&state->entries[idx], &state->entries[idx + 1],
		        sizeof(tcc_registered_entry_t) * (state->entry_count - idx - 1));
	}
	state->entry_count--;
	memset(&state->entries[state->entry_count], 0, sizeof(tcc_registered_entry_t));
	return true;
}
#endif

static const char *tcc_session_runtime_path(tcc_module_state_t *state, const char *override_path) {
	if (override_path && override_path[0] != '\0') {
		return override_path;
	}
	if (state->session.runtime_path && state->session.runtime_path[0] != '\0') {
		return state->session.runtime_path;
	}
#ifdef DUCKTINYCC_DEFAULT_RUNTIME_PATH
	return DUCKTINYCC_DEFAULT_RUNTIME_PATH;
#else
	return "third_party/tinycc";
#endif
}

static void destroy_tcc_module_state(void *ptr) {
	tcc_module_state_t *state = (tcc_module_state_t *)ptr;
	idx_t i;
	if (!state) {
		return;
	}
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	for (i = 0; i < state->entry_count; i++) {
		tcc_registered_entry_destroy(&state->entries[i]);
	}
#endif
	if (state->entries) {
		duckdb_free(state->entries);
	}
	if (state->session.runtime_path) {
		duckdb_free(state->session.runtime_path);
	}
	duckdb_free(state);
}

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
	if (bind->arg_bigint) {
		duckdb_free(bind->arg_bigint);
	}
	duckdb_free(bind);
}

static void destroy_tcc_module_init_data(void *ptr) {
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)ptr;
	if (!init) {
		return;
	}
	duckdb_free(init);
}

static void tcc_module_bind(duckdb_bind_info info) {
	tcc_module_bind_data_t *bind;
	duckdb_value value;
	duckdb_logical_type varchar_type;
	duckdb_logical_type bool_type;

	bind = (tcc_module_bind_data_t *)duckdb_malloc(sizeof(tcc_module_bind_data_t));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(tcc_module_bind_data_t));

	value = duckdb_bind_get_named_parameter(info, "mode");
	if (value && !duckdb_is_null_value(value)) {
		bind->mode = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}
	if (!bind->mode || bind->mode[0] == '\0') {
		bind->mode = tcc_strdup("config_get");
	}

	value = duckdb_bind_get_named_parameter(info, "runtime_path");
	if (value && !duckdb_is_null_value(value)) {
		bind->runtime_path = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}

	value = duckdb_bind_get_named_parameter(info, "source");
	if (value && !duckdb_is_null_value(value)) {
		bind->source = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}

	value = duckdb_bind_get_named_parameter(info, "symbol");
	if (value && !duckdb_is_null_value(value)) {
		bind->symbol = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}

	value = duckdb_bind_get_named_parameter(info, "sql_name");
	if (value && !duckdb_is_null_value(value)) {
		bind->sql_name = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}

	value = duckdb_bind_get_named_parameter(info, "arg_bigint");
	if (value && !duckdb_is_null_value(value)) {
		bind->arg_bigint = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}

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

static void tcc_module_init(duckdb_init_info info) {
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)duckdb_malloc(sizeof(tcc_module_init_data_t));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	init->emitted = false;
	duckdb_init_set_init_data(info, init, destroy_tcc_module_init_data);
	duckdb_init_set_max_threads(info, 1);
}

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

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
static int tcc_compile_and_relocate(const char *runtime_path, const char *source, const char *symbol, TCCState **out_tcc,
                                    void **out_symbol, tcc_error_buffer_t *error_buf) {
	TCCState *s;
	if (!source || source[0] == '\0') {
		snprintf(error_buf->message, sizeof(error_buf->message), "missing source");
		return -1;
	}
	s = tcc_new();
	if (!s) {
		snprintf(error_buf->message, sizeof(error_buf->message), "tcc_new failed");
		return -1;
	}
	tcc_set_error_func(s, error_buf, tcc_append_error);
	if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) != 0) {
		snprintf(error_buf->message, sizeof(error_buf->message), "tcc_set_output_type failed");
		tcc_delete(s);
		return -1;
	}
	tcc_configure_runtime_paths(s, runtime_path);
	if (tcc_compile_string(s, source) != 0) {
		if (error_buf->message[0] == '\0') {
			snprintf(error_buf->message, sizeof(error_buf->message), "tcc_compile_string failed");
		}
		tcc_delete(s);
		return -1;
	}
	if (tcc_relocate(s) != 0) {
		if (error_buf->message[0] == '\0') {
			snprintf(error_buf->message, sizeof(error_buf->message), "tcc_relocate failed");
		}
		tcc_delete(s);
		return -1;
	}
	if (symbol && symbol[0] != '\0') {
		void *sym = tcc_get_symbol(s, symbol);
		if (!sym) {
			snprintf(error_buf->message, sizeof(error_buf->message), "symbol '%s' not found", symbol);
			tcc_delete(s);
			return -1;
		}
		*out_symbol = sym;
	}
	*out_tcc = s;
	return 0;
}
#endif

static bool tcc_parse_int64(const char *text, int64_t *out) {
	char *endptr;
	long long val;
	if (!text || !out) {
		return false;
	}
	endptr = NULL;
	val = strtoll(text, &endptr, 10);
	if (endptr == text || (endptr && *endptr != '\0')) {
		return false;
	}
	*out = (int64_t)val;
	return true;
}

static void tcc_module_function(duckdb_function_info info, duckdb_data_chunk output) {
	tcc_module_state_t *state = (tcc_module_state_t *)duckdb_function_get_extra_info(info);
	tcc_module_bind_data_t *bind = (tcc_module_bind_data_t *)duckdb_function_get_bind_data(info);
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)duckdb_function_get_init_data(info);
	const char *runtime_path;

	if (!state || !bind || !init || init->emitted) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}

	runtime_path = tcc_session_runtime_path(state, bind->runtime_path);

	if (strcmp(bind->mode, "config_set") == 0) {
		tcc_session_set_runtime_path(&state->session, bind->runtime_path);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration updated",
		              state->session.runtime_path ? state->session.runtime_path : "(empty)", NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "config_get") == 0) {
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration", runtime_path, NULL, NULL,
		              NULL, "connection");
	} else if (strcmp(bind->mode, "config_reset") == 0) {
		tcc_session_set_runtime_path(&state->session, NULL);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration reset", "(unset)", NULL, NULL,
		              NULL, "connection");
	} else if (strcmp(bind->mode, "list") == 0) {
		char detail[64];
		snprintf(detail, sizeof(detail), "registered_count=%llu", (unsigned long long)state->entry_count);
		tcc_write_row(output, true, bind->mode, "registry", "OK", "registered artifact count", detail, NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "compile") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
		tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
		              "TinyCC compile not supported for WASM build", NULL, NULL, NULL, NULL, "connection");
#else
		TCCState *compiled = NULL;
		void *sym = NULL;
		tcc_error_buffer_t err;
		memset(&err, 0, sizeof(err));
		if (tcc_compile_and_relocate(runtime_path, bind->source, bind->symbol, &compiled, &sym, &err) == 0) {
			(void)sym;
			tcc_delete(compiled);
			tcc_write_row(output, true, bind->mode, "compile", "OK", "compiled and relocated", runtime_path, NULL,
			              bind->symbol, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "compile", "E_COMPILE_FAILED", "compile failed",
			              err.message[0] ? err.message : NULL, NULL, bind->symbol, NULL, "connection");
		}
#endif
	} else if (strcmp(bind->mode, "call") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
		tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
		              "TinyCC call not supported for WASM build", NULL, bind->sql_name, bind->symbol, NULL,
		              "connection");
#else
		int64_t arg = 0;
		int64_t result = 0;
		char result_buf[64];
		if (!bind->arg_bigint || !tcc_parse_int64(bind->arg_bigint, &arg)) {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "arg_bigint is required", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		} else {
			int64_t (*fn)(int64_t) = NULL;
			const char *effective_symbol = bind->symbol;
			if (bind->source && bind->source[0] != '\0') {
				TCCState *compiled = NULL;
				void *sym = NULL;
				tcc_error_buffer_t err;
				memset(&err, 0, sizeof(err));
				if (tcc_compile_and_relocate(runtime_path, bind->source, bind->symbol, &compiled, &sym, &err) != 0) {
					tcc_write_row(output, false, bind->mode, "compile", "E_COMPILE_FAILED", "compile failed",
					              err.message[0] ? err.message : NULL, bind->sql_name, bind->symbol, NULL,
					              "connection");
					init->emitted = true;
					return;
				}
				fn = (int64_t(*)(int64_t))sym;
				result = fn(arg);
				snprintf(result_buf, sizeof(result_buf), "%lld", (long long)result);
				tcc_delete(compiled);
			} else {
				idx_t idx = (idx_t)-1;
				if (bind->sql_name && bind->sql_name[0] != '\0') {
					idx = tcc_registry_find_sql_name(state, bind->sql_name);
				}
				if (idx == (idx_t)-1 && bind->symbol && bind->symbol[0] != '\0') {
					idx = tcc_registry_find_symbol(state, bind->symbol);
				}
				if (idx == (idx_t)-1) {
					tcc_write_row(output, false, bind->mode, "execute", "E_NOT_FOUND", "no registered function found",
					              NULL, bind->sql_name, bind->symbol, NULL, "connection");
					init->emitted = true;
					return;
				}
				fn = state->entries[idx].fn;
				effective_symbol = state->entries[idx].symbol;
				result = fn(arg);
				snprintf(result_buf, sizeof(result_buf), "%lld", (long long)result);
			}
			tcc_write_row(output, true, bind->mode, "execute", "OK", "compiled and executed", result_buf, bind->sql_name,
			              effective_symbol, NULL, "connection");
		}
#endif
	} else if (strcmp(bind->mode, "register") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
		tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
		              "TinyCC register not supported for WASM build", NULL, bind->sql_name, bind->symbol, NULL,
		              "connection");
#else
		TCCState *compiled = NULL;
		void *sym = NULL;
		tcc_error_buffer_t err;
		memset(&err, 0, sizeof(err));
		if (!bind->source || bind->source[0] == '\0' || !bind->symbol || bind->symbol[0] == '\0' ||
		    !bind->sql_name || bind->sql_name[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS",
			              "source, symbol and sql_name are required", NULL, bind->sql_name, bind->symbol, NULL,
			              "connection");
		} else if (tcc_compile_and_relocate(runtime_path, bind->source, bind->symbol, &compiled, &sym, &err) != 0) {
			tcc_write_row(output, false, bind->mode, "compile", "E_COMPILE_FAILED", "compile failed",
			              err.message[0] ? err.message : NULL, bind->sql_name, bind->symbol, NULL, "connection");
		} else if (!tcc_registry_store(state, bind->sql_name, bind->symbol, compiled, sym)) {
			tcc_delete(compiled);
			tcc_write_row(output, false, bind->mode, "register", "E_STORE_FAILED",
			              "failed to store compiled artifact", NULL, bind->sql_name, bind->symbol, NULL, "connection");
		} else {
			tcc_write_row(output, true, bind->mode, "register", "OK", "compiled artifact registered in session",
			              runtime_path, bind->sql_name, bind->symbol, bind->sql_name, "connection");
		}
#endif
	} else if (strcmp(bind->mode, "unregister") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
		tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
		              "TinyCC unregister not supported for WASM build", NULL, bind->sql_name, bind->symbol, NULL,
		              "connection");
#else
		if (!bind->sql_name || bind->sql_name[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "sql_name is required", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		} else if (tcc_registry_remove(state, bind->sql_name)) {
			tcc_write_row(output, true, bind->mode, "register", "OK", "artifact removed from session registry", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "register", "E_NOT_FOUND", "no matching artifact", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		}
#endif
	} else {
		tcc_write_row(output, false, bind->mode, "bind", "E_BAD_MODE", "unknown mode", NULL, NULL, NULL, NULL,
		              "connection");
	}

	init->emitted = true;
}

void RegisterTccModuleFunction(duckdb_connection connection) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	tcc_module_state_t *state;

	state = (tcc_module_state_t *)duckdb_malloc(sizeof(tcc_module_state_t));
	if (!state) {
		duckdb_destroy_logical_type(&varchar_type);
		duckdb_destroy_table_function(&tf);
		return;
	}
	memset(state, 0, sizeof(tcc_module_state_t));
	state->connection = connection;

	duckdb_table_function_set_name(tf, "tcc_module");
	duckdb_table_function_add_named_parameter(tf, "mode", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "runtime_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "source", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "symbol", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "sql_name", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "arg_bigint", varchar_type);

	duckdb_table_function_set_extra_info(tf, state, destroy_tcc_module_state);
	duckdb_table_function_set_bind(tf, tcc_module_bind);
	duckdb_table_function_set_init(tf, tcc_module_init);
	duckdb_table_function_set_function(tf, tcc_module_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);

	duckdb_register_table_function(connection, tf);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
}
