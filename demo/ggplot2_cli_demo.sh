#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXT_PATH="${1:-$ROOT_DIR/build/release/ducktinycc.duckdb_extension}"
DUCKDB_BIN="${DUCKDB_BIN:-duckdb}"

if ! command -v R >/dev/null 2>&1; then
  echo "R is required for this demo" >&2
  exit 2
fi
if ! Rscript -e 'quit(status = !requireNamespace("ggplot2", quietly = TRUE))' >/dev/null 2>&1; then
  echo "R package 'ggplot2' is required for this demo" >&2
  exit 2
fi
if [[ ! -f "$EXT_PATH" ]]; then
  echo "Extension not found: $EXT_PATH" >&2
  echo "Build first, e.g. make release, or pass a .duckdb_extension path." >&2
  exit 2
fi

export R_HOME="${R_HOME:-$(R RHOME)}"
R_INCLUDE_FLAGS="$(R CMD config --cppflags | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
R_LIBDIR_FLAGS="$(R CMD config --ldflags | tr ' ' '\n' | grep '^-L' | tr '\n' ' ')"
SOURCE_SQL="$(sed "s/'/''/g" "$ROOT_DIR/demo/ggplot2_cli_udf.c")"

"$DUCKDB_BIN" -unsigned <<SQL
.bail on
.headers off
.mode list
.separator ""
LOAD '${EXT_PATH}';
PRAGMA threads=1;

SELECT 'DuckTinyCC ggplot2 CLI demo';
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'tcc_new_state');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'add_include', include_path := 'third_party/tinycc/include');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'add_sysinclude', sysinclude_path := 'third_party/tinycc/include');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'add_define', define_name := 'R_LEGACY_RCOMPLEX', define_value := '1');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'add_option', option := '${R_INCLUDE_FLAGS} ${R_LIBDIR_FLAGS}');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'add_library', library := 'R');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'add_source', source := '${SOURCE_SQL}');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'tinycc_bind', symbol := 'r_ggplot2_cli_plot', sql_name := 'r_ggplot2_cli_plot', stability := 'volatile');
SELECT ok || ' ' || mode || ' ' || code FROM tcc_module(mode := 'compile', return_type := 'varchar', arg_types := []);

SELECT '';
SELECT r_ggplot2_cli_plot();
SQL
