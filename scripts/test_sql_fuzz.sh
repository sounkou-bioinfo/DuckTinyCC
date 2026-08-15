#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
extension=${1:-$root/build/release/ducktinycc.duckdb_extension}
seed=${DUCKTINYCC_SQL_FUZZ_SEED:-169}
trials=${DUCKTINYCC_SQL_FUZZ_TRIALS:-250}
duckdb_bin=${DUCKDB_BIN:-duckdb}
cc=${CC:-cc}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ducktinycc-sql-fuzz.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$cc" -std=c11 -Wall -Wextra -Werror -O2 \
  "$root/test/fuzz/generate_sql_fuzz.c" -o "$tmp/generate_sql_fuzz"
"$tmp/generate_sql_fuzz" "$extension" "$seed" "$trials" >"$tmp/campaign.sql"

set +e
"$duckdb_bin" -unsigned -csv -noheader <"$tmp/campaign.sql" >"$tmp/stdout" 2>"$tmp/stderr"
rc=$?
set -e

if [ "$rc" -ne 0 ] || \
   grep -Eq 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$tmp/stderr" || \
   ! grep -q "^DUCKTINYCC_SQL_FUZZ_OK seed=$seed trials=$trials$" "$tmp/stdout"; then
  artifact_dir=${DUCKTINYCC_FUZZ_ARTIFACT_DIR:-$root/.fuzz/artifacts}
  mkdir -p "$artifact_dir"
  cp "$tmp/campaign.sql" "$artifact_dir/sql-seed-${seed}.sql"
  cp "$tmp/stdout" "$artifact_dir/sql-seed-${seed}.stdout"
  cp "$tmp/stderr" "$artifact_dir/sql-seed-${seed}.stderr"
  cat "$tmp/stderr" >&2
  echo "SQL fuzz failure saved under $artifact_dir" >&2
  exit 1
fi

echo "DuckTinyCC SQL fuzz: OK ($trials trials, seed=$seed)"
