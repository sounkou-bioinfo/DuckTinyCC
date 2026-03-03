# C Function Documentation Tags (Roxygen2-Style for C)

DuckTinyCC uses Doxygen-compatible block comments with a roxygen2-like tag style to make ownership and memory behavior explicit.

## Required Tags

- `@function`: C symbol name.
- `@brief`: one-sentence purpose.
- `@param[in|out|in,out]`: parameter contract.
- `@return`: result contract.
- `@ownership`: borrowed/owned/transferred semantics.
- `@heap`: allocations/frees and allocator domain (`duckdb_malloc/free` vs `malloc/free`).
- `@stack`: notable stack usage and bounds.
- `@thread_safety`: concurrency guarantees/assumptions.
- `@locks`: lock acquisition/release rules.
- `@errors`: how errors are signaled (bool return, DuckDB error setter, etc.).

## Optional Tags

- `@pre`: required preconditions.
- `@post`: guaranteed postconditions.
- `@invariant`: maintained invariants.
- `@note`: non-obvious compatibility or ABI detail.

## Template

```c
/**
 * @function tcc_example
 * @brief Short description.
 * @param[in,out] ctx Borrowed context pointer.
 * @param[in] value Input value.
 * @param[out] out_result Output slot.
 * @return true on success, false on failure.
 * @ownership borrows(ctx), transfers(none)
 * @heap allocates duckdb_malloc(...), freed by caller via destroy helper
 * @stack fixed-size locals only
 * @thread_safety protected by caller-held lock
 * @locks requires ctx->lock held
 * @errors boolean return; caller maps to duckdb_scalar_function_set_error
 */
```

## Rules

- Always document allocator domain when returning or storing pointers.
- If ownership moves, state the exact transfer point.
- Keep cleanup path singular when possible; reference the destroy helper.
- For descriptor bridges, mark fields as borrowed unless owned buffers are explicitly allocated.
