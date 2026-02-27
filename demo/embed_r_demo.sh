#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXT_PATH="${1:-$ROOT_DIR/build/release/ducktinycc.duckdb_extension}"

export R_HOME="${R_HOME:-$(R RHOME)}"
R_INCLUDE_FLAGS="$(R CMD config --cppflags | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
R_LIBDIR_FLAGS="$(R CMD config --ldflags | tr ' ' '\n' | grep '^-L' | tr '\n' ' ')"
SOURCE_SQL="$(sed "s/'/''/g" "$ROOT_DIR/demo/embed_r_udf.c")"

duckdb -unsigned <<SQL
LOAD '${EXT_PATH}';
PRAGMA threads=1;

SELECT ok, mode, code FROM tcc_module(mode := 'tcc_new_state');
SELECT ok, mode, code FROM tcc_module(mode := 'add_include', include_path := 'third_party/tinycc/include');
SELECT ok, mode, code FROM tcc_module(mode := 'add_sysinclude', sysinclude_path := 'third_party/tinycc/include');
SELECT ok, mode, code FROM tcc_module(mode := 'add_define', define_name := 'R_LEGACY_RCOMPLEX', define_value := '1');
SELECT ok, mode, code FROM tcc_module(mode := 'add_option', option := '${R_INCLUDE_FLAGS} ${R_LIBDIR_FLAGS}');
SELECT ok, mode, code FROM tcc_module(mode := 'add_library', library := 'R');
SELECT ok, mode, code FROM tcc_module(mode := 'add_source', source := '${SOURCE_SQL}');
SELECT ok, mode, code FROM tcc_module(mode := 'tinycc_bind', symbol := 'r_hello_from_embedded', sql_name := 'r_hello_from_embedded');
SELECT ok, mode, code FROM tcc_module(mode := 'compile', return_type := 'varchar', arg_types := []);
SELECT r_hello_from_embedded() AS msg;
SQL
