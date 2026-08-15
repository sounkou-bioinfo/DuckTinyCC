/*
 * DuckTinyCC wrapper/helper source generation and compile/load orchestration.
 *
 * Included by tcc_module.c after parser/type helpers.  Keeping this scoped separates string
 * emission and helper binding machinery from SQL mode dispatch and runtime vector bridging.
 */

/* ===== Section: Wrapper/Helper Code Generation ===== */

/* tcc_codegen_ret_null_check: Returns the C expression that tests whether a return value should be treated as NULL.
 * Returns NULL for VOID (special case) and for plain scalars (no null-check). */
static const char *tcc_codegen_ret_null_check(tcc_ffi_type_t ret_type) {
	if (ret_type == TCC_FFI_VOID) {
		return NULL;
	}
	if (ret_type == TCC_FFI_VARCHAR) {
		return "!result";
	}
	if (ret_type == TCC_FFI_BLOB) {
		return "result.len > 0 && !result.ptr";
	}
	if (ret_type == TCC_FFI_PTR) {
		return "!result";
	}
	if (tcc_ffi_type_is_list(ret_type) || tcc_ffi_type_is_array(ret_type)) {
		return "result.len > 0 && !result.ptr";
	}
	if (tcc_ffi_type_is_struct(ret_type)) {
		return "!result.field_ptrs || result.field_count == 0";
	}
	if (tcc_ffi_type_is_map(ret_type)) {
		return "result.len > 0 && (!result.key_ptr || !result.value_ptr)";
	}
	if (tcc_ffi_type_is_union(ret_type)) {
		return "!result.tag_ptr || !result.member_ptrs || result.member_count == 0";
	}
	/* Plain scalar: no null-check */
	return NULL;
}

static bool tcc_codegen_append_loader_header(tcc_text_buf_t *src, const char *ret_c_type,
                                              const char *target_symbol, const char *args_decl,
                                              bool emit_extern_decl) {
	if (!tcc_text_buf_appendf(
	        src,
	        "#include <stdint.h>\n"
	        "typedef struct _duckdb_connection *duckdb_connection;\n"
	        "extern _Bool ducktinycc_register_signature(duckdb_connection con, const char *name, void *fn_ptr, "
	        "const char *return_type, const char *arg_types_csv, const char *wrapper_mode, const char *stability);\n")) {
		return false;
	}
	/* Emit an extern forward declaration of the user function only when the user source
	 * is NOT bundled in the same compilation unit (i.e. the function is defined via a
	 * separate session source or symbol injection).  When user source IS present in the
	 * compilation unit it already provides the prototype; emitting an extern with our
	 * stdint.h-derived type (e.g. int64_t) would conflict if the user wrote a raw
	 * primitive (e.g. long long) — which on LP64 Linux resolves to a different type. */
	if (emit_extern_decl) {
		return tcc_text_buf_appendf(src, "extern %s %s(%s);\n", ret_c_type, target_symbol,
		                            args_decl ? args_decl : "");
	}
	return true;
}

static char *tcc_codegen_generate_wrapper_source(const char *module_symbol, const char *target_symbol,
                                                 const char *sql_name, const char *return_type,
                                                 const char *arg_types_csv, const char *wrapper_mode_token,
                                                 tcc_wrapper_mode_t wrapper_mode, const char *stability_token,
                                                 tcc_ffi_type_t ret_type, const tcc_ffi_type_t *arg_types,
                                                 int arg_count, bool emit_extern_decl) {
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
	    !stability_token || arg_count < 0) {
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
		if (!tcc_text_buf_appendf(&args_decl, "%s%s a%d", i == 0 ? "" : ", ", arg_c_type, i)) {
			ok = false;
			break;
		}
		if (arg_types[i] == TCC_FFI_PTR) {
			if (!tcc_text_buf_appendf(&row_unpack_lines,
			                          "  void *a%d = (void *)(uintptr_t)(*(unsigned long long *)args[%d]);\n", i, i) ||
			    !tcc_text_buf_appendf(&row_call_args, "%sa%d", i == 0 ? "" : ", ", i) ||
			    !tcc_text_buf_appendf(&batch_col_decls, "  unsigned long long *col%d_ptr = (unsigned long long *)arg_data[%d];\n",
			                          i, i) ||
			    !tcc_text_buf_appendf(&batch_call_args, "%s(void *)(uintptr_t)col%d_ptr[row]", i == 0 ? "" : ", ", i) ||
			    !tcc_text_buf_appendf(
			        &batch_null_checks,
			        "%s(arg_validity[%d] && ((arg_validity[%d][row >> 6] & (1ULL << (row & 63))) == 0))",
			        i == 0 ? "" : " || ", i, i)) {
				ok = false;
				break;
			}
		} else {
			if (!tcc_text_buf_appendf(&row_unpack_lines, "  %s a%d = *(%s *)args[%d];\n", arg_c_type, i, arg_c_type, i) ||
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
	}
	if (ok && arg_count == 0) {
		ok = tcc_text_buf_appendf(&args_decl, "void");
	}
	if (ok && wrapper_mode == TCC_WRAPPER_MODE_ROW) {
		ok = tcc_codegen_append_loader_header(&src, ret_c_type, target_symbol,
		                                      args_decl.data ? args_decl.data : "", emit_extern_decl);
		if (ok) {
			ok = tcc_text_buf_appendf(
			    &src,
			    "static _Bool %s(void **args, void *out_value, _Bool *out_is_null) {\n%s",
			    wrapper_name,
			    row_unpack_lines.data ? row_unpack_lines.data : "");
		}
		if (ok && ret_type == TCC_FFI_VOID) {
			ok = tcc_text_buf_appendf(&src,
			                          "  %s(%s);\n"
			                          "  if (out_is_null) { *out_is_null = 1; }\n"
			                          "  (void)out_value;\n"
			                          "  return 1;\n"
			                          "}\n",
			                          target_symbol, row_call_args.data ? row_call_args.data : "");
		} else if (ok && ret_type == TCC_FFI_PTR) {
			const char *nc = tcc_codegen_ret_null_check(ret_type);
			ok = tcc_text_buf_appendf(&src,
			                          "  void *result = (void *)%s(%s);\n"
			                          "  if (%s) {\n"
			                          "    if (out_is_null) { *out_is_null = 1; }\n"
			                          "    return 1;\n"
			                          "  }\n"
			                          "  *(unsigned long long *)out_value = (unsigned long long)(uintptr_t)result;\n"
			                          "  if (out_is_null) { *out_is_null = 0; }\n"
			                          "  return 1;\n"
			                          "}\n",
			                          target_symbol, row_call_args.data ? row_call_args.data : "", nc);
		} else if (ok) {
			const char *nc = tcc_codegen_ret_null_check(ret_type);
			ok = tcc_text_buf_appendf(&src, "  %s result = %s(%s);\n",
			                          ret_c_type, target_symbol, row_call_args.data ? row_call_args.data : "");
			if (ok && nc) {
				ok = tcc_text_buf_appendf(&src,
				                          "  if (%s) {\n"
				                          "    if (out_is_null) { *out_is_null = 1; }\n"
				                          "    return 1;\n"
				                          "  }\n", nc);
			}
			if (ok) {
				ok = tcc_text_buf_appendf(&src,
				                          "  *(%s *)out_value = result;\n"
				                          "  if (out_is_null) { *out_is_null = 0; }\n"
				                          "  return 1;\n"
				                          "}\n", ret_c_type);
			}
		}
	} else if (ok && wrapper_mode == TCC_WRAPPER_MODE_BATCH) {
		ok = tcc_codegen_append_loader_header(&src, ret_c_type, target_symbol,
		                                      args_decl.data ? args_decl.data : "", emit_extern_decl);
		if (ok) {
			ok = tcc_text_buf_appendf(
			    &src,
			    "static _Bool %s(void **arg_data, uint64_t **arg_validity, uint64_t count, void *out_data, uint64_t "
			    "*out_validity) {\n%s",
			    wrapper_name,
			    batch_col_decls.data ? batch_col_decls.data : "");
		}
		if (ok && ret_type == TCC_FFI_VOID) {
			ok = tcc_text_buf_appendf(&src, "  (void)out_data;\n");
		} else if (ok && ret_type == TCC_FFI_PTR) {
			ok = tcc_text_buf_appendf(&src, "  unsigned long long *out = (unsigned long long *)out_data;\n");
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
		} else if (ok && ret_type == TCC_FFI_PTR) {
			const char *nc = tcc_codegen_ret_null_check(ret_type);
			ok = tcc_text_buf_appendf(&src,
			                          "    void *result = (void *)%s(%s);\n"
			                          "    if (%s) {\n"
			                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
			                          "      continue;\n"
			                          "    }\n"
			                          "    out[row] = (unsigned long long)(uintptr_t)result;\n",
			                          target_symbol, batch_call_args.data ? batch_call_args.data : "", nc);
		} else if (ok) {
			const char *nc = tcc_codegen_ret_null_check(ret_type);
			if (nc) {
				ok = tcc_text_buf_appendf(&src,
				                          "    %s result = %s(%s);\n"
				                          "    if (%s) {\n"
				                          "      if (out_validity) { out_validity[row >> 6] &= ~(1ULL << (row & 63)); }\n"
				                          "      continue;\n"
				                          "    }\n"
				                          "    out[row] = result;\n",
				                          ret_c_type, target_symbol,
				                          batch_call_args.data ? batch_call_args.data : "", nc);
			} else {
				ok = tcc_text_buf_appendf(&src, "    out[row] = %s(%s);\n", target_symbol,
				                          batch_call_args.data ? batch_call_args.data : "");
			}
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
		                          "\"%s\", \"%s\");\n"
		                          "}\n",
		                          module_symbol, sql_name, wrapper_name, return_type, arg_types_csv,
		                          resolved_wrapper_mode, stability_token);
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

static char *tcc_codegen_build_compilation_unit(const char *user_source, const char *wrapper_loader_source) {
	char *compilation_unit_source;
	const char *prelude = ducktinycc_udf_abi_prelude;
	size_t n0;
	size_t n1;
	size_t n2;
	if (!wrapper_loader_source) {
		return NULL;
	}
	if (!user_source || user_source[0] == '\0') {
		n0 = strlen(prelude);
		n2 = strlen(wrapper_loader_source);
		compilation_unit_source = (char *)duckdb_malloc(n0 + n2 + 2);
		if (!compilation_unit_source) {
			return NULL;
		}
		memcpy(compilation_unit_source, prelude, n0);
		memcpy(compilation_unit_source + n0, wrapper_loader_source, n2);
		compilation_unit_source[n0 + n2] = '\0';
		return compilation_unit_source;
	}
	n0 = strlen(prelude);
	n1 = strlen(user_source);
	n2 = strlen(wrapper_loader_source);
	compilation_unit_source = (char *)duckdb_malloc(n0 + n1 + n2 + 3);
	if (!compilation_unit_source) {
		return NULL;
	}
	memcpy(compilation_unit_source, prelude, n0);
	memcpy(compilation_unit_source + n0, user_source, n1);
	compilation_unit_source[n0 + n1] = '\n';
	memcpy(compilation_unit_source + n0 + n1 + 1, wrapper_loader_source, n2);
	compilation_unit_source[n0 + n1 + 1 + n2] = '\0';
	return compilation_unit_source;
}

/* tcc_helper_binding_list_add_prefixed: Growable container utility used by parsing/codegen flows. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_helper_binding_list_add_prefixed(tcc_helper_binding_list_t *bindings, const char *prefix,
                                                 const char *suffix, const char *return_type,
                                                 const char *arg_types_csv, const char *stability) {
	char symbol[512];
	char sql_name[512];
	int n_symbol;
	int n_sql;
	if (!bindings || !prefix || !suffix || !return_type || !arg_types_csv || !stability) {
		return false;
	}
	n_symbol = snprintf(symbol, sizeof(symbol), "%s_%s", prefix, suffix);
	n_sql = snprintf(sql_name, sizeof(sql_name), "%s_%s", prefix, suffix);
	if (n_symbol < 0 || n_sql < 0 || (size_t)n_symbol >= sizeof(symbol) || (size_t)n_sql >= sizeof(sql_name)) {
		return false;
	}
	return tcc_helper_binding_list_add(bindings, symbol, sql_name, return_type, arg_types_csv, stability);
}

/* tcc_format_cstr: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_format_cstr(char *buf, size_t buf_size, const char *fmt, ...) {
	va_list args;
	int n;
	if (!buf || buf_size == 0 || !fmt) {
		return false;
	}
	va_start(args, fmt);
	n = vsnprintf(buf, buf_size, fmt, args);
	va_end(args);
	return n >= 0 && (size_t)n < buf_size;
}

static char *tcc_generate_c_composite_helpers_source(const char *kind_keyword, const char *type_name,
                                                     const char *prefix, const tcc_c_field_list_t *fields,
                                                     tcc_error_buffer_t *error_buf) {
	tcc_text_buf_t src = {0};
	idx_t i;
	bool ok;
	if (!kind_keyword || !type_name || !prefix || !fields) {
		tcc_set_error(error_buf, "invalid composite helper arguments");
		return NULL;
	}
	ok = tcc_text_buf_appendf(
	    &src,
	    "extern void *ducktinycc_helper_malloc(unsigned long long);\n"
	    "extern void ducktinycc_helper_free(void *);\n"
	    "/* Generated helpers allocate through the host libc domain. Pair %s_new with %s_free. */\n"
	    "#ifndef DUCKTINYCC_OFFSETOF\n"
	    "#define DUCKTINYCC_OFFSETOF(type, member) ((unsigned long long)((const char *)&(((type *)0)->member) - (const char *)0))\n"
	    "#endif\n"
	    "unsigned long long %s_sizeof(void){ return (unsigned long long)sizeof(%s %s); }\n"
	    "unsigned long long %s_alignof(void){ struct __ducktinycc_align_%s { char c; %s %s v; };"
	    " return (unsigned long long)(sizeof(struct __ducktinycc_align_%s) - sizeof(%s %s)); }\n"
	    "void *%s_new(void){ return ducktinycc_helper_malloc(sizeof(%s %s)); }\n"
	    "void %s_free(void *p){ if (p) ducktinycc_helper_free(p); }\n",
	    prefix, prefix, prefix, kind_keyword, type_name, prefix, prefix, kind_keyword, type_name, prefix,
	    kind_keyword, type_name, prefix, kind_keyword, type_name, prefix);
	if (!ok) {
		tcc_set_error(error_buf, "out of memory");
		tcc_text_buf_destroy(&src);
		return NULL;
	}
	for (i = 0; i < fields->count; i++) {
		const tcc_c_field_spec_t *field = &fields->items[i];
		const char *c_type = tcc_ffi_type_to_c_type_name(field->type);
		if (!c_type) {
			tcc_set_error(error_buf, "field type is unsupported for helper codegen");
			tcc_text_buf_destroy(&src);
			return NULL;
		}
		if (field->array_size > 0) {
			ok = tcc_text_buf_appendf(
			    &src,
			    "%s %s_get_%s_elt(void *p, unsigned long long idx){ %s out = (%s){0};"
			    " if (!p || idx >= %lluULL) return out; out = ((%s %s *)p)->%s[idx]; return out; }\n"
			    "void *%s_set_%s_elt(void *p, unsigned long long idx, %s value){"
			    " if (!p || idx >= %lluULL) return (void *)0; ((%s %s *)p)->%s[idx] = value; return p; }\n"
			    "unsigned long long %s_off_%s(void){ return DUCKTINYCC_OFFSETOF(%s %s, %s); }\n"
			    "void *%s_%s_addr(void *p){ if (!p) return (void *)0; return (void *)&((%s %s *)p)->%s[0]; }\n",
			    c_type, prefix, field->name, c_type, c_type, (unsigned long long)field->array_size, kind_keyword,
			    type_name, field->name, prefix, field->name, c_type, (unsigned long long)field->array_size,
			    kind_keyword, type_name, field->name, prefix, field->name, kind_keyword, type_name, field->name,
			    prefix, field->name, kind_keyword, type_name, field->name);
		} else {
			ok = tcc_text_buf_appendf(
			    &src,
			    "%s %s_get_%s(void *p){ %s out = (%s){0};"
			    " if (!p) return out; out = ((%s %s *)p)->%s; return out; }\n"
			    "void *%s_set_%s(void *p, %s value){ if (!p) return (void *)0; ((%s %s *)p)->%s = value; return p; }\n",
			    c_type, prefix, field->name, c_type, c_type, kind_keyword, type_name, field->name, prefix, field->name,
			    c_type, kind_keyword, type_name, field->name);
			if (ok && !field->is_bitfield) {
				ok = tcc_text_buf_appendf(
				    &src,
				    "unsigned long long %s_off_%s(void){ return DUCKTINYCC_OFFSETOF(%s %s, %s); }\n"
				    "void *%s_%s_addr(void *p){ if (!p) return (void *)0; return (void *)&((%s %s *)p)->%s; }\n",
				    prefix, field->name, kind_keyword, type_name, field->name, prefix, field->name, kind_keyword,
				    type_name, field->name);
			}
		}
		if (!ok) {
			tcc_set_error(error_buf, "out of memory");
			tcc_text_buf_destroy(&src);
			return NULL;
		}
	}
	if (!src.data) {
		tcc_set_error(error_buf, "out of memory");
		return NULL;
	}
	{
		char *out = tcc_strdup(src.data);
		tcc_text_buf_destroy(&src);
		if (!out) {
			tcc_set_error(error_buf, "out of memory");
		}
		return out;
	}
}

static char *tcc_generate_c_enum_helpers_source(const char *enum_name, const char *prefix,
                                                const tcc_string_list_t *constants, tcc_error_buffer_t *error_buf) {
	tcc_text_buf_t src = {0};
	idx_t i;
	if (!enum_name || !prefix || !constants) {
		tcc_set_error(error_buf, "invalid enum helper arguments");
		return NULL;
	}
	if (!tcc_text_buf_appendf(&src, "unsigned long long %s_sizeof(void){ return (unsigned long long)sizeof(enum %s); }\n",
	                          prefix, enum_name)) {
		tcc_set_error(error_buf, "out of memory");
		tcc_text_buf_destroy(&src);
		return NULL;
	}
	for (i = 0; i < constants->count; i++) {
		if (!tcc_text_buf_appendf(&src, "long long %s_%s(void){ return (long long)(%s); }\n", prefix,
		                          constants->items[i], constants->items[i])) {
			tcc_set_error(error_buf, "out of memory");
			tcc_text_buf_destroy(&src);
			return NULL;
		}
	}
	if (!src.data) {
		tcc_set_error(error_buf, "out of memory");
		return NULL;
	}
	{
		char *out = tcc_strdup(src.data);
		tcc_text_buf_destroy(&src);
		if (!out) {
			tcc_set_error(error_buf, "out of memory");
		}
		return out;
	}
}

/* tcc_build_c_composite_bindings: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_build_c_composite_bindings(const char *prefix, const tcc_c_field_list_t *fields,
                                           tcc_helper_binding_list_t *out_bindings, tcc_error_buffer_t *error_buf) {
	idx_t i;
	if (!prefix || !fields || !out_bindings) {
		tcc_set_error(error_buf, "invalid helper binding arguments");
		return false;
	}
#define TCC_ADD_BINDING_FORMAT(RET_TOKEN, ARG_CSV, STABILITY, SUFFIX_FMT, ...)                                           \
	do {                                                                                                                  \
		if (!tcc_format_cstr(suffix, sizeof(suffix), SUFFIX_FMT, __VA_ARGS__) ||                                         \
		    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, suffix, RET_TOKEN, ARG_CSV, STABILITY)) {       \
			tcc_set_error(error_buf, "out of memory");                                                                    \
			return false;                                                                                                 \
		}                                                                                                                 \
	} while (0)
	char suffix[512];
	if (!tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "sizeof", "u64", "", "consistent") ||
	    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "alignof", "u64", "", "consistent") ||
	    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "new", "ptr", "", "volatile") ||
	    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "free", "void", "ptr", "volatile")) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	for (i = 0; i < fields->count; i++) {
		const tcc_c_field_spec_t *field = &fields->items[i];
		const char *token = tcc_ffi_type_to_token(field->type);
		char arg_csv[128];
		if (!token) {
			tcc_set_error(error_buf, "field type token is unsupported for helper bindings");
			return false;
		}
		if (field->array_size > 0) {
			if (!tcc_format_cstr(arg_csv, sizeof(arg_csv), "ptr,u64")) {
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			TCC_ADD_BINDING_FORMAT(token, arg_csv, "volatile", "get_%s_elt", field->name);
			if (!tcc_format_cstr(arg_csv, sizeof(arg_csv), "ptr,u64,%s", token)) {
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			TCC_ADD_BINDING_FORMAT("ptr", arg_csv, "volatile", "set_%s_elt", field->name);
			TCC_ADD_BINDING_FORMAT("u64", "", "consistent", "off_%s", field->name);
			TCC_ADD_BINDING_FORMAT("ptr", "ptr", "consistent", "%s_addr", field->name);
		} else {
			TCC_ADD_BINDING_FORMAT(token, "ptr", "volatile", "get_%s", field->name);
			if (!tcc_format_cstr(arg_csv, sizeof(arg_csv), "ptr,%s", token)) {
				tcc_set_error(error_buf, "out of memory");
				return false;
			}
			TCC_ADD_BINDING_FORMAT("ptr", arg_csv, "volatile", "set_%s", field->name);
			if (!field->is_bitfield) {
				TCC_ADD_BINDING_FORMAT("u64", "", "consistent", "off_%s", field->name);
				TCC_ADD_BINDING_FORMAT("ptr", "ptr", "consistent", "%s_addr", field->name);
			}
		}
	}
#undef TCC_ADD_BINDING_FORMAT
	return true;
}

/* tcc_build_c_enum_bindings: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_build_c_enum_bindings(const char *prefix, const tcc_string_list_t *constants,
                                      tcc_helper_binding_list_t *out_bindings, tcc_error_buffer_t *error_buf) {
	idx_t i;
	char suffix[512];
	if (!prefix || !constants || !out_bindings) {
		tcc_set_error(error_buf, "invalid enum binding arguments");
		return false;
	}
	if (!tcc_helper_binding_list_add_prefixed(out_bindings, prefix, "sizeof", "u64", "", "consistent")) {
		tcc_set_error(error_buf, "out of memory");
		return false;
	}
	for (i = 0; i < constants->count; i++) {
		if (!tcc_format_cstr(suffix, sizeof(suffix), "%s", constants->items[i]) ||
		    !tcc_helper_binding_list_add_prefixed(out_bindings, prefix, suffix, "i64", "", "consistent")) {
			tcc_set_error(error_buf, "out of memory");
			return false;
		}
	}
	return true;
}

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* tcc_codegen_compile_and_load_module: Codegen helper for wrapper source assembly and compile/load orchestration. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static int tcc_codegen_compile_and_load_module(const char *runtime_path, tcc_module_state_t *state,
                                               const tcc_module_bind_data_t *bind, const char *sql_name,
                                               const char *target_symbol, tcc_registered_artifact_t **out_artifact,
                                               tcc_error_buffer_t *error_buf, char *out_module_symbol, size_t symbol_len) {
	tcc_codegen_source_ctx_t source_ctx;
	tcc_module_bind_data_t bind_copy;
	tcc_registered_artifact_t *artifact = NULL;

	if (!state || !bind || !sql_name || !target_symbol || !out_artifact || !error_buf || !out_module_symbol ||
	    symbol_len == 0) {
		tcc_set_error(error_buf, "invalid codegen compile arguments");
		return -1;
	}
	if (!state->connection) {
		tcc_set_error(error_buf, "no persistent extension connection available");
		return -1;
	}
	tcc_codegen_source_ctx_init(&source_ctx);
	if (!tcc_codegen_prepare_sources(state, bind, sql_name, target_symbol, &source_ctx, error_buf)) {
		tcc_codegen_source_ctx_destroy(&source_ctx);
		return -1;
	}
	snprintf(out_module_symbol, symbol_len, "%s", source_ctx.module_symbol);

	memset(&bind_copy, 0, sizeof(bind_copy));
	bind_copy = *bind;
	bind_copy.source = source_ctx.compilation_unit_source;
	if (tcc_build_module_artifact(runtime_path, state, &bind_copy, out_module_symbol, sql_name, &artifact, error_buf) !=
	    0) {
		tcc_codegen_source_ctx_destroy(&source_ctx);
		return -1;
	}
	tcc_codegen_source_ctx_destroy(&source_ctx);

	if (!artifact->module_init(state->connection)) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "generated module init returned false");
		return -1;
	}
	*out_artifact = artifact;
	return 0;
}
#endif

#ifndef DUCKTINYCC_WASM_UNSUPPORTED
/* tcc_compile_generated_binding: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: may allocate owned memory; caller or owning context must release via matching destroy path. */
static bool tcc_compile_generated_binding(const char *runtime_path, tcc_module_state_t *state, const char *source,
                                          const tcc_helper_binding_t *binding, tcc_error_buffer_t *error_buf) {
	tcc_module_bind_data_t generated_bind;
	tcc_registered_artifact_t *artifact = NULL;
	char module_symbol[128];
	if (!state || !binding || !source || !error_buf) {
		tcc_set_error(error_buf, "invalid generated binding arguments");
		return false;
	}
	memset(&generated_bind, 0, sizeof(generated_bind));
	memset(module_symbol, 0, sizeof(module_symbol));
	generated_bind.source = (char *)source;
	generated_bind.symbol = binding->symbol;
	generated_bind.sql_name = binding->sql_name;
	generated_bind.arg_types = binding->arg_types_csv;
	generated_bind.return_type = binding->return_type;
	generated_bind.wrapper_mode = "row";
	generated_bind.stability = binding->stability;
	if (tcc_codegen_compile_and_load_module(runtime_path, state, &generated_bind, binding->sql_name, binding->symbol,
	                                        &artifact, error_buf, module_symbol, sizeof(module_symbol)) != 0) {
		return false;
	}
	if (!tcc_registry_store_metadata(state, binding->sql_name, module_symbol, artifact->state_id, artifact)) {
		tcc_artifact_destroy(artifact);
		tcc_set_error(error_buf, "failed to store generated helper metadata");
		return false;
	}
	return true;
}
#endif

/* ===== Section: tcc_module Dispatcher ===== */
