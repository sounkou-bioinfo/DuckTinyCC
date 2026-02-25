#include "duckdb_extension.h"
#include "tcc_module.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

typedef struct {
	char *runtime_path;
	uint64_t config_version;
} tcc_session_t;

typedef struct {
	duckdb_connection connection;
	tcc_session_t session;
} tcc_module_state_t;

typedef struct {
	char *mode;
	char *runtime_path;
} tcc_module_bind_data_t;

typedef struct {
	bool emitted;
} tcc_module_init_data_t;

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

static void destroy_tcc_module_state(void *ptr) {
	tcc_module_state_t *state = (tcc_module_state_t *)ptr;
	if (!state) {
		return;
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
	duckdb_value mode_value;
	duckdb_value runtime_path_value;
	duckdb_logical_type varchar_type;
	duckdb_logical_type bool_type;

	bind = (tcc_module_bind_data_t *)duckdb_malloc(sizeof(tcc_module_bind_data_t));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(tcc_module_bind_data_t));

	mode_value = duckdb_bind_get_named_parameter(info, "mode");
	if (mode_value && !duckdb_is_null_value(mode_value)) {
		bind->mode = duckdb_get_varchar(mode_value);
	}
	if (mode_value) {
		duckdb_destroy_value(&mode_value);
	}
	if (!bind->mode || bind->mode[0] == '\0') {
		bind->mode = tcc_strdup("config_get");
	}

	runtime_path_value = duckdb_bind_get_named_parameter(info, "runtime_path");
	if (runtime_path_value && !duckdb_is_null_value(runtime_path_value)) {
		bind->runtime_path = duckdb_get_varchar(runtime_path_value);
	}
	if (runtime_path_value) {
		duckdb_destroy_value(&runtime_path_value);
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

static void tcc_module_function(duckdb_function_info info, duckdb_data_chunk output) {
	tcc_module_state_t *state = (tcc_module_state_t *)duckdb_function_get_extra_info(info);
	tcc_module_bind_data_t *bind = (tcc_module_bind_data_t *)duckdb_function_get_bind_data(info);
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)duckdb_function_get_init_data(info);

	if (!state || !bind || !init || init->emitted) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}

	if (strcmp(bind->mode, "config_set") == 0) {
		tcc_session_set_runtime_path(&state->session, bind->runtime_path);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration updated",
		              state->session.runtime_path ? state->session.runtime_path : "(empty)", NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "config_get") == 0) {
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration",
		              state->session.runtime_path ? state->session.runtime_path : "(unset)", NULL, NULL, NULL,
		              "connection");
	} else if (strcmp(bind->mode, "config_reset") == 0) {
		tcc_session_set_runtime_path(&state->session, NULL);
		tcc_write_row(output, true, bind->mode, "config", "OK", "session configuration reset", "(unset)", NULL, NULL,
		              NULL, "connection");
	} else if (strcmp(bind->mode, "list") == 0) {
		tcc_write_row(output, true, bind->mode, "registry", "OK", "no registered tcc_* artifacts yet", NULL, NULL,
		              NULL, NULL, "connection");
	} else if (strcmp(bind->mode, "compile") == 0 || strcmp(bind->mode, "register") == 0 ||
	           strcmp(bind->mode, "unregister") == 0) {
		tcc_write_row(output, false, bind->mode, "runtime", "E_NOT_IMPLEMENTED",
		              "tinycc engine not wired yet in this revision",
		              "next step: vendor/build tinycc and implement compile+relocate+register flow", NULL, NULL, NULL,
		              "connection");
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

	duckdb_table_function_set_extra_info(tf, state, destroy_tcc_module_state);
	duckdb_table_function_set_bind(tf, tcc_module_bind);
	duckdb_table_function_set_init(tf, tcc_module_init);
	duckdb_table_function_set_function(tf, tcc_module_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);

	duckdb_register_table_function(connection, tf);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
}
