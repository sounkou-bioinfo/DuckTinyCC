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

typedef struct {
	char **items;
	idx_t count;
	idx_t capacity;
} tcc_string_list_t;

typedef struct {
	atomic_bool writer;
	atomic_uint readers;
	atomic_uint pending_writers;
} tcc_rwlock_t;

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

typedef enum {
	TCC_WRAPPER_MODE_ROW = 0,
	TCC_WRAPPER_MODE_BATCH = 1
} tcc_wrapper_mode_t;

typedef bool (*tcc_dynamic_init_fn_t)(duckdb_connection connection);
typedef bool (*tcc_host_row_wrapper_fn_t)(void **args, void *out_value, bool *out_is_null);
typedef bool (*tcc_host_batch_wrapper_fn_t)(void **arg_data, uint64_t **arg_validity, uint64_t count, void *out_data,
                                            uint64_t *out_validity);

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
typedef struct {
	TCCState *tcc;
	bool is_module;
	tcc_dynamic_init_fn_t module_init;
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
	tcc_rwlock_t lock;
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

typedef struct {
	atomic_bool emitted;
} tcc_module_init_data_t;

typedef struct {
	char message[4096];
} tcc_error_buffer_t;

typedef struct {
	int field_count;
	char **field_names;
	tcc_ffi_type_t *field_types;
	size_t *field_sizes;
} tcc_ffi_struct_meta_t;

typedef struct {
	tcc_ffi_type_t key_type;
	tcc_ffi_type_t value_type;
	size_t key_size;
	size_t value_size;
} tcc_ffi_map_meta_t;

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
	tcc_ffi_struct_meta_t *arg_struct_metas;
	tcc_ffi_map_meta_t *arg_map_metas;
} tcc_host_sig_ctx_t;

typedef struct {
	char *kind;
	char *key;
	char *value;
	bool exists;
	char *detail;
} tcc_diag_row_t;

typedef struct {
	tcc_diag_row_t *rows;
	idx_t count;
	idx_t capacity;
} tcc_diag_rows_t;

typedef struct {
	tcc_diag_rows_t rows;
} tcc_diag_bind_data_t;

typedef struct {
	atomic_uint_fast64_t offset;
} tcc_diag_init_data_t;

static bool tcc_parse_signature(const char *return_type, const char *arg_types_csv, tcc_ffi_type_t *out_return_type,
	                                size_t *out_return_array_size, tcc_ffi_type_t **out_arg_types,
	                                size_t **out_arg_array_sizes, tcc_ffi_struct_meta_t *out_return_struct_meta,
	                                tcc_ffi_map_meta_t *out_return_map_meta,
	                                tcc_ffi_struct_meta_t **out_arg_struct_metas,
	                                tcc_ffi_map_meta_t **out_arg_map_metas, int *out_arg_count,
	                                tcc_error_buffer_t *error_buf);
static bool tcc_equals_ci(const char *a, const char *b);
static bool tcc_parse_wrapper_mode(const char *wrapper_mode, tcc_wrapper_mode_t *out_mode,
                                   tcc_error_buffer_t *error_buf);
static duckdb_logical_type tcc_ffi_type_create_logical_type(tcc_ffi_type_t type, size_t array_size,
                                                             const tcc_ffi_struct_meta_t *struct_meta,
                                                             const tcc_ffi_map_meta_t *map_meta);
static bool tcc_ffi_type_is_list(tcc_ffi_type_t type);
static bool tcc_ffi_list_child_type(tcc_ffi_type_t list_type, tcc_ffi_type_t *out_child);
static bool tcc_ffi_list_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_list_type);
static bool tcc_ffi_type_is_array(tcc_ffi_type_t type);
static bool tcc_ffi_array_child_type(tcc_ffi_type_t array_type, tcc_ffi_type_t *out_child);
static bool tcc_ffi_array_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_array_type);
static bool tcc_ffi_type_is_struct(tcc_ffi_type_t type);
static bool tcc_ffi_type_is_map(tcc_ffi_type_t type);
static size_t tcc_ffi_type_size(tcc_ffi_type_t type);
static bool tcc_ffi_type_is_fixed_width_scalar(tcc_ffi_type_t type);
static void tcc_struct_meta_destroy(tcc_ffi_struct_meta_t *meta);
static void tcc_struct_meta_array_destroy(tcc_ffi_struct_meta_t *metas, int count);

static void tcc_rwlock_init(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	atomic_store_explicit(&lock->writer, false, memory_order_relaxed);
	atomic_store_explicit(&lock->readers, 0, memory_order_relaxed);
	atomic_store_explicit(&lock->pending_writers, 0, memory_order_relaxed);
}

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

static void tcc_rwlock_read_unlock(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	atomic_fetch_sub_explicit(&lock->readers, 1, memory_order_release);
}

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

static void tcc_rwlock_write_unlock(tcc_rwlock_t *lock) {
	if (!lock) {
		return;
	}
	atomic_store_explicit(&lock->writer, false, memory_order_release);
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

static const char *tcc_default_runtime_path(void) {
#ifdef DUCKTINYCC_DEFAULT_RUNTIME_PATH
	return DUCKTINYCC_DEFAULT_RUNTIME_PATH;
#else
	return "third_party/tinycc";
#endif
}

static bool tcc_path_exists(const char *path) {
	return path && path[0] != '\0' && TCC_ACCESS(path, TCC_ACCESS_FOK) == 0;
}

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

static bool tcc_string_list_append_unique(tcc_string_list_t *list, const char *value) {
	if (!list || !value || value[0] == '\0') {
		return false;
	}
	if (tcc_string_list_contains(list, value)) {
		return true;
	}
	return tcc_string_list_append(list, value);
}

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
	return tcc_default_runtime_path();
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
	if (ctx->arg_map_metas) {
		duckdb_free(ctx->arg_map_metas);
	}
	tcc_struct_meta_destroy(&ctx->return_struct_meta);
	duckdb_free(ctx);
}

static bool tcc_ffi_type_is_list(tcc_ffi_type_t type) {
	switch (type) {
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
		return true;
	default:
		return false;
	}
}

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
	if (meta->field_types) {
		duckdb_free(meta->field_types);
	}
	if (meta->field_sizes) {
		duckdb_free(meta->field_sizes);
	}
	memset(meta, 0, sizeof(*meta));
}

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

static bool tcc_ffi_type_is_array(tcc_ffi_type_t type) {
	switch (type) {
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

static bool tcc_ffi_type_is_struct(tcc_ffi_type_t type) {
	return type == TCC_FFI_STRUCT;
}

static bool tcc_ffi_type_is_map(tcc_ffi_type_t type) {
	return type == TCC_FFI_MAP;
}

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

static duckdb_logical_type tcc_ffi_type_create_logical_type(tcc_ffi_type_t type, size_t array_size,
                                                             const tcc_ffi_struct_meta_t *struct_meta,
                                                             const tcc_ffi_map_meta_t *map_meta) {
	duckdb_type base_type;
	if (tcc_ffi_type_is_list(type)) {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		duckdb_logical_type child_logical = NULL;
		duckdb_logical_type list_logical = NULL;
		if (!tcc_ffi_list_child_type(type, &child_type)) {
			return NULL;
		}
		child_logical = tcc_ffi_type_create_logical_type(child_type, 0, NULL, NULL);
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
		child_logical = tcc_ffi_type_create_logical_type(child_type, 0, NULL, NULL);
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
			child_types[i] = NULL;
			child_names[i] = struct_meta->field_names[i];
			child_types[i] = tcc_ffi_type_create_logical_type(struct_meta->field_types[i], 0, NULL, NULL);
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
		if (!map_meta || !tcc_ffi_type_is_fixed_width_scalar(map_meta->key_type) ||
		    !tcc_ffi_type_is_fixed_width_scalar(map_meta->value_type)) {
			return NULL;
		}
		key_type = tcc_ffi_type_create_logical_type(map_meta->key_type, 0, NULL, NULL);
		value_type = tcc_ffi_type_create_logical_type(map_meta->value_type, 0, NULL, NULL);
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

static void tcc_host_signature_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
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
	ducktinycc_blob_t **batch_blob_columns = NULL;
	ducktinycc_list_t **batch_list_columns = NULL;
	ducktinycc_array_t **batch_array_columns = NULL;
	ducktinycc_struct_t **batch_struct_columns = NULL;
	ducktinycc_map_t **batch_map_columns = NULL;
	const char ***batch_varchar_columns = NULL;
	char ***batch_varchar_owned = NULL;
	duckdb_list_entry **list_entries_columns = NULL;
	uint8_t **list_child_data_columns = NULL;
	uint64_t **list_child_validity_columns = NULL;
	size_t *list_child_sizes = NULL;
	uint8_t **array_child_data_columns = NULL;
	uint64_t **array_child_validity_columns = NULL;
	size_t *array_child_sizes = NULL;
	uint8_t ***struct_child_data_columns = NULL;
	uint64_t ***struct_child_validity_columns = NULL;
	size_t **struct_child_sizes = NULL;
	idx_t *struct_field_counts = NULL;
	duckdb_list_entry **map_entries_columns = NULL;
	uint8_t **map_key_data_columns = NULL;
	uint64_t **map_key_validity_columns = NULL;
	size_t *map_key_sizes = NULL;
	uint8_t **map_value_data_columns = NULL;
	uint64_t **map_value_validity_columns = NULL;
	size_t *map_value_sizes = NULL;
	const char **batch_out_varchar = NULL;
	ducktinycc_blob_t *batch_out_blob = NULL;
	ducktinycc_list_t *batch_out_list = NULL;
	ducktinycc_array_t *batch_out_array = NULL;
	ducktinycc_struct_t *batch_out_struct = NULL;
	ducktinycc_map_t *batch_out_map = NULL;
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
	uint8_t out_value[64];
	const char *out_varchar_value = NULL;
	ducktinycc_blob_t out_blob_value;
	ducktinycc_list_t out_list_value;
	ducktinycc_array_t out_array_value;
	ducktinycc_struct_t out_struct_value;
	ducktinycc_map_t out_map_value;
	idx_t row;
	int col;
	const char *error = NULL;
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
			}
			list_entries_columns =
			    (duckdb_list_entry **)duckdb_malloc(sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			list_child_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			list_child_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			list_child_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			array_child_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			array_child_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			array_child_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			struct_child_data_columns = (uint8_t ***)duckdb_malloc(sizeof(uint8_t **) * (size_t)ctx->arg_count);
			struct_child_validity_columns =
			    (uint64_t ***)duckdb_malloc(sizeof(uint64_t **) * (size_t)ctx->arg_count);
			struct_child_sizes = (size_t **)duckdb_malloc(sizeof(size_t *) * (size_t)ctx->arg_count);
			struct_field_counts = (idx_t *)duckdb_malloc(sizeof(idx_t) * (size_t)ctx->arg_count);
			map_entries_columns = (duckdb_list_entry **)duckdb_malloc(sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			map_key_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			map_key_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			map_key_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			map_value_data_columns = (uint8_t **)duckdb_malloc(sizeof(uint8_t *) * (size_t)ctx->arg_count);
			map_value_validity_columns = (uint64_t **)duckdb_malloc(sizeof(uint64_t *) * (size_t)ctx->arg_count);
			map_value_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)ctx->arg_count);
			if (!in_data || !in_validity || !synthetic_validity_columns ||
			    (ctx->wrapper_mode == TCC_WRAPPER_MODE_ROW &&
			     (!arg_ptrs || !row_varchar_values || !row_blob_values || !row_list_values || !row_array_values ||
			      !row_struct_values || !row_map_values)) ||
			    (ctx->wrapper_mode == TCC_WRAPPER_MODE_BATCH &&
			     (!batch_arg_data || !batch_varchar_columns || !batch_varchar_owned || !batch_blob_columns ||
			      !batch_list_columns || !batch_array_columns || !batch_struct_columns || !batch_map_columns)) ||
			    !list_entries_columns || !list_child_data_columns || !list_child_validity_columns || !list_child_sizes ||
			    !array_child_data_columns || !array_child_validity_columns || !array_child_sizes ||
			    !struct_child_data_columns || !struct_child_validity_columns || !struct_child_sizes ||
			    !struct_field_counts || !map_entries_columns || !map_key_data_columns || !map_key_validity_columns ||
			    !map_key_sizes || !map_value_data_columns || !map_value_validity_columns || !map_value_sizes) {
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
			memset(list_entries_columns, 0, sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			memset(list_child_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(list_child_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(list_child_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(array_child_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(array_child_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(array_child_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(synthetic_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(struct_child_data_columns, 0, sizeof(uint8_t **) * (size_t)ctx->arg_count);
			memset(struct_child_validity_columns, 0, sizeof(uint64_t **) * (size_t)ctx->arg_count);
			memset(struct_child_sizes, 0, sizeof(size_t *) * (size_t)ctx->arg_count);
			memset(struct_field_counts, 0, sizeof(idx_t) * (size_t)ctx->arg_count);
			memset(map_entries_columns, 0, sizeof(duckdb_list_entry *) * (size_t)ctx->arg_count);
			memset(map_key_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(map_key_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(map_key_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
			memset(map_value_data_columns, 0, sizeof(uint8_t *) * (size_t)ctx->arg_count);
			memset(map_value_validity_columns, 0, sizeof(uint64_t *) * (size_t)ctx->arg_count);
			memset(map_value_sizes, 0, sizeof(size_t) * (size_t)ctx->arg_count);
		}
	ret_size = tcc_ffi_type_size(ctx->return_type);

		for (col = 0; col < ctx->arg_count; col++) {
			duckdb_vector v = duckdb_data_chunk_get_vector(input, (idx_t)col);
			in_data[col] = (uint8_t *)duckdb_vector_get_data(v);
			in_validity[col] = duckdb_vector_get_validity(v);
			if (!ctx->arg_sizes || ctx->arg_sizes[col] == 0) {
				error = "ducktinycc invalid arg type size";
				goto cleanup;
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
				if (!struct_child_data_columns[col] || !struct_child_validity_columns[col] || !struct_child_sizes[col]) {
					error = "ducktinycc out of memory";
					goto cleanup;
				}
				for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
					duckdb_vector child_vector = duckdb_struct_vector_get_child(v, field_idx);
					if (!child_vector) {
						error = "ducktinycc struct child vector missing";
						goto cleanup;
					}
					struct_child_data_columns[col][field_idx] = (uint8_t *)duckdb_vector_get_data(child_vector);
					struct_child_validity_columns[col][field_idx] = duckdb_vector_get_validity(child_vector);
					struct_child_sizes[col][field_idx] = meta->field_sizes[field_idx];
					if (struct_child_sizes[col][field_idx] == 0) {
						error = "ducktinycc invalid struct child type size";
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
						for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
							if (!struct_child_validity_columns[col][field_idx] ||
							    duckdb_validity_row_is_valid(struct_child_validity_columns[col][field_idx], row)) {
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
			}
			tcc_validity_set_all(out_validity, n, ctx->return_type != TCC_FFI_VOID);
			if (!ctx->batch_wrapper(batch_arg_data, in_validity, (uint64_t)n,
			                        ctx->return_type == TCC_FFI_VARCHAR
			                            ? (void *)batch_out_varchar
			                            : (ctx->return_type == TCC_FFI_BLOB
			                                   ? (void *)batch_out_blob
			                                   : (tcc_ffi_type_is_list(ctx->return_type)
			                                          ? (void *)batch_out_list
			                                          : (tcc_ffi_type_is_array(ctx->return_type)
			                                                 ? (void *)batch_out_array
			                                                 : (tcc_ffi_type_is_struct(ctx->return_type)
			                                                        ? (void *)batch_out_struct
			                                                        : (tcc_ffi_type_is_map(ctx->return_type)
			                                                               ? (void *)batch_out_map
			                                                               : out_data))))),
			                        out_validity)) {
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
			}
			goto cleanup;
		}

	for (row = 0; row < n; row++) {
		bool valid = true;
		bool out_is_null = false;
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
				}
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
		if (out_struct_child_data) {
			duckdb_free((void *)out_struct_child_data);
		}
		if (out_struct_child_validity) {
			duckdb_free((void *)out_struct_child_validity);
		}
		if (out_struct_child_sizes) {
			duckdb_free((void *)out_struct_child_sizes);
		}
	if (error) {
		duckdb_scalar_function_set_error(info, error);
	}
}

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
	tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
	tcc_ffi_map_meta_t *arg_map_metas = NULL;
	tcc_wrapper_mode_t mode = TCC_WRAPPER_MODE_ROW;
	int arg_count = 0;
	tcc_error_buffer_t err;
	int i;
	duckdb_type ret_duckdb_type;
	memset(&err, 0, sizeof(err));
	memset(&ret_struct_meta, 0, sizeof(ret_struct_meta));
	memset(&ret_map_meta, 0, sizeof(ret_map_meta));
	if (!con || !name || name[0] == '\0' || !fn_ptr) {
		return false;
	}
	if (!tcc_parse_signature(return_type, arg_types_csv, &ret_type, &ret_array_size, &arg_types, &arg_array_sizes,
	                         &ret_struct_meta, &ret_map_meta, &arg_struct_metas, &arg_map_metas, &arg_count, &err)) {
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
			duckdb_free(arg_map_metas);
		}
		tcc_struct_meta_destroy(&ret_struct_meta);
		return false;
	}
	ret_duckdb_type = tcc_ffi_type_to_duckdb_type(ret_type);
	if (ret_duckdb_type == DUCKDB_TYPE_INVALID) {
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
			duckdb_free(arg_map_metas);
		}
		tcc_struct_meta_destroy(&ret_struct_meta);
		return false;
	}

	fn = duckdb_create_scalar_function();
	if (!fn) {
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
			duckdb_free(arg_map_metas);
		}
		tcc_struct_meta_destroy(&ret_struct_meta);
		if (fn) {
			duckdb_destroy_scalar_function(&fn);
		}
		return false;
	}
	ctx = (tcc_host_sig_ctx_t *)duckdb_malloc(sizeof(tcc_host_sig_ctx_t));
	if (!ctx) {
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
			duckdb_free(arg_map_metas);
		}
		tcc_struct_meta_destroy(&ret_struct_meta);
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
	ctx->arg_struct_metas = arg_struct_metas;
	ctx->arg_map_metas = arg_map_metas;
	arg_types = NULL;
	arg_array_sizes = NULL;
	memset(&ret_struct_meta, 0, sizeof(ret_struct_meta));
	memset(&ret_map_meta, 0, sizeof(ret_map_meta));
	arg_struct_metas = NULL;
	arg_map_metas = NULL;
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
		duckdb_logical_type arg_type =
		    tcc_ffi_type_create_logical_type(ctx->arg_types[i], ctx->arg_array_sizes ? ctx->arg_array_sizes[i] : 0,
		                                     ctx->arg_struct_metas ? &ctx->arg_struct_metas[i] : NULL,
		                                     ctx->arg_map_metas ? &ctx->arg_map_metas[i] : NULL);
		if (!arg_type) {
			tcc_host_sig_ctx_destroy(ctx);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		duckdb_scalar_function_add_parameter(fn, arg_type);
		duckdb_destroy_logical_type(&arg_type);
	}
	{
		duckdb_logical_type ret_type_obj =
		    tcc_ffi_type_create_logical_type(ret_type, ret_array_size, &ctx->return_struct_meta, &ctx->return_map_meta);
		if (!ret_type_obj) {
			tcc_host_sig_ctx_destroy(ctx);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		duckdb_scalar_function_set_return_type(fn, ret_type_obj);
		duckdb_destroy_logical_type(&ret_type_obj);
	}
	duckdb_scalar_function_set_function(fn, tcc_host_signature_scalar);
	duckdb_scalar_function_set_extra_info(fn, ctx, tcc_host_sig_ctx_destroy);
	rc = duckdb_register_scalar_function(con, fn);
	if (rc != DuckDBSuccess) {
		duckdb_destroy_scalar_function(&fn);
		return false;
	}
	return true;
}

static int ducktinycc_valid_is_set(const uint64_t *validity, uint64_t idx) {
	if (!validity) {
		return 1;
	}
	return (validity[idx >> 6] & (1ULL << (idx & 63))) != 0;
}

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

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
static void tcc_add_host_symbols(TCCState *s) {
	if (!s) {
		return;
	}
	(void)tcc_add_symbol(s, "duckdb_ext_api", &duckdb_ext_api);
	(void)tcc_add_symbol(s, "ducktinycc_register_signature", ducktinycc_register_signature);
	(void)tcc_add_symbol(s, "ducktinycc_valid_is_set", ducktinycc_valid_is_set);
	(void)tcc_add_symbol(s, "ducktinycc_valid_set", ducktinycc_valid_set);
	(void)tcc_add_symbol(s, "ducktinycc_span_contains", ducktinycc_span_contains);
	(void)tcc_add_symbol(s, "ducktinycc_ptr_add", ducktinycc_ptr_add);
	(void)tcc_add_symbol(s, "ducktinycc_ptr_add_mut", ducktinycc_ptr_add_mut);
	(void)tcc_add_symbol(s, "ducktinycc_list_is_valid", ducktinycc_list_is_valid);
	(void)tcc_add_symbol(s, "ducktinycc_list_elem_ptr", ducktinycc_list_elem_ptr);
	(void)tcc_add_symbol(s, "ducktinycc_array_is_valid", ducktinycc_array_is_valid);
	(void)tcc_add_symbol(s, "ducktinycc_array_elem_ptr", ducktinycc_array_elem_ptr);
	(void)tcc_add_symbol(s, "ducktinycc_struct_field_ptr", ducktinycc_struct_field_ptr);
	(void)tcc_add_symbol(s, "ducktinycc_struct_field_is_valid", ducktinycc_struct_field_is_valid);
	(void)tcc_add_symbol(s, "ducktinycc_map_key_ptr", ducktinycc_map_key_ptr);
	(void)tcc_add_symbol(s, "ducktinycc_map_value_ptr", ducktinycc_map_value_ptr);
	(void)tcc_add_symbol(s, "ducktinycc_map_key_is_valid", ducktinycc_map_key_is_valid);
	(void)tcc_add_symbol(s, "ducktinycc_map_value_is_valid", ducktinycc_map_value_is_valid);
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

static void tcc_module_init(duckdb_init_info info) {
	tcc_module_init_data_t *init = (tcc_module_init_data_t *)duckdb_malloc(sizeof(tcc_module_init_data_t));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	atomic_store_explicit(&init->emitted, false, memory_order_relaxed);
	duckdb_init_set_init_data(info, init, destroy_tcc_module_init_data);
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
	if (token_len >= 5 && (token[0] == 'l' || token[0] == 'L') && (token[1] == 'i' || token[1] == 'I') &&
	    (token[2] == 's' || token[2] == 'S') && (token[3] == 't' || token[3] == 'T') && token[4] == '_') {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		size_t child_array_size = 0;
		if (tcc_parse_type_token(token + 5, false, &child_type, &child_array_size) &&
		    !tcc_ffi_type_is_list(child_type) && !tcc_ffi_type_is_array(child_type) && child_array_size == 0 &&
		    tcc_ffi_list_type_from_child(child_type, out_type)) {
			return true;
		}
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
				    !tcc_ffi_type_is_list(child_type) && !tcc_ffi_type_is_array(child_type) &&
				    child_array_size == 0 && tcc_ffi_array_type_from_child(child_type, out_type)) {
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
			    !tcc_ffi_type_is_list(child_type) && !tcc_ffi_type_is_array(child_type) && child_array_size == 0 &&
			    tcc_ffi_list_type_from_child(child_type, out_type)) {
				return true;
			}
		}
	}
	return false;
}

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

static bool tcc_parse_struct_meta_token(const char *token, tcc_ffi_struct_meta_t *out_meta,
                                        tcc_error_buffer_t *error_buf) {
	size_t token_len;
	char *inner = NULL;
	char *cur;
	int field_count = 0;
	int cap = 0;
	char **field_names = NULL;
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
		char *part = cur;
		char *next = strchr(part, ';');
		char *colon;
		char *name_part;
		char *type_part;
		size_t part_len;
		tcc_ffi_type_t field_type = TCC_FFI_VOID;
		size_t field_array_size = 0;
		size_t field_size;
		char auto_name[32];
		char **new_names;
		tcc_ffi_type_t *new_types;
		size_t *new_sizes;
		if (next) {
			*next = '\0';
			cur = next + 1;
		} else {
			cur = NULL;
		}
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
		colon = strchr(part, ':');
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
		if (!tcc_parse_type_token(type_part, false, &field_type, &field_array_size) ||
		    field_array_size != 0 || !tcc_ffi_type_is_fixed_width_scalar(field_type)) {
			tcc_set_error(error_buf,
			              "struct fields currently support fixed-width scalar tokens only (no varchar/blob/nested)");
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
			new_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)new_cap);
			new_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)new_cap);
			if (!new_names || !new_types || !new_sizes) {
				if (new_names) {
					duckdb_free(new_names);
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
			if (field_names && field_count > 0) {
				memcpy(new_names, field_names, sizeof(char *) * (size_t)field_count);
				memcpy(new_types, field_types, sizeof(tcc_ffi_type_t) * (size_t)field_count);
				memcpy(new_sizes, field_sizes, sizeof(size_t) * (size_t)field_count);
				duckdb_free(field_names);
				duckdb_free(field_types);
				duckdb_free(field_sizes);
			}
			field_names = new_names;
			field_types = new_types;
			field_sizes = new_sizes;
			cap = new_cap;
		}
		field_names[field_count] = tcc_strdup(name_part);
		if (!field_names[field_count]) {
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
	if (field_types) {
		duckdb_free(field_types);
	}
	if (field_sizes) {
		duckdb_free(field_sizes);
	}
	memset(out_meta, 0, sizeof(*out_meta));
	return false;
}

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
	sep = strchr(inner, ';');
	if (!sep || strchr(sep + 1, ';')) {
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
	if (!tcc_parse_type_token(key_token, false, &key_type, &key_array_size) ||
	    !tcc_parse_type_token(value_token, false, &value_type, &value_array_size) || key_array_size != 0 ||
	    value_array_size != 0 || !tcc_ffi_type_is_fixed_width_scalar(key_type) ||
	    !tcc_ffi_type_is_fixed_width_scalar(value_type)) {
		duckdb_free(inner);
		tcc_set_error(error_buf,
		              "map key/value currently support fixed-width scalar tokens only (no varchar/blob/nested)");
		return false;
	}
	key_size = tcc_ffi_type_size(key_type);
	value_size = tcc_ffi_type_size(value_type);
	duckdb_free(inner);
	if (key_size == 0 || value_size == 0) {
		tcc_set_error(error_buf, "map key/value has unsupported storage type");
		return false;
	}
	out_meta->key_type = key_type;
	out_meta->value_type = value_type;
	out_meta->key_size = key_size;
	out_meta->value_size = value_size;
	return true;
}

static bool tcc_parse_signature(const char *return_type, const char *arg_types_csv, tcc_ffi_type_t *out_return_type,
	                                size_t *out_return_array_size, tcc_ffi_type_t **out_arg_types,
	                                size_t **out_arg_array_sizes, tcc_ffi_struct_meta_t *out_return_struct_meta,
	                                tcc_ffi_map_meta_t *out_return_map_meta,
	                                tcc_ffi_struct_meta_t **out_arg_struct_metas,
	                                tcc_ffi_map_meta_t **out_arg_map_metas, int *out_arg_count,
	                                tcc_error_buffer_t *error_buf) {
	char *args_copy = NULL;
	char *cur = NULL;
	int argc = 0;
	int cap = 0;
	tcc_ffi_type_t *arg_types = NULL;
	size_t *arg_array_sizes = NULL;
	tcc_ffi_struct_meta_t return_struct_meta;
	tcc_ffi_map_meta_t return_map_meta;
	tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
	tcc_ffi_map_meta_t *arg_map_metas = NULL;
	tcc_ffi_type_t ret_type = TCC_FFI_I64;
	size_t ret_array_size = 0;
	if (!return_type || return_type[0] == '\0' || !out_return_type || !out_return_array_size || !out_arg_types ||
	    !out_arg_array_sizes || !out_return_struct_meta || !out_return_map_meta || !out_arg_struct_metas ||
	    !out_arg_map_metas || !out_arg_count) {
		tcc_set_error(error_buf, "return_type is required");
		return false;
	}
	memset(&return_struct_meta, 0, sizeof(return_struct_meta));
	memset(&return_map_meta, 0, sizeof(return_map_meta));
	if (!arg_types_csv) {
		tcc_set_error(error_buf, "arg_types is required (use [] for no args)");
		return false;
	}
	if (!tcc_parse_type_token(return_type, true, &ret_type, &ret_array_size)) {
		tcc_set_error(error_buf, "return_type contains unsupported type token");
		return false;
	}
	if (ret_type == TCC_FFI_STRUCT && !tcc_parse_struct_meta_token(return_type, &return_struct_meta, error_buf)) {
		return false;
	}
	if (ret_type == TCC_FFI_MAP && !tcc_parse_map_meta_token(return_type, &return_map_meta, error_buf)) {
		tcc_struct_meta_destroy(&return_struct_meta);
		return false;
	}
	if (arg_types_csv[0] == '\0') {
		*out_return_type = ret_type;
		*out_return_array_size = ret_array_size;
		*out_arg_types = NULL;
		*out_arg_array_sizes = NULL;
		*out_return_struct_meta = return_struct_meta;
		*out_return_map_meta = return_map_meta;
		*out_arg_struct_metas = NULL;
		*out_arg_map_metas = NULL;
		*out_arg_count = 0;
		return true;
	}
	args_copy = tcc_strdup(arg_types_csv);
	if (!args_copy) {
		tcc_struct_meta_destroy(&return_struct_meta);
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
					duckdb_free(arg_map_metas);
				}
				tcc_struct_meta_destroy(&return_struct_meta);
				tcc_set_error(error_buf, "arg_types contains empty token");
				return false;
			}
			if (argc >= cap) {
				int new_cap = cap == 0 ? 8 : cap * 2;
				tcc_ffi_type_t *new_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)new_cap);
				size_t *new_array_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)new_cap);
				tcc_ffi_struct_meta_t *new_struct_metas =
				    (tcc_ffi_struct_meta_t *)duckdb_malloc(sizeof(tcc_ffi_struct_meta_t) * (size_t)new_cap);
				tcc_ffi_map_meta_t *new_map_metas =
				    (tcc_ffi_map_meta_t *)duckdb_malloc(sizeof(tcc_ffi_map_meta_t) * (size_t)new_cap);
				if (!new_types) {
					if (new_array_sizes) {
						duckdb_free(new_array_sizes);
					}
					if (new_struct_metas) {
						duckdb_free(new_struct_metas);
					}
					if (new_map_metas) {
						duckdb_free(new_map_metas);
					}
					duckdb_free(args_copy);
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
						duckdb_free(arg_map_metas);
					}
					tcc_struct_meta_destroy(&return_struct_meta);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				if (!new_array_sizes || !new_struct_metas || !new_map_metas) {
					duckdb_free(new_types);
					duckdb_free(args_copy);
					if (arg_types) {
						duckdb_free(arg_types);
					}
					if (arg_array_sizes) {
						duckdb_free(arg_array_sizes);
					}
					if (new_array_sizes) {
						duckdb_free(new_array_sizes);
					}
					if (new_struct_metas) {
						duckdb_free(new_struct_metas);
					}
					if (new_map_metas) {
						duckdb_free(new_map_metas);
					}
					if (arg_struct_metas) {
						tcc_struct_meta_array_destroy(arg_struct_metas, argc);
					}
					if (arg_map_metas) {
						duckdb_free(arg_map_metas);
					}
					tcc_struct_meta_destroy(&return_struct_meta);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memset(new_struct_metas, 0, sizeof(tcc_ffi_struct_meta_t) * (size_t)new_cap);
				memset(new_map_metas, 0, sizeof(tcc_ffi_map_meta_t) * (size_t)new_cap);
				if (arg_types && argc > 0) {
					memcpy(new_types, arg_types, sizeof(tcc_ffi_type_t) * (size_t)argc);
					duckdb_free(arg_types);
				}
				if (arg_array_sizes && argc > 0) {
					memcpy(new_array_sizes, arg_array_sizes, sizeof(size_t) * (size_t)argc);
					duckdb_free(arg_array_sizes);
				}
				if (arg_struct_metas && argc > 0) {
					memcpy(new_struct_metas, arg_struct_metas, sizeof(tcc_ffi_struct_meta_t) * (size_t)argc);
					duckdb_free(arg_struct_metas);
				}
				if (arg_map_metas && argc > 0) {
					memcpy(new_map_metas, arg_map_metas, sizeof(tcc_ffi_map_meta_t) * (size_t)argc);
					duckdb_free(arg_map_metas);
				}
				arg_types = new_types;
				arg_array_sizes = new_array_sizes;
				arg_struct_metas = new_struct_metas;
				arg_map_metas = new_map_metas;
				cap = new_cap;
			}
			if (!tcc_parse_type_token(token, false, &arg_types[argc], &arg_array_sizes[argc])) {
				duckdb_free(args_copy);
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
					duckdb_free(arg_map_metas);
				}
				tcc_struct_meta_destroy(&return_struct_meta);
				tcc_set_error(error_buf, "arg_types contains unsupported type token");
				return false;
			}
			if (arg_types[argc] == TCC_FFI_STRUCT &&
			    !tcc_parse_struct_meta_token(token, &arg_struct_metas[argc], error_buf)) {
				duckdb_free(args_copy);
				if (arg_types) {
					duckdb_free(arg_types);
				}
				if (arg_array_sizes) {
					duckdb_free(arg_array_sizes);
				}
				if (arg_struct_metas) {
					tcc_struct_meta_array_destroy(arg_struct_metas, argc + 1);
				}
				if (arg_map_metas) {
					duckdb_free(arg_map_metas);
				}
				tcc_struct_meta_destroy(&return_struct_meta);
				return false;
			}
			if (arg_types[argc] == TCC_FFI_MAP && !tcc_parse_map_meta_token(token, &arg_map_metas[argc], error_buf)) {
				duckdb_free(args_copy);
				if (arg_types) {
					duckdb_free(arg_types);
				}
				if (arg_array_sizes) {
					duckdb_free(arg_array_sizes);
				}
				if (arg_struct_metas) {
					tcc_struct_meta_array_destroy(arg_struct_metas, argc + 1);
				}
				if (arg_map_metas) {
					duckdb_free(arg_map_metas);
				}
				tcc_struct_meta_destroy(&return_struct_meta);
				return false;
			}
			argc++;
		}
	duckdb_free(args_copy);
	*out_return_type = ret_type;
	*out_return_array_size = ret_array_size;
	*out_return_struct_meta = return_struct_meta;
	*out_return_map_meta = return_map_meta;
	*out_arg_types = arg_types;
	*out_arg_array_sizes = arg_array_sizes;
	*out_arg_struct_metas = arg_struct_metas;
	*out_arg_map_metas = arg_map_metas;
	*out_arg_count = argc;
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

static char *tcc_generate_ffi_loader_source(const char *module_symbol, const char *target_symbol, const char *sql_name,
                                            const char *return_type, const char *arg_types_csv,
                                            const char *wrapper_mode_token, tcc_wrapper_mode_t wrapper_mode,
                                            tcc_ffi_type_t ret_type, const tcc_ffi_type_t *arg_types, int arg_count) {
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
		if (!tcc_text_buf_appendf(&args_decl, "%s%s a%d", i == 0 ? "" : ", ", arg_c_type, i) ||
		    !tcc_text_buf_appendf(&row_unpack_lines, "  %s a%d = *(%s *)args[%d];\n", arg_c_type, i, arg_c_type, i) ||
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

static char *tcc_build_codegen_unit_source(const char *user_source, const char *loader_source) {
	char *combined_src;
	const char *prelude = "#include <stdint.h>\n"
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
		                      "extern int ducktinycc_valid_is_set(const uint64_t *validity, uint64_t idx);\n"
		                      "extern void ducktinycc_valid_set(uint64_t *validity, uint64_t idx, int valid);\n"
		                      "extern int ducktinycc_span_contains(uint64_t len, uint64_t idx);\n"
		                      "extern const void *ducktinycc_ptr_add(const void *base, uint64_t byte_offset);\n"
		                      "extern void *ducktinycc_ptr_add_mut(void *base, uint64_t byte_offset);\n"
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
	if (!loader_source) {
		return NULL;
	}
	if (!user_source || user_source[0] == '\0') {
		n0 = strlen(prelude);
		n2 = strlen(loader_source);
		combined_src = (char *)duckdb_malloc(n0 + n2 + 2);
		if (!combined_src) {
			return NULL;
		}
		memcpy(combined_src, prelude, n0);
		memcpy(combined_src + n0, loader_source, n2);
		combined_src[n0 + n2] = '\0';
		return combined_src;
	}
	n0 = strlen(prelude);
	n1 = strlen(user_source);
	n2 = strlen(loader_source);
	combined_src = (char *)duckdb_malloc(n0 + n1 + n2 + 3);
	if (!combined_src) {
		return NULL;
	}
	memcpy(combined_src, prelude, n0);
	memcpy(combined_src + n0, user_source, n1);
	combined_src[n0 + n1] = '\n';
	memcpy(combined_src + n0 + n1 + 1, loader_source, n2);
	combined_src[n0 + n1 + 1 + n2] = '\0';
	return combined_src;
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
static int tcc_compile_and_load_codegen_module(const char *runtime_path, tcc_module_state_t *state,
                                               const tcc_module_bind_data_t *bind, const char *sql_name,
                                               const char *target_symbol, tcc_registered_artifact_t **out_artifact,
                                               tcc_error_buffer_t *error_buf, char *out_module_symbol, size_t symbol_len) {
	tcc_ffi_type_t ret_type = TCC_FFI_I64;
	size_t ret_array_size = 0;
	tcc_ffi_type_t *arg_types = NULL;
	size_t *arg_array_sizes = NULL;
	tcc_ffi_struct_meta_t ret_struct_meta;
	tcc_ffi_map_meta_t ret_map_meta;
	tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
	tcc_ffi_map_meta_t *arg_map_metas = NULL;
	tcc_wrapper_mode_t wrapper_mode = TCC_WRAPPER_MODE_ROW;
	const char *wrapper_mode_token;
	int arg_count = 0;
	char *loader_src = NULL;
	char *combined_src = NULL;
	tcc_module_bind_data_t bind_copy;
	tcc_registered_artifact_t *artifact = NULL;

	if (!state || !bind || !sql_name || !target_symbol || !out_artifact || !error_buf || !out_module_symbol ||
	    symbol_len == 0) {
		tcc_set_error(error_buf, "invalid codegen compile arguments");
		return -1;
	}
	memset(&ret_struct_meta, 0, sizeof(ret_struct_meta));
	memset(&ret_map_meta, 0, sizeof(ret_map_meta));
	if (!state->connection) {
		tcc_set_error(error_buf, "no persistent extension connection available");
		return -1;
	}
	if (!tcc_parse_signature(bind->return_type, bind->arg_types, &ret_type, &ret_array_size, &arg_types,
	                         &arg_array_sizes, &ret_struct_meta, &ret_map_meta, &arg_struct_metas,
	                         &arg_map_metas, &arg_count, error_buf)) {
		return -1;
	}
	if (!tcc_parse_wrapper_mode(bind->wrapper_mode, &wrapper_mode, error_buf)) {
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
			duckdb_free(arg_map_metas);
		}
		tcc_struct_meta_destroy(&ret_struct_meta);
		return -1;
	}
	wrapper_mode_token = tcc_wrapper_mode_token(wrapper_mode);
	if (!wrapper_mode_token) {
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
			duckdb_free(arg_map_metas);
		}
		tcc_struct_meta_destroy(&ret_struct_meta);
		tcc_set_error(error_buf, "wrapper_mode contains unsupported token");
		return -1;
	}

	snprintf(out_module_symbol, symbol_len, "__ducktinycc_ffi_init_%llu_%llu", (unsigned long long)state->session.state_id,
	         (unsigned long long)state->session.config_version);
	loader_src = tcc_generate_ffi_loader_source(out_module_symbol, target_symbol, sql_name,
	                                            bind->return_type ? bind->return_type : "i64",
	                                            bind->arg_types ? bind->arg_types : "", wrapper_mode_token,
	                                            wrapper_mode, ret_type, arg_types, arg_count);
	if (arg_types) {
		duckdb_free(arg_types);
		arg_types = NULL;
	}
	if (arg_array_sizes) {
		duckdb_free(arg_array_sizes);
		arg_array_sizes = NULL;
	}
	if (arg_struct_metas) {
		tcc_struct_meta_array_destroy(arg_struct_metas, arg_count);
		arg_struct_metas = NULL;
	}
	if (arg_map_metas) {
		duckdb_free(arg_map_metas);
		arg_map_metas = NULL;
	}
	tcc_struct_meta_destroy(&ret_struct_meta);
	if (!loader_src) {
		tcc_set_error(error_buf, "failed to generate codegen wrapper");
		return -1;
	}
	combined_src = tcc_build_codegen_unit_source(bind->source, loader_src);
	duckdb_free(loader_src);
	if (!combined_src) {
		tcc_set_error(error_buf, "out of memory");
		return -1;
	}

	memset(&bind_copy, 0, sizeof(bind_copy));
	bind_copy = *bind;
	bind_copy.source = combined_src;
	if (tcc_build_module_artifact(runtime_path, state, &bind_copy, out_module_symbol, sql_name, &artifact, error_buf) !=
	    0) {
		duckdb_free(combined_src);
		return -1;
	}
	duckdb_free(combined_src);

	if (!artifact->module_init(state->connection)) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "generated module init returned false");
		return -1;
	}
	*out_artifact = artifact;
	return 0;
}
#endif

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
	       strcmp(mode, "compile") == 0 || strcmp(mode, "quick_compile") == 0;
}

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
			if (tcc_string_list_append(&state->session.define_names, bind->define_name) &&
			    tcc_string_list_append(&state->session.define_values, define_value)) {
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
	} else if (strcmp(bind->mode, "codegen_preview") == 0) {
		const char *target_symbol = tcc_effective_symbol(state, bind);
			const char *sql_name = tcc_effective_sql_name(state, bind, target_symbol);
			tcc_ffi_type_t ret_type = TCC_FFI_I64;
			size_t ret_array_size = 0;
			tcc_ffi_type_t *arg_types = NULL;
			size_t *arg_array_sizes = NULL;
			tcc_ffi_struct_meta_t ret_struct_meta;
			tcc_ffi_map_meta_t ret_map_meta;
			tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
			tcc_ffi_map_meta_t *arg_map_metas = NULL;
			tcc_wrapper_mode_t wrapper_mode = TCC_WRAPPER_MODE_ROW;
		const char *wrapper_mode_token = NULL;
		int arg_count = 0;
		tcc_error_buffer_t err;
		char module_symbol[128];
		char *loader_src = NULL;
		char *preview_src = NULL;
		memset(&err, 0, sizeof(err));
		memset(&ret_struct_meta, 0, sizeof(ret_struct_meta));
		memset(&ret_map_meta, 0, sizeof(ret_map_meta));
		if (!target_symbol || target_symbol[0] == '\0') {
			tcc_write_row(output, false, bind->mode, "bind", "E_MISSING_ARGS", "symbol is required (bind or argument)",
			              NULL, sql_name, target_symbol, NULL, "database");
			goto tcc_module_done;
		}
			if (!tcc_parse_signature(bind->return_type, bind->arg_types, &ret_type, &ret_array_size, &arg_types,
			                         &arg_array_sizes, &ret_struct_meta, &ret_map_meta, &arg_struct_metas,
			                         &arg_map_metas, &arg_count, &err)) {
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_SIGNATURE", "invalid return_type/arg_types",
				              err.message[0] ? err.message : NULL, sql_name, target_symbol, NULL, "database");
				goto tcc_module_done;
			}
			if (!tcc_parse_wrapper_mode(bind->wrapper_mode, &wrapper_mode, &err)) {
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
					duckdb_free(arg_map_metas);
				}
				tcc_struct_meta_destroy(&ret_struct_meta);
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_WRAPPER_MODE", "invalid wrapper_mode",
				              err.message[0] ? err.message : NULL, sql_name, target_symbol, NULL, "database");
				goto tcc_module_done;
			}
		wrapper_mode_token = tcc_wrapper_mode_token(wrapper_mode);
			if (!wrapper_mode_token) {
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
					duckdb_free(arg_map_metas);
				}
				tcc_struct_meta_destroy(&ret_struct_meta);
				tcc_write_row(output, false, bind->mode, "bind", "E_BAD_WRAPPER_MODE", "invalid wrapper_mode",
				              "wrapper_mode contains unsupported token", sql_name, target_symbol, NULL, "database");
				goto tcc_module_done;
			}
		snprintf(module_symbol, sizeof(module_symbol), "__ducktinycc_ffi_init_%llu_%llu",
		         (unsigned long long)state->session.state_id, (unsigned long long)state->session.config_version);
		loader_src = tcc_generate_ffi_loader_source(module_symbol, target_symbol, sql_name,
		                                            bind->return_type ? bind->return_type : "i64",
		                                            bind->arg_types ? bind->arg_types : "", wrapper_mode_token,
		                                            wrapper_mode, ret_type, arg_types, arg_count);
			if (arg_types) {
				duckdb_free(arg_types);
				arg_types = NULL;
			}
			if (arg_array_sizes) {
				duckdb_free(arg_array_sizes);
				arg_array_sizes = NULL;
			}
			if (arg_struct_metas) {
				tcc_struct_meta_array_destroy(arg_struct_metas, arg_count);
				arg_struct_metas = NULL;
			}
			if (arg_map_metas) {
				duckdb_free(arg_map_metas);
				arg_map_metas = NULL;
			}
			tcc_struct_meta_destroy(&ret_struct_meta);
		if (!loader_src) {
			tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED", "ffi codegen failed",
			              "failed to generate codegen wrapper", sql_name, target_symbol, NULL, "database");
			goto tcc_module_done;
		}
		preview_src = tcc_build_codegen_unit_source(bind->source, loader_src);
		duckdb_free(loader_src);
		if (!preview_src) {
			tcc_write_row(output, false, bind->mode, "codegen", "E_CODEGEN_FAILED", "ffi codegen failed",
			              "out of memory", sql_name, target_symbol, NULL, "database");
			goto tcc_module_done;
		}
		tcc_write_row(output, true, bind->mode, "codegen", "OK", "generated codegen source", preview_src, sql_name,
		              target_symbol, module_symbol, "database");
		duckdb_free(preview_src);
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

				if (tcc_compile_and_load_codegen_module(runtime_path, state, bind, sql_name, target_symbol, &artifact, &err,
				                                        module_symbol, sizeof(module_symbol)) != 0) {
					if (strstr(err.message, "wrapper_mode")) {
						phase = "bind";
						code = "E_BAD_WRAPPER_MODE";
						message = "invalid wrapper_mode";
					} else if (strstr(err.message, "return_type") || strstr(err.message, "arg_types") ||
					           strstr(err.message, "struct token") || strstr(err.message, "map token") ||
					           strstr(err.message, "fixed-width scalar tokens only")) {
						phase = "bind";
						code = "E_BAD_SIGNATURE";
						message = "invalid return_type/arg_types";
					} else if (strstr(err.message, "failed to generate codegen wrapper") || strstr(err.message, "out of memory")) {
						phase = "codegen";
						code = "E_CODEGEN_FAILED";
						message = "ffi codegen failed";
				} else if (strstr(err.message, "no persistent extension connection")) {
					phase = "load";
					code = "E_NO_CONNECTION";
					message = "no persistent extension connection available";
				} else if (strstr(err.message, "generated module init returned false")) {
					phase = "load";
					code = "E_INIT_FAILED";
					message = "generated module init returned false";
				}
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

static void destroy_tcc_diag_bind_data(void *ptr) {
	tcc_diag_bind_data_t *bind = (tcc_diag_bind_data_t *)ptr;
	if (!bind) {
		return;
	}
	tcc_diag_rows_destroy(&bind->rows);
	duckdb_free(bind);
}

static void destroy_tcc_diag_init_data(void *ptr) {
	tcc_diag_init_data_t *init = (tcc_diag_init_data_t *)ptr;
	if (!init) {
		return;
	}
	duckdb_free(init);
}

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

static void tcc_diag_table_init(duckdb_init_info info) {
	tcc_diag_init_data_t *init = (tcc_diag_init_data_t *)duckdb_malloc(sizeof(tcc_diag_init_data_t));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	atomic_store_explicit(&init->offset, 0, memory_order_relaxed);
	duckdb_init_set_init_data(info, init, destroy_tcc_diag_init_data);
}

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
		rc = register_tcc_system_paths_function(connection) && register_tcc_library_probe_function(connection)
		         ? DuckDBSuccess
		         : DuckDBError;
	}

	duckdb_destroy_logical_type(&list_varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_table_function(&tf);
	return rc == DuckDBSuccess;
}
