#ifndef TCC_WASM32_DUCKTINYCC_HOST_H
#define TCC_WASM32_DUCKTINYCC_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned long long idx_t;
typedef uintptr_t duckdb_data_chunk;
typedef uintptr_t duckdb_vector;
typedef uintptr_t duckdb_scalar_function;
typedef uintptr_t duckdb_function_info;

void *duckdb_malloc(size_t size);
void duckdb_free(void *ptr);
idx_t duckdb_data_chunk_get_size(duckdb_data_chunk chunk);
duckdb_vector duckdb_data_chunk_get_vector(duckdb_data_chunk chunk, idx_t col);
void *duckdb_vector_get_data(duckdb_vector vector);
void duckdb_scalar_function_set_error(duckdb_function_info info, const char *error);

#endif
