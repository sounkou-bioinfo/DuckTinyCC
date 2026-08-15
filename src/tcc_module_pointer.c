/*
 * DuckTinyCC pointer registry and pointer-helper scalar UDFs.
 *
 * Included by tcc_module.c while pointer/session types are still file-local.  The registry owns
 * allocations only when entry->owned is true; SQL helpers expose raw handles and checked reads/writes.
 */

/* Pointer-registry primitives backing `tcc_alloc` and pointer helper scalar UDFs. */
/* ===== Section: Pointer Registry + Pointer SQL Helpers ===== */
static void tcc_ptr_registry_lock(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	while (atomic_flag_test_and_set_explicit(&registry->lock, memory_order_acquire)) {
	}
}

/* tcc_ptr_registry_unlock: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_ptr_registry_unlock(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	atomic_flag_clear_explicit(&registry->lock, memory_order_release);
}

static tcc_ptr_registry_t *tcc_ptr_registry_create(void) {
	tcc_ptr_registry_t *registry = (tcc_ptr_registry_t *)duckdb_malloc(sizeof(tcc_ptr_registry_t));
	if (!registry) {
		return NULL;
	}
	memset(registry, 0, sizeof(*registry));
	atomic_init(&registry->ref_count, 1);
	atomic_flag_clear(&registry->lock);
	registry->next_handle = 1;
	return registry;
}

/* tcc_ptr_registry_destroy: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_ptr_registry_destroy(tcc_ptr_registry_t *registry) {
	idx_t i;
	if (!registry) {
		return;
	}
	for (i = 0; i < registry->count; i++) {
		if (registry->entries[i].owned && registry->entries[i].ptr) {
			free(registry->entries[i].ptr);
		}
	}
	if (registry->entries) {
		duckdb_free(registry->entries);
	}
	duckdb_free(registry);
}

/* tcc_ptr_registry_ref: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_ptr_registry_ref(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	atomic_fetch_add_explicit(&registry->ref_count, 1, memory_order_relaxed);
}

/* tcc_ptr_registry_unref: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_ptr_registry_unref(tcc_ptr_registry_t *registry) {
	if (!registry) {
		return;
	}
	if (atomic_fetch_sub_explicit(&registry->ref_count, 1, memory_order_acq_rel) == 1) {
		tcc_ptr_registry_destroy(registry);
	}
}

/* tcc_ptr_registry_find_handle_unlocked: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static idx_t tcc_ptr_registry_find_handle_unlocked(tcc_ptr_registry_t *registry, uint64_t handle) {
	idx_t i;
	if (!registry || handle == 0) {
		return (idx_t)-1;
	}
	for (i = 0; i < registry->count; i++) {
		if (registry->entries[i].handle == handle) {
			return i;
		}
	}
	return (idx_t)-1;
}

/* tcc_ptr_registry_reserve_unlocked: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_reserve_unlocked(tcc_ptr_registry_t *registry, idx_t wanted) {
	tcc_ptr_entry_t *new_entries;
	idx_t new_capacity;
	if (!registry) {
		return false;
	}
	if (registry->capacity >= wanted) {
		return true;
	}
	new_capacity = registry->capacity == 0 ? 16 : registry->capacity * 2;
	while (new_capacity < wanted) {
		if (new_capacity > (idx_t)-1 / 2) {
			return false;
		}
		new_capacity *= 2;
	}
	new_entries = (tcc_ptr_entry_t *)duckdb_malloc(sizeof(tcc_ptr_entry_t) * (size_t)new_capacity);
	if (!new_entries) {
		return false;
	}
	memset(new_entries, 0, sizeof(tcc_ptr_entry_t) * (size_t)new_capacity);
	if (registry->entries && registry->count > 0) {
		memcpy(new_entries, registry->entries, sizeof(tcc_ptr_entry_t) * (size_t)registry->count);
		duckdb_free(registry->entries);
	}
	registry->entries = new_entries;
	registry->capacity = new_capacity;
	return true;
}

/* tcc_ptr_span_fits: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_span_fits(uint64_t len, uint64_t offset, uint64_t width) {
	if (offset > len) {
		return false;
	}
	return width <= (len - offset);
}

/**
 * @function tcc_ptr_registry_alloc
 * @brief Allocate zeroed heap memory and register it under a new pointer handle.
 * @param[in,out] registry Borrowed registry pointer.
 * @param[in] size Requested allocation size in bytes.
 * @param[out] out_handle Receives the new non-zero handle on success.
 * @return true on success, false on invalid args or allocation/registry failure.
 * @ownership borrows(registry), transfers(none)
 * @heap allocates malloc(size) for handle-owned memory; freed by tcc_ptr_registry_free/destroy
 * @stack fixed-size locals only
 * @thread_safety protected by registry spin lock
 * @locks acquires/releases registry->lock
 * @errors reports failure via boolean return
 */
static bool tcc_ptr_registry_alloc(tcc_ptr_registry_t *registry, uint64_t size, uint64_t *out_handle) {
	void *ptr;
	tcc_ptr_entry_t *entry;
	uint64_t handle;
	if (!registry || !out_handle || size == 0 || size > (uint64_t)(~(size_t)0)) {
		return false;
	}
	ptr = malloc((size_t)size);
	if (!ptr) {
		return false;
	}
	memset(ptr, 0, (size_t)size);
	tcc_ptr_registry_lock(registry);
	if (!tcc_ptr_registry_reserve_unlocked(registry, registry->count + 1)) {
		tcc_ptr_registry_unlock(registry);
		free(ptr);
		return false;
	}
	handle = registry->next_handle++;
	if (handle == 0) {
		handle = registry->next_handle++;
	}
	entry = &registry->entries[registry->count++];
	memset(entry, 0, sizeof(*entry));
	entry->handle = handle;
	entry->ptr = ptr;
	entry->size = size;
	entry->owned = true;
	tcc_ptr_registry_unlock(registry);
	*out_handle = handle;
	return true;
}

/**
 * @function tcc_ptr_registry_free
 * @brief Remove a handle from the registry and free its owned memory.
 * @param[in,out] registry Borrowed registry pointer.
 * @param[in] handle Handle returned by tcc_ptr_registry_alloc.
 * @return true if handle existed and was released, false otherwise.
 * @ownership borrows(registry), transfers(none)
 * @heap frees malloc-owned entry payload when owned=true
 * @stack fixed-size locals only
 * @thread_safety protected by registry spin lock
 * @locks acquires/releases registry->lock
 * @errors reports failure via boolean return
 */
static bool tcc_ptr_registry_free(tcc_ptr_registry_t *registry, uint64_t handle) {
	void *ptr = NULL;
	bool owned = false;
	idx_t idx;
	idx_t last;
	if (!registry || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	ptr = registry->entries[idx].ptr;
	owned = registry->entries[idx].owned;
	last = registry->count - 1;
	if (idx != last) {
		registry->entries[idx] = registry->entries[last];
	}
	memset(&registry->entries[last], 0, sizeof(registry->entries[last]));
	registry->count--;
	tcc_ptr_registry_unlock(registry);
	if (owned && ptr) {
		free(ptr);
	}
	return true;
}

/* tcc_ptr_registry_get_ptr_size: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_get_ptr_size(tcc_ptr_registry_t *registry, uint64_t handle, uintptr_t *out_ptr,
                                          uint64_t *out_size) {
	idx_t idx;
	if (!registry || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	if (out_ptr) {
		*out_ptr = (uintptr_t)registry->entries[idx].ptr;
	}
	if (out_size) {
		*out_size = registry->entries[idx].size;
	}
	tcc_ptr_registry_unlock(registry);
	return true;
}

/* tcc_ptr_registry_read: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_read(tcc_ptr_registry_t *registry, uint64_t handle, uint64_t offset, void *out,
                                  uint64_t width) {
	idx_t idx;
	uint8_t *src;
	if (!registry || !out || width == 0 || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1 || !registry->entries[idx].ptr ||
	    !tcc_ptr_span_fits(registry->entries[idx].size, offset, width)) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	src = (uint8_t *)registry->entries[idx].ptr + (size_t)offset;
	memcpy(out, src, (size_t)width);
	tcc_ptr_registry_unlock(registry);
	return true;
}

/* tcc_ptr_registry_write: State/registry primitive used by runtime and helper UDFs. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool tcc_ptr_registry_write(tcc_ptr_registry_t *registry, uint64_t handle, uint64_t offset, const void *in,
                                   uint64_t width) {
	idx_t idx;
	uint8_t *dst;
	if (!registry || !in || width == 0 || handle == 0) {
		return false;
	}
	tcc_ptr_registry_lock(registry);
	idx = tcc_ptr_registry_find_handle_unlocked(registry, handle);
	if (idx == (idx_t)-1 || !registry->entries[idx].ptr ||
	    !tcc_ptr_span_fits(registry->entries[idx].size, offset, width)) {
		tcc_ptr_registry_unlock(registry);
		return false;
	}
	dst = (uint8_t *)registry->entries[idx].ptr + (size_t)offset;
	memcpy(dst, in, (size_t)width);
	tcc_ptr_registry_unlock(registry);
	return true;
}

/* tcc_ptr_helper_ctx_destroy: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: releases owned allocations (duckdb_malloc/duckdb_free and/or libc malloc/free per member contract). */
static void tcc_ptr_helper_ctx_destroy(void *ptr) {
	tcc_ptr_helper_ctx_t *ctx = (tcc_ptr_helper_ctx_t *)ptr;
	if (!ctx) {
		return;
	}
	tcc_ptr_registry_unref(ctx->registry);
	duckdb_free(ctx);
}

/* Vector validity helpers for scalar UDF implementations. */
static bool tcc_valid_input_row(uint64_t *validity, idx_t row) {
	return !validity || duckdb_validity_row_is_valid(validity, row);
}

/* tcc_set_output_row_null: Vector validity/error/output helper. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static void tcc_set_output_row_null(uint64_t *validity, idx_t row) {
	if (!validity) {
		return;
	}
	duckdb_validity_set_row_invalid(validity, row);
}

static tcc_ptr_registry_t *tcc_get_ptr_registry(duckdb_function_info info) {
	tcc_ptr_helper_ctx_t *ctx = (tcc_ptr_helper_ctx_t *)duckdb_scalar_function_get_extra_info(info);
	if (!ctx || !ctx->registry) {
		duckdb_scalar_function_set_error(info, "tcc pointer helper missing registry context");
		return NULL;
	}
	return ctx->registry;
}

/* Pointer helper SQL functions (`tcc_alloc`, `tcc_free_ptr`, `tcc_dataptr`, reads/writes). */
static void tcc_alloc_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_size;
	uint64_t *in_validity;
	uint64_t *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_size = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (uint64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uint64_t handle = 0;
		if (!tcc_valid_input_row(in_validity, row) || !tcc_ptr_registry_alloc(registry, in_size[row], &handle)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = handle;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_free_ptr_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_free_ptr_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_handle;
	uint64_t *in_validity;
	bool *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_handle = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (bool *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		if (!tcc_valid_input_row(in_validity, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = tcc_ptr_registry_free(registry, in_handle[row]);
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_dataptr_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_dataptr_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_handle;
	uint64_t *in_validity;
	uint64_t *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_handle = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (uint64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uintptr_t addr = 0;
		if (!tcc_valid_input_row(in_validity, row) ||
		    !tcc_ptr_registry_get_ptr_size(registry, in_handle[row], &addr, NULL)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = (uint64_t)addr;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_ptr_size_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_ptr_size_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	uint64_t *in_handle;
	uint64_t *in_validity;
	uint64_t *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in_handle = (uint64_t *)duckdb_vector_get_data(in0);
	in_validity = duckdb_vector_get_validity(in0);
	out_data = (uint64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uint64_t size = 0;
		if (!tcc_valid_input_row(in_validity, row) ||
		    !tcc_ptr_registry_get_ptr_size(registry, in_handle[row], NULL, &size)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		out_data[row] = size;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/* tcc_ptr_add_scalar: Internal helper in the TinyCC module/runtime pipeline. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_ptr_add_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	idx_t row_count = duckdb_data_chunk_get_size(input);
	idx_t row;
	duckdb_vector in0 = duckdb_data_chunk_get_vector(input, 0);
	duckdb_vector in1 = duckdb_data_chunk_get_vector(input, 1);
	uint64_t *base = (uint64_t *)duckdb_vector_get_data(in0);
	uint64_t *off = (uint64_t *)duckdb_vector_get_data(in1);
	uint64_t *valid0 = duckdb_vector_get_validity(in0);
	uint64_t *valid1 = duckdb_vector_get_validity(in1);
	uint64_t *out_data = (uint64_t *)duckdb_vector_get_data(output);
	uint64_t *out_validity;
	(void)info;
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		uintptr_t addr;
		if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		addr = (uintptr_t)base[row];
		addr += (uintptr_t)off[row];
		out_data[row] = (uint64_t)addr;
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

#define TCC_DEFINE_PTR_READ_SCALAR(name, ctype)                                                                             \
	static void tcc_read_##name##_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {     \
		tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);                                                     \
		idx_t row_count;                                                                                                \
		idx_t row;                                                                                                      \
		duckdb_vector in0;                                                                                              \
		duckdb_vector in1;                                                                                              \
		uint64_t *handles;                                                                                              \
		uint64_t *offsets;                                                                                              \
		uint64_t *valid0;                                                                                                \
		uint64_t *valid1;                                                                                                \
		ctype *out_data;                                                                                                \
		uint64_t *out_validity;                                                                                          \
		if (!registry) {                                                                                                \
			return;                                                                                                 \
		}                                                                                                               \
		row_count = duckdb_data_chunk_get_size(input);                                                                  \
		in0 = duckdb_data_chunk_get_vector(input, 0);                                                                   \
		in1 = duckdb_data_chunk_get_vector(input, 1);                                                                   \
		handles = (uint64_t *)duckdb_vector_get_data(in0);                                                             \
		offsets = (uint64_t *)duckdb_vector_get_data(in1);                                                             \
		valid0 = duckdb_vector_get_validity(in0);                                                                       \
		valid1 = duckdb_vector_get_validity(in1);                                                                       \
		out_data = (ctype *)duckdb_vector_get_data(output);                                                             \
		duckdb_vector_ensure_validity_writable(output);                                                                 \
		out_validity = duckdb_vector_get_validity(output);                                                              \
		for (row = 0; row < row_count; row++) {                                                                         \
			ctype value;                                                                                            \
			if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) ||                        \
			    !tcc_ptr_registry_read(registry, handles[row], offsets[row], &value, (uint64_t)sizeof(ctype))) { \
				tcc_set_output_row_null(out_validity, row);                                                       \
				continue;                                                                                         \
			}                                                                                                       \
			out_data[row] = value;                                                                                  \
			duckdb_validity_set_row_validity(out_validity, row, true);                                             \
		}                                                                                                               \
	}

#define TCC_DEFINE_PTR_WRITE_SCALAR(name, ctype)                                                                            \
	static void tcc_write_##name##_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {    \
		tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);                                                     \
		idx_t row_count;                                                                                                \
		idx_t row;                                                                                                      \
		duckdb_vector in0;                                                                                              \
		duckdb_vector in1;                                                                                              \
		duckdb_vector in2;                                                                                              \
		uint64_t *handles;                                                                                              \
		uint64_t *offsets;                                                                                              \
		ctype *values;                                                                                                  \
		uint64_t *valid0;                                                                                                \
		uint64_t *valid1;                                                                                                \
		uint64_t *valid2;                                                                                                \
		bool *out_data;                                                                                                 \
		uint64_t *out_validity;                                                                                          \
		if (!registry) {                                                                                                \
			return;                                                                                                 \
		}                                                                                                               \
		row_count = duckdb_data_chunk_get_size(input);                                                                  \
		in0 = duckdb_data_chunk_get_vector(input, 0);                                                                   \
		in1 = duckdb_data_chunk_get_vector(input, 1);                                                                   \
		in2 = duckdb_data_chunk_get_vector(input, 2);                                                                   \
		handles = (uint64_t *)duckdb_vector_get_data(in0);                                                             \
		offsets = (uint64_t *)duckdb_vector_get_data(in1);                                                             \
		values = (ctype *)duckdb_vector_get_data(in2);                                                                  \
		valid0 = duckdb_vector_get_validity(in0);                                                                       \
		valid1 = duckdb_vector_get_validity(in1);                                                                       \
		valid2 = duckdb_vector_get_validity(in2);                                                                       \
		out_data = (bool *)duckdb_vector_get_data(output);                                                              \
		duckdb_vector_ensure_validity_writable(output);                                                                 \
		out_validity = duckdb_vector_get_validity(output);                                                              \
		for (row = 0; row < row_count; row++) {                                                                         \
			if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) ||                         \
			    !tcc_valid_input_row(valid2, row)) {                                                                \
				tcc_set_output_row_null(out_validity, row);                                                       \
				continue;                                                                                         \
			}                                                                                                       \
			out_data[row] =                                                                                         \
			    tcc_ptr_registry_write(registry, handles[row], offsets[row], &values[row], (uint64_t)sizeof(ctype)); \
			duckdb_validity_set_row_validity(out_validity, row, true);                                             \
		}                                                                                                               \
	}

TCC_DEFINE_PTR_READ_SCALAR(i8, int8_t)
TCC_DEFINE_PTR_READ_SCALAR(u8, uint8_t)
TCC_DEFINE_PTR_READ_SCALAR(i16, int16_t)
TCC_DEFINE_PTR_READ_SCALAR(u16, uint16_t)
TCC_DEFINE_PTR_READ_SCALAR(i32, int32_t)
TCC_DEFINE_PTR_READ_SCALAR(u32, uint32_t)
TCC_DEFINE_PTR_READ_SCALAR(i64, int64_t)
TCC_DEFINE_PTR_READ_SCALAR(u64, uint64_t)
TCC_DEFINE_PTR_READ_SCALAR(f32, float)
TCC_DEFINE_PTR_READ_SCALAR(f64, double)

TCC_DEFINE_PTR_WRITE_SCALAR(i8, int8_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u8, uint8_t)
TCC_DEFINE_PTR_WRITE_SCALAR(i16, int16_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u16, uint16_t)
TCC_DEFINE_PTR_WRITE_SCALAR(i32, int32_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u32, uint32_t)
TCC_DEFINE_PTR_WRITE_SCALAR(i64, int64_t)
TCC_DEFINE_PTR_WRITE_SCALAR(u64, uint64_t)
TCC_DEFINE_PTR_WRITE_SCALAR(f32, float)
TCC_DEFINE_PTR_WRITE_SCALAR(f64, double)

#undef TCC_DEFINE_PTR_READ_SCALAR
#undef TCC_DEFINE_PTR_WRITE_SCALAR

/* tcc_read_bytes_scalar: Pointer helper scalar UDF implementation. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_read_bytes_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	duckdb_vector in1;
	duckdb_vector in2;
	uint64_t *handles;
	uint64_t *offsets;
	uint64_t *widths;
	uint64_t *valid0;
	uint64_t *valid1;
	uint64_t *valid2;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in1 = duckdb_data_chunk_get_vector(input, 1);
	in2 = duckdb_data_chunk_get_vector(input, 2);
	handles = (uint64_t *)duckdb_vector_get_data(in0);
	offsets = (uint64_t *)duckdb_vector_get_data(in1);
	widths = (uint64_t *)duckdb_vector_get_data(in2);
	valid0 = duckdb_vector_get_validity(in0);
	valid1 = duckdb_vector_get_validity(in1);
	valid2 = duckdb_vector_get_validity(in2);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		char *buffer = NULL;
		if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) || !tcc_valid_input_row(valid2, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		if (widths[row] == 0) {
			duckdb_vector_assign_string_element_len(output, row, "", 0);
			duckdb_validity_set_row_validity(out_validity, row, true);
			continue;
		}
		if (widths[row] > (uint64_t)(~(size_t)0)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		buffer = (char *)duckdb_malloc((size_t)widths[row]);
		if (!buffer) {
			duckdb_scalar_function_set_error(info, "tcc_read_bytes out of memory");
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		if (!tcc_ptr_registry_read(registry, handles[row], offsets[row], buffer, widths[row])) {
			duckdb_free(buffer);
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		duckdb_vector_assign_string_element_len(output, row, buffer, (idx_t)widths[row]);
		duckdb_validity_set_row_validity(out_validity, row, true);
		duckdb_free(buffer);
	}
}

/* tcc_write_bytes_scalar: Pointer helper scalar UDF implementation. Allocation/Lifetime: operates on DuckDB/vector memory and bridge descriptors; treat pointers as borrowed unless explicitly allocated. */
static void tcc_write_bytes_scalar(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	tcc_ptr_registry_t *registry = tcc_get_ptr_registry(info);
	idx_t row_count;
	idx_t row;
	duckdb_vector in0;
	duckdb_vector in1;
	duckdb_vector in2;
	uint64_t *handles;
	uint64_t *offsets;
	duckdb_string_t *blobs;
	uint64_t *valid0;
	uint64_t *valid1;
	uint64_t *valid2;
	bool *out_data;
	uint64_t *out_validity;
	if (!registry) {
		return;
	}
	row_count = duckdb_data_chunk_get_size(input);
	in0 = duckdb_data_chunk_get_vector(input, 0);
	in1 = duckdb_data_chunk_get_vector(input, 1);
	in2 = duckdb_data_chunk_get_vector(input, 2);
	handles = (uint64_t *)duckdb_vector_get_data(in0);
	offsets = (uint64_t *)duckdb_vector_get_data(in1);
	blobs = (duckdb_string_t *)duckdb_vector_get_data(in2);
	valid0 = duckdb_vector_get_validity(in0);
	valid1 = duckdb_vector_get_validity(in1);
	valid2 = duckdb_vector_get_validity(in2);
	out_data = (bool *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	out_validity = duckdb_vector_get_validity(output);
	for (row = 0; row < row_count; row++) {
		const char *blob_data;
		uint64_t blob_len;
		if (!tcc_valid_input_row(valid0, row) || !tcc_valid_input_row(valid1, row) || !tcc_valid_input_row(valid2, row)) {
			tcc_set_output_row_null(out_validity, row);
			continue;
		}
		blob_data = duckdb_string_t_data(&blobs[row]);
		blob_len = (uint64_t)duckdb_string_t_length(blobs[row]);
		if (blob_len == 0) {
			out_data[row] = true;
			duckdb_validity_set_row_validity(out_validity, row, true);
			continue;
		}
		out_data[row] = tcc_ptr_registry_write(registry, handles[row], offsets[row], blob_data, blob_len);
		duckdb_validity_set_row_validity(out_validity, row, true);
	}
}

/**
 * @function tcc_register_pointer_scalar
 * @brief Create/register one pointer helper scalar UDF and release temporary function objects.
 * @param[in] connection Borrowed DuckDB connection.
 * @param[in] name SQL function name.
 * @param[in] fn_ptr Scalar callback implementation.
 * @param[in] return_type DuckDB logical return type id.
 * @param[in] arg_types Parameter logical type ids.
 * @param[in] arg_count Number of parameters.
 * @param[in] registry Optional pointer registry context attached as extra info.
 * @return true when registration succeeds.
 * @ownership borrows(connection, name, arg_types, registry), transfers(none)
 * @heap allocates temporary DuckDB logical/scalar function objects and optional ctx; all released on all paths
 * @stack fixed-size locals only
 * @thread_safety delegated to DuckDB registration contract
 * @locks none
 * @errors reports failure via boolean return
 */
static bool tcc_register_pointer_scalar(duckdb_connection connection, const char *name, duckdb_scalar_function_t fn_ptr,
                                        duckdb_type return_type, const duckdb_type *arg_types, idx_t arg_count,
                                        tcc_ptr_registry_t *registry) {
	duckdb_scalar_function fn = duckdb_create_scalar_function();
	duckdb_logical_type ret_obj = NULL;
	duckdb_logical_type *args = NULL;
	tcc_ptr_helper_ctx_t *ctx = NULL;
	idx_t i;
	duckdb_state rc;
	if (!fn) {
		return false;
	}
	ret_obj = duckdb_create_logical_type(return_type);
	if (!ret_obj) {
		duckdb_destroy_scalar_function(&fn);
		return false;
	}
	duckdb_scalar_function_set_name(fn, name);
	duckdb_scalar_function_set_return_type(fn, ret_obj);
	duckdb_scalar_function_set_volatile(fn);
	duckdb_scalar_function_set_function(fn, fn_ptr);
	if (arg_count > 0) {
		args = (duckdb_logical_type *)duckdb_malloc(sizeof(duckdb_logical_type) * (size_t)arg_count);
		if (!args) {
			duckdb_destroy_logical_type(&ret_obj);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		memset(args, 0, sizeof(duckdb_logical_type) * (size_t)arg_count);
		for (i = 0; i < arg_count; i++) {
			args[i] = duckdb_create_logical_type(arg_types[i]);
			if (!args[i]) {
				for (idx_t j = 0; j < i; j++) {
					duckdb_destroy_logical_type(&args[j]);
				}
				duckdb_free(args);
				duckdb_destroy_logical_type(&ret_obj);
				duckdb_destroy_scalar_function(&fn);
				return false;
			}
			duckdb_scalar_function_add_parameter(fn, args[i]);
		}
	}
	if (registry) {
		ctx = (tcc_ptr_helper_ctx_t *)duckdb_malloc(sizeof(tcc_ptr_helper_ctx_t));
		if (!ctx) {
			if (args) {
				for (i = 0; i < arg_count; i++) {
					duckdb_destroy_logical_type(&args[i]);
				}
				duckdb_free(args);
			}
			duckdb_destroy_logical_type(&ret_obj);
			duckdb_destroy_scalar_function(&fn);
			return false;
		}
		ctx->registry = registry;
		tcc_ptr_registry_ref(registry);
		duckdb_scalar_function_set_extra_info(fn, ctx, tcc_ptr_helper_ctx_destroy);
	}
	rc = duckdb_register_scalar_function(connection, fn);
	if (args) {
		for (i = 0; i < arg_count; i++) {
			duckdb_destroy_logical_type(&args[i]);
		}
		duckdb_free(args);
	}
	duckdb_destroy_logical_type(&ret_obj);
	duckdb_destroy_scalar_function(&fn);
	return rc == DuckDBSuccess;
}

/* register_tcc_pointer_helper_functions: Registers extension SQL helper/table functions. Allocation/Lifetime: borrows caller-owned inputs; no ownership transfer. */
static bool register_tcc_pointer_helper_functions(duckdb_connection connection, tcc_ptr_registry_t *registry) {
	typedef struct {
		const char *name;
		duckdb_scalar_function_t fn_ptr;
		duckdb_type return_type;
		const duckdb_type *arg_types;
		idx_t arg_count;
		bool use_registry;
	} tcc_ptr_helper_spec_t;
	static const duckdb_type sig_u64[] = {DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64_u64[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT};
	static const duckdb_type sig_u64_u64_blob[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_BLOB};
#define TCC_PTR_NUMERIC_ROWS(X)      \
	X(i8, DUCKDB_TYPE_TINYINT)     \
	X(u8, DUCKDB_TYPE_UTINYINT)    \
	X(i16, DUCKDB_TYPE_SMALLINT)   \
	X(u16, DUCKDB_TYPE_USMALLINT)  \
	X(i32, DUCKDB_TYPE_INTEGER)    \
	X(u32, DUCKDB_TYPE_UINTEGER)   \
	X(i64, DUCKDB_TYPE_BIGINT)     \
	X(u64, DUCKDB_TYPE_UBIGINT)    \
	X(f32, DUCKDB_TYPE_FLOAT)      \
	X(f64, DUCKDB_TYPE_DOUBLE)
#define TCC_DEFINE_WRITE_SIG(name, duck_type) \
	static const duckdb_type sig_write_##name[] = {DUCKDB_TYPE_UBIGINT, DUCKDB_TYPE_UBIGINT, duck_type};
	TCC_PTR_NUMERIC_ROWS(TCC_DEFINE_WRITE_SIG)
#undef TCC_DEFINE_WRITE_SIG
	static const tcc_ptr_helper_spec_t specs[] = {
	    {"tcc_alloc", tcc_alloc_scalar, DUCKDB_TYPE_UBIGINT, sig_u64, 1, true},
	    {"tcc_free_ptr", tcc_free_ptr_scalar, DUCKDB_TYPE_BOOLEAN, sig_u64, 1, true},
	    {"tcc_dataptr", tcc_dataptr_scalar, DUCKDB_TYPE_UBIGINT, sig_u64, 1, true},
	    {"tcc_ptr_size", tcc_ptr_size_scalar, DUCKDB_TYPE_UBIGINT, sig_u64, 1, true},
	    {"tcc_ptr_add", tcc_ptr_add_scalar, DUCKDB_TYPE_UBIGINT, sig_u64_u64, 2, false},
	    {"tcc_read_bytes", tcc_read_bytes_scalar, DUCKDB_TYPE_BLOB, sig_u64_u64_u64, 3, true},
	    {"tcc_write_bytes", tcc_write_bytes_scalar, DUCKDB_TYPE_BOOLEAN, sig_u64_u64_blob, 3, true},
#define TCC_NUMERIC_RW_SPECS(name, duck_type)                                                              \
	    {"tcc_read_" #name, tcc_read_##name##_scalar, duck_type, sig_u64_u64, 2, true},                    \
	    {"tcc_write_" #name, tcc_write_##name##_scalar, DUCKDB_TYPE_BOOLEAN, sig_write_##name, 3, true},
	    TCC_PTR_NUMERIC_ROWS(TCC_NUMERIC_RW_SPECS)
#undef TCC_NUMERIC_RW_SPECS
	};
#undef TCC_PTR_NUMERIC_ROWS
	idx_t i;
	for (i = 0; i < (idx_t)(sizeof(specs) / sizeof(specs[0])); i++) {
		if (!tcc_register_pointer_scalar(connection, specs[i].name, specs[i].fn_ptr, specs[i].return_type,
		                                 specs[i].arg_types, specs[i].arg_count,
		                                 specs[i].use_registry ? registry : NULL)) {
			return false;
		}
	}
	return true;
}

