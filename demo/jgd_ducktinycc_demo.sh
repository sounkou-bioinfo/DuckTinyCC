#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXT_PATH="${1:-$ROOT_DIR/build/release/ducktinycc.duckdb_extension}"
DUCKDB_BIN="${DUCKDB_BIN:-duckdb}"
PORT="${JGD_PORT:-$((20000 + RANDOM % 20000))}"
CAPTURE_JSON="${JGD_CAPTURE_JSON:-$(mktemp /tmp/ducktinycc_jgd_capture.XXXXXX.json)}"
SERVER_LOG="${JGD_SERVER_LOG:-$(mktemp /tmp/ducktinycc_jgd_server.XXXXXX.log)}"

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

python3 "$ROOT_DIR/demo/jgd_capture_server.py" "$PORT" "$CAPTURE_JSON" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
  if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
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
  echo "jgd capture server did not start" >&2
  cat "$SERVER_LOG" >&2 || true
  exit 1
fi

export R_HOME="${R_HOME:-$(R RHOME)}"
R_INCLUDE_FLAGS="$(R CMD config --cppflags | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
R_LIBDIR_FLAGS="$(R CMD config --ldflags | tr ' ' '\n' | grep '^-L' | tr '\n' ' ')"
SOURCE_SQL="$(sed "s/'/''/g" "$ROOT_DIR/demo/jgd_ducktinycc_udf.c")"
SOCKET_URI="tcp://127.0.0.1:${PORT}"

echo "DuckTinyCC + jgd demo"
echo "capture server: ${SOCKET_URI}"
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
echo "captured jgd protocol: ${CAPTURE_JSON}"
python3 - "$CAPTURE_JSON" <<'PY'
import collections
import json
import sys
path = sys.argv[1]
with open(path, encoding='utf-8') as fp:
    data = json.load(fp)
messages = data.get('messages', [])
frames = data.get('frames', [])
print(f"messages: {len(messages)}")
print(f"frames:   {len(frames)}")
if frames:
    frame_ops = [frame.get('plot', {}).get('ops', []) for frame in frames]
    all_ops = [op for ops in frame_ops for op in ops]
    counts = collections.Counter(op.get('op', '<missing>') for op in all_ops)
    print(f"plotNumber(s): {sorted({frame.get('plotNumber') for frame in frames})}")
    print(f"frame operations: {', '.join(str(len(ops)) for ops in frame_ops)}")
    print(f"total operations: {len(all_ops)}")
    print("operation histogram:")
    for name, count in counts.most_common():
        print(f"  {name:12s} {count}")
    print("first drawing ops:")
    for op in all_ops[:8]:
        small = {k: op[k] for k in op if k in ('op', 'x', 'y', 'x0', 'y0', 'x1', 'y1', 'r', 'text')}
        print("  " + json.dumps(small, ensure_ascii=False))
PY
