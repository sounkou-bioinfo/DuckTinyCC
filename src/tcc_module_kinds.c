/*
 * DuckTinyCC aggregate and table function kinds.
 *
 * Included by tcc_module.c after the scalar registration path.  For
 * `kind := 'aggregate'` and `kind := 'table'` the code generator emits small C
 * adapters around the user's <symbol>_* functions; the generated module_init
 * hands them to ducktinycc_register_aggregate / ducktinycc_register_table,
 * whose DuckDB callbacks below drive them.  Input marshalling, result memory,
 * and per-row result copies reuse the scalar execution bridge.
 */

static const char *tcc_ffi_type_to_c_type_name(tcc_ffi_type_t type);
static const char *tcc_codegen_ret_null_check(tcc_ffi_type_t ret_type);

typedef enum {
	TCC_FUNCTION_KIND_SCALAR = 0,
	TCC_FUNCTION_KIND_AGGREGATE,
	TCC_FUNCTION_KIND_TABLE
} tcc_function_kind_t;

/*
 * Every aggregate state starts with a header holding its context pointer:
 * DuckDB's aggregate destructor receives states but no function info.  The
 * generated adapters address the user's state TCC_AGG_STATE_HDR bytes in.
 */
#define TCC_AGG_STATE_HDR 16

static bool tcc_parse_function_kind(const char *token, tcc_function_kind_t *out, tcc_error_buffer_t *err) {
	if (!token || token[0] == '\0' || tcc_equals_ci(token, "scalar")) {
		*out = TCC_FUNCTION_KIND_SCALAR;
		return true;
	}
	if (tcc_equals_ci(token, "aggregate")) {
		*out = TCC_FUNCTION_KIND_AGGREGATE;
		return true;
	}
	if (tcc_equals_ci(token, "table")) {
		*out = TCC_FUNCTION_KIND_TABLE;
		return true;
	}
	tcc_set_error(err, "function kind must be scalar, aggregate, or table");
	return false;
}

/* Results that are pointers into memory the C code may reuse; copied per row. */
static bool tcc_ffi_type_is_borrowed_result(tcc_ffi_type_t type) {
	return type == TCC_FFI_VARCHAR || type == TCC_FFI_BLOB || tcc_ffi_type_is_any_composite(type);
}

static bool tcc_ffi_type_is_table_arg(tcc_ffi_type_t type) {
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
	case TCC_FFI_VARCHAR:
		return true;
	default:
		return false;
	}
}

/* ===== Section: code generation ===== */

/* Column declarations, call arguments, and NULL tests for a chunk loop over arg_data. */
static bool tcc_kind_append_arg_columns(tcc_text_buf_t *decls, tcc_text_buf_t *call_args, tcc_text_buf_t *null_checks,
                                        const tcc_ffi_type_t *arg_types, int arg_count) {
	int i;

	for (i = 0; i < arg_count; i++) {
		const char *c_type = tcc_ffi_type_to_c_type_name(arg_types[i]);

		if (!c_type) {
			return false;
		}
		if (arg_types[i] == TCC_FFI_PTR) {
			if (!tcc_text_buf_appendf(decls, "  unsigned long long *col%d = (unsigned long long *)arg_data[%d];\n", i,
			                          i) ||
			    !tcc_text_buf_appendf(call_args, ", (void *)(uintptr_t)col%d[row]", i)) {
				return false;
			}
		} else if (!tcc_text_buf_appendf(decls, "  %s *col%d = (%s *)arg_data[%d];\n", c_type, i, c_type, i) ||
		           !tcc_text_buf_appendf(call_args, ", col%d[row]", i)) {
			return false;
		}
		if (!tcc_text_buf_appendf(null_checks,
		                          "%s(arg_validity[%d] && ((arg_validity[%d][row >> 6] & (1ULL << (row & 63))) == 0))",
		                          i == 0 ? "" : " || ", i, i)) {
			return false;
		}
	}
	return true;
}

/*
 * Adapters for kind aggregate.  User contract for symbol S:
 *   S_state                              state type (required)
 *   void S_init(S_state *)               optional; the state starts zeroed
 *   void S_step(S_state *, args...)      rows with a NULL argument are skipped
 *   void S_combine(S_state *into, S_state *from)
 *   int  S_final(S_state *, R *out)      0 gives SQL NULL
 *   void S_destroy(S_state *)            optional
 */
static char *tcc_codegen_generate_aggregate_source(const char *module_symbol, const char *sym, const char *sql_name,
                                                   const char *return_type, const char *arg_types_csv,
                                                   tcc_ffi_type_t ret_type, const tcc_ffi_type_t *arg_types,
                                                   int arg_count) {
	tcc_text_buf_t decls = {0};
	tcc_text_buf_t call_args = {0};
	tcc_text_buf_t null_checks = {0};
	tcc_text_buf_t src = {0};
	const char *ret_c_type = tcc_ffi_type_to_c_type_name(ret_type);
	const char *nc = tcc_codegen_ret_null_check(ret_type);
	bool emit = tcc_ffi_type_is_borrowed_result(ret_type);
	char *out = NULL;
	bool ok;

	if (!ret_c_type || ret_type == TCC_FFI_VOID ||
	    !tcc_kind_append_arg_columns(&decls, &call_args, &null_checks, arg_types, arg_count)) {
		goto done;
	}
	ok = tcc_text_buf_appendf(
	         &src,
	         "#include <stdint.h>\n"
	         "typedef struct _duckdb_connection *duckdb_connection;\n"
	         "extern _Bool ducktinycc_register_aggregate(duckdb_connection con, const char *name, "
	         "const char *return_type, const char *arg_types_csv, uint64_t state_size, void *init_fn, "
	         "void *step_fn, void *combine_fn, void *final_fn, void *destroy_fn);\n"
	         "extern void %s_init(%s_state *st) __attribute__((weak));\n"
	         "extern void %s_destroy(%s_state *st) __attribute__((weak));\n"
	         "#define __DTCC_ST(p) ((%s_state *)((char *)(p) + %d))\n"
	         "typedef %s __dtcc_ret_t;\n"
	         "static void __dtcc_agg_init(void *st) {\n"
	         "  if (%s_init) %s_init(__DTCC_ST(st));\n"
	         "}\n"
	         "static void __dtcc_agg_step(void **states, void **arg_data, uint64_t **arg_validity, uint64_t count) {\n"
	         "%s"
	         "  (void)arg_data;\n"
	         "  (void)arg_validity;\n"
	         "  for (uint64_t row = 0; row < count; row++) {\n",
	         sym, sym, sym, sym, sym, TCC_AGG_STATE_HDR, ret_c_type, sym, sym, decls.data ? decls.data : "") &&
	     (arg_count == 0 ||
	      tcc_text_buf_appendf(&src, "    if (%s) continue;\n", null_checks.data ? null_checks.data : "0")) &&
	     tcc_text_buf_appendf(
	         &src,
	         "    %s_step(__DTCC_ST(states[row])%s);\n"
	         "  }\n"
	         "}\n"
	         "static void __dtcc_agg_combine(void **src, void **dst, uint64_t count) {\n"
	         "  for (uint64_t i = 0; i < count; i++) %s_combine(__DTCC_ST(dst[i]), __DTCC_ST(src[i]));\n"
	         "}\n"
	         "static _Bool __dtcc_agg_final(void **states, uint64_t count, void *out_data, uint64_t *out_validity) {\n"
	         "  static const __dtcc_ret_t zero;\n"
	         "  __dtcc_ret_t *out = (__dtcc_ret_t *)out_data;\n"
	         "  for (uint64_t row = 0; row < count; row++) {\n"
	         "    __dtcc_ret_t result = zero;\n"
	         "    if (!%s_final(__DTCC_ST(states[row]), &result)%s%s%s) {\n"
	         "      out_validity[row >> 6] &= ~(1ULL << (row & 63));\n"
	         "      continue;\n"
	         "    }\n"
	         "    out[row] = result;\n"
	         "%s"
	         "  }\n"
	         "  return 1;\n"
	         "}\n"
	         "static void __dtcc_agg_destroy(void **states, uint64_t count) {\n"
	         "  if (!%s_destroy) return;\n"
	         "  for (uint64_t i = 0; i < count; i++) %s_destroy(__DTCC_ST(states[i]));\n"
	         "}\n"
	         "_Bool %s(duckdb_connection con) {\n"
	         "  return ducktinycc_register_aggregate(con, \"%s\", \"%s\", \"%s\", (uint64_t)sizeof(%s_state) + %d, "
	         "(void *)__dtcc_agg_init, (void *)__dtcc_agg_step, (void *)__dtcc_agg_combine, "
	         "(void *)__dtcc_agg_final, (void *)__dtcc_agg_destroy);\n"
	         "}\n",
	         sym, call_args.data ? call_args.data : "", sym, sym, nc ? " || (" : "", nc ? nc : "", nc ? ")" : "",
	         emit ? "    if (!ducktinycc_batch_emit(row)) return 0;\n" : "", sym, sym, module_symbol, sql_name,
	         return_type, arg_types_csv, sym, TCC_AGG_STATE_HDR);
	if (ok && src.data) {
		out = tcc_strdup(src.data);
	}
done:
	tcc_text_buf_destroy(&decls);
	tcc_text_buf_destroy(&call_args);
	tcc_text_buf_destroy(&null_checks);
	tcc_text_buf_destroy(&src);
	return out;
}

/*
 * Adapters for kind table.  User contract for symbol S with columns C1..Cm
 * (fields of a struct<...> return_type, or one column named value):
 *   S_state                                   state type (required)
 *   void S_init(S_state *, args...)           required when there are arguments
 *   int  S_next(S_state *, C1 *, ..., Cm *)   fill one row and return 1, or return 0
 *   void S_destroy(S_state *)                 optional
 */
static char *tcc_codegen_generate_table_source(const char *module_symbol, const char *sym, const char *sql_name,
                                               const char *return_type, const char *arg_types_csv,
                                               const tcc_ffi_type_t *col_types, int col_count,
                                               const tcc_ffi_type_t *arg_types, int arg_count) {
	tcc_text_buf_t init_decl = {0};
	tcc_text_buf_t init_body = {0};
	tcc_text_buf_t col_decls = {0};
	tcc_text_buf_t col_zero = {0};
	tcc_text_buf_t next_args = {0};
	tcc_text_buf_t src = {0};
	bool emit = false;
	char *out = NULL;
	bool ok = true;
	int i;

	if (col_count < 1) {
		goto done;
	}
	if (arg_count == 0) {
		ok = tcc_text_buf_appendf(&init_decl, "extern void %s_init(%s_state *st) __attribute__((weak));\n", sym, sym) &&
		     tcc_text_buf_appendf(&init_body, "  (void)args;\n  if (%s_init) %s_init((%s_state *)st);\n", sym, sym,
		                          sym);
	} else {
		for (i = 0; ok && i < arg_count; i++) {
			const char *c_type = tcc_ffi_type_to_c_type_name(arg_types[i]);

			ok = c_type && tcc_text_buf_appendf(&init_body, "  %s a%d = *(%s *)args[%d];\n", c_type, i, c_type, i);
		}
		ok = ok && tcc_text_buf_appendf(&init_body, "  %s_init((%s_state *)st", sym, sym);
		for (i = 0; ok && i < arg_count; i++) {
			ok = tcc_text_buf_appendf(&init_body, ", a%d", i);
		}
		ok = ok && tcc_text_buf_appendf(&init_body, ");\n");
	}
	for (i = 0; ok && i < col_count; i++) {
		const char *c_type = tcc_ffi_type_to_c_type_name(col_types[i]);

		if (!c_type || col_types[i] == TCC_FFI_VOID) {
			ok = false;
			break;
		}
		emit = emit || tcc_ffi_type_is_borrowed_result(col_types[i]);
		ok = tcc_text_buf_appendf(&col_decls,
		                          "  typedef %s __dtcc_col%d_t;\n"
		                          "  static const __dtcc_col%d_t zero%d;\n"
		                          "  __dtcc_col%d_t *col%d = (__dtcc_col%d_t *)cols[%d];\n",
		                          c_type, i, i, i, i, i, i, i) &&
		     tcc_text_buf_appendf(&col_zero, "    col%d[row] = zero%d;\n", i, i) &&
		     tcc_text_buf_appendf(&next_args, ", &col%d[row]", i);
	}
	ok = ok &&
	     tcc_text_buf_appendf(
	         &src,
	         "#include <stdint.h>\n"
	         "typedef struct _duckdb_connection *duckdb_connection;\n"
	         "extern _Bool ducktinycc_register_table(duckdb_connection con, const char *name, "
	         "const char *return_type, const char *arg_types_csv, uint64_t state_size, void *init_fn, "
	         "void *fill_fn, void *destroy_fn);\n"
	         "%s"
	         "extern void %s_destroy(%s_state *st) __attribute__((weak));\n"
	         "static void __dtcc_tf_init(void *st, void **args) {\n"
	         "%s"
	         "}\n"
	         "static int64_t __dtcc_tf_fill(void *st, void **cols, uint64_t cap) {\n"
	         "%s"
	         "  uint64_t row;\n"
	         "  for (row = 0; row < cap; row++) {\n"
	         "%s"
	         "    if (!%s_next((%s_state *)st%s)) break;\n"
	         "%s"
	         "  }\n"
	         "  return (int64_t)row;\n"
	         "}\n"
	         "static void __dtcc_tf_destroy(void *st) {\n"
	         "  if (%s_destroy) %s_destroy((%s_state *)st);\n"
	         "}\n"
	         "_Bool %s(duckdb_connection con) {\n"
	         "  return ducktinycc_register_table(con, \"%s\", \"%s\", \"%s\", (uint64_t)sizeof(%s_state), "
	         "(void *)__dtcc_tf_init, (void *)__dtcc_tf_fill, (void *)__dtcc_tf_destroy);\n"
	         "}\n",
	         init_decl.data ? init_decl.data : "", sym, sym, init_body.data ? init_body.data : "", col_decls.data ? col_decls.data : "",
	         col_zero.data ? col_zero.data : "", sym, sym, next_args.data ? next_args.data : "",
	         emit ? "    if (!ducktinycc_batch_emit(row)) return -1;\n" : "", sym, sym, sym, module_symbol, sql_name,
	         return_type, arg_types_csv, sym);
	if (ok && src.data) {
		out = tcc_strdup(src.data);
	}
done:
	tcc_text_buf_destroy(&init_decl);
	tcc_text_buf_destroy(&init_body);
	tcc_text_buf_destroy(&col_decls);
	tcc_text_buf_destroy(&col_zero);
	tcc_text_buf_destroy(&next_args);
	tcc_text_buf_destroy(&src);
	return out;
}

/* ===== Section: aggregate runtime ===== */

typedef void (*tcc_agg_init_fn_t)(void *state);
typedef void (*tcc_agg_step_fn_t)(void **states, void **arg_data, uint64_t **arg_validity, uint64_t count);
typedef void (*tcc_agg_combine_fn_t)(void **src, void **dst, uint64_t count);
typedef _Bool (*tcc_agg_final_fn_t)(void **states, uint64_t count, void *out_data, uint64_t *out_validity);
typedef void (*tcc_agg_destroy_fn_t)(void **states, uint64_t count);

typedef struct {
	tcc_host_sig_ctx_t *sig;
	uint64_t state_size;
	tcc_agg_init_fn_t init;
	tcc_agg_step_fn_t step;
	tcc_agg_combine_fn_t combine;
	tcc_agg_final_fn_t final;
	tcc_agg_destroy_fn_t destroy;
} tcc_host_agg_ctx_t;

static void tcc_host_agg_ctx_destroy(void *ptr) {
	tcc_host_agg_ctx_t *ctx = (tcc_host_agg_ctx_t *)ptr;

	if (!ctx) {
		return;
	}
	tcc_host_sig_ctx_destroy(ctx->sig);
	duckdb_free(ctx);
}

static idx_t tcc_agg_state_size(duckdb_function_info info) {
	tcc_host_agg_ctx_t *ctx = (tcc_host_agg_ctx_t *)duckdb_aggregate_function_get_extra_info(info);

	return (idx_t)ctx->state_size;
}

static void tcc_agg_state_init(duckdb_function_info info, duckdb_aggregate_state state) {
	tcc_host_agg_ctx_t *ctx = (tcc_host_agg_ctx_t *)duckdb_aggregate_function_get_extra_info(info);

	memset(state, 0, (size_t)ctx->state_size);
	memcpy(state, &ctx, sizeof(ctx));
	ctx->init(state);
}

/*
 * No current call is set while S_step and S_combine run: result memory would
 * be freed after the chunk, so a state must never hold it.
 */
static void tcc_agg_update(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
	tcc_host_agg_ctx_t *ctx = (tcc_host_agg_ctx_t *)duckdb_aggregate_function_get_extra_info(info);
	tcc_exec_call_t call;

	tcc_exec_call_setup(&call, ctx->sig, input, NULL);
	if (tcc_exec_alloc_args(&call) && tcc_exec_build_input_columns(&call) && tcc_exec_prepare_batch_args(&call)) {
		ctx->step((void **)states, call.batch_arg_data, call.in_validity, (uint64_t)call.n);
	}
	tcc_exec_cleanup(&call);
	if (call.error) {
		duckdb_aggregate_function_set_error(info, call.error);
	}
}

static void tcc_agg_combine(duckdb_function_info info, duckdb_aggregate_state *source,
                            duckdb_aggregate_state *target, idx_t count) {
	tcc_host_agg_ctx_t *ctx = (tcc_host_agg_ctx_t *)duckdb_aggregate_function_get_extra_info(info);

	ctx->combine((void **)source, (void **)target, (uint64_t)count);
}

/*
 * Results land at result[offset + i].  Fixed-width values are written in
 * place; VARCHAR/BLOB/composites are copied per row through emit_row.  The
 * generated loop marks NULL rows in a private bitmap indexed from 0, which is
 * then applied to the vector (with NULL STRUCT/UNION children).
 */
static void tcc_agg_finalize(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_vector result,
                             idx_t count, idx_t offset) {
	tcc_host_agg_ctx_t *ctx = (tcc_host_agg_ctx_t *)duckdb_aggregate_function_get_extra_info(info);
	tcc_exec_call_t call;
	tcc_exec_call_t *prev_call;
	uint64_t *valid = NULL;
	uint64_t *vec_valid;
	idx_t words = (count + 63) / 64;
	idx_t row;

	if (count == 0) {
		return;
	}
	tcc_exec_call_setup(&call, ctx->sig, NULL, result);
	call.n = count;
	call.out_offset = offset;
	call.ret_size = tcc_ffi_type_size(ctx->sig->return_type);
	if (call.out_data && !tcc_ffi_type_is_borrowed_result(ctx->sig->return_type)) {
		call.out_data += (size_t)offset * call.ret_size;
	}
	valid = (uint64_t *)tcc_duckdb_calloc(words, sizeof(uint64_t));
	if (!valid || !tcc_exec_alloc_batch_output(&call)) {
		call.error = "ducktinycc out of memory";
		goto done;
	}
	memset(valid, 0xff, (size_t)words * sizeof(uint64_t));
	duckdb_vector_ensure_validity_writable(result);
	vec_valid = duckdb_vector_get_validity(result);
	for (row = 0; row < count; row++) {
		duckdb_validity_set_row_validity(vec_valid, offset + row, true);
	}
	call.out_validity = valid;
	call.emit_row = tcc_exec_emit_batch_row;
	prev_call = tcc_exec_current;
	tcc_exec_current = &call;
	if (!ctx->final((void **)source, (uint64_t)count, call.batch_out_ptr, valid) && !call.error) {
		call.error = "ducktinycc aggregate finalize failed";
	}
	tcc_exec_current = prev_call;
	if (!call.error) {
		for (row = 0; row < count; row++) {
			if (!duckdb_validity_row_is_valid(valid, row)) {
				duckdb_validity_set_row_validity(vec_valid, offset + row, false);
				tcc_vector_null_children(result, ctx->sig->return_desc, offset + row);
			}
		}
	}
done:
	if (valid) {
		duckdb_free(valid);
	}
	tcc_exec_cleanup(&call);
	if (call.error) {
		duckdb_aggregate_function_set_error(info, call.error);
	}
}

static void tcc_agg_destroy(duckdb_aggregate_state *states, idx_t count) {
	idx_t i;

	for (i = 0; i < count; i++) {
		tcc_host_agg_ctx_t *ctx;

		if (!states[i]) {
			continue;
		}
		memcpy(&ctx, states[i], sizeof(ctx));
		if (ctx) {
			ctx->destroy((void **)&states[i], 1);
		}
	}
}

static void tcc_aggregate_add_parameter(void *fn, duckdb_logical_type type) {
	duckdb_aggregate_function_add_parameter((duckdb_aggregate_function)fn, type);
}

/* Host symbol: called from a generated module_init for kind aggregate. */
static bool ducktinycc_register_aggregate(duckdb_connection con, const char *name, const char *return_type,
                                          const char *arg_types_csv, uint64_t state_size, void *init_fn,
                                          void *step_fn, void *combine_fn, void *final_fn, void *destroy_fn) {
	duckdb_aggregate_function fn = NULL;
	duckdb_logical_type ret = NULL;
	tcc_host_agg_ctx_t *ctx;
	tcc_error_buffer_t err;
	duckdb_state rc;

	memset(&err, 0, sizeof(err));
	if (!con || !name || name[0] == '\0' || state_size < TCC_AGG_STATE_HDR || !init_fn || !step_fn ||
	    !combine_fn || !final_fn || !destroy_fn) {
		return false;
	}
	ctx = (tcc_host_agg_ctx_t *)duckdb_malloc(sizeof(tcc_host_agg_ctx_t));
	if (!ctx) {
		return false;
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->sig = tcc_host_sig_ctx_create(return_type, arg_types_csv, &err);
	if (!ctx->sig) {
		duckdb_free(ctx);
		return false;
	}
	ctx->sig->wrapper_mode = TCC_WRAPPER_MODE_BATCH;
	ctx->state_size = state_size;
	ctx->init = (tcc_agg_init_fn_t)init_fn;
	ctx->step = (tcc_agg_step_fn_t)step_fn;
	ctx->combine = (tcc_agg_combine_fn_t)combine_fn;
	ctx->final = (tcc_agg_final_fn_t)final_fn;
	ctx->destroy = (tcc_agg_destroy_fn_t)destroy_fn;

	fn = duckdb_create_aggregate_function();
	ret = tcc_typedesc_create_logical_type(ctx->sig->return_desc);
	if (!fn || !ret || !tcc_host_sig_add_parameters(ctx->sig, fn, tcc_aggregate_add_parameter)) {
		if (ret) {
			duckdb_destroy_logical_type(&ret);
		}
		if (fn) {
			duckdb_destroy_aggregate_function(&fn);
		}
		tcc_host_agg_ctx_destroy(ctx);
		return false;
	}
	duckdb_aggregate_function_set_name(fn, name);
	duckdb_aggregate_function_set_return_type(fn, ret);
	duckdb_destroy_logical_type(&ret);
	duckdb_aggregate_function_set_functions(fn, tcc_agg_state_size, tcc_agg_state_init, tcc_agg_update,
	                                        tcc_agg_combine, tcc_agg_finalize);
	duckdb_aggregate_function_set_destructor(fn, tcc_agg_destroy);
	duckdb_aggregate_function_set_extra_info(fn, ctx, tcc_host_agg_ctx_destroy);
	rc = duckdb_register_aggregate_function(con, fn);
	duckdb_destroy_aggregate_function(&fn);
	return rc == DuckDBSuccess;
}

/* ===== Section: table runtime ===== */

typedef void (*tcc_tf_init_fn_t)(void *state, void **args);
typedef int64_t (*tcc_tf_fill_fn_t)(void *state, void **cols, uint64_t cap);
typedef void (*tcc_tf_destroy_fn_t)(void *state);

typedef struct {
	tcc_host_sig_ctx_t *sig;
	uint64_t state_size;
	tcc_tf_init_fn_t init;
	tcc_tf_fill_fn_t fill;
	tcc_tf_destroy_fn_t destroy;
	idx_t col_count;
	const tcc_typedesc_t **col_descs; /* borrowed from sig->return_desc */
	const char **col_names;           /* borrowed from sig->return_desc */
} tcc_host_table_ctx_t;

/* Argument values captured at bind: one 16-byte slot per argument. */
typedef struct {
	int arg_count;
	uint64_t *slots;
	void **arg_ptrs;
	char **strings;
} tcc_table_bind_t;

typedef struct {
	const tcc_host_table_ctx_t *ctx;
	void *state;
	bool done;
} tcc_table_init_t;

/* One fill call: output chunk plus scratch columns for borrowed-result types. */
typedef struct {
	const tcc_host_table_ctx_t *ctx;
	duckdb_data_chunk output;
	void **cols;
	void **scratch;
} tcc_table_fill_t;

static void tcc_host_table_ctx_destroy(void *ptr) {
	tcc_host_table_ctx_t *ctx = (tcc_host_table_ctx_t *)ptr;

	if (!ctx) {
		return;
	}
	if (ctx->col_descs) {
		duckdb_free((void *)ctx->col_descs);
	}
	if (ctx->col_names) {
		duckdb_free((void *)ctx->col_names);
	}
	tcc_host_sig_ctx_destroy(ctx->sig);
	duckdb_free(ctx);
}

static void tcc_table_bind_destroy(void *ptr) {
	tcc_table_bind_t *bind = (tcc_table_bind_t *)ptr;
	int i;

	if (!bind) {
		return;
	}
	if (bind->strings) {
		for (i = 0; i < bind->arg_count; i++) {
			if (bind->strings[i]) {
				duckdb_free(bind->strings[i]);
			}
		}
		duckdb_free(bind->strings);
	}
	if (bind->slots) {
		duckdb_free(bind->slots);
	}
	if (bind->arg_ptrs) {
		duckdb_free(bind->arg_ptrs);
	}
	duckdb_free(bind);
}

/* Store one constant argument, already cast by DuckDB to the declared type. */
static bool tcc_table_store_arg(tcc_table_bind_t *bind, int i, tcc_ffi_type_t type, duckdb_value v) {
	void *slot = &bind->slots[2 * i];

	switch (type) {
	case TCC_FFI_BOOL:
		*(bool *)slot = duckdb_get_bool(v);
		return true;
	case TCC_FFI_I8:
		*(int8_t *)slot = duckdb_get_int8(v);
		return true;
	case TCC_FFI_U8:
		*(uint8_t *)slot = duckdb_get_uint8(v);
		return true;
	case TCC_FFI_I16:
		*(int16_t *)slot = duckdb_get_int16(v);
		return true;
	case TCC_FFI_U16:
		*(uint16_t *)slot = duckdb_get_uint16(v);
		return true;
	case TCC_FFI_I32:
		*(int32_t *)slot = duckdb_get_int32(v);
		return true;
	case TCC_FFI_U32:
		*(uint32_t *)slot = duckdb_get_uint32(v);
		return true;
	case TCC_FFI_I64:
		*(int64_t *)slot = duckdb_get_int64(v);
		return true;
	case TCC_FFI_U64:
		*(uint64_t *)slot = duckdb_get_uint64(v);
		return true;
	case TCC_FFI_F32:
		*(float *)slot = duckdb_get_float(v);
		return true;
	case TCC_FFI_F64:
		*(double *)slot = duckdb_get_double(v);
		return true;
	case TCC_FFI_VARCHAR:
		bind->strings[i] = duckdb_get_varchar(v);
		*(const char **)slot = bind->strings[i];
		return bind->strings[i] != NULL;
	default:
		return false;
	}
}

static void tcc_table_bind(duckdb_bind_info info) {
	tcc_host_table_ctx_t *ctx = (tcc_host_table_ctx_t *)duckdb_bind_get_extra_info(info);
	tcc_table_bind_t *bind;
	char msg[128];
	idx_t c;
	int i;

	bind = (tcc_table_bind_t *)duckdb_malloc(sizeof(tcc_table_bind_t));
	if (!bind) {
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	bind->arg_count = ctx->sig->arg_count;
	if (bind->arg_count > 0) {
		bind->slots = (uint64_t *)tcc_duckdb_calloc((idx_t)bind->arg_count * 2, sizeof(uint64_t));
		bind->arg_ptrs = (void **)tcc_duckdb_calloc((idx_t)bind->arg_count, sizeof(void *));
		bind->strings = (char **)tcc_duckdb_calloc((idx_t)bind->arg_count, sizeof(char *));
		if (!bind->slots || !bind->arg_ptrs || !bind->strings) {
			tcc_table_bind_destroy(bind);
			duckdb_bind_set_error(info, "out of memory");
			return;
		}
	}
	for (i = 0; i < bind->arg_count; i++) {
		duckdb_value v = duckdb_bind_get_parameter(info, (idx_t)i);
		bool stored;

		if (!v || duckdb_is_null_value(v)) {
			if (v) {
				duckdb_destroy_value(&v);
			}
			snprintf(msg, sizeof(msg), "table function argument %d is NULL", i + 1);
			tcc_table_bind_destroy(bind);
			duckdb_bind_set_error(info, msg);
			return;
		}
		stored = tcc_table_store_arg(bind, i, ctx->sig->arg_types[i], v);
		duckdb_destroy_value(&v);
		if (!stored) {
			tcc_table_bind_destroy(bind);
			duckdb_bind_set_error(info, "unsupported table function argument");
			return;
		}
		bind->arg_ptrs[i] = &bind->slots[2 * i];
	}
	for (c = 0; c < ctx->col_count; c++) {
		duckdb_logical_type t = tcc_typedesc_create_logical_type(ctx->col_descs[c]);

		if (!t) {
			tcc_table_bind_destroy(bind);
			duckdb_bind_set_error(info, "unsupported table function column type");
			return;
		}
		duckdb_bind_add_result_column(info, ctx->col_names[c], t);
		duckdb_destroy_logical_type(&t);
	}
	duckdb_bind_set_bind_data(info, bind, tcc_table_bind_destroy);
}

static void tcc_table_init_destroy(void *ptr) {
	tcc_table_init_t *init = (tcc_table_init_t *)ptr;

	if (!init) {
		return;
	}
	if (init->state) {
		init->ctx->destroy(init->state);
		duckdb_free(init->state);
	}
	duckdb_free(init);
}

/* S_next keeps its position in one state, so a scan runs on one thread. */
static void tcc_table_init(duckdb_init_info info) {
	tcc_host_table_ctx_t *ctx = (tcc_host_table_ctx_t *)duckdb_init_get_extra_info(info);
	tcc_table_bind_t *bind = (tcc_table_bind_t *)duckdb_init_get_bind_data(info);
	tcc_table_init_t *init;

	init = (tcc_table_init_t *)duckdb_malloc(sizeof(tcc_table_init_t));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	memset(init, 0, sizeof(*init));
	init->ctx = ctx;
	init->state = duckdb_malloc(ctx->state_size > 0 ? (size_t)ctx->state_size : 1);
	if (!init->state) {
		duckdb_free(init);
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	memset(init->state, 0, ctx->state_size > 0 ? (size_t)ctx->state_size : 1);
	duckdb_init_set_max_threads(info, 1);
	ctx->init(init->state, bind->arg_ptrs);
	duckdb_init_set_init_data(info, init, tcc_table_init_destroy);
}

/* emit_row for table fills: copy row of every borrowed-result column. */
static bool tcc_table_emit_row(tcc_exec_call_t *call, idx_t row) {
	tcc_table_fill_t *fill = (tcc_table_fill_t *)call->emit_ctx;
	idx_t c;

	for (c = 0; c < fill->ctx->col_count; c++) {
		const tcc_typedesc_t *desc = fill->ctx->col_descs[c];
		duckdb_vector vec;

		if (!fill->scratch[c]) {
			continue;
		}
		vec = duckdb_data_chunk_get_vector(fill->output, c);
		if (desc->ffi_type == TCC_FFI_VARCHAR) {
			const char *v = ((const char **)fill->scratch[c])[row];

			if (!v) {
				(void)tcc_set_vector_row_validity(vec, row, false);
			} else {
				duckdb_vector_assign_string_element(vec, row, v);
			}
		} else if (desc->ffi_type == TCC_FFI_BLOB) {
			ducktinycc_blob_t b = ((ducktinycc_blob_t *)fill->scratch[c])[row];

			if (b.len > 0 && !b.ptr) {
				(void)tcc_set_vector_row_validity(vec, row, false);
			} else {
				duckdb_vector_assign_string_element_len(vec, row, (const char *)b.ptr, (idx_t)b.len);
			}
		} else if (!tcc_write_value_to_vector(vec, desc, row, fill->scratch[c], (uint64_t)row, NULL, &call->error)) {
			return false;
		}
	}
	return true;
}

static void tcc_table_function(duckdb_function_info info, duckdb_data_chunk output) {
	tcc_table_init_t *init = (tcc_table_init_t *)duckdb_function_get_init_data(info);
	const tcc_host_table_ctx_t *ctx = init->ctx;
	idx_t cap = duckdb_vector_size();
	tcc_exec_call_t call;
	tcc_exec_call_t *prev_call;
	tcc_table_fill_t fill;
	int64_t produced = 0;
	idx_t c;

	duckdb_data_chunk_set_size(output, 0);
	if (init->done) {
		return;
	}
	tcc_exec_call_setup(&call, ctx->sig, NULL, NULL);
	call.n = cap;
	memset(&fill, 0, sizeof(fill));
	fill.ctx = ctx;
	fill.output = output;
	fill.cols = (void **)tcc_duckdb_calloc(ctx->col_count, sizeof(void *));
	fill.scratch = (void **)tcc_duckdb_calloc(ctx->col_count, sizeof(void *));
	if (!fill.cols || !fill.scratch) {
		call.error = "ducktinycc out of memory";
		goto done;
	}
	for (c = 0; c < ctx->col_count; c++) {
		tcc_ffi_type_t type = ctx->col_descs[c]->ffi_type;
		duckdb_vector vec = duckdb_data_chunk_get_vector(output, c);

		if (tcc_ffi_type_is_borrowed_result(type)) {
			fill.scratch[c] = tcc_duckdb_calloc(cap, tcc_ffi_type_size(type));
			fill.cols[c] = fill.scratch[c];
		} else {
			fill.cols[c] = duckdb_vector_get_data(vec);
		}
		if (!fill.cols[c]) {
			call.error = "ducktinycc out of memory";
			goto done;
		}
	}
	call.emit_row = tcc_table_emit_row;
	call.emit_ctx = &fill;
	prev_call = tcc_exec_current;
	tcc_exec_current = &call;
	produced = ctx->fill(init->state, fill.cols, (uint64_t)cap);
	tcc_exec_current = prev_call;
	if (produced < 0) {
		if (!call.error) {
			call.error = "ducktinycc table function fill failed";
		}
		goto done;
	}
	if ((idx_t)produced < cap) {
		init->done = true;
	}
	duckdb_data_chunk_set_size(output, (idx_t)produced);

done:
	if (fill.scratch) {
		for (c = 0; c < ctx->col_count; c++) {
			if (fill.scratch[c]) {
				duckdb_free(fill.scratch[c]);
			}
		}
		duckdb_free(fill.scratch);
	}
	if (fill.cols) {
		duckdb_free(fill.cols);
	}
	tcc_exec_cleanup(&call);
	if (call.error) {
		init->done = true;
		duckdb_function_set_error(info, call.error);
	}
}

static void tcc_table_add_parameter(void *fn, duckdb_logical_type type) {
	duckdb_table_function_add_parameter((duckdb_table_function)fn, type);
}

/* Columns are the fields of a STRUCT return_type, or one column named value. */
static bool tcc_host_table_ctx_set_columns(tcc_host_table_ctx_t *ctx) {
	const tcc_typedesc_t *ret = ctx->sig->return_desc;
	idx_t c;

	ctx->col_count = ret->kind == TCC_TYPEDESC_STRUCT ? ret->as.struct_like.count : 1;
	ctx->col_descs = (const tcc_typedesc_t **)tcc_duckdb_calloc(ctx->col_count, sizeof(tcc_typedesc_t *));
	ctx->col_names = (const char **)tcc_duckdb_calloc(ctx->col_count, sizeof(char *));
	if (!ctx->col_descs || !ctx->col_names) {
		return false;
	}
	if (ret->kind != TCC_TYPEDESC_STRUCT) {
		ctx->col_descs[0] = ret;
		ctx->col_names[0] = "value";
		return ret->ffi_type != TCC_FFI_VOID;
	}
	for (c = 0; c < ctx->col_count; c++) {
		ctx->col_descs[c] = ret->as.struct_like.fields[c].type;
		ctx->col_names[c] = ret->as.struct_like.fields[c].name;
	}
	return true;
}

/* Host symbol: called from a generated module_init for kind table. */
static bool ducktinycc_register_table(duckdb_connection con, const char *name, const char *return_type,
                                      const char *arg_types_csv, uint64_t state_size, void *init_fn, void *fill_fn,
                                      void *destroy_fn) {
	duckdb_table_function tf = NULL;
	tcc_host_table_ctx_t *ctx;
	tcc_error_buffer_t err;
	duckdb_state rc;
	int i;

	memset(&err, 0, sizeof(err));
	if (!con || !name || name[0] == '\0' || !init_fn || !fill_fn || !destroy_fn) {
		return false;
	}
	ctx = (tcc_host_table_ctx_t *)duckdb_malloc(sizeof(tcc_host_table_ctx_t));
	if (!ctx) {
		return false;
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->sig = tcc_host_sig_ctx_create(return_type, arg_types_csv, &err);
	if (!ctx->sig || !tcc_host_table_ctx_set_columns(ctx)) {
		tcc_host_table_ctx_destroy(ctx);
		return false;
	}
	for (i = 0; i < ctx->sig->arg_count; i++) {
		if (!tcc_ffi_type_is_table_arg(ctx->sig->arg_types[i])) {
			tcc_host_table_ctx_destroy(ctx);
			return false;
		}
	}
	ctx->state_size = state_size;
	ctx->init = (tcc_tf_init_fn_t)init_fn;
	ctx->fill = (tcc_tf_fill_fn_t)fill_fn;
	ctx->destroy = (tcc_tf_destroy_fn_t)destroy_fn;

	tf = duckdb_create_table_function();
	if (!tf || !tcc_host_sig_add_parameters(ctx->sig, tf, tcc_table_add_parameter)) {
		if (tf) {
			duckdb_destroy_table_function(&tf);
		}
		tcc_host_table_ctx_destroy(ctx);
		return false;
	}
	duckdb_table_function_set_name(tf, name);
	duckdb_table_function_set_extra_info(tf, ctx, tcc_host_table_ctx_destroy);
	duckdb_table_function_set_bind(tf, tcc_table_bind);
	duckdb_table_function_set_init(tf, tcc_table_init);
	duckdb_table_function_set_function(tf, tcc_table_function);
	duckdb_table_function_supports_projection_pushdown(tf, false);
	rc = duckdb_register_table_function(con, tf);
	duckdb_destroy_table_function(&tf);
	return rc == DuckDBSuccess;
}
