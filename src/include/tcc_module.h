/*
 * DuckTinyCC
 * SPDX-License-Identifier: MIT
 *
 * Role: public registration entrypoint for `tcc_module(...)` and related helper tables/functions.
 * Architecture details and TinyCC/Rtinycc precedent are documented in `src/tcc_module.c`.
 */

#pragma once

#include "duckdb_extension.h"

bool RegisterTccModuleFunction(duckdb_connection connection, duckdb_database database);
