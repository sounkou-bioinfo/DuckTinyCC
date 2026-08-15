#!/usr/bin/env bash
set -euo pipefail

sanitizer=${1:-asan}
case "$sanitizer" in
  asan)
    compile_flags=(-O1 -g -fsanitize=address -fno-omit-frame-pointer)
    link_flags=(-fsanitize=address)
    runtime=(env "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1" \
      "LD_PRELOAD=$(${CC:-cc} -print-file-name=libasan.so)")
    ;;
  ubsan)
    compile_flags=(-O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined)
    link_flags=(-fsanitize=undefined)
    runtime=(env "UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1")
    ;;
  *)
    echo "usage: $0 {asan|ubsan}" >&2
    exit 2
    ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
build_dir="cmake_build/sanitizer-$sanitizer"
out_dir="build/sanitizer-$sanitizer"
rm -rf "$build_dir" "$out_dir"
cmake -S . -B "$build_dir" \
  -DEXTENSION_NAME=ducktinycc \
  -DTARGET_DUCKDB_VERSION_MAJOR=1 \
  -DTARGET_DUCKDB_VERSION_MINOR=2 \
  -DTARGET_DUCKDB_VERSION_PATCH=0 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="${compile_flags[*]}" \
  -DDUCKTINYCC_TINYCC_EXTRA_CFLAGS="${compile_flags[*]}" \
  -DDUCKTINYCC_TINYCC_EXTRA_LDFLAGS="${link_flags[*]}" \
  -DCMAKE_SHARED_LINKER_FLAGS="${link_flags[*]}"
cmake --build "$build_dir" --config Debug -j2
mkdir -p "$out_dir"
cp "$build_dir/libducktinycc.so" "$out_dir/libducktinycc.so"
./configure/venv/bin/python3 extension-ci-tools/scripts/append_extension_metadata.py \
  -l "$out_dir/libducktinycc.so" \
  -o "$out_dir/ducktinycc.duckdb_extension" \
  -n ducktinycc -dv v1.2.0 \
  -evf configure/extension_version.txt -pf configure/platform.txt

extension="$root/$out_dir/ducktinycc.duckdb_extension"
DUCKTINYCC_SQL_FUZZ_TRIALS=${DUCKTINYCC_SQL_FUZZ_TRIALS:-500} \
  "${runtime[@]}" bash scripts/test_sql_fuzz.sh "$extension"

echo "$sanitizer complete-extension SQL fuzz gate: OK"
