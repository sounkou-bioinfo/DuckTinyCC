# Internals and ownership

This page records the invariants that implementation changes and generated C
must preserve.

## Compile pipeline

A compile operation follows one explicit path:

```text
tcc_new
  → set embedded runtime path
  → set TCC_OUTPUT_MEMORY and -nostdlib
  → replay staged paths/options/defines/symbols
  → compile source and generated wrapper
  → tcc_relocate
  → resolve module_init
  → module_init(connection)
  → retain artifact in the extension registry
```

`tcc_new_state` only clears staged session inputs and increments `state_id`.
Real `TCCState` objects are created by compile/codegen paths.

## Artifact lifetime

Relocated function pointers remain valid only while their owning `TCCState`
exists. Registration metadata therefore owns or reaches the compiled artifact.

- Compile errors call `tcc_delete` immediately.
- Successful registration transfers the artifact to the module registry.
- Replacement or extension shutdown destroys it through
  `tcc_artifact_destroy`.
- Registered SQL functions must never point into a deleted artifact.

A raw symbol address is not an ownership mechanism. The object, module, or
foreign runtime behind an injected address must outlive every compiled call.

## Heap domains

| Domain | Allocate/free pair | Representative ownership |
|---|---|---|
| DuckDB | `duckdb_malloc` / `duckdb_free` | Extension state, parsed metadata, bridge scratch, generated source, UDF extra-info. |
| libc | `malloc` / `free` | Pointer-registry buffers and generated helper allocations through host wrappers. |
| TinyCC | libtcc APIs / `tcc_delete` | Compiler state, object sections, and relocated module code. |

Never cross allocator domains. Generated `{prefix}_new()` and
`{prefix}_free()` use host wrappers so allocation and release stay in one libc
CRT domain.

TinyCC may lower aggregate copies to `memcpy`, `memmove`, or `memset`.
DuckTinyCC injects host-backed definitions for those compiler-support symbols;
that is not general implicit libc linking.

## Descriptor views

Composite arguments are borrowed views into DuckDB vector memory. Generated C
must not free their fields or retain them after the wrapper returns.

| Descriptor | Data view | Offset rule |
|---|---|---|
| `ducktinycc_list_t` | Row-sliced child pointer plus length. | Index data locally; use `offset + i` only for child validity. |
| `ducktinycc_array_t` | Same layout as LIST with fixed length. | Same as LIST. |
| `ducktinycc_map_t` | Row-sliced key and value pointers. | Index data locally; use `offset + i` for key/value validity. |
| `ducktinycc_struct_t` | Arrays of per-field data and validity pointers. | `offset` is the row index into each child field. |
| `ducktinycc_union_t` | Tag pointer plus member data/validity arrays. | `offset` indexes the tag and active member row. |

A `NULL` validity pointer means all values are valid. Otherwise bit 1 is valid
and bit 0 is SQL `NULL`. Use the injected `ducktinycc_*` accessors rather than
reimplementing bitmap or offset arithmetic.

The row-sliced LIST/ARRAY/MAP rule is deliberate: adding the child offset to a
data pointer a second time reads the wrong row.

## Pointer registry

`tcc_alloc(n)` returns a process-local handle for a libc allocation. Registry
operations are mutex-protected and bounds-checked. `tcc_free_ptr(handle)`
removes the entry and frees it; a second free sees no entry.

`tcc_dataptr(handle)` exposes the backing address as `UBIGINT`. Once converted
to an address, bounds, type, and ownership are the caller's responsibility.
Freeing the handle invalidates every derived address.

## Runtime and embedded assets

`libtcc1.a` and TinyCC headers are generated into the extension as deterministic
byte arrays. Extraction uses a content key covering the archive and every
manifest name and byte. The runtime is published to a temporary directory and
reused only after its expected files are verified.

The runtime path is set before `TCC_OUTPUT_MEMORY`, because TinyCC expands `{B}`
while configuring system include paths.

## Trusted native-code boundary

Generated C runs inside DuckDB with the process's authority. DuckTinyCC checks
SQL metadata and bridge bounds, but cannot contain arbitrary native behavior.
Explicit libc links, foreign callbacks, process-control APIs, assembly, syscalls,
and invalid pointers can terminate or corrupt the process.

Unsafe control-flow probes therefore run in subprocesses. Hostile or crash-prone
C requires process isolation outside DuckTinyCC.
