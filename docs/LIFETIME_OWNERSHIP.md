# Lifetime and Ownership Reference

This document specifies memory ownership, allocation domains, and lifetime
contracts for DuckTinyCC's C-facing bridge, descriptor, and helper APIs.

## Heap Domains

DuckTinyCC uses two heap domains.  Mixing allocators across domains is
undefined behaviour.

| Domain | Allocator / Deallocator | Used By |
|--------|------------------------|---------|
| **DuckDB heap** | `duckdb_malloc` / `duckdb_free` | Extension state, bind/init payloads, parsed metadata, bridge scratch buffers, generated source text, compiled UDF extra-info. |
| **libc heap** | `malloc` / `free` | Pointer-registry payloads (`tcc_alloc` → `tcc_free_ptr`), generated helper `*_new` / `*_free` functions. |

**Rule:** never `duckdb_free` a pointer returned by `malloc`, and vice versa.

## Trusted Native-Code Boundary

Generated UDFs execute as trusted in-process native code inside DuckDB. DuckTinyCC
cleans up its own bridge allocations when a generated wrapper returns normally or
reports a cooperative failure, but it does **not** sandbox or muffle non-local
native control-flow escapes.

DuckTinyCC compiles generated modules with `-nostdlib` by default, so ordinary
libc symbols such as `exit`, `abort`, `setjmp`, and `longjmp` are not linked
implicitly. If a user explicitly links libc (`library := 'c'` or staged
`add_library`), injects process-control symbols, or uses inline assembly/syscalls,
those calls run in the DuckDB process and may terminate, hang, or corrupt the
process. The caller is responsible for keeping generated C code inside the normal
function-return contract.

True containment for hostile or crash-prone C requires process isolation outside
DuckTinyCC's current in-process UDF model.

## Composite Bridge Descriptors

The five descriptor structs are **borrowed views** into DuckDB vector memory.
Generated wrappers receive them by value; the wrapper must **not** free any
field pointer and must **not** retain any pointer after the wrapper invocation
returns.

### `ducktinycc_list_t`

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `ptr` | `const void *` | **Borrowed** from DuckDB list child vector data buffer. | Points to flat element data; index with `offset + i` × element size. |
| `validity` | `const uint64_t *` | **Borrowed** from DuckDB list child validity buffer. | May be `NULL` (all-valid). |
| `offset` | `uint64_t` | Value | Global start offset into child vector for this row's slice. |
| `len` | `uint64_t` | Value | Number of elements in this row's list. |

Access helpers: `ducktinycc_list_elem_ptr(list, idx, elem_size)`,
`ducktinycc_list_is_valid(list, idx)`.

### `ducktinycc_array_t`

Layout-identical to `ducktinycc_list_t`.  Same ownership rules apply.

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `ptr` | `const void *` | **Borrowed** | Flat element data. |
| `validity` | `const uint64_t *` | **Borrowed** | May be `NULL`. |
| `offset` | `uint64_t` | Value | Global child-vector start offset. |
| `len` | `uint64_t` | Value | Fixed array size (from type metadata). |

Access helpers: `ducktinycc_array_elem_ptr(arr, idx, elem_size)`,
`ducktinycc_array_is_valid(arr, idx)`.

### `ducktinycc_struct_t`

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `field_ptrs` | `const void *const *` | **Borrowed** array of per-field data pointers. | Each entry points into a child vector's data buffer.  The array itself is bridge-allocated scratch (DuckDB heap) freed after the wrapper call returns. |
| `field_validity` | `const uint64_t *const *` | **Borrowed** array of per-field validity bitmaps. | Each entry may be `NULL` (all-valid). Array is bridge-allocated scratch. |
| `field_count` | `uint64_t` | Value | Number of struct fields. |
| `offset` | `uint64_t` | Value | Global row offset for indexing into field data/validity. |

Access helpers: `ducktinycc_struct_field_ptr(st, idx)`,
`ducktinycc_struct_field_is_valid(st, field_idx)`.

**Validity indexing:** `ducktinycc_struct_field_is_valid` uses `st->offset` as
the row index into each field's validity bitmap (not element-within-field).

### `ducktinycc_map_t`

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `key_ptr` | `const void *` | **Borrowed** from list-child key vector. | Flat key data. |
| `key_validity` | `const uint64_t *` | **Borrowed** | May be `NULL`. |
| `value_ptr` | `const void *` | **Borrowed** from list-child value vector. | Flat value data. |
| `value_validity` | `const uint64_t *` | **Borrowed** | May be `NULL`. |
| `offset` | `uint64_t` | Value | Global child-vector start offset. |
| `len` | `uint64_t` | Value | Number of key/value pairs in this row's map. |

Access helpers: `ducktinycc_map_key_ptr(m, idx, key_size)`,
`ducktinycc_map_value_ptr(m, idx, value_size)`,
`ducktinycc_map_key_is_valid(m, idx)`,
`ducktinycc_map_value_is_valid(m, idx)`.

### `ducktinycc_union_t`

DuckDB UNIONs are stored as STRUCTs: child\[0\] is the tag vector (`uint8_t`),
children\[1..N\] are member vectors.

| Field | Type | Ownership | Notes |
|-------|------|-----------|-------|
| `tag_ptr` | `const uint8_t *` | **Borrowed** from tag child vector data buffer. | Index with `tag_ptr[offset]` to get active member index. |
| `member_ptrs` | `const void *const *` | **Borrowed** array of per-member data pointers. | Bridge-allocated scratch (DuckDB heap). |
| `member_validity` | `const uint64_t *const *` | **Borrowed** array of per-member validity bitmaps. | Bridge-allocated scratch. Each entry may be `NULL`. |
| `member_count` | `uint64_t` | Value | Number of union members. |
| `offset` | `uint64_t` | Value | Global row offset for tag and member indexing. |

Access helpers: `ducktinycc_union_tag(u)`,
`ducktinycc_union_member_ptr(u, member_idx)`,
`ducktinycc_union_member_is_valid(u, member_idx)`.

## Validity Bitmap Semantics

Validity bitmaps follow DuckDB conventions:

- Bit *set* (1) = value is valid (non-NULL).
- Bit *clear* (0) = value is NULL.
- A `NULL` validity pointer means all values are valid (no NULLs in the vector).
- Index calculation: word = `idx >> 6`, bit = `idx & 63`.

The `offset` field in descriptors is a **global** row offset into child vectors.
For list/array/map elements, the global index is `offset + local_idx`.
For struct/union fields, `offset` is the row index into each field's validity
bitmap.

Helper functions (`ducktinycc_valid_is_set`, `ducktinycc_valid_set`) handle
all of this transparently; the `*_is_valid` descriptor accessors add the offset
automatically.

## Pointer Registry (`tcc_alloc` / `tcc_free_ptr`)

The pointer registry is a process-global, mutex-protected table of
libc-heap-allocated memory spans, exposed to SQL via scalar UDFs.

| Operation | SQL Function | Heap | Notes |
|-----------|-------------|------|-------|
| Allocate | `tcc_alloc(nbytes)` | `malloc` | Returns `uint64_t` handle. |
| Free | `tcc_free_ptr(handle)` | `free` | Frees the backing buffer and removes the registry entry. |
| Read | `tcc_read_*(handle, offset)` | — | Reads from the registered buffer (bounds-checked). |
| Write | `tcc_write_*(handle, offset, value)` | — | Writes to the registered buffer (bounds-checked). |
| Data pointer | `tcc_dataptr(handle)` | — | Returns raw `uint64_t` pointer value (for passing to C). |

**Ownership:** `tcc_alloc` returns an owned handle.  The caller is responsible
for calling `tcc_free_ptr` exactly once.  Double-free is a no-op (registry
entry already removed).

**Thread safety:** the registry is protected by a mutex; concurrent
`tcc_alloc`/`tcc_free_ptr` from different connections is safe.

## Generated Helper Functions (`c_struct` / `c_union` / `c_bitfield` / `c_enum`)

Generated helpers operate on **caller-owned** memory (typically a `tcc_alloc`
buffer or stack allocation in user C code). Helper scalar-function stability is
assigned explicitly: metadata helpers (`sizeof`, `alignof`, offsets, enum
constants) are consistent; helpers that allocate/free/mutate memory or read
mutable pointer-backed fields are volatile so DuckDB re-runs them per row.

| Helper | Heap | Contract |
|--------|------|----------|
| `{prefix}_new()` | `malloc` | Returns owned pointer; caller must call `{prefix}_free`. |
| `{prefix}_free(p)` | `free` | Frees pointer from `{prefix}_new`. NULL-safe. |
| `{prefix}_sizeof()` | — | Returns `sizeof` the underlying C type. |
| `{prefix}_alignof()` | — | Returns alignment of the underlying C type. |
| `{prefix}_get_{field}(p)` | — | Reads field; `p` is **borrowed** (not freed). |
| `{prefix}_set_{field}(p, v)` | — | Writes field; returns `p`. |
| `{prefix}_off_{field}()` | — | Returns `offsetof`. Not generated for bitfields. |
| `{prefix}_{field}_addr(p)` | — | Returns pointer to field within `p`. Not generated for bitfields. |
| `{prefix}_get_{field}_elt(p, idx)` | — | Array field element getter (bounds-checked). |
| `{prefix}_set_{field}_elt(p, idx, v)` | — | Array field element setter (bounds-checked). |
| `{prefix}_{CONSTANT}()` | — | Enum constant value (returns `long long`). |

## Buffer R/W Helpers

These operate on arbitrary memory spans (typically from descriptor `ptr` fields
or pointer-registry buffers).  All are bounds-checked; out-of-bounds access
returns 0 / `NULL`.

| Helper | Signature | Contract |
|--------|-----------|----------|
| `ducktinycc_read_bytes` | `(base, len, offset, out, width) → int` | Copies `width` bytes. `base` borrowed, `out` caller-owned. |
| `ducktinycc_write_bytes` | `(base, len, offset, in, width) → int` | Copies `width` bytes. `base` caller-owned, `in` borrowed. |
| `ducktinycc_read_{T}` | `(base, len, offset, out) → int` | Typed variant for `T` ∈ {i8,u8,i16,u16,i32,u32,i64,u64,f32,f64,ptr}. |
| `ducktinycc_write_{T}` | `(base, len, offset, value) → int` | Typed variant. |
| `ducktinycc_buf_ptr_at` | `(base, len, offset, width) → const void *` | Returns pointer or `NULL` if out of bounds. **Borrowed** from `base`. |
| `ducktinycc_buf_ptr_at_mut` | `(base, len, offset, width) → void *` | Mutable variant. |
| `ducktinycc_ptr_add` | `(base, byte_offset) → const void *` | Unchecked pointer arithmetic. `NULL`-safe. |
| `ducktinycc_ptr_add_mut` | `(base, byte_offset) → void *` | Mutable variant. |
| `ducktinycc_span_contains` | `(len, idx) → int` | Returns 1 if `idx < len`. |
| `ducktinycc_span_fits` | `(len, offset, width) → int` | Returns 1 if `[offset, offset+width)` fits within `[0, len)`. |

## DuckDB Extension State Lifecycle

| Object | Allocator | Freed By | When |
|--------|-----------|----------|------|
| Bind payload | `duckdb_malloc` | `destroy_bind_data` callback | DuckDB disposes function state. |
| Init payload | `duckdb_malloc` | `destroy_init_data` callback | DuckDB disposes function state. |
| Host signature context | `duckdb_malloc` | `tcc_host_sig_ctx_destroy` (extra-info destructor) | DuckDB drops the registered scalar UDF. |
| TCC artifact (`TCCState` + relocated code) | libtcc internal | `tcc_artifact_destroy` (registry cleanup / module-state destructor) | Replacement compile or extension shutdown. |
| Generated C source | `duckdb_malloc` | Caller after `tcc_compile_string` | Immediately after compilation. |
| Bridge scratch (field_ptrs, member_ptrs arrays) | `duckdb_malloc` | `tcc_execute_compiled_scalar_udf` cleanup path | After each UDF chunk execution. |
