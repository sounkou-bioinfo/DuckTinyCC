/*
 * DuckTinyCC
 * SPDX-License-Identifier: MIT
 *
 * Role: public registration entrypoint for `tcc_module(...)` and related helper tables/functions.
 * Architecture details and TinyCC/Rtinycc precedent are documented in `src/tcc_module.c`.
 *
 * Runtime model notes:
 * - Compile/codegen paths create and relocate TinyCC modules in memory (no per-UDF shared object artifact).
 * - Generated module init functions register scalar UDFs against a persistent host DuckDB connection.
 * - TinyCC state ownership is internal to the module registry and finalized by module-state/artifact destructors.
 *
 * Linking notes for SQL surface:
 * - `add_library_path` configures explicit linker search paths.
 * - `add_library` accepts bare names and full path-like library values.
 */

#pragma once

#include "duckdb_extension.h"

bool RegisterTccModuleFunction(duckdb_connection connection, duckdb_database database);
