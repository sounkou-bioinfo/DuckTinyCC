#!/usr/bin/env bash
set -euo pipefail

# Destructive/negative control-flow probes for DuckTinyCC generated UDFs.
# These are intentionally not part of sqllogictest: some cases terminate or hang
# the DuckDB subprocess when user C bypasses the normal UDF return path.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXTENSION_PATH="${1:-$ROOT_DIR/build/debug/ducktinycc.duckdb_extension}"
DUCKDB_BIN="${DUCKDB_BIN:-duckdb}"

if [[ ! -f "$EXTENSION_PATH" ]]; then
  echo "Extension not found: $EXTENSION_PATH" >&2
  echo "Build first, e.g. make debug, or pass a .duckdb_extension path." >&2
  exit 2
fi

run_sql_csv() {
  local sql="$1"
  "$DUCKDB_BIN" -unsigned -csv -noheader -c "LOAD '$EXTENSION_PATH'; $sql"
}

assert_eq() {
  local got="$1"
  local expected="$2"
  local label="$3"
  if [[ "$got" != "$expected" ]]; then
    echo "FAIL: $label" >&2
    echo "  expected: $expected" >&2
    echo "  got:      $got" >&2
    exit 1
  fi
  echo "ok: $label"
}

# Normal libc process-control functions are not exported into the JIT by default.
# Today they fail at relocation/compile time instead of being catchable runtime errors.
got=$(run_sql_csv "SELECT code FROM tcc_module(
  mode := 'quick_compile',
  source := 'extern void exit(int); int udf_calls_exit(int x){ exit(77); return x; }',
  symbol := 'udf_calls_exit',
  sql_name := 'udf_calls_exit',
  return_type := 'i32',
  arg_types := ['i32'],
  stability := 'volatile'
);")
assert_eq "$got" "E_COMPILE_FAILED" "extern exit() without an explicit libc link is rejected as an unresolved symbol"

# If libc is explicitly linked and available to TinyCC, exit resolves and remains
# an uncaught process-control escape.
tmp_sql=$(mktemp)
cat > "$tmp_sql" <<SQL
LOAD '$EXTENSION_PATH';
SELECT code FROM tcc_module(
  mode := 'quick_compile',
  source := 'extern void exit(int); int udf_calls_exit_libc(int x){ exit(79); return x; }',
  symbol := 'udf_calls_exit_libc',
  sql_name := 'udf_calls_exit_libc',
  return_type := 'i32',
  arg_types := ['i32'],
  library := 'c',
  stability := 'volatile'
);
SELECT udf_calls_exit_libc(1);
SQL
set +e
timeout 5s "$DUCKDB_BIN" -unsigned -csv -noheader < "$tmp_sql" >/tmp/ducktinycc_udf_exit_libc.out 2>/tmp/ducktinycc_udf_exit_libc.err
rc=$?
set -e
rm -f "$tmp_sql"
assert_eq "$rc" "79" "explicitly linked libc exit() terminates DuckDB subprocess"

got=$(run_sql_csv "SELECT code FROM tcc_module(
  mode := 'quick_compile',
  source := \$\$#include <setjmp.h>
static jmp_buf env;
int udf_longjmp_self(int x){ if (setjmp(env)==0) longjmp(env, 1); return x + 1; }\$\$,
  symbol := 'udf_longjmp_self',
  sql_name := 'udf_longjmp_self',
  return_type := 'i32',
  arg_types := ['i32'],
  stability := 'volatile'
);")
assert_eq "$got" "E_COMPILE_FAILED" "setjmp/longjmp are rejected as unresolved symbols"

# A malicious/native UDF can still bypass symbols entirely. On Linux x86_64,
# invoking exit_group via inline syscall terminates the DuckDB subprocess. This is
# the key safety boundary: DuckTinyCC does not currently sandbox or catch this.
if [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "x86_64" ]]; then
  tmp_sql=$(mktemp)
  cat > "$tmp_sql" <<SQL
LOAD '$EXTENSION_PATH';
SELECT code FROM tcc_module(
  mode := 'quick_compile',
  source := 'int udf_exit_group(int x){ __asm__("mov \$78,%rdi; mov \$231,%rax; syscall"); return x; }',
  symbol := 'udf_exit_group',
  sql_name := 'udf_exit_group',
  return_type := 'i32',
  arg_types := ['i32'],
  stability := 'volatile'
);
SELECT udf_exit_group(1);
SQL
  set +e
  timeout 5s "$DUCKDB_BIN" -unsigned -csv -noheader < "$tmp_sql" >/tmp/ducktinycc_udf_exit_group.out 2>/tmp/ducktinycc_udf_exit_group.err
  rc=$?
  set -e
  rm -f "$tmp_sql"
  assert_eq "$rc" "78" "inline exit_group syscall terminates DuckDB subprocess"
else
  echo "skip: inline exit_group syscall probe is Linux x86_64-specific"
fi

# Optional demonstration of a worse failure mode: raw SYS_exit exits only the
# executing thread and can leave the DuckDB process hung until killed by timeout.
if [[ "${DUCKTINYCC_RUN_HANG_PROBE:-0}" == "1" && "$(uname -s)" == "Linux" && "$(uname -m)" == "x86_64" ]]; then
  tmp_sql=$(mktemp)
  cat > "$tmp_sql" <<SQL
LOAD '$EXTENSION_PATH';
SELECT code FROM tcc_module(
  mode := 'quick_compile',
  source := 'int udf_thread_exit(int x){ __asm__("mov \$77,%rdi; mov \$60,%rax; syscall"); return x; }',
  symbol := 'udf_thread_exit',
  sql_name := 'udf_thread_exit',
  return_type := 'i32',
  arg_types := ['i32'],
  stability := 'volatile'
);
SELECT udf_thread_exit(1);
SQL
  set +e
  timeout 5s "$DUCKDB_BIN" -unsigned -csv -noheader < "$tmp_sql" >/tmp/ducktinycc_udf_thread_exit.out 2>/tmp/ducktinycc_udf_thread_exit.err
  rc=$?
  set -e
  rm -f "$tmp_sql"
  assert_eq "$rc" "124" "inline SYS_exit can hang DuckDB until timeout"
fi
