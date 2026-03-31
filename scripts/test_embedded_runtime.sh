#!/usr/bin/env bash
# scripts/test_embedded_runtime.sh
#
# Verifies that the embedded TinyCC runtime (libtcc1.a + headers baked into the
# extension binary) works correctly when the compile-time build directory is absent.
#
# Strategy:
#   1. Rename cmake_build/{BUILD}/tinycc_build -> tinycc_build_hidden so that
#      DUCKTINYCC_DEFAULT_RUNTIME_PATH (which points there) returns !exists().
#   2. Run the full sqllogictest suite against the extension.
#      Every test that calls compile/quick_compile will hit tcc_ensure_embedded_runtime()
#      and extract libtcc1.a + headers to a fresh temp directory.
#   3. Restore the directory unconditionally on exit (trap).
#
# Usage:
#   scripts/test_embedded_runtime.sh          # tests debug build (default)
#   scripts/test_embedded_runtime.sh release  # tests release build
#
# Prerequisites:
#   make debug  (or make release) must have been run first.
#   configure/venv must exist (make configure).

set -euo pipefail

BUILD="${1:-debug}"
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
VENV_BIN="${PROJ}/configure/venv/bin/python3"
EXT="${PROJ}/build/${BUILD}/ducktinycc.duckdb_extension"
TINYCC_BUILD_DIR="${PROJ}/cmake_build/${BUILD}/tinycc_build"
TINYCC_BUILD_HIDDEN="${PROJ}/cmake_build/${BUILD}/tinycc_build_hidden"

# ---- sanity checks ----------------------------------------------------------

if [ ! -f "${EXT}" ]; then
    echo "ERROR: extension not built at ${EXT}" >&2
    echo "       Run: make ${BUILD}" >&2
    exit 1
fi

if [ ! -d "${TINYCC_BUILD_DIR}" ]; then
    echo "WARNING: ${TINYCC_BUILD_DIR} already absent; embedded path will be used unconditionally."
    SKIP_RESTORE=1
else
    SKIP_RESTORE=0
fi

# ---- hide the build dir and restore on exit ---------------------------------

restore_tinycc_build() {
    if [ "${SKIP_RESTORE:-0}" = "0" ] && [ -d "${TINYCC_BUILD_HIDDEN}" ]; then
        mv "${TINYCC_BUILD_HIDDEN}" "${TINYCC_BUILD_DIR}"
        echo "Restored ${TINYCC_BUILD_DIR}"
    fi
}
trap restore_tinycc_build EXIT

if [ "${SKIP_RESTORE}" = "0" ]; then
    mv "${TINYCC_BUILD_DIR}" "${TINYCC_BUILD_HIDDEN}"
    echo "Hid ${TINYCC_BUILD_DIR} -> tinycc_build_hidden"
fi

# ---- run the standard test suite --------------------------------------------

echo ""
echo "=== Running embedded-runtime test (build: ${BUILD}) ==="
echo "    Extension : ${EXT}"
echo "    Build dir : HIDDEN (embedded extraction will be used)"
echo ""

"${VENV_BIN}" -m duckdb_sqllogictest \
    --test-dir "${PROJ}/test/sql" \
    --external-extension "${EXT}"

echo ""
echo "=== Embedded runtime test PASSED ==="
