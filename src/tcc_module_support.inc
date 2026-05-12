/*
 * DuckTinyCC support utilities: runtime path selection, diagnostics row buffers,
 * search-path helpers, and staged session lifecycle.
 */

/* tcc_default_runtime_path: Returns the TinyCC runtime directory to use when the session
 * has no explicit override.  Always uses the self-contained embedded extraction so that
 * the extension works identically in all environments without any external runtime files. */
static const char *tcc_default_runtime_path(void) {
#ifndef DUCKTINYCC_WASM_UNSUPPORTED
	return tcc_ensure_embedded_runtime();
#else
	return NULL;
#endif
}

/* tcc_string_ends_with: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_diag_rows_reserve: Diagnostics/probe table-function helper. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
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

/* tcc_diag_rows_add: Diagnostics/probe table-function helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_diag_rows_destroy: Diagnostics/probe table-function helper. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
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

/* tcc_string_equals_path: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_string_list_contains: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_string_list_destroy: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
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

/* tcc_string_list_reserve: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
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

/* tcc_string_list_append: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_string_list_pop_last: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_list_pop_last(tcc_string_list_t *list) {
	if (!list || list->count == 0) {
		return false;
	}
	list->count--;
	if (list->items[list->count]) {
		duckdb_free(list->items[list->count]);
		list->items[list->count] = NULL;
	}
	return true;
}

/* tcc_string_list_append_unique: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_string_list_append_unique(tcc_string_list_t *list, const char *value) {
	if (!list || !value || value[0] == '\0') {
		return false;
	}
	if (tcc_string_list_contains(list, value)) {
		return true;
	}
	return tcc_string_list_append(list, value);
}

/* tcc_is_path_like: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_has_library_suffix: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_append_env_path_list: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_add_platform_library_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_collect_library_search_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_collect_include_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_build_library_candidates: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
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

/* tcc_session_clear_bind: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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
	if (session->bound_stability) {
		duckdb_free(session->bound_stability);
		session->bound_stability = NULL;
	}
}

/* tcc_session_clear_build_state: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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
	tcc_string_list_destroy(&session->symbol_names);
	if (session->symbol_ptrs) {
		duckdb_free(session->symbol_ptrs);
		session->symbol_ptrs = NULL;
	}
	session->symbol_count = 0;
	session->symbol_capacity = 0;
	tcc_session_clear_bind(session);
	session->state_id++;
	session->config_version++;
}

/* tcc_session_set_runtime_path: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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
/* tcc_configure_runtime_paths: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* Destructor for per-UDF host signature context.
 * All members are treated as owned by `ctx` once attached via
 * `duckdb_scalar_function_set_extra_info`.
 */
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
	if (ctx->arg_map_metas && ctx->arg_count > 0) {
		tcc_map_meta_array_destroy(ctx->arg_map_metas, ctx->arg_count);
	}
	if (ctx->arg_union_metas && ctx->arg_count > 0) {
		tcc_union_meta_array_destroy(ctx->arg_union_metas, ctx->arg_count);
	}
	if (ctx->arg_descs && ctx->arg_count > 0) {
		int i;
		for (i = 0; i < ctx->arg_count; i++) {
			if (ctx->arg_descs[i]) {
				tcc_typedesc_destroy(ctx->arg_descs[i]);
			}
		}
		duckdb_free(ctx->arg_descs);
	}
	if (ctx->return_desc) {
		tcc_typedesc_destroy(ctx->return_desc);
	}
	tcc_struct_meta_destroy(&ctx->return_struct_meta);
	tcc_map_meta_destroy(&ctx->return_map_meta);
	tcc_union_meta_destroy(&ctx->return_union_meta);
	duckdb_free(ctx);
}

