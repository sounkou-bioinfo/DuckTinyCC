/*
 * DuckTinyCC host-exported bridge/accessor symbols.
 *
 * These functions are injected into each TinyCC state and called from generated wrappers.
 * They are intentionally small, bounds-checked, and operate only on borrowed spans/descriptors.
 */

/* ducktinycc_valid_is_set: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_valid_is_set(const uint64_t *validity, uint64_t idx) {
	if (!validity) {
		return 1;
	}
	return (validity[idx >> 6] & (1ULL << (idx & 63))) != 0;
}

/* ducktinycc_valid_set: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_span_contains: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_span_fits: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_span_fits(uint64_t len, uint64_t offset, uint64_t width) {
	if (offset > len) {
		return 0;
	}
	return width <= (len - offset);
}

static const void *ducktinycc_buf_ptr_at(const void *base, uint64_t len, uint64_t offset, uint64_t width) {
	if (!base || !ducktinycc_span_fits(len, offset, width)) {
		return NULL;
	}
	return ducktinycc_ptr_add(base, offset);
}

static void *ducktinycc_buf_ptr_at_mut(void *base, uint64_t len, uint64_t offset, uint64_t width) {
	if (!base || !ducktinycc_span_fits(len, offset, width)) {
		return NULL;
	}
	return ducktinycc_ptr_add_mut(base, offset);
}

/* ducktinycc_read_bytes: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_read_bytes(const void *base, uint64_t len, uint64_t offset, void *out, uint64_t width) {
	const void *src;
	if (!out && width > 0) {
		return 0;
	}
	if (width == 0) {
		return 1;
	}
	src = ducktinycc_buf_ptr_at(base, len, offset, width);
	if (!src) {
		return 0;
	}
	memcpy(out, src, (size_t)width);
	return 1;
}

/* ducktinycc_write_bytes: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_write_bytes(void *base, uint64_t len, uint64_t offset, const void *in, uint64_t width) {
	void *dst;
	if (!in && width > 0) {
		return 0;
	}
	if (width == 0) {
		return 1;
	}
	dst = ducktinycc_buf_ptr_at_mut(base, len, offset, width);
	if (!dst) {
		return 0;
	}
	memcpy(dst, in, (size_t)width);
	return 1;
}

#define DUCKTINYCC_DEFINE_BUF_RW(name, type)                                                                                \
	static int ducktinycc_read_##name(const void *base, uint64_t len, uint64_t offset, type *out) {                     \
		return ducktinycc_read_bytes(base, len, offset, out, (uint64_t)sizeof(type));                               \
	}                                                                                                                      \
	static int ducktinycc_write_##name(void *base, uint64_t len, uint64_t offset, type value) {                        \
		return ducktinycc_write_bytes(base, len, offset, &value, (uint64_t)sizeof(type));                          \
	}

DUCKTINYCC_DEFINE_BUF_RW(i8, int8_t)
DUCKTINYCC_DEFINE_BUF_RW(u8, uint8_t)
DUCKTINYCC_DEFINE_BUF_RW(i16, int16_t)
DUCKTINYCC_DEFINE_BUF_RW(u16, uint16_t)
DUCKTINYCC_DEFINE_BUF_RW(i32, int32_t)
DUCKTINYCC_DEFINE_BUF_RW(u32, uint32_t)
DUCKTINYCC_DEFINE_BUF_RW(i64, int64_t)
DUCKTINYCC_DEFINE_BUF_RW(u64, uint64_t)
DUCKTINYCC_DEFINE_BUF_RW(f32, float)
DUCKTINYCC_DEFINE_BUF_RW(f64, double)

#undef DUCKTINYCC_DEFINE_BUF_RW

/* ducktinycc_read_ptr: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_read_ptr(const void *base, uint64_t len, uint64_t offset, const void **out) {
	uintptr_t tmp = 0;
	if (!out) {
		return 0;
	}
	if (!ducktinycc_read_bytes(base, len, offset, &tmp, (uint64_t)sizeof(uintptr_t))) {
		return 0;
	}
	*out = (const void *)tmp;
	return 1;
}

/* ducktinycc_write_ptr: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_write_ptr(void *base, uint64_t len, uint64_t offset, const void *value) {
	uintptr_t tmp = (uintptr_t)value;
	return ducktinycc_write_bytes(base, len, offset, &tmp, (uint64_t)sizeof(uintptr_t));
}

/* ducktinycc_list_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_array_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_struct_field_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_map_key_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_map_value_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
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

/* ducktinycc_union_tag: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_union_tag(const ducktinycc_union_t *u) {
	if (!u || !u->tag_ptr) {
		return -1;
	}
	return (int)u->tag_ptr[u->offset];
}

/* ducktinycc_union_member_ptr: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static const void *ducktinycc_union_member_ptr(const ducktinycc_union_t *u, uint64_t member_idx) {
	if (!u || !u->member_ptrs || member_idx >= u->member_count) {
		return NULL;
	}
	return u->member_ptrs[member_idx];
}

/* ducktinycc_union_member_is_valid: Host-exported bridge/accessor helper for generated wrappers. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static int ducktinycc_union_member_is_valid(const ducktinycc_union_t *u, uint64_t member_idx) {
	if (!u || !u->member_ptrs || member_idx >= u->member_count) {
		return 0;
	}
	if (!u->member_validity || !u->member_validity[member_idx]) {
		return 1;
	}
	return ducktinycc_valid_is_set(u->member_validity[member_idx], u->offset);
}

#define TCC_HOST_SYMBOL_TABLE(X)                                                                                          \
	X("duckdb_ext_api", &duckdb_ext_api)                                                                                 \
	X("ducktinycc_register_signature", ducktinycc_register_signature)                                                    \
	X("ducktinycc_valid_is_set", ducktinycc_valid_is_set)                                                                \
	X("ducktinycc_valid_set", ducktinycc_valid_set)                                                                      \
	X("ducktinycc_span_contains", ducktinycc_span_contains)                                                              \
	X("ducktinycc_ptr_add", ducktinycc_ptr_add)                                                                          \
	X("ducktinycc_ptr_add_mut", ducktinycc_ptr_add_mut)                                                                  \
	X("ducktinycc_span_fits", ducktinycc_span_fits)                                                                      \
	X("ducktinycc_buf_ptr_at", ducktinycc_buf_ptr_at)                                                                    \
	X("ducktinycc_buf_ptr_at_mut", ducktinycc_buf_ptr_at_mut)                                                            \
	X("ducktinycc_read_bytes", ducktinycc_read_bytes)                                                                    \
	X("ducktinycc_write_bytes", ducktinycc_write_bytes)                                                                  \
	X("ducktinycc_read_i8", ducktinycc_read_i8)                                                                          \
	X("ducktinycc_write_i8", ducktinycc_write_i8)                                                                        \
	X("ducktinycc_read_u8", ducktinycc_read_u8)                                                                          \
	X("ducktinycc_write_u8", ducktinycc_write_u8)                                                                        \
	X("ducktinycc_read_i16", ducktinycc_read_i16)                                                                        \
	X("ducktinycc_write_i16", ducktinycc_write_i16)                                                                      \
	X("ducktinycc_read_u16", ducktinycc_read_u16)                                                                        \
	X("ducktinycc_write_u16", ducktinycc_write_u16)                                                                      \
	X("ducktinycc_read_i32", ducktinycc_read_i32)                                                                        \
	X("ducktinycc_write_i32", ducktinycc_write_i32)                                                                      \
	X("ducktinycc_read_u32", ducktinycc_read_u32)                                                                        \
	X("ducktinycc_write_u32", ducktinycc_write_u32)                                                                      \
	X("ducktinycc_read_i64", ducktinycc_read_i64)                                                                        \
	X("ducktinycc_write_i64", ducktinycc_write_i64)                                                                      \
	X("ducktinycc_read_u64", ducktinycc_read_u64)                                                                        \
	X("ducktinycc_write_u64", ducktinycc_write_u64)                                                                      \
	X("ducktinycc_read_f32", ducktinycc_read_f32)                                                                        \
	X("ducktinycc_write_f32", ducktinycc_write_f32)                                                                      \
	X("ducktinycc_read_f64", ducktinycc_read_f64)                                                                        \
	X("ducktinycc_write_f64", ducktinycc_write_f64)                                                                      \
	X("ducktinycc_read_ptr", ducktinycc_read_ptr)                                                                        \
	X("ducktinycc_write_ptr", ducktinycc_write_ptr)                                                                      \
	X("ducktinycc_list_is_valid", ducktinycc_list_is_valid)                                                              \
	X("ducktinycc_list_elem_ptr", ducktinycc_list_elem_ptr)                                                              \
	X("ducktinycc_array_is_valid", ducktinycc_array_is_valid)                                                            \
	X("ducktinycc_array_elem_ptr", ducktinycc_array_elem_ptr)                                                            \
	X("ducktinycc_struct_field_ptr", ducktinycc_struct_field_ptr)                                                        \
	X("ducktinycc_struct_field_is_valid", ducktinycc_struct_field_is_valid)                                              \
	X("ducktinycc_map_key_ptr", ducktinycc_map_key_ptr)                                                                  \
	X("ducktinycc_map_value_ptr", ducktinycc_map_value_ptr)                                                              \
	X("ducktinycc_map_key_is_valid", ducktinycc_map_key_is_valid)                                                        \
	X("ducktinycc_map_value_is_valid", ducktinycc_map_value_is_valid)                                                      \
	X("ducktinycc_union_tag", ducktinycc_union_tag)                                                                        \
	X("ducktinycc_union_member_ptr", ducktinycc_union_member_ptr)                                                          \
	X("ducktinycc_union_member_is_valid", ducktinycc_union_member_is_valid)                                                \
	X("duckdb_validity_row_is_valid", duckdb_validity_row_is_valid)

/* tcc_add_host_symbols: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_add_host_symbols(TCCState *s) {
	if (!s) {
		return;
	}
#define TCC_ADD_HOST_SYMBOL(name, ptr) (void)tcc_add_symbol(s, name, ptr);
	TCC_HOST_SYMBOL_TABLE(TCC_ADD_HOST_SYMBOL)
#undef TCC_ADD_HOST_SYMBOL
}
#undef TCC_HOST_SYMBOL_TABLE

