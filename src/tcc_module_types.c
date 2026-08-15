/*
 * DuckTinyCC type grammar, FFI type mapping, and recursive vector bridges.
 *
 * Included by tcc_module.c after session/runtime helpers.  This keeps parser/type metadata
 * and vector descriptor marshalling together, because typedesc layout drives both.
 */

/* ===== Section: Type Grammar + FFI Type Mapping ===== */
/* X-macro mapping each scalar FFI type to its LIST and ARRAY compound variants. */
#define TCC_FFI_COMPOUND_SCALAR_MAP(X)                                                                                       \
	X(TCC_FFI_BOOL, TCC_FFI_LIST_BOOL, TCC_FFI_ARRAY_BOOL)                                                                \
	X(TCC_FFI_I8, TCC_FFI_LIST_I8, TCC_FFI_ARRAY_I8)                                                                      \
	X(TCC_FFI_U8, TCC_FFI_LIST_U8, TCC_FFI_ARRAY_U8)                                                                      \
	X(TCC_FFI_I16, TCC_FFI_LIST_I16, TCC_FFI_ARRAY_I16)                                                                    \
	X(TCC_FFI_U16, TCC_FFI_LIST_U16, TCC_FFI_ARRAY_U16)                                                                    \
	X(TCC_FFI_I32, TCC_FFI_LIST_I32, TCC_FFI_ARRAY_I32)                                                                    \
	X(TCC_FFI_U32, TCC_FFI_LIST_U32, TCC_FFI_ARRAY_U32)                                                                    \
	X(TCC_FFI_I64, TCC_FFI_LIST_I64, TCC_FFI_ARRAY_I64)                                                                    \
	X(TCC_FFI_U64, TCC_FFI_LIST_U64, TCC_FFI_ARRAY_U64)                                                                    \
	X(TCC_FFI_F32, TCC_FFI_LIST_F32, TCC_FFI_ARRAY_F32)                                                                    \
	X(TCC_FFI_F64, TCC_FFI_LIST_F64, TCC_FFI_ARRAY_F64)                                                                    \
	X(TCC_FFI_UUID, TCC_FFI_LIST_UUID, TCC_FFI_ARRAY_UUID)                                                                  \
	X(TCC_FFI_DATE, TCC_FFI_LIST_DATE, TCC_FFI_ARRAY_DATE)                                                                  \
	X(TCC_FFI_TIME, TCC_FFI_LIST_TIME, TCC_FFI_ARRAY_TIME)                                                                  \
	X(TCC_FFI_TIMESTAMP, TCC_FFI_LIST_TIMESTAMP, TCC_FFI_ARRAY_TIMESTAMP)                                                    \
	X(TCC_FFI_INTERVAL, TCC_FFI_LIST_INTERVAL, TCC_FFI_ARRAY_INTERVAL)                                                      \
	X(TCC_FFI_DECIMAL, TCC_FFI_LIST_DECIMAL, TCC_FFI_ARRAY_DECIMAL)

/* tcc_ffi_type_is_list: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_list(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_LIST:
#define TCC_IS_LIST_CASE(SCALAR, LIST, ARRAY) case LIST:
		TCC_FFI_COMPOUND_SCALAR_MAP(TCC_IS_LIST_CASE)
#undef TCC_IS_LIST_CASE
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_type_is_fixed_width_scalar: Type-system conversion/parsing helper. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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
	case TCC_FFI_PTR:
		return true;
	default:
		return false;
	}
}

/* Shared helper to free the parallel arrays in struct/union composite metadata. */
static void tcc_composite_meta_free_inner(int count, char **names, char **tokens, tcc_ffi_type_t *types, size_t *sizes) {
	int i;
	if (names) {
		for (i = 0; i < count; i++) {
			if (names[i]) {
				duckdb_free(names[i]);
			}
		}
		duckdb_free(names);
	}
	if (tokens) {
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
}

/* tcc_struct_meta_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_struct_meta_destroy(tcc_ffi_struct_meta_t *meta) {
	if (!meta) {
		return;
	}
	tcc_composite_meta_free_inner(meta->field_count, meta->field_names, meta->field_tokens, meta->field_types,
	                              meta->field_sizes);
	memset(meta, 0, sizeof(*meta));
}

/* tcc_struct_meta_array_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
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

/* tcc_map_meta_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_map_meta_destroy(tcc_ffi_map_meta_t *meta) {
	if (!meta) {
		return;
	}
	if (meta->key_token) {
		duckdb_free(meta->key_token);
	}
	if (meta->value_token) {
		duckdb_free(meta->value_token);
	}
	memset(meta, 0, sizeof(*meta));
}

/* tcc_map_meta_array_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_map_meta_array_destroy(tcc_ffi_map_meta_t *metas, int count) {
	int i;
	if (!metas || count <= 0) {
		if (metas) {
			duckdb_free(metas);
		}
		return;
	}
	for (i = 0; i < count; i++) {
		tcc_map_meta_destroy(&metas[i]);
	}
	duckdb_free(metas);
}

/* tcc_union_meta_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_union_meta_destroy(tcc_ffi_union_meta_t *meta) {
	if (!meta) {
		return;
	}
	tcc_composite_meta_free_inner(meta->member_count, meta->member_names, meta->member_tokens, meta->member_types,
	                              meta->member_sizes);
	memset(meta, 0, sizeof(*meta));
}

/* tcc_union_meta_array_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_union_meta_array_destroy(tcc_ffi_union_meta_t *metas, int count) {
	int i;
	if (!metas) {
		return;
	}
	for (i = 0; i < count; i++) {
		tcc_union_meta_destroy(&metas[i]);
	}
	duckdb_free(metas);
}

/* tcc_ffi_list_child_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_list_child_type(tcc_ffi_type_t list_type, tcc_ffi_type_t *out_child) {
	if (!out_child) {
		return false;
	}
	switch (list_type) {
#define TCC_LIST_CHILD_CASE(SCALAR, LIST, ARRAY) case LIST: *out_child = SCALAR; return true;
		TCC_FFI_COMPOUND_SCALAR_MAP(TCC_LIST_CHILD_CASE)
#undef TCC_LIST_CHILD_CASE
	default:
		return false;
	}
}

/* tcc_ffi_list_type_from_child: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_list_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_list_type) {
	if (!out_list_type) {
		return false;
	}
	switch (child_type) {
#define TCC_LIST_FROM_CHILD_CASE(SCALAR, LIST, ARRAY) case SCALAR: *out_list_type = LIST; return true;
		TCC_FFI_COMPOUND_SCALAR_MAP(TCC_LIST_FROM_CHILD_CASE)
#undef TCC_LIST_FROM_CHILD_CASE
	default:
		return false;
	}
}

/* tcc_ffi_type_is_array: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_array(tcc_ffi_type_t type) {
	switch (type) {
	case TCC_FFI_ARRAY:
#define TCC_IS_ARRAY_CASE(SCALAR, LIST, ARRAY) case ARRAY:
		TCC_FFI_COMPOUND_SCALAR_MAP(TCC_IS_ARRAY_CASE)
#undef TCC_IS_ARRAY_CASE
		return true;
	default:
		return false;
	}
}

/* tcc_ffi_type_is_struct: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_struct(tcc_ffi_type_t type) {
	return type == TCC_FFI_STRUCT;
}

/* tcc_ffi_type_is_map: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_map(tcc_ffi_type_t type) {
	return type == TCC_FFI_MAP;
}

/* tcc_ffi_type_is_union: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_type_is_union(tcc_ffi_type_t type) {
	return type == TCC_FFI_UNION;
}

/* tcc_ffi_array_child_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_array_child_type(tcc_ffi_type_t array_type, tcc_ffi_type_t *out_child) {
	if (!out_child) {
		return false;
	}
	switch (array_type) {
#define TCC_ARRAY_CHILD_CASE(SCALAR, LIST, ARRAY) case ARRAY: *out_child = SCALAR; return true;
		TCC_FFI_COMPOUND_SCALAR_MAP(TCC_ARRAY_CHILD_CASE)
#undef TCC_ARRAY_CHILD_CASE
	default:
		return false;
	}
}

/* tcc_ffi_array_type_from_child: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ffi_array_type_from_child(tcc_ffi_type_t child_type, tcc_ffi_type_t *out_array_type) {
	if (!out_array_type) {
		return false;
	}
	switch (child_type) {
#define TCC_ARRAY_FROM_CHILD_CASE(SCALAR, LIST, ARRAY) case SCALAR: *out_array_type = ARRAY; return true;
		TCC_FFI_COMPOUND_SCALAR_MAP(TCC_ARRAY_FROM_CHILD_CASE)
#undef TCC_ARRAY_FROM_CHILD_CASE
	default:
		return false;
	}
}

#define TCC_FFI_SCALAR_ROWS(X)                                                                                               \
	X(TCC_FFI_BOOL, "bool", "_Bool", DUCKDB_TYPE_BOOLEAN, 1)                                                                \
	X(TCC_FFI_I8, "i8", "int8_t", DUCKDB_TYPE_TINYINT, 1)                                                                   \
	X(TCC_FFI_U8, "u8", "uint8_t", DUCKDB_TYPE_UTINYINT, 1)                                                                 \
	X(TCC_FFI_I16, "i16", "int16_t", DUCKDB_TYPE_SMALLINT, 2)                                                               \
	X(TCC_FFI_U16, "u16", "uint16_t", DUCKDB_TYPE_USMALLINT, 2)                                                             \
	X(TCC_FFI_I32, "i32", "int32_t", DUCKDB_TYPE_INTEGER, 4)                                                                \
	X(TCC_FFI_U32, "u32", "uint32_t", DUCKDB_TYPE_UINTEGER, 4)                                                              \
	X(TCC_FFI_I64, "i64", "int64_t", DUCKDB_TYPE_BIGINT, 8)                                                                 \
	X(TCC_FFI_U64, "u64", "uint64_t", DUCKDB_TYPE_UBIGINT, 8)                                                               \
	X(TCC_FFI_F32, "f32", "float", DUCKDB_TYPE_FLOAT, 4)                                                                    \
	X(TCC_FFI_F64, "f64", "double", DUCKDB_TYPE_DOUBLE, 8)

/* tcc_ffi_type_size: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static size_t tcc_ffi_type_size(tcc_ffi_type_t type) {
	if (tcc_ffi_type_is_list(type)) {
		return sizeof(ducktinycc_list_t);
	}
	if (tcc_ffi_type_is_array(type)) {
		return sizeof(ducktinycc_array_t);
	}
	switch (type) {
#define TCC_FFI_SIZE_CASE(type_id, token_str, c_type_name, duckdb_type_id, width_bytes)                                     \
	case type_id:                                                                                                          \
		return width_bytes;
		TCC_FFI_SCALAR_ROWS(TCC_FFI_SIZE_CASE)
#undef TCC_FFI_SIZE_CASE
	case TCC_FFI_PTR:
		return sizeof(void *);
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
	case TCC_FFI_UNION:
		return sizeof(ducktinycc_union_t);
	case TCC_FFI_VOID:
	default:
		return 0;
	}
}

/* tcc_ffi_type_to_duckdb_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static duckdb_type tcc_ffi_type_to_duckdb_type(tcc_ffi_type_t type) {
	if (tcc_ffi_type_is_list(type)) {
		return DUCKDB_TYPE_LIST;
	}
	if (tcc_ffi_type_is_array(type)) {
		return DUCKDB_TYPE_ARRAY;
	}
	switch (type) {
	case TCC_FFI_VOID:
		/* Scalar UDFs need a concrete type; void returns are emitted as NULL BIGINT. */
		return DUCKDB_TYPE_BIGINT;
#define TCC_FFI_DUCKDB_CASE(type_id, token_str, c_type_name, duckdb_type_id, width_bytes)                                   \
	case type_id:                                                                                                          \
		return duckdb_type_id;
		TCC_FFI_SCALAR_ROWS(TCC_FFI_DUCKDB_CASE)
#undef TCC_FFI_DUCKDB_CASE
	case TCC_FFI_PTR:
		return DUCKDB_TYPE_UBIGINT;
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
	case TCC_FFI_UNION:
		return DUCKDB_TYPE_UNION;
	default:
		return DUCKDB_TYPE_INVALID;
	}
}

/* tcc_ffi_type_create_logical_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static duckdb_logical_type tcc_ffi_type_create_logical_type(tcc_ffi_type_t type, size_t array_size,
                                                             const tcc_ffi_struct_meta_t *struct_meta,
                                                             const tcc_ffi_map_meta_t *map_meta,
                                                             const tcc_ffi_union_meta_t *union_meta) {
	duckdb_type base_type;
	if (tcc_ffi_type_is_list(type)) {
		tcc_ffi_type_t child_type = TCC_FFI_VOID;
		duckdb_logical_type child_logical = NULL;
		duckdb_logical_type list_logical = NULL;
		if (!tcc_ffi_list_child_type(type, &child_type)) {
			return NULL;
		}
		child_logical = tcc_ffi_type_create_logical_type(child_type, 0, NULL, NULL, NULL);
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
		child_logical = tcc_ffi_type_create_logical_type(child_type, 0, NULL, NULL, NULL);
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
			tcc_ffi_type_t child_type = struct_meta->field_types[i];
			size_t child_array_size = 0;
			tcc_ffi_struct_meta_t child_struct_meta;
			tcc_ffi_map_meta_t child_map_meta;
			tcc_ffi_union_meta_t child_union_meta;
			child_types[i] = NULL;
			child_names[i] = struct_meta->field_names[i];
			memset(&child_struct_meta, 0, sizeof(child_struct_meta));
			memset(&child_map_meta, 0, sizeof(child_map_meta));
			memset(&child_union_meta, 0, sizeof(child_union_meta));
			if (struct_meta->field_tokens && struct_meta->field_tokens[i] && struct_meta->field_tokens[i][0] != '\0') {
				if (!tcc_parse_type_token(struct_meta->field_tokens[i], false, &child_type, &child_array_size)) {
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
				if (child_type == TCC_FFI_STRUCT &&
				    !tcc_parse_struct_meta_token(struct_meta->field_tokens[i], &child_struct_meta, NULL)) {
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
				if (child_type == TCC_FFI_MAP &&
				    !tcc_parse_map_meta_token(struct_meta->field_tokens[i], &child_map_meta, NULL)) {
					tcc_struct_meta_destroy(&child_struct_meta);
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
				if (child_type == TCC_FFI_UNION &&
				    !tcc_parse_union_meta_token(struct_meta->field_tokens[i], &child_union_meta, NULL)) {
					tcc_struct_meta_destroy(&child_struct_meta);
					tcc_map_meta_destroy(&child_map_meta);
					int j;
					for (j = 0; j < i; j++) {
						duckdb_destroy_logical_type(&child_types[j]);
					}
					duckdb_free(child_types);
					duckdb_free((void *)child_names);
					return NULL;
				}
			}
			child_types[i] =
			    tcc_ffi_type_create_logical_type(child_type, child_array_size, &child_struct_meta, &child_map_meta,
			                                     &child_union_meta);
			tcc_struct_meta_destroy(&child_struct_meta);
			tcc_map_meta_destroy(&child_map_meta);
			tcc_union_meta_destroy(&child_union_meta);
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
		tcc_ffi_type_t key_ffi_type;
		tcc_ffi_type_t value_ffi_type;
		size_t key_array_size = 0;
		size_t value_array_size = 0;
		tcc_ffi_struct_meta_t key_struct_meta;
		tcc_ffi_struct_meta_t value_struct_meta;
		tcc_ffi_map_meta_t key_map_meta;
		tcc_ffi_map_meta_t value_map_meta;
		tcc_ffi_union_meta_t key_union_meta;
		tcc_ffi_union_meta_t value_union_meta;
		if (!map_meta) {
			return NULL;
		}
		memset(&key_struct_meta, 0, sizeof(key_struct_meta));
		memset(&value_struct_meta, 0, sizeof(value_struct_meta));
		memset(&key_map_meta, 0, sizeof(key_map_meta));
		memset(&value_map_meta, 0, sizeof(value_map_meta));
		memset(&key_union_meta, 0, sizeof(key_union_meta));
		memset(&value_union_meta, 0, sizeof(value_union_meta));
		key_ffi_type = map_meta->key_type;
		value_ffi_type = map_meta->value_type;
		if (map_meta->key_token && map_meta->key_token[0] != '\0') {
			if (!tcc_parse_type_token(map_meta->key_token, false, &key_ffi_type, &key_array_size)) {
				return NULL;
			}
			if (key_ffi_type == TCC_FFI_STRUCT &&
			    !tcc_parse_struct_meta_token(map_meta->key_token, &key_struct_meta, NULL)) {
				return NULL;
			}
			if (key_ffi_type == TCC_FFI_MAP && !tcc_parse_map_meta_token(map_meta->key_token, &key_map_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				return NULL;
			}
			if (key_ffi_type == TCC_FFI_UNION &&
			    !tcc_parse_union_meta_token(map_meta->key_token, &key_union_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				return NULL;
			}
		}
		if (map_meta->value_token && map_meta->value_token[0] != '\0') {
			if (!tcc_parse_type_token(map_meta->value_token, false, &value_ffi_type, &value_array_size)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				return NULL;
			}
			if (value_ffi_type == TCC_FFI_STRUCT &&
			    !tcc_parse_struct_meta_token(map_meta->value_token, &value_struct_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				return NULL;
			}
			if (value_ffi_type == TCC_FFI_MAP &&
			    !tcc_parse_map_meta_token(map_meta->value_token, &value_map_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				tcc_struct_meta_destroy(&value_struct_meta);
				return NULL;
			}
			if (value_ffi_type == TCC_FFI_UNION &&
			    !tcc_parse_union_meta_token(map_meta->value_token, &value_union_meta, NULL)) {
				tcc_struct_meta_destroy(&key_struct_meta);
				tcc_map_meta_destroy(&key_map_meta);
				tcc_union_meta_destroy(&key_union_meta);
				tcc_struct_meta_destroy(&value_struct_meta);
				tcc_map_meta_destroy(&value_map_meta);
				return NULL;
			}
		}
		key_type = tcc_ffi_type_create_logical_type(key_ffi_type, key_array_size, &key_struct_meta, &key_map_meta,
		                                            &key_union_meta);
		value_type = tcc_ffi_type_create_logical_type(value_ffi_type, value_array_size, &value_struct_meta,
		                                              &value_map_meta, &value_union_meta);
		tcc_struct_meta_destroy(&key_struct_meta);
		tcc_struct_meta_destroy(&value_struct_meta);
		tcc_map_meta_destroy(&key_map_meta);
		tcc_map_meta_destroy(&value_map_meta);
		tcc_union_meta_destroy(&key_union_meta);
		tcc_union_meta_destroy(&value_union_meta);
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
	if (type == TCC_FFI_UNION) {
		duckdb_logical_type union_type = NULL;
		duckdb_logical_type *member_types = NULL;
		const char **member_names = NULL;
		int i;
		if (!union_meta || union_meta->member_count <= 0 || !union_meta->member_names || !union_meta->member_types) {
			return NULL;
		}
		member_types = (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)union_meta->member_count);
		member_names = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)union_meta->member_count);
		if (!member_types || !member_names) {
			if (member_types) {
				duckdb_free(member_types);
			}
			if (member_names) {
				duckdb_free((void *)member_names);
			}
			return NULL;
		}
		for (i = 0; i < union_meta->member_count; i++) {
			member_types[i] = NULL;
			member_names[i] = union_meta->member_names[i];
			member_types[i] = tcc_ffi_type_create_logical_type(union_meta->member_types[i], 0, NULL, NULL, NULL);
			if (!member_types[i]) {
				int j;
				for (j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&member_types[j]);
				}
				duckdb_free(member_types);
				duckdb_free((void *)member_names);
				return NULL;
			}
		}
		union_type = duckdb_create_union_type(member_types, member_names, (idx_t)union_meta->member_count);
		for (i = 0; i < union_meta->member_count; i++) {
			duckdb_destroy_logical_type(&member_types[i]);
		}
		duckdb_free(member_types);
		duckdb_free((void *)member_names);
		return union_type;
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

static duckdb_logical_type tcc_typedesc_create_named_logical_type(const tcc_typedesc_field_t *fields,
                                                                 idx_t count, bool is_union) {
	idx_t i;
	duckdb_logical_type *child_types = NULL;
	const char **child_names = NULL;
	duckdb_logical_type out = NULL;

	if (!fields || count <= 0) {
		return NULL;
	}
	child_types = (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)count);
	child_names = (const char **)duckdb_malloc(sizeof(const char *) * (size_t)count);
	if (!child_types || !child_names) {
		if (child_types) {
			duckdb_free(child_types);
		}
		if (child_names) {
			duckdb_free((void *)child_names);
		}
		return NULL;
	}
	for (i = 0; i < count; i++) {
		child_types[i] = tcc_typedesc_create_logical_type(fields[i].type);
		child_names[i] = fields[i].name;
		if (!child_types[i]) {
			idx_t j;
			for (j = 0; j < i; j++) {
				duckdb_destroy_logical_type(&child_types[j]);
			}
			duckdb_free(child_types);
			duckdb_free((void *)child_names);
			return NULL;
		}
	}
	out = is_union ? duckdb_create_union_type(child_types, child_names, count)
	               : duckdb_create_struct_type(child_types, child_names, count);
	for (i = 0; i < count; i++) {
		duckdb_destroy_logical_type(&child_types[i]);
	}
	duckdb_free(child_types);
	duckdb_free((void *)child_names);
	return out;
}

/* tcc_typedesc_create_logical_type: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static duckdb_logical_type tcc_typedesc_create_logical_type(const tcc_typedesc_t *desc) {
	duckdb_type base_type;
	if (!desc) {
		return NULL;
	}
	switch (desc->kind) {
	case TCC_TYPEDESC_LIST: {
		duckdb_logical_type child = tcc_typedesc_create_logical_type(desc->as.list_like.child);
		duckdb_logical_type out = NULL;
		if (!child) {
			return NULL;
		}
		out = duckdb_create_list_type(child);
		duckdb_destroy_logical_type(&child);
		return out;
	}
	case TCC_TYPEDESC_ARRAY: {
		duckdb_logical_type child = tcc_typedesc_create_logical_type(desc->as.list_like.child);
		duckdb_logical_type out = NULL;
		if (!child || desc->array_size == 0) {
			if (child) {
				duckdb_destroy_logical_type(&child);
			}
			return NULL;
		}
		out = duckdb_create_array_type(child, (idx_t)desc->array_size);
		duckdb_destroy_logical_type(&child);
		return out;
	}
	case TCC_TYPEDESC_STRUCT:
		return tcc_typedesc_create_named_logical_type(desc->as.struct_like.fields, desc->as.struct_like.count, false);
	case TCC_TYPEDESC_MAP: {
		duckdb_logical_type key_type = tcc_typedesc_create_logical_type(desc->as.map_like.key);
		duckdb_logical_type value_type = tcc_typedesc_create_logical_type(desc->as.map_like.value);
		duckdb_logical_type out = NULL;
		if (!key_type || !value_type) {
			if (key_type) {
				duckdb_destroy_logical_type(&key_type);
			}
			if (value_type) {
				duckdb_destroy_logical_type(&value_type);
			}
			return NULL;
		}
		out = duckdb_create_map_type(key_type, value_type);
		duckdb_destroy_logical_type(&key_type);
		duckdb_destroy_logical_type(&value_type);
		return out;
	}
	case TCC_TYPEDESC_UNION:
		return tcc_typedesc_create_named_logical_type(desc->as.union_like.members, desc->as.union_like.count, true);
	case TCC_TYPEDESC_PRIMITIVE:
	default:
		if (desc->ffi_type == TCC_FFI_DECIMAL) {
			return duckdb_create_decimal_type(18, 3);
		}
		base_type = tcc_ffi_type_to_duckdb_type(desc->ffi_type);
		if (base_type == DUCKDB_TYPE_INVALID) {
			return NULL;
		}
		return duckdb_create_logical_type(base_type);
	}
}

/* tcc_validity_set_all: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
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

/* Copies DuckDB varchar payload into `duckdb_malloc` memory.
 * Caller owns and must `duckdb_free` the returned buffer.
 */
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

/* Returns a borrowed view over a DuckDB varchar payload as blob bytes.
 * Lifetime is limited to the current vector/chunk scope.
 */
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

/* tcc_nested_struct_bridge_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_nested_struct_bridge_destroy(tcc_nested_struct_bridge_t *bridge) {
	idx_t i;
	if (!bridge) {
		return;
	}
	if (bridge->field_bridges && bridge->field_count > 0) {
		for (i = 0; i < bridge->field_count; i++) {
			if (bridge->field_bridges[i]) {
				tcc_nested_struct_bridge_destroy(bridge->field_bridges[i]);
			}
		}
		duckdb_free((void *)bridge->field_bridges);
	}
	if (bridge->field_ptrs) {
		duckdb_free((void *)bridge->field_ptrs);
	}
	if (bridge->field_validity) {
		duckdb_free((void *)bridge->field_validity);
	}
	if (bridge->rows) {
		duckdb_free((void *)bridge->rows);
	}
	if (bridge->row_validity_mask) {
		duckdb_free((void *)bridge->row_validity_mask);
	}
	duckdb_free((void *)bridge);
}

static tcc_nested_struct_bridge_t *tcc_build_struct_bridge_from_vector(duckdb_vector struct_vector,
                                                                       const tcc_ffi_struct_meta_t *meta, idx_t n,
                                                                       const char **out_error) {
	tcc_nested_struct_bridge_t *bridge = NULL;
	ducktinycc_struct_t *rows = NULL;
	const void **field_ptrs = NULL;
	const uint64_t **field_validity = NULL;
	tcc_nested_struct_bridge_t **field_bridges = NULL;
	uint64_t *row_validity_mask = NULL;
	const uint64_t *struct_validity;
	idx_t field_idx;
	idx_t row;
	if (out_error) {
		*out_error = NULL;
	}
	if (!struct_vector || !meta || meta->field_count <= 0 || !meta->field_types || !meta->field_sizes) {
		if (out_error) {
			*out_error = "ducktinycc invalid struct metadata";
		}
		return NULL;
	}
	bridge = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
	if (!bridge) {
		if (out_error) {
			*out_error = "ducktinycc out of memory";
		}
		return NULL;
	}
	memset(bridge, 0, sizeof(*bridge));
	bridge->kind = TCC_NESTED_BRIDGE_STRUCT;
	bridge->field_count = (idx_t)meta->field_count;
	if (n > 0) {
		rows = (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)n);
	}
	field_ptrs = (const void **)duckdb_malloc(sizeof(const void *) * (size_t)meta->field_count);
	field_validity = (const uint64_t **)duckdb_malloc(sizeof(const uint64_t *) * (size_t)meta->field_count);
	field_bridges =
	    (tcc_nested_struct_bridge_t **)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t *) * (size_t)meta->field_count);
	if ((n > 0 && !rows) || !field_ptrs || !field_validity || !field_bridges) {
		if (rows) {
			duckdb_free((void *)rows);
		}
		if (field_ptrs) {
			duckdb_free((void *)field_ptrs);
		}
		if (field_validity) {
			duckdb_free((void *)field_validity);
		}
		if (field_bridges) {
			duckdb_free((void *)field_bridges);
		}
		duckdb_free((void *)bridge);
		if (out_error) {
			*out_error = "ducktinycc out of memory";
		}
		return NULL;
	}
	memset(field_ptrs, 0, sizeof(const void *) * (size_t)meta->field_count);
	memset(field_validity, 0, sizeof(const uint64_t *) * (size_t)meta->field_count);
	memset(field_bridges, 0, sizeof(tcc_nested_struct_bridge_t *) * (size_t)meta->field_count);
	for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
		duckdb_vector child_vector = duckdb_struct_vector_get_child(struct_vector, field_idx);
		const uint64_t *child_row_validity = NULL;
		if (!child_vector) {
			if (out_error) {
				*out_error = "ducktinycc struct child vector missing";
			}
			goto fail;
		}
		if (meta->field_types[field_idx] == TCC_FFI_STRUCT) {
			tcc_ffi_struct_meta_t nested_meta;
			tcc_nested_struct_bridge_t *nested = NULL;
			memset(&nested_meta, 0, sizeof(nested_meta));
			if (!meta->field_tokens || !meta->field_tokens[field_idx] ||
			    !tcc_parse_struct_meta_token(meta->field_tokens[field_idx], &nested_meta, NULL)) {
				if (out_error) {
					*out_error = "ducktinycc nested struct metadata parse failed";
				}
				goto fail;
			}
			nested = tcc_build_struct_bridge_from_vector(child_vector, &nested_meta, n, out_error);
			tcc_struct_meta_destroy(&nested_meta);
			if (!nested) {
				if (out_error && !*out_error) {
					*out_error = "ducktinycc nested struct bridge failed";
				}
				goto fail;
			}
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)nested->rows;
			child_row_validity = nested->row_validity_mask ? (const uint64_t *)nested->row_validity_mask
			                                               : (const uint64_t *)duckdb_vector_get_validity(child_vector);
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		if (tcc_ffi_type_is_list(meta->field_types[field_idx])) {
			tcc_nested_struct_bridge_t *nested = NULL;
			ducktinycc_list_t *list_rows = NULL;
			duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(child_vector);
			duckdb_vector list_child_vector = duckdb_list_vector_get_child(child_vector);
			uint8_t *list_child_data;
			const uint64_t *list_child_validity;
			tcc_ffi_type_t list_child_type = TCC_FFI_VOID;
			size_t list_child_size = 0;
			if (!list_child_vector || !tcc_ffi_list_child_type(meta->field_types[field_idx], &list_child_type)) {
				if (out_error) {
					*out_error = "ducktinycc invalid list child type";
				}
				goto fail;
			}
			list_child_size = tcc_ffi_type_size(list_child_type);
			if (list_child_size == 0) {
				if (out_error) {
					*out_error = "ducktinycc invalid list child type size";
				}
				goto fail;
			}
			nested = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
			if (!nested) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			memset(nested, 0, sizeof(*nested));
			nested->kind = TCC_NESTED_BRIDGE_LIST;
			if (n > 0) {
				list_rows = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)n);
			}
			if (n > 0 && !list_rows) {
				duckdb_free((void *)nested);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			list_child_data = (uint8_t *)duckdb_vector_get_data(list_child_vector);
			list_child_validity = (const uint64_t *)duckdb_vector_get_validity(list_child_vector);
			child_row_validity = (const uint64_t *)duckdb_vector_get_validity(child_vector);
			for (row = 0; row < n; row++) {
				if (child_row_validity && !duckdb_validity_row_is_valid((uint64_t *)child_row_validity, row)) {
					list_rows[row].ptr = NULL;
					list_rows[row].validity = NULL;
					list_rows[row].offset = 0;
					list_rows[row].len = 0;
				} else {
					duckdb_list_entry entry = entries[row];
					list_rows[row].ptr =
					    list_child_data
					        ? (const void *)(list_child_data + ((size_t)entry.offset * list_child_size))
					        : NULL;
					list_rows[row].validity = list_child_validity;
					list_rows[row].offset = (uint64_t)entry.offset;
					list_rows[row].len = (uint64_t)entry.length;
				}
			}
			nested->rows = (ducktinycc_struct_t *)list_rows;
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)list_rows;
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		if (tcc_ffi_type_is_array(meta->field_types[field_idx])) {
			tcc_nested_struct_bridge_t *nested = NULL;
			ducktinycc_array_t *array_rows = NULL;
			duckdb_vector array_child_vector = duckdb_array_vector_get_child(child_vector);
			uint8_t *array_child_data;
			const uint64_t *array_child_validity;
			tcc_ffi_type_t array_child_type = TCC_FFI_VOID;
			size_t array_child_size = 0;
			size_t array_len = 0;
			tcc_ffi_type_t parsed_type = TCC_FFI_VOID;
			if (!meta->field_tokens || !meta->field_tokens[field_idx] ||
			    !tcc_parse_type_token(meta->field_tokens[field_idx], false, &parsed_type, &array_len) || array_len == 0 ||
			    !tcc_ffi_array_child_type(meta->field_types[field_idx], &array_child_type) || !array_child_vector) {
				if (out_error) {
					*out_error = "ducktinycc invalid array child type";
				}
				goto fail;
			}
			array_child_size = tcc_ffi_type_size(array_child_type);
			if (array_child_size == 0) {
				if (out_error) {
					*out_error = "ducktinycc invalid array child type size";
				}
				goto fail;
			}
			nested = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
			if (!nested) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			memset(nested, 0, sizeof(*nested));
			nested->kind = TCC_NESTED_BRIDGE_ARRAY;
			if (n > 0) {
				array_rows = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)n);
			}
			if (n > 0 && !array_rows) {
				duckdb_free((void *)nested);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			array_child_data = (uint8_t *)duckdb_vector_get_data(array_child_vector);
			array_child_validity = (const uint64_t *)duckdb_vector_get_validity(array_child_vector);
			child_row_validity = (const uint64_t *)duckdb_vector_get_validity(child_vector);
			for (row = 0; row < n; row++) {
				if (child_row_validity && !duckdb_validity_row_is_valid((uint64_t *)child_row_validity, row)) {
					array_rows[row].ptr = NULL;
					array_rows[row].validity = NULL;
					array_rows[row].offset = 0;
					array_rows[row].len = 0;
				} else {
					uint64_t off = (uint64_t)row * (uint64_t)array_len;
					array_rows[row].ptr =
					    array_child_data
					        ? (const void *)(array_child_data + ((size_t)off * array_child_size))
					        : NULL;
					array_rows[row].validity = array_child_validity;
					array_rows[row].offset = off;
					array_rows[row].len = (uint64_t)array_len;
				}
			}
			nested->rows = (ducktinycc_struct_t *)array_rows;
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)array_rows;
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		if (tcc_ffi_type_is_map(meta->field_types[field_idx])) {
			tcc_nested_struct_bridge_t *nested = NULL;
			ducktinycc_map_t *map_rows = NULL;
			tcc_ffi_map_meta_t map_meta;
			duckdb_list_entry *entries;
			duckdb_vector map_struct_vector;
			duckdb_vector map_key_vector;
			duckdb_vector map_value_vector;
			uint8_t *key_data;
			uint8_t *value_data;
			const uint64_t *key_validity;
			const uint64_t *value_validity;
			memset(&map_meta, 0, sizeof(map_meta));
			if (!meta->field_tokens || !meta->field_tokens[field_idx] ||
			    !tcc_parse_map_meta_token(meta->field_tokens[field_idx], &map_meta, NULL)) {
				if (out_error) {
					*out_error = "ducktinycc map metadata parse failed";
				}
				goto fail;
			}
			if (map_meta.key_size == 0 || map_meta.value_size == 0) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc invalid map metadata";
				}
				goto fail;
			}
			entries = (duckdb_list_entry *)duckdb_vector_get_data(child_vector);
			map_struct_vector = duckdb_list_vector_get_child(child_vector);
			if (!map_struct_vector) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc map child vector missing";
				}
				goto fail;
			}
			map_key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
			map_value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
			if (!map_key_vector || !map_value_vector) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc map key/value child vector missing";
				}
				goto fail;
			}
			nested = (tcc_nested_struct_bridge_t *)duckdb_malloc(sizeof(tcc_nested_struct_bridge_t));
			if (!nested) {
				tcc_map_meta_destroy(&map_meta);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			memset(nested, 0, sizeof(*nested));
			nested->kind = TCC_NESTED_BRIDGE_MAP;
			if (n > 0) {
				map_rows = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)n);
			}
			if (n > 0 && !map_rows) {
				tcc_map_meta_destroy(&map_meta);
				duckdb_free((void *)nested);
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			key_data = (uint8_t *)duckdb_vector_get_data(map_key_vector);
			value_data = (uint8_t *)duckdb_vector_get_data(map_value_vector);
			key_validity = (const uint64_t *)duckdb_vector_get_validity(map_key_vector);
			value_validity = (const uint64_t *)duckdb_vector_get_validity(map_value_vector);
			child_row_validity = (const uint64_t *)duckdb_vector_get_validity(child_vector);
			for (row = 0; row < n; row++) {
				if (child_row_validity && !duckdb_validity_row_is_valid((uint64_t *)child_row_validity, row)) {
					map_rows[row].key_ptr = NULL;
					map_rows[row].key_validity = NULL;
					map_rows[row].value_ptr = NULL;
					map_rows[row].value_validity = NULL;
					map_rows[row].offset = 0;
					map_rows[row].len = 0;
				} else {
					duckdb_list_entry entry = entries[row];
					map_rows[row].key_ptr =
					    key_data ? (const void *)(key_data + ((size_t)entry.offset * map_meta.key_size)) : NULL;
					map_rows[row].key_validity = key_validity;
					map_rows[row].value_ptr =
					    value_data ? (const void *)(value_data + ((size_t)entry.offset * map_meta.value_size)) : NULL;
					map_rows[row].value_validity = value_validity;
					map_rows[row].offset = (uint64_t)entry.offset;
					map_rows[row].len = (uint64_t)entry.length;
				}
			}
			tcc_map_meta_destroy(&map_meta);
			nested->rows = (ducktinycc_struct_t *)map_rows;
			field_bridges[field_idx] = nested;
			field_ptrs[field_idx] = (const void *)map_rows;
			field_validity[field_idx] = child_row_validity;
			continue;
		}
		field_ptrs[field_idx] = (const void *)duckdb_vector_get_data(child_vector);
		field_validity[field_idx] = (const uint64_t *)duckdb_vector_get_validity(child_vector);
	}
	struct_validity = (const uint64_t *)duckdb_vector_get_validity(struct_vector);
	if (!struct_validity && n > 0) {
		idx_t words = (n + 63) / 64;
		row_validity_mask = (uint64_t *)duckdb_malloc(sizeof(uint64_t) * (size_t)words);
		if (!row_validity_mask) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			goto fail;
		}
		tcc_validity_set_all(row_validity_mask, n, true);
		for (row = 0; row < n; row++) {
			bool any_valid = false;
			for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
				if (!field_validity[field_idx] ||
				    duckdb_validity_row_is_valid((uint64_t *)field_validity[field_idx], row)) {
					any_valid = true;
					break;
				}
			}
			if (!any_valid) {
				duckdb_validity_set_row_validity(row_validity_mask, row, false);
			}
		}
		struct_validity = (const uint64_t *)row_validity_mask;
	}
	for (row = 0; row < n; row++) {
		if (struct_validity && !duckdb_validity_row_is_valid((uint64_t *)struct_validity, row)) {
			rows[row].field_ptrs = NULL;
			rows[row].field_validity = NULL;
			rows[row].field_count = 0;
			rows[row].offset = 0;
		} else {
			rows[row].field_ptrs = field_ptrs;
			rows[row].field_validity = field_validity;
			rows[row].field_count = (uint64_t)meta->field_count;
			rows[row].offset = (uint64_t)row;
		}
	}
	bridge->rows = rows;
	bridge->field_ptrs = field_ptrs;
	bridge->field_validity = field_validity;
	bridge->field_bridges = field_bridges;
	bridge->row_validity_mask = row_validity_mask;
	return bridge;
fail:
	if (row_validity_mask) {
		duckdb_free((void *)row_validity_mask);
	}
	if (field_bridges) {
		for (field_idx = 0; field_idx < (idx_t)meta->field_count; field_idx++) {
			if (field_bridges[field_idx]) {
				tcc_nested_struct_bridge_destroy(field_bridges[field_idx]);
			}
		}
		duckdb_free((void *)field_bridges);
	}
	if (field_ptrs) {
		duckdb_free((void *)field_ptrs);
	}
	if (field_validity) {
		duckdb_free((void *)field_validity);
	}
	if (rows) {
		duckdb_free((void *)rows);
	}
	if (bridge) {
		duckdb_free((void *)bridge);
	}
	return NULL;
}

/* tcc_typedesc_is_composite: Type-system conversion/parsing helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_typedesc_is_composite(const tcc_typedesc_t *desc) {
	if (!desc) {
		return false;
	}
	return tcc_ffi_type_is_list(desc->ffi_type) || tcc_ffi_type_is_array(desc->ffi_type) ||
	       tcc_ffi_type_is_struct(desc->ffi_type) || tcc_ffi_type_is_map(desc->ffi_type) ||
	       tcc_ffi_type_is_union(desc->ffi_type);
}

/* tcc_value_bridge_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_value_bridge_destroy(tcc_value_bridge_t *bridge) {
	idx_t i;
	if (!bridge) {
		return;
	}
	if (bridge->children) {
		for (i = 0; i < bridge->child_count; i++) {
			if (bridge->children[i]) {
				tcc_value_bridge_destroy(bridge->children[i]);
			}
		}
		duckdb_free(bridge->children);
	}
	if (bridge->child_ptrs) {
		duckdb_free((void *)bridge->child_ptrs);
	}
	if (bridge->child_validity_ptrs) {
		duckdb_free((void *)bridge->child_validity_ptrs);
	}
	if (bridge->owned_validity) {
		duckdb_free(bridge->owned_validity);
	}
	if (bridge->owns_rows && bridge->rows) {
		duckdb_free(bridge->rows);
	}
	duckdb_free(bridge);
}

static tcc_value_bridge_t *tcc_build_value_bridge(duckdb_vector vector, const tcc_typedesc_t *desc, idx_t count,
                                                  const char **out_error) {
	tcc_value_bridge_t *bridge = NULL;
	idx_t row;
	if (out_error) {
		*out_error = NULL;
	}
	if (!vector || !desc) {
		if (out_error) {
			*out_error = "ducktinycc invalid bridge input";
		}
		return NULL;
	}
	bridge = (tcc_value_bridge_t *)duckdb_malloc(sizeof(tcc_value_bridge_t));
	if (!bridge) {
		if (out_error) {
			*out_error = "ducktinycc out of memory";
		}
		return NULL;
	}
	memset(bridge, 0, sizeof(*bridge));
	bridge->desc = desc;
	bridge->count = count;
	bridge->elem_size = tcc_ffi_type_size(desc->ffi_type);
	bridge->rows = duckdb_vector_get_data(vector);
	bridge->validity = (const uint64_t *)duckdb_vector_get_validity(vector);
	if (bridge->elem_size == 0 && desc->ffi_type != TCC_FFI_VOID) {
		if (out_error) {
			*out_error = "ducktinycc unsupported bridge type";
		}
		goto fail;
	}
	if (tcc_ffi_type_is_list(desc->ffi_type)) {
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector child_vector = duckdb_list_vector_get_child(vector);
		tcc_value_bridge_t *child_bridge = NULL;
		ducktinycc_list_t *rows = NULL;
		idx_t child_count = duckdb_list_vector_get_size(vector);
		if (!entries || !child_vector || !desc->as.list_like.child) {
			if (out_error) {
				*out_error = "ducktinycc invalid list bridge shape";
			}
			goto fail;
		}
		child_bridge = tcc_build_value_bridge(child_vector, desc->as.list_like.child, child_count, out_error);
		if (!child_bridge) {
			goto fail;
		}
		if (count > 0) {
			rows = (ducktinycc_list_t *)duckdb_malloc(sizeof(ducktinycc_list_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				tcc_value_bridge_destroy(child_bridge);
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			duckdb_list_entry entry = entries[row];
			rows[row].ptr = child_bridge->rows
			                    ? (const void *)((const uint8_t *)child_bridge->rows +
			                                     ((size_t)entry.offset * child_bridge->elem_size))
			                    : NULL;
			rows[row].validity = child_bridge->validity;
			rows[row].offset = (uint64_t)entry.offset;
			rows[row].len = (uint64_t)entry.length;
		}
		bridge->children = (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *));
		if (!bridge->children) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			if (rows) {
				duckdb_free(rows);
			}
			tcc_value_bridge_destroy(child_bridge);
			goto fail;
		}
		bridge->children[0] = child_bridge;
		bridge->child_count = 1;
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->elem_size = sizeof(ducktinycc_list_t);
		return bridge;
	}
	if (tcc_ffi_type_is_array(desc->ffi_type)) {
		duckdb_vector child_vector = duckdb_array_vector_get_child(vector);
		tcc_value_bridge_t *child_bridge = NULL;
		ducktinycc_array_t *rows = NULL;
		size_t array_len = desc->array_size;
		uint64_t child_count_u64;
		if (!child_vector || !desc->as.list_like.child || array_len == 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid array bridge shape";
			}
			goto fail;
		}
		child_count_u64 = (uint64_t)count * (uint64_t)array_len;
		if (array_len > 0 && count > (idx_t)(UINT64_MAX / array_len)) {
			if (out_error) {
				*out_error = "ducktinycc array bridge overflow";
			}
			goto fail;
		}
		child_bridge = tcc_build_value_bridge(child_vector, desc->as.list_like.child, (idx_t)child_count_u64, out_error);
		if (!child_bridge) {
			goto fail;
		}
		if (count > 0) {
			rows = (ducktinycc_array_t *)duckdb_malloc(sizeof(ducktinycc_array_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				tcc_value_bridge_destroy(child_bridge);
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			uint64_t off = (uint64_t)row * (uint64_t)array_len;
			rows[row].ptr = child_bridge->rows
			                    ? (const void *)((const uint8_t *)child_bridge->rows +
			                                     ((size_t)off * child_bridge->elem_size))
			                    : NULL;
			rows[row].validity = child_bridge->validity;
			rows[row].offset = off;
			rows[row].len = (uint64_t)array_len;
		}
		bridge->children = (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *));
		if (!bridge->children) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			if (rows) {
				duckdb_free(rows);
			}
			tcc_value_bridge_destroy(child_bridge);
			goto fail;
		}
		bridge->children[0] = child_bridge;
		bridge->child_count = 1;
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->elem_size = sizeof(ducktinycc_array_t);
		return bridge;
	}
	if (tcc_ffi_type_is_struct(desc->ffi_type)) {
		idx_t i;
		ducktinycc_struct_t *rows = NULL;
		idx_t field_count = desc->as.struct_like.count;
		if (!desc->as.struct_like.fields || field_count <= 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid struct bridge metadata";
			}
			goto fail;
		}
		bridge->children =
		    (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * (size_t)field_count);
		bridge->child_ptrs = (const void **)duckdb_malloc(sizeof(const void *) * (size_t)field_count);
		bridge->child_validity_ptrs = (const uint64_t **)duckdb_malloc(sizeof(const uint64_t *) * (size_t)field_count);
		if (!bridge->children || !bridge->child_ptrs || !bridge->child_validity_ptrs) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			goto fail;
		}
		memset(bridge->children, 0, sizeof(tcc_value_bridge_t *) * (size_t)field_count);
		for (i = 0; i < field_count; i++) {
			duckdb_vector child_vector = duckdb_struct_vector_get_child(vector, i);
			tcc_value_bridge_t *child_bridge;
			if (!child_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing struct child vector";
				}
				goto fail;
			}
			child_bridge = tcc_build_value_bridge(child_vector, desc->as.struct_like.fields[i].type, count, out_error);
			if (!child_bridge) {
				goto fail;
			}
			bridge->children[i] = child_bridge;
			bridge->child_ptrs[i] = child_bridge->rows;
			bridge->child_validity_ptrs[i] = child_bridge->validity;
		}
		if (count > 0) {
			rows = (ducktinycc_struct_t *)duckdb_malloc(sizeof(ducktinycc_struct_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			rows[row].field_ptrs = bridge->child_ptrs;
			rows[row].field_validity = bridge->child_validity_ptrs;
			rows[row].field_count = (uint64_t)field_count;
			rows[row].offset = (uint64_t)row;
		}
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->child_count = field_count;
		bridge->elem_size = sizeof(ducktinycc_struct_t);
		return bridge;
	}
	if (tcc_ffi_type_is_map(desc->ffi_type)) {
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector map_struct_vector = duckdb_list_vector_get_child(vector);
		duckdb_vector key_vector;
		duckdb_vector value_vector;
		tcc_value_bridge_t *key_bridge = NULL;
		tcc_value_bridge_t *value_bridge = NULL;
		ducktinycc_map_t *rows = NULL;
		idx_t child_count = duckdb_list_vector_get_size(vector);
		if (!entries || !map_struct_vector || !desc->as.map_like.key || !desc->as.map_like.value) {
			if (out_error) {
				*out_error = "ducktinycc invalid map bridge shape";
			}
			goto fail;
		}
		key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
		value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
		if (!key_vector || !value_vector) {
			if (out_error) {
				*out_error = "ducktinycc invalid map key/value vector";
			}
			goto fail;
		}
		key_bridge = tcc_build_value_bridge(key_vector, desc->as.map_like.key, child_count, out_error);
		if (!key_bridge) {
			goto fail;
		}
		value_bridge = tcc_build_value_bridge(value_vector, desc->as.map_like.value, child_count, out_error);
		if (!value_bridge) {
			tcc_value_bridge_destroy(key_bridge);
			goto fail;
		}
		if (count > 0) {
			rows = (ducktinycc_map_t *)duckdb_malloc(sizeof(ducktinycc_map_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				tcc_value_bridge_destroy(key_bridge);
				tcc_value_bridge_destroy(value_bridge);
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			duckdb_list_entry entry = entries[row];
			rows[row].key_ptr = key_bridge->rows
			                        ? (const void *)((const uint8_t *)key_bridge->rows +
			                                         ((size_t)entry.offset * key_bridge->elem_size))
			                        : NULL;
			rows[row].key_validity = key_bridge->validity;
			rows[row].value_ptr = value_bridge->rows
			                          ? (const void *)((const uint8_t *)value_bridge->rows +
			                                           ((size_t)entry.offset * value_bridge->elem_size))
			                          : NULL;
			rows[row].value_validity = value_bridge->validity;
			rows[row].offset = (uint64_t)entry.offset;
			rows[row].len = (uint64_t)entry.length;
		}
		bridge->children = (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * 2);
		if (!bridge->children) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			if (rows) {
				duckdb_free(rows);
			}
			tcc_value_bridge_destroy(key_bridge);
			tcc_value_bridge_destroy(value_bridge);
			goto fail;
		}
		bridge->children[0] = key_bridge;
		bridge->children[1] = value_bridge;
		bridge->child_count = 2;
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->elem_size = sizeof(ducktinycc_map_t);
		return bridge;
	}
	if (tcc_ffi_type_is_union(desc->ffi_type)) {
		idx_t i;
		/* UNION layout: child[0] = tag vector (uint8_t), child[1..N] = member vectors.
		 * The union vector itself has no data buffer (PhysicalType::STRUCT size=0),
		 * so we must access tags via the tag child vector. */
		duckdb_vector tag_vector = duckdb_struct_vector_get_child(vector, 0);
		uint8_t *tags = tag_vector ? (uint8_t *)duckdb_vector_get_data(tag_vector) : NULL;
		ducktinycc_union_t *rows = NULL;
		idx_t member_count = desc->as.union_like.count;
		if (!tags || !desc->as.union_like.members || member_count <= 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid union bridge shape";
			}
			goto fail;
		}
		bridge->children =
		    (tcc_value_bridge_t **)duckdb_malloc(sizeof(tcc_value_bridge_t *) * (size_t)member_count);
		bridge->child_ptrs = (const void **)duckdb_malloc(sizeof(const void *) * (size_t)member_count);
		bridge->child_validity_ptrs =
		    (const uint64_t **)duckdb_malloc(sizeof(const uint64_t *) * (size_t)member_count);
		if (!bridge->children || !bridge->child_ptrs || !bridge->child_validity_ptrs) {
			if (out_error) {
				*out_error = "ducktinycc out of memory";
			}
			goto fail;
		}
		memset(bridge->children, 0, sizeof(tcc_value_bridge_t *) * (size_t)member_count);
		for (i = 0; i < member_count; i++) {
			/* member i is at child[i+1] since child[0] is the tag */
			duckdb_vector member_vector = duckdb_struct_vector_get_child(vector, i + 1);
			tcc_value_bridge_t *member_bridge;
			if (!member_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing union member vector";
				}
				goto fail;
			}
			member_bridge =
			    tcc_build_value_bridge(member_vector, desc->as.union_like.members[i].type, count, out_error);
			if (!member_bridge) {
				goto fail;
			}
			bridge->children[i] = member_bridge;
			bridge->child_ptrs[i] = member_bridge->rows;
			bridge->child_validity_ptrs[i] = member_bridge->validity;
		}
		if (!bridge->validity && count > 0) {
			idx_t words = (count + 63) / 64;
			uint64_t *mask = (uint64_t *)duckdb_malloc(sizeof(uint64_t) * (size_t)words);
			if (!mask) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
			tcc_validity_set_all(mask, count, true);
			for (row = 0; row < count; row++) {
				uint8_t tag = tags[row];
				bool row_valid = true;
				if ((idx_t)tag >= member_count) {
					row_valid = false;
				} else if (bridge->child_validity_ptrs[tag] &&
				           !duckdb_validity_row_is_valid((uint64_t *)bridge->child_validity_ptrs[tag], row)) {
					row_valid = false;
				}
				duckdb_validity_set_row_validity(mask, row, row_valid);
			}
			bridge->owned_validity = mask;
			bridge->validity = mask;
		}
		if (count > 0) {
			rows = (ducktinycc_union_t *)duckdb_malloc(sizeof(ducktinycc_union_t) * (size_t)count);
			if (!rows) {
				if (out_error) {
					*out_error = "ducktinycc out of memory";
				}
				goto fail;
			}
		}
		for (row = 0; row < count; row++) {
			rows[row].tag_ptr = tags;
			rows[row].member_ptrs = bridge->child_ptrs;
			rows[row].member_validity = bridge->child_validity_ptrs;
			rows[row].member_count = (uint64_t)member_count;
			rows[row].offset = (uint64_t)row;
		}
		bridge->rows = rows;
		bridge->owns_rows = true;
		bridge->child_count = member_count;
		bridge->elem_size = sizeof(ducktinycc_union_t);
		return bridge;
	}
	return bridge;
fail:
	tcc_value_bridge_destroy(bridge);
	return NULL;
}

/* tcc_set_vector_row_validity: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_set_vector_row_validity(duckdb_vector vector, idx_t row, bool valid) {
	uint64_t *validity;
	if (!vector) {
		return false;
	}
	duckdb_vector_ensure_validity_writable(vector);
	validity = duckdb_vector_get_validity(vector);
	if (!validity) {
		return false;
	}
	duckdb_validity_set_row_validity(validity, row, valid);
	return true;
}

/* tcc_write_value_to_vector: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_write_value_to_vector(duckdb_vector vector, const tcc_typedesc_t *desc, idx_t row,
                                      const void *src_base, uint64_t src_offset, const uint64_t *src_validity,
                                      const char **out_error) {
	const uint8_t *src_ptr = NULL;
	size_t src_size;
	bool row_valid = true;
	if (!vector || !desc) {
		if (out_error) {
			*out_error = "ducktinycc invalid return bridge arguments";
		}
		return false;
	}
	if (src_validity) {
		row_valid = (src_validity[src_offset >> 6] & (1ULL << (src_offset & 63))) != 0;
	}
	if (!tcc_set_vector_row_validity(vector, row, row_valid)) {
		if (out_error) {
			*out_error = "ducktinycc failed to set output validity";
		}
		return false;
	}
	if (!row_valid) {
		return true;
	}
	src_size = tcc_ffi_type_size(desc->ffi_type);
	if (src_size == 0 && desc->ffi_type != TCC_FFI_VOID) {
		if (out_error) {
			*out_error = "ducktinycc unsupported output type size";
		}
		return false;
	}
	if (src_size > 0) {
		if (!src_base) {
			if (!tcc_set_vector_row_validity(vector, row, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set output validity";
				}
				return false;
			}
			return true;
		}
		src_ptr = (const uint8_t *)src_base + ((size_t)src_offset * src_size);
	}
	if (desc->ffi_type == TCC_FFI_VARCHAR) {
		const duckdb_string_t *str = (const duckdb_string_t *)src_ptr;
		const char *s = duckdb_string_t_data((duckdb_string_t *)str);
		uint32_t len = duckdb_string_t_length(*str);
		duckdb_vector_assign_string_element_len(vector, row, s ? s : "", (idx_t)len);
		return true;
	}
	if (desc->ffi_type == TCC_FFI_BLOB) {
		const ducktinycc_blob_t *blob = (const ducktinycc_blob_t *)src_ptr;
		if (!blob->ptr && blob->len > 0) {
			if (!tcc_set_vector_row_validity(vector, row, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set output validity";
				}
				return false;
			}
			return true;
		}
		duckdb_vector_assign_string_element_len(vector, row, (const char *)blob->ptr, (idx_t)blob->len);
		return true;
	}
	if (tcc_ffi_type_is_list(desc->ffi_type)) {
		const ducktinycc_list_t *list = (const ducktinycc_list_t *)src_ptr;
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector child_vector = duckdb_list_vector_get_child(vector);
		idx_t child_offset = duckdb_list_vector_get_size(vector);
		idx_t i;
		if (!entries || !child_vector || !desc->as.list_like.child) {
			if (out_error) {
				*out_error = "ducktinycc invalid list return bridge";
			}
			return false;
		}
		if (list->len > 0 && !list->ptr) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		if (duckdb_list_vector_reserve(vector, child_offset + (idx_t)list->len) != DuckDBSuccess ||
		    duckdb_list_vector_set_size(vector, child_offset + (idx_t)list->len) != DuckDBSuccess) {
			if (out_error) {
				*out_error = "ducktinycc list return reserve/set_size failed";
			}
			return false;
		}
		for (i = 0; i < (idx_t)list->len; i++) {
			uint64_t src_idx = list->offset + (uint64_t)i;
			bool child_valid = true;
			if (list->validity) {
				child_valid = (list->validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (!child_valid) {
				if (!tcc_set_vector_row_validity(child_vector, child_offset + i, false)) {
					if (out_error) {
						*out_error = "ducktinycc failed to set list child validity";
					}
					return false;
				}
				continue;
			}
			if (!tcc_write_value_to_vector(child_vector, desc->as.list_like.child, child_offset + i, list->ptr,
			                               (uint64_t)i, NULL, out_error)) {
				return false;
			}
			if (!tcc_set_vector_row_validity(child_vector, child_offset + i, child_valid)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set list child validity";
				}
				return false;
			}
		}
		entries[row].offset = child_offset;
		entries[row].length = (idx_t)list->len;
		return true;
	}
	if (tcc_ffi_type_is_array(desc->ffi_type)) {
		const ducktinycc_array_t *arr = (const ducktinycc_array_t *)src_ptr;
		duckdb_vector child_vector = duckdb_array_vector_get_child(vector);
		size_t array_len = desc->array_size;
		idx_t i;
		if (!child_vector || !desc->as.list_like.child || array_len == 0) {
			if (out_error) {
				*out_error = "ducktinycc invalid array return bridge";
			}
			return false;
		}
		if ((size_t)arr->len != array_len || (arr->len > 0 && !arr->ptr)) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		for (i = 0; i < (idx_t)array_len; i++) {
			uint64_t src_idx = arr->offset + (uint64_t)i;
			bool child_valid = true;
			if (arr->validity) {
				child_valid = (arr->validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (!child_valid) {
				if (!tcc_set_vector_row_validity(child_vector, (idx_t)((size_t)row * array_len) + i, false)) {
					if (out_error) {
						*out_error = "ducktinycc failed to set array child validity";
					}
					return false;
				}
				continue;
			}
			if (!tcc_write_value_to_vector(child_vector, desc->as.list_like.child, (idx_t)((size_t)row * array_len) + i,
			                               arr->ptr, (uint64_t)i, NULL, out_error)) {
				return false;
			}
			if (!tcc_set_vector_row_validity(child_vector, (idx_t)((size_t)row * array_len) + i, child_valid)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set array child validity";
				}
				return false;
			}
		}
		return true;
	}
	if (tcc_ffi_type_is_struct(desc->ffi_type)) {
		const ducktinycc_struct_t *st = (const ducktinycc_struct_t *)src_ptr;
		idx_t field_idx;
		if (!st->field_ptrs || !desc->as.struct_like.fields || st->field_count != (uint64_t)desc->as.struct_like.count) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		for (field_idx = 0; field_idx < desc->as.struct_like.count; field_idx++) {
			duckdb_vector field_vector = duckdb_struct_vector_get_child(vector, field_idx);
			const uint64_t *field_validity =
			    (st->field_validity && st->field_validity[field_idx]) ? st->field_validity[field_idx] : NULL;
			if (!field_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing struct output child vector";
				}
				return false;
			}
			if (!tcc_write_value_to_vector(field_vector, desc->as.struct_like.fields[field_idx].type, row,
			                               st->field_ptrs[field_idx], st->offset, field_validity, out_error)) {
				return false;
			}
		}
		return true;
	}
	if (tcc_ffi_type_is_map(desc->ffi_type)) {
		const ducktinycc_map_t *m = (const ducktinycc_map_t *)src_ptr;
		duckdb_list_entry *entries = (duckdb_list_entry *)duckdb_vector_get_data(vector);
		duckdb_vector map_struct_vector = duckdb_list_vector_get_child(vector);
		duckdb_vector key_vector;
		duckdb_vector value_vector;
		idx_t child_offset;
		idx_t i;
		if (!entries || !map_struct_vector || !desc->as.map_like.key || !desc->as.map_like.value) {
			if (out_error) {
				*out_error = "ducktinycc invalid map return bridge";
			}
			return false;
		}
		key_vector = duckdb_struct_vector_get_child(map_struct_vector, 0);
		value_vector = duckdb_struct_vector_get_child(map_struct_vector, 1);
		if (!key_vector || !value_vector) {
			if (out_error) {
				*out_error = "ducktinycc invalid map output key/value vector";
			}
			return false;
		}
		if (m->len > 0 && (!m->key_ptr || !m->value_ptr)) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		child_offset = duckdb_list_vector_get_size(vector);
		if (duckdb_list_vector_reserve(vector, child_offset + (idx_t)m->len) != DuckDBSuccess ||
		    duckdb_list_vector_set_size(vector, child_offset + (idx_t)m->len) != DuckDBSuccess) {
			if (out_error) {
				*out_error = "ducktinycc map return reserve/set_size failed";
			}
			return false;
		}
		for (i = 0; i < (idx_t)m->len; i++) {
			uint64_t src_idx = m->offset + (uint64_t)i;
			bool key_valid = true;
			bool value_valid = true;
			if (m->key_validity) {
				key_valid = (m->key_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (m->value_validity) {
				value_valid = (m->value_validity[src_idx >> 6] & (1ULL << (src_idx & 63))) != 0;
			}
			if (key_valid) {
				if (!tcc_write_value_to_vector(key_vector, desc->as.map_like.key, child_offset + i, m->key_ptr,
				                               (uint64_t)i, NULL, out_error)) {
					return false;
				}
			} else if (!tcc_set_vector_row_validity(key_vector, child_offset + i, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set map key validity";
				}
				return false;
			}
			if (value_valid) {
				if (!tcc_write_value_to_vector(value_vector, desc->as.map_like.value, child_offset + i, m->value_ptr,
				                               (uint64_t)i, NULL, out_error)) {
					return false;
				}
			} else if (!tcc_set_vector_row_validity(value_vector, child_offset + i, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set map value validity";
				}
				return false;
			}
			if (!tcc_set_vector_row_validity(key_vector, child_offset + i, key_valid) ||
			    !tcc_set_vector_row_validity(value_vector, child_offset + i, value_valid)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set map child validity";
				}
				return false;
			}
		}
		entries[row].offset = child_offset;
		entries[row].length = (idx_t)m->len;
		return true;
	}
	if (tcc_ffi_type_is_union(desc->ffi_type)) {
		const ducktinycc_union_t *u = (const ducktinycc_union_t *)src_ptr;
		/* UNION layout: child[0] = tag vector, child[1..N] = member vectors. */
		duckdb_vector tag_vector = duckdb_struct_vector_get_child(vector, 0);
		uint8_t *tags = tag_vector ? (uint8_t *)duckdb_vector_get_data(tag_vector) : NULL;
		idx_t member_count = desc->as.union_like.count;
		idx_t member_idx;
		uint8_t tag;
		if (!tags || !u->tag_ptr || !u->member_ptrs || !desc->as.union_like.members ||
		    u->member_count != (uint64_t)member_count) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		tag = u->tag_ptr[u->offset];
		if ((idx_t)tag >= member_count) {
			return tcc_set_vector_row_validity(vector, row, false);
		}
		tags[row] = tag;
		for (member_idx = 0; member_idx < member_count; member_idx++) {
			/* member member_idx is at child[member_idx+1] since child[0] is the tag */
			duckdb_vector member_vector = duckdb_struct_vector_get_child(vector, member_idx + 1);
			if (!member_vector) {
				if (out_error) {
					*out_error = "ducktinycc missing union output member vector";
				}
				return false;
			}
			if (member_idx == (idx_t)tag) {
				const uint64_t *member_validity =
				    (u->member_validity && u->member_validity[member_idx]) ? u->member_validity[member_idx] : NULL;
				if (!tcc_write_value_to_vector(member_vector, desc->as.union_like.members[member_idx].type, row,
				                               u->member_ptrs[member_idx], u->offset, member_validity, out_error)) {
					return false;
				}
			} else if (!tcc_set_vector_row_validity(member_vector, row, false)) {
				if (out_error) {
					*out_error = "ducktinycc failed to set union member validity";
				}
				return false;
			}
		}
		return true;
	}
	if (desc->ffi_type != TCC_FFI_VOID && src_size > 0) {
		uint8_t *dst = (uint8_t *)duckdb_vector_get_data(vector);
		if (!dst || !src_ptr) {
			if (out_error) {
				*out_error = "ducktinycc output copy failed";
			}
			return false;
		}
		memcpy(dst + ((size_t)row * src_size), src_ptr, src_size);
	}
	return true;
}


