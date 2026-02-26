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
#include <ctype.h>

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

#define TCC_MAX_ARGS 10

typedef enum {
	TCC_RET_I64 = 0,
	TCC_RET_VOID = 1
} tcc_return_type_t;

typedef bool (*tcc_dynamic_init_fn_t)(duckdb_connection connection);
typedef bool (*tcc_runtime_invoke_fn_t)(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64);
typedef void (*tcc_runtime_destroy_fn_t)(void *user_data);

typedef struct {
	tcc_runtime_invoke_fn_t invoke;
	tcc_runtime_destroy_fn_t destroy;
	void *user_data;
} tcc_runtime_trampoline_t;

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
typedef struct {
	TCCState *tcc;
	void *symbol_ptr;
	int arg_count;
	tcc_return_type_t return_type;
	tcc_runtime_trampoline_t trampoline;
	bool is_module;
	tcc_dynamic_init_fn_t module_init;
	char *signature;
	char *sql_name;
	char *symbol;
	uint64_t state_id;
} tcc_registered_artifact_t;
#endif

typedef struct {
	char *sql_name;
	char *symbol;
	uint64_t state_id;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	tcc_registered_artifact_t *artifact;
#endif
} tcc_registered_entry_t;

typedef struct {
	duckdb_connection connection;
	duckdb_database database;
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
	char *arg_types;
	char *return_type;
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

static bool tcc_parse_explicit_types(const char *return_type, const char *arg_types, tcc_return_type_t *out_ret,
                                     int *out_arg_count, tcc_error_buffer_t *error_buf);
static void tcc_rt_destroy_noop(void *user_data);
static tcc_runtime_invoke_fn_t tcc_select_runtime_invoke(tcc_return_type_t ret_type, int argc);

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
typedef int64_t (*tcc_host_i64_unary_fn_t)(int64_t);

typedef struct {
	tcc_host_i64_unary_fn_t fn;
} tcc_host_i64_unary_ctx_t;

typedef struct {
	tcc_runtime_invoke_fn_t invoke;
	void *fn_ptr;
	int arg_count;
} tcc_host_i64_sig_ctx_t;

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

static void tcc_host_i64_unary_ctx_destroy(void *ptr) {
	if (ptr) {
		duckdb_free(ptr);
	}
}

static void tcc_host_i64_unary_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_host_i64_unary_ctx_t *ctx = (tcc_host_i64_unary_ctx_t *)duckdb_scalar_function_get_extra_info(info);
	idx_t n = duckdb_data_chunk_get_size(input);
	duckdb_vector in0;
	int64_t *in_data;
	int64_t *out_data;
	uint64_t *in_validity;
	uint64_t *out_validity;
	idx_t i;
	if (!ctx || !ctx->fn) {
		duckdb_scalar_function_set_error(info, "ducktinycc i64 unary ctx missing");
		return;
	}
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_data = (int64_t *)duckdb_vector_get_data(in0);
	out_data = (int64_t *)duckdb_vector_get_data(output);
	in_validity = duckdb_vector_get_validity(in0);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (i = 0; i < n; i++) {
		bool valid = (!in_validity) || duckdb_validity_row_is_valid(in_validity, i);
		duckdb_validity_set_row_validity(out_validity, i, valid);
		if (valid) {
			out_data[i] = ctx->fn(in_data[i]);
		}
	}
}

static void tcc_host_i64_sig_ctx_destroy(void *ptr) {
	if (ptr) {
		duckdb_free(ptr);
	}
}

static void tcc_host_i64_signature_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_host_i64_sig_ctx_t *ctx = (tcc_host_i64_sig_ctx_t *)duckdb_scalar_function_get_extra_info(info);
	idx_t n = duckdb_data_chunk_get_size(input);
	int64_t *out_data = (int64_t *)duckdb_vector_get_data(output);
	uint64_t *out_validity;
	int64_t *in_data[TCC_MAX_ARGS];
	uint64_t *in_validity[TCC_MAX_ARGS];
	int64_t args[TCC_MAX_ARGS];
	idx_t row;
	int col;
	if (!ctx || !ctx->invoke || !ctx->fn_ptr || ctx->arg_count < 0 || ctx->arg_count > TCC_MAX_ARGS) {
		duckdb_scalar_function_set_error(info, "ducktinycc i64 signature ctx missing");
		return;
	}

	for (col = 0; col < ctx->arg_count; col++) {
		duckdb_vector v = duckdb_data_chunk_get_vector(input, (idx_t)col);
		in_data[col] = (int64_t *)duckdb_vector_get_data(v);
		in_validity[col] = duckdb_vector_get_validity(v);
	}

	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);

	for (row = 0; row < n; row++) {
		bool valid = true;
		int64_t result = 0;
		for (col = 0; col < ctx->arg_count; col++) {
			if (in_validity[col] && !duckdb_validity_row_is_valid(in_validity[col], row)) {
				valid = false;
				break;
			}
			args[col] = in_data[col][row];
		}
		if (!valid) {
			duckdb_validity_set_row_validity(out_validity, row, false);
			continue;
		}
		if (!ctx->invoke(ctx->fn_ptr, args, &result)) {
			duckdb_scalar_function_set_error(info, "ducktinycc invoke failed");
			return;
		}
		duckdb_validity_set_row_validity(out_validity, row, true);
		out_data[row] = result;
	}
}

static bool ducktinycc_register_i64_signature(duckdb_connection con, const char *name, void *fn_ptr,
                                              const char *return_type, const char *arg_types_csv) {
	duckdb_logical_type bigint = NULL;
	duckdb_scalar_function fn = NULL;
	tcc_host_i64_sig_ctx_t *ctx = NULL;
	duckdb_state rc;
	tcc_return_type_t ret_type = TCC_RET_I64;
	int arg_count = 0;
	tcc_error_buffer_t err;
	int i;
	memset(&err, 0, sizeof(err));
	if (!con || !name || name[0] == '\0' || !fn_ptr) {
		return false;
	}
	if (!tcc_parse_explicit_types(return_type, arg_types_csv, &ret_type, &arg_count, &err)) {
		return false;
	}
	if (ret_type != TCC_RET_I64) {
		return false;
	}

	bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	fn = duckdb_create_scalar_function();
	if (!bigint || !fn) {
		if (fn) {
			duckdb_destroy_scalar_function(&fn);
		}
		if (bigint) {
			duckdb_destroy_logical_type(&bigint);
		}
		return false;
	}
	ctx = (tcc_host_i64_sig_ctx_t *)duckdb_malloc(sizeof(tcc_host_i64_sig_ctx_t));
	if (!ctx) {
		duckdb_destroy_scalar_function(&fn);
		duckdb_destroy_logical_type(&bigint);
		return false;
	}
	ctx->invoke = tcc_select_runtime_invoke(ret_type, arg_count);
	ctx->fn_ptr = fn_ptr;
	ctx->arg_count = arg_count;
	if (!ctx->invoke) {
		tcc_host_i64_sig_ctx_destroy(ctx);
		duckdb_destroy_scalar_function(&fn);
		duckdb_destroy_logical_type(&bigint);
		return false;
	}

	duckdb_scalar_function_set_name(fn, name);
	for (i = 0; i < arg_count; i++) {
		duckdb_scalar_function_add_parameter(fn, bigint);
	}
	duckdb_scalar_function_set_return_type(fn, bigint);
	duckdb_scalar_function_set_function(fn, tcc_host_i64_signature_scalar);
	duckdb_scalar_function_set_extra_info(fn, ctx, tcc_host_i64_sig_ctx_destroy);
	rc = duckdb_register_scalar_function(con, fn);
	if (rc != DuckDBSuccess) {
		duckdb_destroy_scalar_function(&fn);
		duckdb_destroy_logical_type(&bigint);
		return false;
	}
	duckdb_destroy_logical_type(&bigint);
	return true;
}

static bool ducktinycc_register_i64_unary(duckdb_connection con, const char *name, void *fn_ptr) {
	duckdb_logical_type bigint = NULL;
	duckdb_scalar_function fn = NULL;
	tcc_host_i64_unary_ctx_t *ctx = NULL;
	duckdb_state rc;
	if (!con || !name || name[0] == '\0' || !fn_ptr) {
		return false;
	}
	bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	fn = duckdb_create_scalar_function();
	if (!bigint || !fn) {
		if (fn) {
			duckdb_destroy_scalar_function(&fn);
		}
		if (bigint) {
			duckdb_destroy_logical_type(&bigint);
		}
		return false;
	}
	ctx = (tcc_host_i64_unary_ctx_t *)duckdb_malloc(sizeof(tcc_host_i64_unary_ctx_t));
	if (!ctx) {
		duckdb_destroy_scalar_function(&fn);
		duckdb_destroy_logical_type(&bigint);
		return false;
	}
	ctx->fn = (tcc_host_i64_unary_fn_t)fn_ptr;

	duckdb_scalar_function_set_name(fn, name);
	duckdb_scalar_function_add_parameter(fn, bigint);
	duckdb_scalar_function_set_return_type(fn, bigint);
	duckdb_scalar_function_set_function(fn, tcc_host_i64_unary_scalar);
	duckdb_scalar_function_set_extra_info(fn, ctx, tcc_host_i64_unary_ctx_destroy);
	rc = duckdb_register_scalar_function(con, fn);
	if (rc != DuckDBSuccess) {
		duckdb_destroy_scalar_function(&fn);
		duckdb_destroy_logical_type(&bigint);
		return false;
	}
	duckdb_destroy_logical_type(&bigint);
	return true;
}

static void tcc_add_host_symbols(TCCState *s) {
	if (!s) {
		return;
	}
	(void)tcc_add_symbol(s, "duckdb_ext_api", &duckdb_ext_api);
	(void)tcc_add_symbol(s, "ducktinycc_register_i64_unary", ducktinycc_register_i64_unary);
	(void)tcc_add_symbol(s, "ducktinycc_register_i64_signature", ducktinycc_register_i64_signature);
}

static void tcc_artifact_destroy(void *ptr) {
	tcc_registered_artifact_t *artifact = (tcc_registered_artifact_t *)ptr;
	if (!artifact) {
		return;
	}
	if (artifact->trampoline.destroy) {
		artifact->trampoline.destroy(artifact->trampoline.user_data);
	}
	if (artifact->tcc) {
		tcc_delete(artifact->tcc);
	}
	if (artifact->signature) {
		duckdb_free(artifact->signature);
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
	artifact->symbol_ptr = sym;
	artifact->arg_count = 0;
	artifact->return_type = TCC_RET_I64;
	artifact->trampoline.invoke = NULL;
	artifact->trampoline.destroy = tcc_rt_destroy_noop;
	artifact->trampoline.user_data = NULL;
	artifact->is_module = true;
	artifact->module_init = (tcc_dynamic_init_fn_t)sym;
	artifact->signature = tcc_strdup("module_init(duckdb_connection)->bool");
	artifact->sql_name = tcc_strdup(module_name);
	artifact->symbol = tcc_strdup(module_symbol);
	artifact->state_id = state->session.state_id;
	if (!artifact->module_init || !artifact->signature || !artifact->sql_name || !artifact->symbol) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "invalid module artifact or out of memory");
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
	entry->state_id = 0;
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	if (entry->artifact) {
		tcc_artifact_destroy(entry->artifact);
	}
	entry->artifact = NULL;
#endif
}

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
	if (bind->arg_types) {
		duckdb_free(bind->arg_types);
	}
	if (bind->return_type) {
		duckdb_free(bind->return_type);
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
	tcc_bind_read_named_arg_types(info, &bind->arg_types);
	tcc_bind_read_named_varchar(info, "return_type", &bind->return_type);
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

static const char *tcc_skip_space(const char *p) {
	while (p && *p && isspace((unsigned char)*p)) {
		p++;
	}
	return p;
}

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

static bool tcc_parse_ret_type_token(const char *token, tcc_return_type_t *out_ret) {
	if (tcc_equals_ci(token, "i64") || tcc_equals_ci(token, "bigint") || tcc_equals_ci(token, "longlong")) {
		*out_ret = TCC_RET_I64;
		return true;
	}
	if (tcc_equals_ci(token, "void")) {
		*out_ret = TCC_RET_VOID;
		return true;
	}
	return false;
}

static bool tcc_parse_arg_type_token(const char *token) {
	return tcc_equals_ci(token, "i64") || tcc_equals_ci(token, "bigint") || tcc_equals_ci(token, "longlong");
}

static bool tcc_parse_explicit_types(const char *return_type, const char *arg_types, tcc_return_type_t *out_ret,
                                     int *out_arg_count, tcc_error_buffer_t *error_buf) {
	char *args_copy = NULL;
	char *cur = NULL;
	int argc = 0;
	const char *ret_text;
	tcc_return_type_t ret;
	if (!return_type || return_type[0] == '\0') {
		tcc_set_error(error_buf, "return_type is required");
		return false;
	}
	if (!arg_types) {
		tcc_set_error(error_buf, "arg_types is required (use [] for no args)");
		return false;
	}
	ret_text = return_type;
	if (!tcc_parse_ret_type_token(ret_text, &ret)) {
		tcc_set_error(error_buf, "return_type must be i64/bigint/void");
		return false;
	}
	if (arg_types[0] == '\0') {
		*out_ret = ret;
		*out_arg_count = 0;
		return true;
	}
	args_copy = tcc_strdup(arg_types);
	if (!args_copy) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	cur = args_copy;
	while (cur && *cur) {
		char *token = cur;
		char *next = strchr(token, ',');
		size_t len;
		while (*token && isspace((unsigned char)*token)) {
			token++;
		}
		if (next) {
			*next = '\0';
			cur = next + 1;
		} else {
			cur = NULL;
		}
		len = strlen(token);
		while (len > 0 && isspace((unsigned char)token[len - 1])) {
			token[--len] = '\0';
		}
		if (len == 0) {
			duckdb_free(args_copy);
			tcc_set_error(error_buf, "arg_types contains empty token");
			return false;
		}
		if (!tcc_parse_arg_type_token(token)) {
			duckdb_free(args_copy);
			tcc_set_error(error_buf, "arg_types currently supports only i64/bigint");
			return false;
		}
		argc++;
		if (argc > TCC_MAX_ARGS) {
			duckdb_free(args_copy);
			tcc_set_error(error_buf, "too many args (max 10)");
			return false;
		}
	}
	duckdb_free(args_copy);
	*out_ret = ret;
	*out_arg_count = argc;
	return true;
}

static bool tcc_rt_invoke_i64_0(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(void);
	fn_t fn = (fn_t)user_data;
	(void)args;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn();
	return true;
}

static bool tcc_rt_invoke_i64_1(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0]);
	return true;
}

static bool tcc_rt_invoke_i64_2(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1]);
	return true;
}

static bool tcc_rt_invoke_i64_3(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2]);
	return true;
}

static bool tcc_rt_invoke_i64_4(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3]);
	return true;
}

static bool tcc_rt_invoke_i64_5(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3], args[4]);
	return true;
}

static bool tcc_rt_invoke_i64_6(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3], args[4], args[5]);
	return true;
}

static bool tcc_rt_invoke_i64_7(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
	return true;
}

static bool tcc_rt_invoke_i64_8(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
	return true;
}

static bool tcc_rt_invoke_i64_9(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
	return true;
}

static bool tcc_rt_invoke_i64_10(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef int64_t (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	if (!fn || !out_i64) {
		return false;
	}
	*out_i64 = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
	return true;
}

static bool tcc_rt_invoke_void_0(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(void);
	fn_t fn = (fn_t)user_data;
	(void)args;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn();
	return true;
}

static bool tcc_rt_invoke_void_1(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0]);
	return true;
}

static bool tcc_rt_invoke_void_2(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1]);
	return true;
}

static bool tcc_rt_invoke_void_3(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2]);
	return true;
}

static bool tcc_rt_invoke_void_4(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3]);
	return true;
}

static bool tcc_rt_invoke_void_5(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3], args[4]);
	return true;
}

static bool tcc_rt_invoke_void_6(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3], args[4], args[5]);
	return true;
}

static bool tcc_rt_invoke_void_7(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
	return true;
}

static bool tcc_rt_invoke_void_8(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
	return true;
}

static bool tcc_rt_invoke_void_9(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
	return true;
}

static bool tcc_rt_invoke_void_10(void *user_data, const int64_t args[TCC_MAX_ARGS], int64_t *out_i64) {
	typedef void (*fn_t)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
	fn_t fn = (fn_t)user_data;
	(void)out_i64;
	if (!fn) {
		return false;
	}
	fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
	return true;
}

static void tcc_rt_destroy_noop(void *user_data) {
	(void)user_data;
}

static tcc_runtime_invoke_fn_t tcc_select_runtime_invoke(tcc_return_type_t ret_type, int argc) {
	if (argc < 0 || argc > TCC_MAX_ARGS) {
		return NULL;
	}
	switch (ret_type) {
	case TCC_RET_I64:
		switch (argc) {
		case 0:
			return tcc_rt_invoke_i64_0;
		case 1:
			return tcc_rt_invoke_i64_1;
		case 2:
			return tcc_rt_invoke_i64_2;
		case 3:
			return tcc_rt_invoke_i64_3;
		case 4:
			return tcc_rt_invoke_i64_4;
		case 5:
			return tcc_rt_invoke_i64_5;
		case 6:
			return tcc_rt_invoke_i64_6;
		case 7:
			return tcc_rt_invoke_i64_7;
		case 8:
			return tcc_rt_invoke_i64_8;
		case 9:
			return tcc_rt_invoke_i64_9;
		case 10:
			return tcc_rt_invoke_i64_10;
		default:
			return NULL;
		}
	case TCC_RET_VOID:
		switch (argc) {
		case 0:
			return tcc_rt_invoke_void_0;
		case 1:
			return tcc_rt_invoke_void_1;
		case 2:
			return tcc_rt_invoke_void_2;
		case 3:
			return tcc_rt_invoke_void_3;
		case 4:
			return tcc_rt_invoke_void_4;
		case 5:
			return tcc_rt_invoke_void_5;
		case 6:
			return tcc_rt_invoke_void_6;
		case 7:
			return tcc_rt_invoke_void_7;
		case 8:
			return tcc_rt_invoke_void_8;
		case 9:
			return tcc_rt_invoke_void_9;
		case 10:
			return tcc_rt_invoke_void_10;
		default:
			return NULL;
		}
	default:
		return NULL;
	}
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

static char *tcc_generate_ffi_loader_source(const char *module_symbol, const char *target_symbol, const char *sql_name,
                                            const char *return_type, const char *arg_types_csv, int arg_count) {
	char args_decl[512];
	char src[4096];
	size_t off = 0;
	int i;
	int n;
	if (!module_symbol || !target_symbol || !sql_name || !return_type || !arg_types_csv || arg_count < 0 ||
	    arg_count > TCC_MAX_ARGS) {
		return NULL;
	}
	off = 0;
	for (i = 0; i < arg_count; i++) {
		n = snprintf(args_decl + off, sizeof(args_decl) - off, "%slong long a%d", i == 0 ? "" : ", ", i);
		if (n < 0 || (size_t)n >= sizeof(args_decl) - off) {
			return NULL;
		}
		off += (size_t)n;
	}
	if (arg_count == 0) {
		n = snprintf(args_decl, sizeof(args_decl), "void");
		if (n < 0 || (size_t)n >= sizeof(args_decl)) {
			return NULL;
		}
	}

	n = snprintf(
	    src, sizeof(src),
	    "typedef struct _duckdb_connection *duckdb_connection;\n"
	    "extern _Bool ducktinycc_register_i64_signature(duckdb_connection con, const char *name, void *fn_ptr, const char *return_type, const char *arg_types_csv);\n"
	    "extern long long %s(%s);\n"
	    "_Bool %s(duckdb_connection con) {\n"
	    "  return ducktinycc_register_i64_signature(con, \"%s\", (void*)%s, \"%s\", \"%s\");\n"
	    "}\n",
	    target_symbol, args_decl, module_symbol, sql_name, target_symbol, return_type, arg_types_csv);
	if (n < 0 || (size_t)n >= sizeof(src)) {
		return NULL;
	}
	return tcc_strdup(src);
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
static int tcc_compile_and_load_ffi_module(const char *runtime_path, tcc_module_state_t *state,
                                           const tcc_module_bind_data_t *bind, const char *sql_name,
                                           const char *target_symbol, tcc_registered_artifact_t **out_artifact,
                                           tcc_error_buffer_t *error_buf, char *out_module_symbol, size_t symbol_len) {
	tcc_return_type_t ret_type = TCC_RET_I64;
	int arg_count = 0;
	char *loader_src = NULL;
	char *combined_src = NULL;
	tcc_module_bind_data_t bind_copy;
	tcc_registered_artifact_t *artifact = NULL;

	if (!state || !bind || !sql_name || !target_symbol || !out_artifact || !error_buf || !out_module_symbol ||
	    symbol_len == 0) {
		tcc_set_error(error_buf, "invalid ffi_load arguments");
		return -1;
	}
	if (!state->connection) {
		tcc_set_error(error_buf, "no persistent extension connection available");
		return -1;
	}
	if (!tcc_parse_explicit_types(bind->return_type, bind->arg_types, &ret_type, &arg_count, error_buf)) {
		return -1;
	}
	if (ret_type != TCC_RET_I64) {
		tcc_set_error(error_buf, "ffi loader currently supports only i64 return type");
		return -1;
	}

	snprintf(out_module_symbol, symbol_len, "__ducktinycc_ffi_init_%llu_%llu", (unsigned long long)state->session.state_id,
	         (unsigned long long)state->session.config_version);
	loader_src = tcc_generate_ffi_loader_source(out_module_symbol, target_symbol, sql_name,
	                                            bind->return_type ? bind->return_type : "i64",
	                                            bind->arg_types ? bind->arg_types : "", arg_count);
	if (!loader_src) {
		tcc_set_error(error_buf, "failed to generate ffi loader");
		return -1;
	}

	if (bind->source && bind->source[0] != '\0') {
		size_t n1 = strlen(bind->source);
		size_t n2 = strlen(loader_src);
		combined_src = (char *)duckdb_malloc(n1 + n2 + 3);
		if (!combined_src) {
			duckdb_free(loader_src);
			tcc_set_error(error_buf, "out of memory");
			return -1;
		}
		memcpy(combined_src, bind->source, n1);
		combined_src[n1] = '\n';
		memcpy(combined_src + n1 + 1, loader_src, n2);
		combined_src[n1 + 1 + n2] = '\0';
	} else {
		combined_src = loader_src;
		loader_src = NULL;
	}

	memset(&bind_copy, 0, sizeof(bind_copy));
	bind_copy = *bind;
	bind_copy.source = combined_src;
	if (tcc_build_module_artifact(runtime_path, state, &bind_copy, out_module_symbol, sql_name, &artifact, error_buf) !=
	    0) {
		if (loader_src) {
			duckdb_free(loader_src);
		}
		duckdb_free(combined_src);
		return -1;
	}
	if (loader_src) {
		duckdb_free(loader_src);
	}
	duckdb_free(combined_src);

	if (!artifact->module_init(state->connection)) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "ffi loader init returned false");
		return -1;
	}
	*out_artifact = artifact;
	return 0;
}
#endif

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
		           strcmp(bind->mode, "register") == 0 || strcmp(bind->mode, "ffi_load") == 0 ||
		           strcmp(bind->mode, "tinycc_ffi_load") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
			tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
			              "TinyCC compile/register/ffi_load not supported for WASM build", NULL, bind->sql_name,
			              bind->symbol, NULL, "connection");
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

			if (!target_symbol || target_symbol[0] == '\0') {
				tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required (bind or argument)",
				              NULL, sql_name, target_symbol, NULL, "connection");
				init->emitted = true;
				return;
			}

			if (tcc_compile_and_load_ffi_module(runtime_path, state, bind, sql_name, target_symbol, &artifact, &err,
			                                    module_symbol, sizeof(module_symbol)) != 0) {
				if (strstr(err.message, "return_type") || strstr(err.message, "arg_types") ||
				    strstr(err.message, "too many args")) {
					phase = "bind";
					code = "E_BAD_SIGNATURE";
					message = "invalid return_type/arg_types";
				} else if (strstr(err.message, "only i64 return type")) {
					phase = "bind";
					code = "E_UNSUPPORTED_SIGNATURE";
					message = "unsupported return_type/arg_types";
				} else if (strstr(err.message, "failed to generate ffi loader") || strstr(err.message, "out of memory")) {
					phase = "codegen";
					code = "E_CODEGEN_FAILED";
					message = "ffi codegen failed";
				} else if (strstr(err.message, "no persistent extension connection")) {
					phase = "load";
					code = "E_NO_CONNECTION";
					message = "no persistent extension connection available";
				} else if (strstr(err.message, "ffi loader init returned false")) {
					phase = "load";
					code = "E_INIT_FAILED";
					message = "ffi loader init returned false";
				}
				tcc_write_row(output, false, bind->mode, phase, code, message, err.message[0] ? err.message : NULL, sql_name,
				              target_symbol, NULL, "connection");
				init->emitted = true;
				return;
			}
			if (!tcc_registry_store_metadata(state, sql_name, module_symbol, artifact->state_id, artifact)) {
				tcc_artifact_destroy(artifact);
				tcc_write_row(output, false, bind->mode, "register", "E_STORE_FAILED",
				              "failed to store ffi module artifact metadata", NULL, sql_name, target_symbol, NULL,
				              "connection");
				init->emitted = true;
				return;
			}
			snprintf(artifact_id, sizeof(artifact_id), "%s@ffi_state_%llu", sql_name,
			         (unsigned long long)artifact->state_id);
			tcc_write_row(output, true, bind->mode, "load", "OK", "compiled and registered SQL function via codegen",
			              runtime_path, sql_name, target_symbol, artifact_id, "connection");
#endif
		} else if (strcmp(bind->mode, "load") == 0 || strcmp(bind->mode, "tinycc_load") == 0) {
#ifdef DUCKTINYCC_WASM_UNSUPPORTED
			tcc_write_row(output, false, bind->mode, "runtime", "E_PLATFORM_WASM_UNSUPPORTED",
			              "TinyCC load not supported for WASM build", NULL, bind->sql_name, bind->symbol, NULL,
			              "connection");
#else
				const char *module_symbol = bind->symbol && bind->symbol[0] != '\0' ? bind->symbol : "ducktinycc_init";
				const char *module_name = bind->sql_name && bind->sql_name[0] != '\0' ? bind->sql_name : module_symbol;
				tcc_error_buffer_t err;
				tcc_registered_artifact_t *artifact = NULL;
				char artifact_id[256];
				bool ok;
				memset(&err, 0, sizeof(err));

				if (!state->connection) {
					tcc_write_row(output, false, bind->mode, "load", "E_NO_CONNECTION",
					              "no persistent extension connection available", NULL, module_name, module_symbol, NULL,
					              "connection");
					init->emitted = true;
					return;
				}

			if (tcc_build_module_artifact(runtime_path, state, bind, module_symbol, module_name, &artifact, &err) != 0) {
				tcc_write_row(output, false, bind->mode, "compile", "E_COMPILE_FAILED", "compile failed",
				              err.message[0] ? err.message : NULL, module_name, module_symbol, NULL, "connection");
				init->emitted = true;
				return;
			}

				ok = artifact->module_init(state->connection);
				if (!ok) {
					tcc_artifact_destroy(artifact);
					tcc_write_row(output, false, bind->mode, "load", "E_INIT_FAILED",
				              "dynamic module init returned false", NULL, module_name, module_symbol, NULL,
				              "connection");
				init->emitted = true;
				return;
			}

			if (!tcc_registry_store_metadata(state, module_name, module_symbol, artifact->state_id, artifact)) {
				tcc_artifact_destroy(artifact);
				tcc_write_row(output, false, bind->mode, "register", "E_STORE_FAILED",
				              "failed to store module artifact metadata", NULL, module_name, module_symbol, NULL,
				              "connection");
				init->emitted = true;
				return;
			}

			snprintf(artifact_id, sizeof(artifact_id), "%s@module_state_%llu", module_name,
			         (unsigned long long)artifact->state_id);
			tcc_write_row(output, true, bind->mode, "load", "OK", "compiled and initialized dynamic module", runtime_path,
			              module_name, module_symbol, artifact_id, "connection");
#endif
		} else if (strcmp(bind->mode, "unregister") == 0) {
			if (!bind->sql_name || bind->sql_name[0] == '\0') {
				tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "sql_name is required", NULL,
				              bind->sql_name, bind->symbol, NULL, "connection");
			} else {
				idx_t idx = tcc_registry_find_sql_name(state, bind->sql_name);
				if (idx == (idx_t)-1) {
					tcc_write_row(output, false, bind->mode, "register", "E_NOT_FOUND", "no matching artifact metadata", NULL,
					              bind->sql_name, bind->symbol, NULL, "connection");
					init->emitted = true;
					return;
				}
	#ifndef DUCKTINYCC_WASM_UNSUPPORTED
				if (state->entries[idx].artifact && state->entries[idx].artifact->is_module) {
					tcc_write_row(output, false, bind->mode, "register", "E_UNSAFE_UNLOAD",
					              "cannot unregister loaded dynamic module while SQL functions may still reference it", NULL,
					              bind->sql_name, bind->symbol, NULL, "connection");
					init->emitted = true;
					return;
				}
	#endif
				if (tcc_registry_remove_metadata(state, bind->sql_name)) {
				tcc_write_row(output, true, bind->mode, "register", "OK",
				              "metadata removed from session (DuckDB function remains registered)", NULL, bind->sql_name,
				              bind->symbol, NULL, "connection");
				} else {
					tcc_write_row(output, false, bind->mode, "register", "E_NOT_FOUND", "no matching artifact metadata", NULL,
					              bind->sql_name, bind->symbol, NULL, "connection");
				}
			}
		} else {
		tcc_write_row(output, false, bind->mode, "bind", "E_BAD_MODE", "unknown mode", NULL, NULL, NULL, NULL,
		              "connection");
	}

	init->emitted = true;
}

void RegisterTccModuleFunction(duckdb_connection connection, duckdb_database database) {
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type list_varchar_type = duckdb_create_list_type(varchar_type);
	tcc_module_state_t *state;

	state = (tcc_module_state_t *)duckdb_malloc(sizeof(tcc_module_state_t));
	if (!state) {
		duckdb_destroy_logical_type(&varchar_type);
		duckdb_destroy_table_function(&tf);
		return;
	}
	memset(state, 0, sizeof(tcc_module_state_t));
	state->connection = connection;
	state->database = database;

	duckdb_table_function_set_name(tf, "tcc_module");
	duckdb_table_function_add_named_parameter(tf, "mode", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "runtime_path", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "source", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "symbol", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "sql_name", varchar_type);
	duckdb_table_function_add_named_parameter(tf, "arg_types", list_varchar_type);
	duckdb_table_function_add_named_parameter(tf, "return_type", varchar_type);
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

	duckdb_destroy_logical_type(&list_varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
}
