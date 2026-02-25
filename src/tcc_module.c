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
	char **items;
	idx_t count;
	idx_t capacity;
} tcc_string_list_t;

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

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
typedef struct {
	TCCState *tcc;
	int64_t (*fn)(int64_t);
	char *sql_name;
	char *symbol;
	uint64_t state_id;
} tcc_registered_artifact_t;
#endif

typedef struct {
	char *sql_name;
	char *symbol;
	int64_t (*fn)(int64_t);
	uint64_t state_id;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	tcc_registered_artifact_t *artifact;
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
	char *include_path;
	char *sysinclude_path;
	char *library_path;
	char *library;
	char *option;
	char *header;
	char *define_name;
	char *define_value;
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

static void tcc_set_error(tcc_error_buffer_t *error_buf, const char *message) {
	if (!error_buf || !message) {
		return;
	}
	snprintf(error_buf->message, sizeof(error_buf->message), "%s", message);
}

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
#ifdef DUCKTINYCC_DEFAULT_RUNTIME_PATH
	return DUCKTINYCC_DEFAULT_RUNTIME_PATH;
#else
	return "third_party/tinycc";
#endif
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

static int tcc_build_artifact(const char *runtime_path, tcc_module_state_t *state, const tcc_module_bind_data_t *bind,
                              const char *effective_symbol, const char *effective_sql_name,
                              tcc_registered_artifact_t **out_artifact, tcc_error_buffer_t *error_buf) {
	TCCState *s;
	void *sym;
	tcc_registered_artifact_t *artifact;
	if (!effective_symbol || effective_symbol[0] == '\0') {
		tcc_set_error(error_buf, "symbol is required");
		return -1;
	}
	if (!effective_sql_name || effective_sql_name[0] == '\0') {
		tcc_set_error(error_buf, "sql_name is required");
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
	if (tcc_apply_session_to_state(s, &state->session, error_buf) != 0) {
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
	sym = tcc_get_symbol(s, effective_symbol);
	if (!sym) {
		tcc_set_error(error_buf, "symbol not found after relocation");
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
	artifact->fn = (int64_t(*)(int64_t))sym;
	artifact->sql_name = tcc_strdup(effective_sql_name);
	artifact->symbol = tcc_strdup(effective_symbol);
	artifact->state_id = state->session.state_id;
	if (!artifact->sql_name || !artifact->symbol) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "out of memory");
		return -1;
	}
	*out_artifact = artifact;
	return 0;
}
#endif

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
	entry->fn = NULL;
	entry->state_id = 0;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	if (entry->artifact) {
		tcc_artifact_destroy(entry->artifact);
	}
	entry->artifact = NULL;
#endif
}

static bool tcc_registry_store_metadata(tcc_module_state_t *state, const char *sql_name, const char *symbol,
                                        int64_t (*fn)(int64_t), uint64_t state_id
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
	entry->fn = fn;
	entry->state_id = state_id;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	entry->artifact = artifact;
#endif
	if (!entry->sql_name || !entry->symbol || !entry->fn) {
		tcc_registry_entry_destroy_metadata(entry);
		return false;
	}
	return true;
}

static bool tcc_registry_remove_metadata(tcc_module_state_t *state, const char *sql_name) {
	idx_t idx = tcc_registry_find_sql_name(state, sql_name);
	if (idx == (idx_t)-1) {
		return false;
	}
	tcc_registry_entry_destroy_metadata(&state->entries[idx]);
	if (idx + 1 < state->entry_count) {
		memmove(&state->entries[idx], &state->entries[idx + 1],
		        sizeof(tcc_registered_entry_t) * (state->entry_count - idx - 1));
	}
	state->entry_count--;
	memset(&state->entries[state->entry_count], 0, sizeof(tcc_registered_entry_t));
	return true;
}

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
	if (state->session.runtime_path) {
		duckdb_free(state->session.runtime_path);
	}
	tcc_session_clear_build_state(&state->session);
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

static void destroy_tcc_module_init_data(void *ptr) {
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)ptr;
	if (!init) {
		return;
	}
	duckdb_free(init);
}

static void tcc_bind_read_named_varchar(duckdb_bind_info info, const char *name, char **out_value) {
	duckdb_value value = duckdb_bind_get_named_parameter(info, name);
	if (value && !duckdb_is_null_value(value)) {
		*out_value = duckdb_get_varchar(value);
	}
	if (value) {
		duckdb_destroy_value(&value);
	}
}

static void tcc_module_bind(duckdb_bind_info info) {
	tcc_module_bind_data_t *bind;
	tcc_module_state_t *state;
	const char *runtime_path;
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
	tcc_bind_read_named_varchar(info, "arg_bigint", &bind->arg_bigint);
	tcc_bind_read_named_varchar(info, "include_path", &bind->include_path);
	tcc_bind_read_named_varchar(info, "sysinclude_path", &bind->sysinclude_path);
	tcc_bind_read_named_varchar(info, "library_path", &bind->library_path);
	tcc_bind_read_named_varchar(info, "library", &bind->library);
	tcc_bind_read_named_varchar(info, "option", &bind->option);
	tcc_bind_read_named_varchar(info, "header", &bind->header);
	tcc_bind_read_named_varchar(info, "define_name", &bind->define_name);
	tcc_bind_read_named_varchar(info, "define_value", &bind->define_value);
	state = (tcc_module_state_t *)duckdb_bind_get_extra_info(info);
	runtime_path = state ? tcc_session_runtime_path(state, bind->runtime_path) : NULL;
	(void)runtime_path;

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

static const char *tcc_effective_symbol(tcc_module_state_t *state, tcc_module_bind_data_t *bind) {
	if (bind->symbol && bind->symbol[0] != '\0') {
		return bind->symbol;
	}
	if (state->session.bound_symbol && state->session.bound_symbol[0] != '\0') {
		return state->session.bound_symbol;
	}
	return NULL;
}

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
		              NULL, NULL, NULL, "connection");
	} else if (strcmp(bind->mode, "tcc_new_state") == 0 || strcmp(bind->mode, "new_state") == 0) {
		char detail[128];
		tcc_session_clear_build_state(&state->session);
		snprintf(detail, sizeof(detail), "state_id=%llu", (unsigned long long)state->session.state_id);
		tcc_write_row(output, true, bind->mode, "state", "OK", "new TinyCC build state prepared", detail, NULL,
		              NULL, NULL, "connection");
	} else if (strcmp(bind->mode, "add_include") == 0) {
		if (bind->include_path && tcc_string_list_append(&state->session.include_paths, bind->include_path)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "include path added", bind->include_path, NULL,
			              NULL, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "include_path is required", NULL,
			              NULL, NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_sysinclude") == 0) {
		if (bind->sysinclude_path && tcc_string_list_append(&state->session.sysinclude_paths, bind->sysinclude_path)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "sysinclude path added", bind->sysinclude_path,
			              NULL, NULL, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "sysinclude_path is required", NULL,
			              NULL, NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_library_path") == 0) {
		if (bind->library_path && tcc_string_list_append(&state->session.library_paths, bind->library_path)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "library path added", bind->library_path, NULL,
			              NULL, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "library_path is required", NULL,
			              NULL, NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_library") == 0) {
		if (bind->library && tcc_string_list_append(&state->session.libraries, bind->library)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "library added", bind->library, NULL, NULL, NULL,
			              "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "library is required", NULL, NULL,
			              NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_option") == 0) {
		if (bind->option && tcc_string_list_append(&state->session.options, bind->option)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "compiler option added", bind->option, NULL,
			              NULL, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "option is required", NULL, NULL,
			              NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_header") == 0) {
		if (bind->header && tcc_string_list_append(&state->session.headers, bind->header)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "header source added", "header appended", NULL,
			              NULL, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "header is required", NULL, NULL,
			              NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_source") == 0) {
		if (bind->source && tcc_string_list_append(&state->session.sources, bind->source)) {
			state->session.config_version++;
			tcc_write_row(output, true, bind->mode, "state", "OK", "source appended", "source appended", NULL,
			              NULL, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "source is required", NULL, NULL,
			              NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "add_define") == 0) {
		if (bind->define_name && bind->define_name[0] != '\0') {
			const char *define_value = bind->define_value ? bind->define_value : "1";
			if (tcc_string_list_append(&state->session.define_names, bind->define_name) &&
			    tcc_string_list_append(&state->session.define_values, define_value)) {
				state->session.config_version++;
				tcc_write_row(output, true, bind->mode, "state", "OK", "define added", bind->define_name, NULL, NULL,
				              NULL, "connection");
			} else {
				tcc_write_row(output, false, bind->mode, "state", "E_STORE_FAILED", "failed to store define", NULL,
				              NULL, NULL, NULL, "connection");
			}
		} else {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "define_name is required", NULL, NULL,
			              NULL, NULL, "connection");
		}
	} else if (strcmp(bind->mode, "tinycc_bind") == 0 || strcmp(bind->mode, "bind") == 0) {
		if (!bind->symbol || bind->symbol[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		} else {
			tcc_session_clear_bind(&state->session);
			state->session.bound_symbol = tcc_strdup(bind->symbol);
			state->session.bound_sql_name = tcc_strdup(bind->sql_name && bind->sql_name[0] != '\0' ? bind->sql_name : bind->symbol);
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
	} else if (strcmp(bind->mode, "compile") == 0 || strcmp(bind->mode, "tinycc_compile") == 0 ||
	           strcmp(bind->mode, "register") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
		tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
	              "TinyCC compile/register not supported for WASM build", NULL, bind->sql_name, bind->symbol, NULL,
	              "connection");
#else
		const char *effective_symbol = tcc_effective_symbol(state, bind);
		const char *effective_sql_name = tcc_effective_sql_name(state, bind, effective_symbol);
		tcc_error_buffer_t err;
		tcc_registered_artifact_t *artifact = NULL;
		char artifact_id[256];

		memset(&err, 0, sizeof(err));

		if (!effective_symbol || effective_symbol[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required (bind or argument)",
	              NULL, effective_sql_name, effective_symbol, NULL, "connection");
			init->emitted = true;
			return;
		}

		if (tcc_build_artifact(runtime_path, state, bind, effective_symbol, effective_sql_name, &artifact, &err) != 0) {
			tcc_write_row(output, false, bind->mode, "compile", "E_COMPILE_FAILED", "compile failed",
	              err.message[0] ? err.message : NULL, effective_sql_name, effective_symbol, NULL, "connection");
			init->emitted = true;
			return;
		}

		if (!tcc_registry_store_metadata(state, effective_sql_name, effective_symbol, artifact->fn, artifact->state_id, artifact)) {
			tcc_artifact_destroy(artifact);
			tcc_write_row(output, false, bind->mode, "register", "E_STORE_FAILED",
	                      "failed to store session metadata", NULL, effective_sql_name,
	                      effective_symbol, NULL, "connection");
			init->emitted = true;
			return;
		}
		snprintf(artifact_id, sizeof(artifact_id), "%s@state_%llu", effective_sql_name,
	         (unsigned long long)artifact->state_id);
		tcc_write_row(output, true, bind->mode, "register", "OK", "compiled, relocated and registered in session",
	              runtime_path, effective_sql_name, effective_symbol, artifact_id, "connection");
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
				const char *symbol = bind->symbol;
				const char *sql_name = bind->sql_name && bind->sql_name[0] != '\0' ? bind->sql_name : "__call_once";
				tcc_registered_artifact_t *artifact = NULL;
				tcc_error_buffer_t err;
				memset(&err, 0, sizeof(err));
				if (!symbol || symbol[0] == '\0') {
					tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required with source", NULL,
					              bind->sql_name, bind->symbol, NULL, "connection");
					init->emitted = true;
					return;
				}
				if (tcc_build_artifact(runtime_path, state, bind, symbol, sql_name, &artifact, &err) != 0) {
					tcc_write_row(output, false, bind->mode, "compile", "E_COMPILE_FAILED", "compile failed",
					              err.message[0] ? err.message : NULL, bind->sql_name, bind->symbol, NULL, "connection");
					init->emitted = true;
					return;
				}
				fn = artifact->fn;
				effective_symbol = artifact->symbol;
				result = fn(arg);
				snprintf(result_buf, sizeof(result_buf), "%lld", (long long)result);
				tcc_artifact_destroy(artifact);
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
			tcc_write_row(output, true, bind->mode, "execute", "OK", "executed", result_buf, bind->sql_name,
			              effective_symbol, NULL, "connection");
		}
#endif
	} else if (strcmp(bind->mode, "unregister") == 0) {
		if (!bind->sql_name || bind->sql_name[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "sql_name is required", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		} else if (tcc_registry_remove_metadata(state, bind->sql_name)) {
			tcc_write_row(output, true, bind->mode, "register", "OK",
			              "metadata removed from session (DuckDB function remains registered)", NULL, bind->sql_name,
			              bind->symbol, NULL, "connection");
		} else {
			tcc_write_row(output, false, bind->mode, "register", "E_NOT_FOUND", "no matching artifact metadata", NULL,
			              bind->sql_name, bind->symbol, NULL, "connection");
		}
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

	duckdb_register_table_function(connection, tf);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
}
