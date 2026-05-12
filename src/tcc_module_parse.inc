/*
 * DuckTinyCC signature/type/helper grammar parsers.
 *
 * Included by tcc_module.c after bind/output helpers and type/bridge machinery.  Parser
 * functions own only the metadata they explicitly allocate and report failures through
 * tcc_error_buffer_t.
 */

static const char *tcc_skip_space(const char *p) {
	while (p && *p && isspace((unsigned char)*p)) {
		p++;
	}
	return p;
}

/* tcc_equals_ci: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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
		return "chunk_scalar_loop";
	default:
		return NULL;
	}
}

static const char *tcc_function_stability_token(tcc_function_stability_t stability) {
	switch (stability) {
	case TCC_FUNCTION_STABILITY_CONSISTENT:
		return "consistent";
	case TCC_FUNCTION_STABILITY_VOLATILE:
		return "volatile";
	default:
		return NULL;
	}
}

/* tcc_parse_function_stability: Parser helper for DuckDB scalar-function stability. */
static bool tcc_parse_function_stability(const char *stability, tcc_function_stability_t *out_stability,
                                         tcc_error_buffer_t *error_buf) {
	const char *value = stability;
	char token[32];
	size_t len;
	if (!out_stability) {
		tcc_set_error(error_buf, "stability output is required");
		return false;
	}
	if (!value || value[0] == '\0') {
		*out_stability = TCC_FUNCTION_STABILITY_CONSISTENT;
		return true;
	}
	value = tcc_skip_space(value);
	len = strlen(value);
	while (len > 0 && isspace((unsigned char)value[len - 1])) {
		len--;
	}
	if (len == 0 || len >= sizeof(token)) {
		tcc_set_error(error_buf, "stability contains unsupported token");
		return false;
	}
	memcpy(token, value, len);
	token[len] = '\0';
	if (tcc_equals_ci(token, "consistent")) {
		*out_stability = TCC_FUNCTION_STABILITY_CONSISTENT;
		return true;
	}
	if (tcc_equals_ci(token, "volatile")) {
		*out_stability = TCC_FUNCTION_STABILITY_VOLATILE;
		return true;
	}
	tcc_set_error(error_buf, "stability contains unsupported token");
	return false;
}

/* tcc_parse_wrapper_mode: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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
	if (tcc_equals_ci(token, "chunk_scalar_loop")) {
		*out_mode = TCC_WRAPPER_MODE_BATCH;
		return true;
	}
	tcc_set_error(error_buf, "wrapper_mode contains unsupported token");
	return false;
}

static char *tcc_next_top_level_part(char **cursor, char sep) {
	char *start;
	char *p;
	int angle_depth = 0;
	int square_depth = 0;
	if (!cursor || !*cursor) {
		return NULL;
	}
	start = *cursor;
	p = start;
	while (*p) {
		if (*p == '<') {
			angle_depth++;
		} else if (*p == '>') {
			if (angle_depth > 0) {
				angle_depth--;
			}
		} else if (*p == '[') {
			square_depth++;
		} else if (*p == ']') {
			if (square_depth > 0) {
				square_depth--;
			}
		} else if (*p == sep && angle_depth == 0 && square_depth == 0) {
			*p = '\0';
			*cursor = p + 1;
			return start;
		}
		p++;
	}
	*cursor = NULL;
	return start;
}

static char *tcc_find_top_level_char(char *input, char target) {
	char *p;
	int angle_depth = 0;
	int square_depth = 0;
	if (!input) {
		return NULL;
	}
	p = input;
	while (*p) {
		if (*p == '<') {
			angle_depth++;
		} else if (*p == '>') {
			if (angle_depth > 0) {
				angle_depth--;
			}
		} else if (*p == '[') {
			square_depth++;
		} else if (*p == ']') {
			if (square_depth > 0) {
				square_depth--;
			}
		} else if (*p == target && angle_depth == 0 && square_depth == 0) {
			return p;
		}
		p++;
	}
	return NULL;
}

static bool tcc_starts_with_ci(const char *value, const char *prefix) {
	while (*prefix) {
		if (tolower((unsigned char)*value) != tolower((unsigned char)*prefix)) {
			return false;
		}
		value++;
		prefix++;
	}
	return true;
}

static bool tcc_is_wrapped_type_token(const char *token, size_t token_len, const char *prefix) {
	size_t prefix_len = strlen(prefix);
	return token_len > prefix_len + 1 && tcc_starts_with_ci(token, prefix) && token[token_len - 1] == '>';
}


static char *tcc_copy_wrapped_inner(const char *token, const char *prefix, const char *error_message,
                                    tcc_error_buffer_t *error_buf) {
	size_t token_len;
	size_t prefix_len;
	size_t inner_len;
	char *inner;

	if (!token) {
		tcc_set_error(error_buf, error_message);
		return NULL;
	}
	token_len = strlen(token);
	prefix_len = strlen(prefix);
	if (!tcc_is_wrapped_type_token(token, token_len, prefix)) {
		tcc_set_error(error_buf, error_message);
		return NULL;
	}
	inner_len = token_len - prefix_len - 1;
	inner = (char *)duckdb_malloc(inner_len + 1);
	if (!inner) {
		tcc_set_error(error_buf, "out of memory");
		return NULL;
	}
	memcpy(inner, token + prefix_len, inner_len);
	inner[inner_len] = '\0';
	return inner;
}

static char *tcc_trim_token_inplace(char *token) {
	size_t len;

	token = tcc_trim_inplace(token);
	len = strlen(token);
	while (len > 0 && isspace((unsigned char)token[len - 1])) {
		token[--len] = '\0';
	}
	return token;
}


static bool tcc_parse_sized_type_copy(const char *token, char **out_copy, tcc_ffi_type_t *out_type,
                                      size_t *out_size, const char *parse_error, const char *size_error,
                                      tcc_error_buffer_t *error_buf) {
	size_t array_size = 0;
	char *copy;
	tcc_ffi_type_t type = TCC_FFI_VOID;
	size_t size;

	if (!token || token[0] == '\0' || !out_copy || !out_type || !out_size) {
		tcc_set_error(error_buf, parse_error);
		return false;
	}
	copy = tcc_strdup(token);
	if (!copy) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	if (!tcc_parse_type_token(token, false, &type, &array_size) || type == TCC_FFI_VOID) {
		duckdb_free(copy);
		tcc_set_error(error_buf, parse_error);
		return false;
	}
	size = tcc_ffi_type_size(type);
	if (size == 0) {
		duckdb_free(copy);
		tcc_set_error(error_buf, size_error);
		return false;
	}
	*out_copy = copy;
	*out_type = type;
	*out_size = size;
	return true;
}

static bool tcc_parse_simple_type_token(const char *token, bool allow_void, tcc_ffi_type_t *out_type) {
	static const struct {
		const char *name;
		tcc_ffi_type_t type;
	} simple_tokens[] = {
	    {"bool", TCC_FFI_BOOL},         {"i8", TCC_FFI_I8},
	    {"u8", TCC_FFI_U8},             {"i16", TCC_FFI_I16},
	    {"u16", TCC_FFI_U16},           {"i32", TCC_FFI_I32},
	    {"u32", TCC_FFI_U32},           {"i64", TCC_FFI_I64},
	    {"u64", TCC_FFI_U64},           {"f32", TCC_FFI_F32},
	    {"f64", TCC_FFI_F64},           {"ptr", TCC_FFI_PTR},
	    {"varchar", TCC_FFI_VARCHAR},   {"blob", TCC_FFI_BLOB},
	    {"uuid", TCC_FFI_UUID},         {"date", TCC_FFI_DATE},
	    {"time", TCC_FFI_TIME},         {"timestamp", TCC_FFI_TIMESTAMP},
	    {"interval", TCC_FFI_INTERVAL}, {"decimal", TCC_FFI_DECIMAL},
	};
	size_t i;

	if (allow_void && tcc_equals_ci(token, "void")) {
		*out_type = TCC_FFI_VOID;
		return true;
	}
	for (i = 0; i < sizeof(simple_tokens) / sizeof(simple_tokens[0]); i++) {
		if (tcc_equals_ci(token, simple_tokens[i].name)) {
			*out_type = simple_tokens[i].type;
			return true;
		}
	}
	return false;
}

static bool tcc_parse_child_type_slice(const char *begin, size_t len, tcc_ffi_type_t *out_child) {
	char *copy;
	char *trimmed;
	size_t child_array_size = 0;
	bool ok;

	if (len == 0) {
		return false;
	}
	copy = (char *)duckdb_malloc(len + 1);
	if (!copy) {
		return false;
	}
	memcpy(copy, begin, len);
	copy[len] = '\0';
	trimmed = tcc_trim_inplace(copy);
	ok = tcc_parse_type_token(trimmed, false, out_child, &child_array_size) && *out_child != TCC_FFI_VOID;
	duckdb_free(copy);
	return ok;
}

static bool tcc_parse_list_angle_token(const char *token, size_t token_len, tcc_ffi_type_t *out_type) {
	tcc_ffi_type_t child_type = TCC_FFI_VOID;

	if (!tcc_is_wrapped_type_token(token, token_len, "list<")) {
		return false;
	}
	if (!tcc_parse_child_type_slice(token + 5, token_len - 6, &child_type)) {
		return false;
	}
	if (!tcc_ffi_list_type_from_child(child_type, out_type)) {
		*out_type = TCC_FFI_LIST;
	}
	return true;
}

static bool tcc_parse_fixed_array_suffix_token(const char *token, size_t token_len, tcc_ffi_type_t *out_type,
                                               size_t *out_array_size) {
	const char *lb;
	const char *p;
	uint64_t arr_n = 0;
	tcc_ffi_type_t child_type = TCC_FFI_VOID;

	if (token_len <= 3 || token[token_len - 1] != ']') {
		return false;
	}
	lb = strrchr(token, '[');
	if (!lb || lb >= token + token_len - 1 || lb[1] == ']') {
		return false;
	}
	for (p = lb + 1; p < token + token_len - 1; p++) {
		if (!isdigit((unsigned char)*p)) {
			return false;
		}
		arr_n = arr_n * 10 + (uint64_t)(*p - '0');
	}
	if (arr_n == 0 || arr_n > (uint64_t)SIZE_MAX ||
	    !tcc_parse_child_type_slice(token, (size_t)(lb - token), &child_type)) {
		return false;
	}
	if (!tcc_ffi_array_type_from_child(child_type, out_type)) {
		*out_type = TCC_FFI_ARRAY;
	}
	if (out_array_size) {
		*out_array_size = (size_t)arr_n;
	}
	return true;
}

static bool tcc_parse_list_suffix_token(const char *token, size_t token_len, tcc_ffi_type_t *out_type) {
	tcc_ffi_type_t child_type = TCC_FFI_VOID;

	if (token_len <= 2 || token[token_len - 2] != '[' || token[token_len - 1] != ']') {
		return false;
	}
	if (!tcc_parse_child_type_slice(token, token_len - 2, &child_type)) {
		return false;
	}
	if (!tcc_ffi_list_type_from_child(child_type, out_type)) {
		*out_type = TCC_FFI_LIST;
	}
	return true;
}

/* tcc_parse_type_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_type_token(const char *token, bool allow_void, tcc_ffi_type_t *out_type, size_t *out_array_size) {
	size_t token_len;

	if (!token || token[0] == '\0' || !out_type) {
		return false;
	}
	if (out_array_size) {
		*out_array_size = 0;
	}
	if (tcc_parse_simple_type_token(token, allow_void, out_type)) {
		return true;
	}
	token_len = strlen(token);
	if (tcc_is_wrapped_type_token(token, token_len, "struct<")) {
		*out_type = TCC_FFI_STRUCT;
		return true;
	}
	if (tcc_is_wrapped_type_token(token, token_len, "map<")) {
		*out_type = TCC_FFI_MAP;
		return true;
	}
	if (tcc_is_wrapped_type_token(token, token_len, "union<")) {
		*out_type = TCC_FFI_UNION;
		return true;
	}
	return tcc_parse_list_angle_token(token, token_len, out_type) ||
	       tcc_parse_fixed_array_suffix_token(token, token_len, out_type, out_array_size) ||
	       tcc_parse_list_suffix_token(token, token_len, out_type);
}

/* tcc_is_identifier_token: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* tcc_split_csv_tokens: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_split_csv_tokens(const char *csv, tcc_string_list_t *out_tokens, tcc_error_buffer_t *error_buf) {
	char *copy;
	char *cursor;
	if (!out_tokens) {
		tcc_set_error(error_buf, "token output is required");
		return false;
	}
	memset(out_tokens, 0, sizeof(*out_tokens));
	if (!csv || csv[0] == '\0') {
		return true;
	}
	copy = tcc_strdup(csv);
	if (!copy) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	cursor = copy;
	while (cursor) {
		char *token;
		char *part = tcc_next_top_level_part(&cursor, ',');
		token = tcc_trim_inplace(part);
		if (token[0] == '\0') {
			duckdb_free(copy);
			tcc_string_list_destroy(out_tokens);
			tcc_set_error(error_buf, "arg_types contains an empty token");
			return false;
		}
		if (!tcc_string_list_append(out_tokens, token)) {
			duckdb_free(copy);
			tcc_string_list_destroy(out_tokens);
			tcc_set_error(error_buf, "out of memory");
			return false;
		}
	}
	duckdb_free(copy);
	return true;
}

/* tcc_parse_c_field_spec_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_c_field_spec_token(const char *token, bool force_bitfield, tcc_c_field_spec_t *out_field,
                                         tcc_error_buffer_t *error_buf) {
	char *copy;
	char *type_part;
	char *opts_part = NULL;
	char *sep;
	char *name;
	char *type_token;
	tcc_ffi_type_t parsed_type = TCC_FFI_VOID;
	tcc_ffi_type_t child_type = TCC_FFI_VOID;
	size_t array_size = 0;
	bool is_bitfield = force_bitfield;
	if (!token || !out_field) {
		tcc_set_error(error_buf, "invalid c field specification");
		return false;
	}
	memset(out_field, 0, sizeof(*out_field));
	copy = tcc_strdup(token);
	if (!copy) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	sep = strchr(copy, ':');
	if (!sep) {
		duckdb_free(copy);
		tcc_set_error(error_buf, "field spec must use name:type or name:type:bitfield");
		return false;
	}
	*sep = '\0';
	type_part = sep + 1;
	sep = strchr(type_part, ':');
	if (sep) {
		*sep = '\0';
		opts_part = sep + 1;
	}
	name = tcc_trim_inplace(copy);
	type_token = tcc_trim_inplace(type_part);
	if (!tcc_is_identifier_token(name)) {
		duckdb_free(copy);
		tcc_set_error(error_buf, "field name must be a valid identifier");
		return false;
	}
	if (!tcc_parse_type_token(type_token, false, &parsed_type, &array_size)) {
		duckdb_free(copy);
		tcc_set_error(error_buf, "field type token is unsupported");
		return false;
	}
	if (opts_part) {
		char *opt = tcc_trim_inplace(opts_part);
		if (opt[0] != '\0') {
			if (tcc_equals_ci(opt, "bitfield")) {
				is_bitfield = true;
			} else {
				duckdb_free(copy);
				tcc_set_error(error_buf, "field option is unsupported (expected bitfield)");
				return false;
			}
		}
	}
	if (tcc_ffi_type_is_array(parsed_type)) {
		if (!tcc_ffi_array_child_type(parsed_type, &child_type) || array_size == 0) {
			duckdb_free(copy);
			tcc_set_error(error_buf, "array field type is invalid");
			return false;
		}
		if (is_bitfield) {
			duckdb_free(copy);
			tcc_set_error(error_buf, "bitfield option cannot be used with array fields");
			return false;
		}
		parsed_type = child_type;
	} else {
		array_size = 0;
	}
	if (!(tcc_ffi_type_is_fixed_width_scalar(parsed_type) || parsed_type == TCC_FFI_PTR)) {
		duckdb_free(copy);
		tcc_set_error(error_buf,
		              "fields currently support fixed-width scalar/pointer tokens only (no varchar/blob/nested)");
		return false;
	}
	out_field->name = tcc_strdup(name);
	out_field->type = parsed_type;
	out_field->array_size = array_size;
	out_field->is_bitfield = is_bitfield;
	duckdb_free(copy);
	if (!out_field->name) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	return true;
}

/* tcc_parse_c_field_specs: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_c_field_specs(const char *arg_types_csv, bool force_bitfield, tcc_c_field_list_t *out_fields,
                                    tcc_error_buffer_t *error_buf) {
	tcc_string_list_t tokens;
	idx_t i;
	if (!out_fields) {
		tcc_set_error(error_buf, "field output is required");
		return false;
	}
	memset(out_fields, 0, sizeof(*out_fields));
	if (!tcc_split_csv_tokens(arg_types_csv, &tokens, error_buf)) {
		return false;
	}
	if (tokens.count == 0) {
		tcc_string_list_destroy(&tokens);
		tcc_set_error(error_buf, "arg_types is required for c_struct/c_union/c_bitfield");
		return false;
	}
	for (i = 0; i < tokens.count; i++) {
		tcc_c_field_spec_t field;
		memset(&field, 0, sizeof(field));
		if (!tcc_parse_c_field_spec_token(tokens.items[i], force_bitfield, &field, error_buf) ||
		    !tcc_c_field_list_append(out_fields, &field)) {
			if (field.name) {
				duckdb_free(field.name);
			}
			tcc_string_list_destroy(&tokens);
			tcc_c_field_list_destroy(out_fields);
			if (!error_buf || error_buf->message[0] == '\0') {
				tcc_set_error(error_buf, "out of memory");
			}
			return false;
		}
	}
	tcc_string_list_destroy(&tokens);
	return true;
}

/* tcc_parse_c_enum_constants: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_c_enum_constants(const char *arg_types_csv, tcc_string_list_t *out_constants,
                                       tcc_error_buffer_t *error_buf) {
	idx_t i;
	if (!tcc_split_csv_tokens(arg_types_csv, out_constants, error_buf)) {
		return false;
	}
	if (out_constants->count == 0) {
		tcc_set_error(error_buf, "arg_types must list at least one enum constant");
		tcc_string_list_destroy(out_constants);
		return false;
	}
	for (i = 0; i < out_constants->count; i++) {
		if (!tcc_is_identifier_token(out_constants->items[i])) {
			tcc_set_error(error_buf, "enum constants must be valid identifiers");
			tcc_string_list_destroy(out_constants);
			return false;
		}
	}
	return true;
}

/* tcc_parse_composite_fields_inner: Shared parser for struct/union field lists.
   Parses semicolon-delimited `name:type` fields from an already-extracted inner string.
   If allow_auto_names is true (struct), unnamed fields get auto-generated names (f1, f2, ...).
   If false (union), a colon-separated name is required.
   Outputs are allocated arrays that caller must free on failure via the fail label pattern. */
static bool tcc_parse_composite_fields_inner(char *inner, bool allow_auto_names, const char *kind_label,
                                             int *out_count, char ***out_names, char ***out_tokens,
                                             tcc_ffi_type_t **out_types, size_t **out_sizes,
                                             tcc_error_buffer_t *error_buf) {
	char *cur = inner;
	int count = 0;
	int cap = 0;
	char **names = NULL;
	char **tokens = NULL;
	tcc_ffi_type_t *types = NULL;
	size_t *sizes = NULL;
	char err_msg[128];
	while (cur && *cur) {
		char *part = tcc_next_top_level_part(&cur, ';');
		char *colon;
		char *name_part;
		char *type_part;
		size_t part_len;
		tcc_ffi_type_t parsed_type = TCC_FFI_VOID;
		size_t parsed_array_size = 0;
		size_t parsed_size;
		char auto_name[32];
		char **new_names;
		char **new_tokens;
		tcc_ffi_type_t *new_types;
		size_t *new_sizes;
		while (*part && isspace((unsigned char)*part)) {
			part++;
		}
		part_len = strlen(part);
		while (part_len > 0 && isspace((unsigned char)part[part_len - 1])) {
			part[--part_len] = '\0';
		}
		if (part_len == 0) {
			snprintf(err_msg, sizeof(err_msg), "%s token contains empty field", kind_label);
			tcc_set_error(error_buf, err_msg);
			goto fail;
		}
		colon = tcc_find_top_level_char(part, ':');
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
				snprintf(err_msg, sizeof(err_msg), "%s field names must be identifiers", kind_label);
				tcc_set_error(error_buf, err_msg);
				goto fail;
			}
			if (type_part[0] == '\0') {
				snprintf(err_msg, sizeof(err_msg), "%s field type is missing", kind_label);
				tcc_set_error(error_buf, err_msg);
				goto fail;
			}
		} else if (allow_auto_names) {
			snprintf(auto_name, sizeof(auto_name), "f%d", count + 1);
			name_part = auto_name;
			type_part = part;
		} else {
			snprintf(err_msg, sizeof(err_msg), "%s member must use name:type", kind_label);
			tcc_set_error(error_buf, err_msg);
			goto fail;
		}
		if (!tcc_parse_type_token(type_part, false, &parsed_type, &parsed_array_size)) {
			snprintf(err_msg, sizeof(err_msg), "%s field type token is unsupported", kind_label);
			tcc_set_error(error_buf, err_msg);
			goto fail;
		}
		if (parsed_type == TCC_FFI_VOID) {
			snprintf(err_msg, sizeof(err_msg), "%s fields cannot use void", kind_label);
			tcc_set_error(error_buf, err_msg);
			goto fail;
		}
		parsed_size = tcc_ffi_type_size(parsed_type);
		if (parsed_size == 0) {
			snprintf(err_msg, sizeof(err_msg), "%s field has unsupported storage type", kind_label);
			tcc_set_error(error_buf, err_msg);
			goto fail;
		}
		if (count >= cap) {
			int new_cap = cap == 0 ? 4 : cap * 2;
			new_names = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
			new_tokens = (char **)duckdb_malloc(sizeof(char *) * (size_t)new_cap);
			new_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)new_cap);
			new_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)new_cap);
			if (!new_names || !new_tokens || !new_types || !new_sizes) {
				if (new_names) {
					duckdb_free(new_names);
				}
				if (new_tokens) {
					duckdb_free(new_tokens);
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
			memset(new_tokens, 0, sizeof(char *) * (size_t)new_cap);
			if (names && count > 0) {
				memcpy(new_names, names, sizeof(char *) * (size_t)count);
				memcpy(new_tokens, tokens, sizeof(char *) * (size_t)count);
				memcpy(new_types, types, sizeof(tcc_ffi_type_t) * (size_t)count);
				memcpy(new_sizes, sizes, sizeof(size_t) * (size_t)count);
				duckdb_free(names);
				duckdb_free(tokens);
				duckdb_free(types);
				duckdb_free(sizes);
			}
			names = new_names;
			tokens = new_tokens;
			types = new_types;
			sizes = new_sizes;
			cap = new_cap;
		}
		names[count] = tcc_strdup(name_part);
		tokens[count] = tcc_strdup(type_part);
		if (!names[count] || !tokens[count]) {
			tcc_set_error(error_buf, "out of memory");
			goto fail;
		}
		types[count] = parsed_type;
		sizes[count] = parsed_size;
		count++;
	}
	if (count <= 0) {
		snprintf(err_msg, sizeof(err_msg), "%s token requires at least one field", kind_label);
		tcc_set_error(error_buf, err_msg);
		goto fail;
	}
	*out_count = count;
	*out_names = names;
	*out_tokens = tokens;
	*out_types = types;
	*out_sizes = sizes;
	return true;
fail:
	if (names) {
		int i;
		for (i = 0; i < count; i++) {
			if (names[i]) {
				duckdb_free(names[i]);
			}
		}
		duckdb_free(names);
	}
	if (tokens) {
		int i;
		for (i = 0; i < count; i++) {
			if (tokens[i]) {
				duckdb_free(tokens[i]);
			}
		}
		duckdb_free(tokens);
	}
	if (types) {
		duckdb_free(types);
	}
	if (sizes) {
		duckdb_free(sizes);
	}
	*out_count = 0;
	*out_names = NULL;
	*out_tokens = NULL;
	*out_types = NULL;
	*out_sizes = NULL;
	return false;
}

/* tcc_parse_struct_meta_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_struct_meta_token(const char *token, tcc_ffi_struct_meta_t *out_meta,
                                        tcc_error_buffer_t *error_buf) {
	char *inner = NULL;
	bool ok;
	if (!token || !out_meta) {
		tcc_set_error(error_buf, "invalid struct token");
		return false;
	}
	memset(out_meta, 0, sizeof(*out_meta));
	inner = tcc_copy_wrapped_inner(token, "struct<", "struct token must use struct<...>", error_buf);
	if (!inner) {
		return false;
	}
	ok = tcc_parse_composite_fields_inner(inner, true, "struct", &out_meta->field_count, &out_meta->field_names,
	                                      &out_meta->field_tokens, &out_meta->field_types, &out_meta->field_sizes,
	                                      error_buf);
	duckdb_free(inner);
	return ok;
}

/* tcc_parse_map_meta_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_map_meta_token(const char *token, tcc_ffi_map_meta_t *out_meta, tcc_error_buffer_t *error_buf) {
	char *inner = NULL;
	char *sep;
	char *key_token;
	char *value_token;
	char *key_token_copy = NULL;
	char *value_token_copy = NULL;
	tcc_ffi_type_t key_type = TCC_FFI_VOID;
	tcc_ffi_type_t value_type = TCC_FFI_VOID;
	size_t key_size = 0;
	size_t value_size = 0;

	if (!token || !out_meta) {
		tcc_set_error(error_buf, "invalid map token");
		return false;
	}
	memset(out_meta, 0, sizeof(*out_meta));
	inner = tcc_copy_wrapped_inner(token, "map<", "map token must use map<key_type;value_type>", error_buf);
	if (!inner) {
		return false;
	}
	sep = tcc_find_top_level_char(inner, ';');
	if (!sep || tcc_find_top_level_char(sep + 1, ';')) {
		tcc_set_error(error_buf, "map token must use exactly one ';' separator");
		goto fail;
	}
	*sep = '\0';
	key_token = tcc_trim_token_inplace(inner);
	value_token = tcc_trim_token_inplace(sep + 1);
	if (key_token[0] == '\0' || value_token[0] == '\0') {
		tcc_set_error(error_buf, "map token key/value type is missing");
		goto fail;
	}
	if (!tcc_parse_sized_type_copy(key_token, &key_token_copy, &key_type, &key_size,
	                               "map key/value contains unsupported type token",
	                               "map key/value has unsupported storage type", error_buf) ||
	    !tcc_parse_sized_type_copy(value_token, &value_token_copy, &value_type, &value_size,
	                               "map key/value contains unsupported type token",
	                               "map key/value has unsupported storage type", error_buf)) {
		goto fail;
	}
	out_meta->key_token = key_token_copy;
	out_meta->value_token = value_token_copy;
	out_meta->key_type = key_type;
	out_meta->value_type = value_type;
	out_meta->key_size = key_size;
	out_meta->value_size = value_size;
	duckdb_free(inner);
	return true;

fail:
	if (key_token_copy) {
		duckdb_free(key_token_copy);
	}
	if (value_token_copy) {
		duckdb_free(value_token_copy);
	}
	if (inner) {
		duckdb_free(inner);
	}
	return false;
}

/* tcc_parse_union_meta_token: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_parse_union_meta_token(const char *token, tcc_ffi_union_meta_t *out_meta,
                                       tcc_error_buffer_t *error_buf) {
	char *inner = NULL;
	bool ok;
	if (!token || !out_meta) {
		tcc_set_error(error_buf, "invalid union token");
		return false;
	}
	memset(out_meta, 0, sizeof(*out_meta));
	inner = tcc_copy_wrapped_inner(token, "union<", "union token must use union<name:type;...>", error_buf);
	if (!inner) {
		return false;
	}
	ok = tcc_parse_composite_fields_inner(inner, false, "union", &out_meta->member_count, &out_meta->member_names,
	                                      &out_meta->member_tokens, &out_meta->member_types, &out_meta->member_sizes,
	                                      error_buf);
	duckdb_free(inner);
	return ok;
}

/* tcc_typedesc_destroy: Type-system conversion/parsing helper. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_typedesc_destroy(tcc_typedesc_t *desc) {
	idx_t i;
	if (!desc) {
		return;
	}
	if (desc->token) {
		duckdb_free(desc->token);
	}
	if (desc->kind == TCC_TYPEDESC_LIST || desc->kind == TCC_TYPEDESC_ARRAY) {
		tcc_typedesc_destroy(desc->as.list_like.child);
	} else if (desc->kind == TCC_TYPEDESC_STRUCT) {
		if (desc->as.struct_like.fields) {
			for (i = 0; i < desc->as.struct_like.count; i++) {
				if (desc->as.struct_like.fields[i].name) {
					duckdb_free(desc->as.struct_like.fields[i].name);
				}
				tcc_typedesc_destroy(desc->as.struct_like.fields[i].type);
			}
			duckdb_free(desc->as.struct_like.fields);
		}
	} else if (desc->kind == TCC_TYPEDESC_MAP) {
		tcc_typedesc_destroy(desc->as.map_like.key);
		tcc_typedesc_destroy(desc->as.map_like.value);
	} else if (desc->kind == TCC_TYPEDESC_UNION) {
		if (desc->as.union_like.members) {
			for (i = 0; i < desc->as.union_like.count; i++) {
				if (desc->as.union_like.members[i].name) {
					duckdb_free(desc->as.union_like.members[i].name);
				}
				tcc_typedesc_destroy(desc->as.union_like.members[i].type);
			}
			duckdb_free(desc->as.union_like.members);
		}
	}
	duckdb_free(desc);
}

/* tcc_typedesc_parse_token: Type-system conversion/parsing helper. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_typedesc_parse_token(const char *token, bool allow_void, tcc_typedesc_t **out_desc,
                                     tcc_error_buffer_t *error_buf) {
	tcc_typedesc_t *desc = NULL;
	tcc_ffi_type_t parsed = TCC_FFI_VOID;
	size_t array_size = 0;
	if (!token || !out_desc) {
		tcc_set_error(error_buf, "type token is required");
		return false;
	}
	*out_desc = NULL;
	if (!tcc_parse_type_token(token, allow_void, &parsed, &array_size)) {
		tcc_set_error(error_buf, "type token is unsupported");
		return false;
	}
	desc = (tcc_typedesc_t *)duckdb_malloc(sizeof(tcc_typedesc_t));
	if (!desc) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	memset(desc, 0, sizeof(*desc));
	desc->ffi_type = parsed;
	desc->array_size = array_size;
	desc->token = tcc_strdup(token);
	if (!desc->token) {
		duckdb_free(desc);
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	if (tcc_ffi_type_is_list(parsed)) {
		tcc_ffi_type_t child_ffi = TCC_FFI_VOID;
		const char *child_token = NULL;
		char *owned_child_token = NULL;
		desc->kind = TCC_TYPEDESC_LIST;
		if (tcc_ffi_list_child_type(parsed, &child_ffi)) {
			child_token = tcc_ffi_type_to_token(child_ffi);
		} else {
			size_t token_len_local = strlen(token);
			if (token_len_local > 6 && token[4] == '<' && token[token_len_local - 1] == '>') {
				owned_child_token = (char *)duckdb_malloc(token_len_local - 4);
				if (!owned_child_token) {
					tcc_typedesc_destroy(desc);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memcpy(owned_child_token, token + 5, token_len_local - 6);
				owned_child_token[token_len_local - 6] = '\0';
				child_token = tcc_trim_inplace(owned_child_token);
			} else if (token_len_local >= 5 && (token[0] == 'l' || token[0] == 'L') && (token[1] == 'i' || token[1] == 'I') &&
			           (token[2] == 's' || token[2] == 'S') && (token[3] == 't' || token[3] == 'T') && token[4] == '_') {
				child_token = token + 5;
			} else if (token_len_local > 2 && token[token_len_local - 2] == '[' && token[token_len_local - 1] == ']') {
				owned_child_token = (char *)duckdb_malloc(token_len_local - 1);
				if (!owned_child_token) {
					tcc_typedesc_destroy(desc);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memcpy(owned_child_token, token, token_len_local - 2);
				owned_child_token[token_len_local - 2] = '\0';
				child_token = tcc_trim_inplace(owned_child_token);
			}
		}
		if (!child_token || !tcc_typedesc_parse_token(child_token, false, &desc->as.list_like.child, error_buf)) {
			if (owned_child_token) {
				duckdb_free(owned_child_token);
			}
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (owned_child_token) {
			duckdb_free(owned_child_token);
		}
	} else if (tcc_ffi_type_is_array(parsed)) {
		tcc_ffi_type_t child_ffi = TCC_FFI_VOID;
		const char *child_token = NULL;
		char *owned_child_token = NULL;
		desc->kind = TCC_TYPEDESC_ARRAY;
		if (tcc_ffi_array_child_type(parsed, &child_ffi)) {
			child_token = tcc_ffi_type_to_token(child_ffi);
		} else {
			size_t token_len_local = strlen(token);
			const char *lb = strrchr(token, '[');
			if (lb && token_len_local > 3 && token[token_len_local - 1] == ']' && lb < token + token_len_local - 1) {
				size_t child_len = (size_t)(lb - token);
				owned_child_token = (char *)duckdb_malloc(child_len + 1);
				if (!owned_child_token) {
					tcc_typedesc_destroy(desc);
					tcc_set_error(error_buf, "out of memory");
					return false;
				}
				memcpy(owned_child_token, token, child_len);
				owned_child_token[child_len] = '\0';
				child_token = tcc_trim_inplace(owned_child_token);
			}
		}
		if (!child_token || !tcc_typedesc_parse_token(child_token, false, &desc->as.list_like.child, error_buf)) {
			if (owned_child_token) {
				duckdb_free(owned_child_token);
			}
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (owned_child_token) {
			duckdb_free(owned_child_token);
		}
	} else if (parsed == TCC_FFI_STRUCT) {
		tcc_ffi_struct_meta_t meta;
		idx_t i;
		desc->kind = TCC_TYPEDESC_STRUCT;
		memset(&meta, 0, sizeof(meta));
		if (!tcc_parse_struct_meta_token(token, &meta, error_buf)) {
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (meta.field_count > 0) {
			desc->as.struct_like.fields =
			    (tcc_typedesc_field_t *)duckdb_malloc(sizeof(tcc_typedesc_field_t) * (size_t)meta.field_count);
			if (!desc->as.struct_like.fields) {
				tcc_struct_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			memset(desc->as.struct_like.fields, 0, sizeof(tcc_typedesc_field_t) * (size_t)meta.field_count);
		}
		desc->as.struct_like.count = (idx_t)meta.field_count;
		for (i = 0; i < (idx_t)meta.field_count; i++) {
			desc->as.struct_like.fields[i].name = tcc_strdup(meta.field_names[i]);
			if (!desc->as.struct_like.fields[i].name ||
			    !tcc_typedesc_parse_token(meta.field_tokens[i], false, &desc->as.struct_like.fields[i].type, error_buf)) {
				tcc_struct_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				if (!error_buf || error_buf->message[0] == '\0') {
					tcc_set_error(error_buf, "out of memory");
				}
				return false;
			}
		}
		tcc_struct_meta_destroy(&meta);
	} else if (parsed == TCC_FFI_MAP) {
		tcc_ffi_map_meta_t meta;
		desc->kind = TCC_TYPEDESC_MAP;
		memset(&meta, 0, sizeof(meta));
		if (!tcc_parse_map_meta_token(token, &meta, error_buf) ||
		    !tcc_typedesc_parse_token(meta.key_token, false, &desc->as.map_like.key, error_buf) ||
		    !tcc_typedesc_parse_token(meta.value_token, false, &desc->as.map_like.value, error_buf)) {
			tcc_map_meta_destroy(&meta);
			tcc_typedesc_destroy(desc);
			return false;
		}
		tcc_map_meta_destroy(&meta);
	} else if (parsed == TCC_FFI_UNION) {
		tcc_ffi_union_meta_t meta;
		idx_t i;
		desc->kind = TCC_TYPEDESC_UNION;
		memset(&meta, 0, sizeof(meta));
		if (!tcc_parse_union_meta_token(token, &meta, error_buf)) {
			tcc_typedesc_destroy(desc);
			return false;
		}
		if (meta.member_count > 0) {
			desc->as.union_like.members =
			    (tcc_typedesc_field_t *)duckdb_malloc(sizeof(tcc_typedesc_field_t) * (size_t)meta.member_count);
			if (!desc->as.union_like.members) {
				tcc_union_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			memset(desc->as.union_like.members, 0, sizeof(tcc_typedesc_field_t) * (size_t)meta.member_count);
		}
		desc->as.union_like.count = (idx_t)meta.member_count;
		for (i = 0; i < (idx_t)meta.member_count; i++) {
			const char *member_token = meta.member_tokens ? meta.member_tokens[i] : NULL;
			desc->as.union_like.members[i].name = tcc_strdup(meta.member_names[i]);
			if (!member_token || !desc->as.union_like.members[i].name ||
			    !tcc_typedesc_parse_token(member_token, false, &desc->as.union_like.members[i].type, error_buf)) {
				tcc_union_meta_destroy(&meta);
				tcc_typedesc_destroy(desc);
				if (!error_buf || error_buf->message[0] == '\0') {
					tcc_set_error(error_buf, "out of memory");
				}
				return false;
			}
		}
		tcc_union_meta_destroy(&meta);
	} else {
		desc->kind = TCC_TYPEDESC_PRIMITIVE;
	}
	*out_desc = desc;
	return true;
}

/* tcc_parse_signature: Parser helper for signature, type, or helper-codegen grammar. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_parse_signature(const char *return_type, const char *arg_types_csv, tcc_ffi_type_t *out_return_type,
		                                size_t *out_return_array_size, tcc_ffi_type_t **out_arg_types,
		                                size_t **out_arg_array_sizes, tcc_ffi_struct_meta_t *out_return_struct_meta,
		                                tcc_ffi_map_meta_t *out_return_map_meta,
		                                tcc_ffi_union_meta_t *out_return_union_meta,
		                                tcc_ffi_struct_meta_t **out_arg_struct_metas,
		                                tcc_ffi_map_meta_t **out_arg_map_metas,
		                                tcc_ffi_union_meta_t **out_arg_union_metas, int *out_arg_count,
		                                tcc_error_buffer_t *error_buf) {
	tcc_typedesc_t *return_desc = NULL;
	tcc_typedesc_t *arg_desc = NULL;
	tcc_string_list_t arg_tokens;
	tcc_ffi_struct_meta_t return_struct_meta;
	tcc_ffi_map_meta_t return_map_meta;
	tcc_ffi_union_meta_t return_union_meta;
	tcc_ffi_type_t *arg_types = NULL;
	size_t *arg_array_sizes = NULL;
	tcc_ffi_struct_meta_t *arg_struct_metas = NULL;
	tcc_ffi_map_meta_t *arg_map_metas = NULL;
	tcc_ffi_union_meta_t *arg_union_metas = NULL;
	int argc = 0;
	idx_t i;
	memset(&arg_tokens, 0, sizeof(arg_tokens));
	memset(&return_struct_meta, 0, sizeof(return_struct_meta));
	memset(&return_map_meta, 0, sizeof(return_map_meta));
	memset(&return_union_meta, 0, sizeof(return_union_meta));
	if (!return_type || return_type[0] == '\0' || !out_return_type || !out_return_array_size || !out_arg_types ||
	    !out_arg_array_sizes || !out_return_struct_meta || !out_return_map_meta || !out_return_union_meta ||
	    !out_arg_struct_metas || !out_arg_map_metas || !out_arg_union_metas || !out_arg_count) {
		tcc_set_error(error_buf, "return_type is required");
		return false;
	}
	if (!arg_types_csv) {
		tcc_set_error(error_buf, "arg_types is required (use [] for no args)");
		return false;
	}
	if (!tcc_typedesc_parse_token(return_type, true, &return_desc, error_buf)) {
		tcc_set_error(error_buf, "return_type contains unsupported type token");
		return false;
	}
	if (return_desc->ffi_type == TCC_FFI_STRUCT &&
	    !tcc_parse_struct_meta_token(return_desc->token, &return_struct_meta, error_buf)) {
		goto fail;
	}
	if (return_desc->ffi_type == TCC_FFI_MAP &&
	    !tcc_parse_map_meta_token(return_desc->token, &return_map_meta, error_buf)) {
		goto fail;
	}
	if (return_desc->ffi_type == TCC_FFI_UNION &&
	    !tcc_parse_union_meta_token(return_desc->token, &return_union_meta, error_buf)) {
		goto fail;
	}
	if (!tcc_split_csv_tokens(arg_types_csv, &arg_tokens, error_buf)) {
		goto fail;
	}
	argc = (int)arg_tokens.count;
	if (argc > 0) {
		arg_types = (tcc_ffi_type_t *)duckdb_malloc(sizeof(tcc_ffi_type_t) * (size_t)argc);
		arg_array_sizes = (size_t *)duckdb_malloc(sizeof(size_t) * (size_t)argc);
		arg_struct_metas = (tcc_ffi_struct_meta_t *)duckdb_malloc(sizeof(tcc_ffi_struct_meta_t) * (size_t)argc);
		arg_map_metas = (tcc_ffi_map_meta_t *)duckdb_malloc(sizeof(tcc_ffi_map_meta_t) * (size_t)argc);
		arg_union_metas = (tcc_ffi_union_meta_t *)duckdb_malloc(sizeof(tcc_ffi_union_meta_t) * (size_t)argc);
		if (!arg_types || !arg_array_sizes || !arg_struct_metas || !arg_map_metas || !arg_union_metas) {
			tcc_set_error(error_buf, "out of memory");
			goto fail;
		}
		memset(arg_struct_metas, 0, sizeof(tcc_ffi_struct_meta_t) * (size_t)argc);
		memset(arg_map_metas, 0, sizeof(tcc_ffi_map_meta_t) * (size_t)argc);
		memset(arg_union_metas, 0, sizeof(tcc_ffi_union_meta_t) * (size_t)argc);
	}
	for (i = 0; i < (idx_t)argc; i++) {
		if (!tcc_typedesc_parse_token(arg_tokens.items[i], false, &arg_desc, error_buf)) {
			tcc_set_error(error_buf, "arg_types contains unsupported type token");
			goto fail;
		}
		arg_types[i] = arg_desc->ffi_type;
		arg_array_sizes[i] = arg_desc->array_size;
		if (arg_types[i] == TCC_FFI_STRUCT && !tcc_parse_struct_meta_token(arg_desc->token, &arg_struct_metas[i], error_buf)) {
			goto fail;
		}
		if (arg_types[i] == TCC_FFI_MAP && !tcc_parse_map_meta_token(arg_desc->token, &arg_map_metas[i], error_buf)) {
			goto fail;
		}
		if (arg_types[i] == TCC_FFI_UNION && !tcc_parse_union_meta_token(arg_desc->token, &arg_union_metas[i], error_buf)) {
			goto fail;
		}
		tcc_typedesc_destroy(arg_desc);
		arg_desc = NULL;
	}
	*out_return_type = return_desc->ffi_type;
	*out_return_array_size = return_desc->array_size;
	*out_arg_types = arg_types;
	*out_arg_array_sizes = arg_array_sizes;
	*out_return_struct_meta = return_struct_meta;
	*out_return_map_meta = return_map_meta;
	*out_return_union_meta = return_union_meta;
	*out_arg_struct_metas = arg_struct_metas;
	*out_arg_map_metas = arg_map_metas;
	*out_arg_union_metas = arg_union_metas;
	*out_arg_count = argc;
	tcc_string_list_destroy(&arg_tokens);
	tcc_typedesc_destroy(return_desc);
	return true;
fail:
	if (arg_desc) {
		tcc_typedesc_destroy(arg_desc);
	}
	tcc_typedesc_destroy(return_desc);
	tcc_string_list_destroy(&arg_tokens);
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
		tcc_map_meta_array_destroy(arg_map_metas, argc);
	}
	if (arg_union_metas) {
		tcc_union_meta_array_destroy(arg_union_metas, argc);
	}
	tcc_struct_meta_destroy(&return_struct_meta);
	tcc_map_meta_destroy(&return_map_meta);
	tcc_union_meta_destroy(&return_union_meta);
	return false;
}

