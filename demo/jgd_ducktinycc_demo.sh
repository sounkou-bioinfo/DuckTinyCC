#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXT_PATH="${1:-$ROOT_DIR/build/release/ducktinycc.duckdb_extension}"
DUCKDB_BIN="${DUCKDB_BIN:-duckdb}"
PORT="${JGD_PORT:-$((20000 + RANDOM % 20000))}"
SERVER_LOG="${JGD_SERVER_LOG:-$(mktemp /tmp/ducktinycc_jgd_server.XXXXXX.log)}"
SERVER_BIN="${JGD_SERVER_BIN:-$(mktemp /tmp/ducktinycc_jgd_terminal_server.XXXXXX)}"

if ! command -v R >/dev/null 2>&1; then
  echo "R is required for this demo" >&2
  exit 2
fi
if ! Rscript -e 'quit(status = !requireNamespace("jgd", quietly = TRUE))' >/dev/null 2>&1; then
  echo "R package 'jgd' is required. Try: R CMD INSTALL /tmp/jgd/r-pkg  # after cloning https://github.com/grantmcdermott/jgd" >&2
  exit 2
fi
if [[ ! -f "$EXT_PATH" ]]; then
  echo "Extension not found: $EXT_PATH" >&2
  echo "Build first, e.g. make release, or pass a .duckdb_extension path." >&2
  exit 2
fi

cc -std=c11 -O2 -Wall -Wextra -o "$SERVER_BIN" "$ROOT_DIR/demo/jgd_terminal_server.c" -lm
"$SERVER_BIN" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
  if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
  fi
  if [[ "${JGD_KEEP_SERVER_BIN:-0}" != "1" ]]; then
    rm -f "$SERVER_BIN"
  fi
}
trap cleanup EXIT

for _ in $(seq 1 100); do
  if grep -q '^ready ' "$SERVER_LOG"; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^ready ' "$SERVER_LOG"; then
  echo "jgd terminal server did not start" >&2
  cat "$SERVER_LOG" >&2 || true
  exit 1
fi

export R_HOME="${R_HOME:-$(R RHOME)}"
R_INCLUDE_FLAGS="$(R CMD config --cppflags | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
R_LIBDIR_FLAGS="$(R CMD config --ldflags | tr ' ' '\n' | grep '^-L' | tr '\n' ' ')"
SOURCE_SQL="$(sed "s/'/''/g" "$ROOT_DIR/demo/jgd_ducktinycc_udf.c")"
SOCKET_URI="tcp://127.0.0.1:${PORT}"

echo "DuckTinyCC + jgd demo"
echo "terminal server: ${SOCKET_URI}"
echo

"$DUCKDB_BIN" -unsigned <<SQL
.bail on
LOAD '${EXT_PATH}';
PRAGMA threads=1;

SELECT ok, mode, code FROM tcc_module(mode := 'tcc_new_state');
SELECT ok, mode, code FROM tcc_module(mode := 'add_include', include_path := 'third_party/tinycc/include');
SELECT ok, mode, code FROM tcc_module(mode := 'add_sysinclude', sysinclude_path := 'third_party/tinycc/include');
SELECT ok, mode, code FROM tcc_module(mode := 'add_define', define_name := 'R_LEGACY_RCOMPLEX', define_value := '1');
SELECT ok, mode, code FROM tcc_module(mode := 'add_option', option := '${R_INCLUDE_FLAGS} ${R_LIBDIR_FLAGS}');
SELECT ok, mode, code FROM tcc_module(mode := 'add_library', library := 'R');
SELECT ok, mode, code FROM tcc_module(mode := 'add_source', source := '${SOURCE_SQL}');
SELECT ok, mode, code FROM tcc_module(mode := 'tinycc_bind', symbol := 'r_jgd_plot', sql_name := 'r_jgd_plot', stability := 'volatile');
SELECT ok, mode, code FROM tcc_module(mode := 'compile', return_type := 'varchar', arg_types := ['varchar']);

SELECT r_jgd_plot('${SOCKET_URI}') AS status;
SQL

wait "$SERVER_PID" || true
trap - EXIT

echo
cat "$SERVER_LOG"
rm -f "$SERVER_LOG"
if [[ "${JGD_KEEP_SERVER_BIN:-0}" != "1" ]]; then
  rm -f "$SERVER_BIN"
fi
