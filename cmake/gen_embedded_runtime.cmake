# cmake/gen_embedded_runtime.cmake
#
# Generates src/embedded_runtime.c containing:
#   - ducktinycc_embedded_libtcc1[]   : libtcc1.a binary blob (platform-specific, built at compile time)
#   - ducktinycc_embedded_headers[]   : TinyCC runtime headers as {relpath, content} pairs
#
# Each manifest entry's `name` field is a forward-slash relative path from the
# extraction-root directory (e.g. "include/stdarg.h", "include/winapi/windows.h",
# "lib/kernel32.def").  The extraction code in tcc_ensure_embedded_runtime()
# creates any intermediate directories needed before writing each file.
#
# The extension extracts these to a temp directory at first use so TinyCC can find its runtime
# entirely from the extension binary itself (no external runtime files required at deployment).
#
# Invocation (via CMake custom command):
#   cmake -D LIBTCC1_PATH=<path/to/libtcc1.a>
#         [-D HEADERS_DIR=<path/to/tinycc/include>]          # non-Windows: flat *.h -> include/
#         [-D WIN32_INCLUDE_DIR=<path/to/tinycc/win32/include>]  # Windows: recursive *.h -> include/
#         [-D WIN32_LIB_DIR=<path/to/tinycc/win32/lib>]      # Windows: *.def -> lib/
#         -D OUTPUT_FILE=<path/to/embedded_runtime.c>
#         -P cmake/gen_embedded_runtime.cmake
#
# At least one of HEADERS_DIR or WIN32_INCLUDE_DIR must be provided.

cmake_minimum_required(VERSION 3.5)

foreach(required_var LIBTCC1_PATH OUTPUT_FILE)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "gen_embedded_runtime.cmake: ${required_var} is required")
    endif()
endforeach()

if(NOT DEFINED HEADERS_DIR AND NOT DEFINED WIN32_INCLUDE_DIR)
    message(FATAL_ERROR "gen_embedded_runtime.cmake: at least one of HEADERS_DIR or WIN32_INCLUDE_DIR is required")
endif()

# ---- helpers ----------------------------------------------------------------

# Write a binary file as a named C unsigned char array + size constant.
# Appends the declaration lines to the file at OUTPUT_FILE.
function(append_binary_blob_as_c_array INPUT_PATH SYMBOL_NAME)
    if(NOT EXISTS "${INPUT_PATH}")
        message(FATAL_ERROR "gen_embedded_runtime.cmake: file not found: ${INPUT_PATH}")
    endif()
    file(READ "${INPUT_PATH}" hex_data HEX)
    string(LENGTH "${hex_data}" hex_len)
    math(EXPR byte_count "${hex_len} / 2")

    # Convert "aabbcc..." -> "0xaa,0xbb,0xcc,..."
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex_csv "${hex_data}")

    # Chunk into lines of 16 bytes each (32 hex chars + commas) for readability
    # CMake regex can't do fixed-width chunking cleanly, so we emit a single long
    # initializer wrapped at the outer braces. Compilers handle this fine.
    file(APPEND "${OUTPUT_FILE}"
        "/* ${INPUT_PATH} */\n"
        "const unsigned char ${SYMBOL_NAME}[] = {\n"
        "${hex_csv}\n"
        "};\n"
        "const size_t ${SYMBOL_NAME}_size = ${byte_count};\n\n"
    )
endfunction()

# Write a binary/text file as a named static C byte array.
# relpath is a forward-slash path relative to the extraction root
# (e.g. "include/stdarg.h", "lib/kernel32.def").
# Emits:  static const unsigned char <sym>_bytes[] = {...};
# The runtime writes these bytes directly to disk after creating any needed
# intermediate directories.
function(append_asset_as_c_array INPUT_PATH SYMBOL_NAME)
    if(NOT EXISTS "${INPUT_PATH}")
        message(FATAL_ERROR "gen_embedded_runtime.cmake: file not found: ${INPUT_PATH}")
    endif()
    file(READ "${INPUT_PATH}" hex_data HEX)
    string(LENGTH "${hex_data}" hex_len)
    math(EXPR byte_count "${hex_len} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex_csv "${hex_data}")
    file(APPEND "${OUTPUT_FILE}"
        "static const unsigned char ${SYMBOL_NAME}_bytes[] = {\n"
        "${hex_csv}\n"
        "};\n"
        "static const size_t ${SYMBOL_NAME}_size = ${byte_count};\n\n"
    )
endfunction()

# Collect assets from a directory tree and append to asset_syms/asset_relpaths lists.
# BASE_DIR: root to compute relpaths from
# DEST_PREFIX: prepended to each relpath in the manifest (e.g. "include", "lib")
# GLOB_PATTERN: e.g. "*.h" or "*.def"
# RECURSIVE: TRUE or FALSE
function(collect_assets BASE_DIR DEST_PREFIX GLOB_PATTERN RECURSIVE OUT_SYMS OUT_RELPATHS)
    if(NOT EXISTS "${BASE_DIR}")
        message(WARNING "gen_embedded_runtime.cmake: directory not found, skipping: ${BASE_DIR}")
        set(${OUT_SYMS} "" PARENT_SCOPE)
        set(${OUT_RELPATHS} "" PARENT_SCOPE)
        return()
    endif()

    if(RECURSIVE)
        file(GLOB_RECURSE asset_files "${BASE_DIR}/${GLOB_PATTERN}")
    else()
        file(GLOB asset_files "${BASE_DIR}/${GLOB_PATTERN}")
    endif()
    list(SORT asset_files)

    set(syms "")
    set(relpaths "")
    foreach(asset_path ${asset_files})
        # Compute path relative to BASE_DIR
        file(RELATIVE_PATH rel_to_base "${BASE_DIR}" "${asset_path}")
        # The manifest relpath: DEST_PREFIX/rel_to_base (forward slashes)
        set(manifest_relpath "${DEST_PREFIX}/${rel_to_base}")

        # Turn the relpath into a valid C identifier for the symbol name:
        # "include/winapi/windows.h" -> "ducktinycc_asset_include_winapi_windows_h"
        string(REPLACE "/" "_" sym_suffix "${manifest_relpath}")
        string(REPLACE "." "_" sym_suffix "${sym_suffix}")
        set(sym "ducktinycc_asset_${sym_suffix}")

        list(APPEND syms "${sym}|${asset_path}")  # pack path for later use
        list(APPEND relpaths "${manifest_relpath}")
        append_asset_as_c_array("${asset_path}" "${sym}")
    endforeach()

    set(${OUT_SYMS} "${syms}" PARENT_SCOPE)
    set(${OUT_RELPATHS} "${relpaths}" PARENT_SCOPE)
endfunction()

# ---- write output file -------------------------------------------------------

# Overwrite (not append) at the start
file(WRITE "${OUTPUT_FILE}"
    "/* Auto-generated by cmake/gen_embedded_runtime.cmake - do not edit.\n"
    " * Contains libtcc1.a and TinyCC runtime assets embedded as byte arrays.\n"
    " * Each manifest entry name is a forward-slash relpath from the extraction root\n"
    " * (e.g. \"include/stdarg.h\", \"include/winapi/windows.h\", \"lib/kernel32.def\").\n"
    " * Extracted to a temp directory at runtime by tcc_ensure_embedded_runtime().\n"
    " */\n"
    "#include <stddef.h>\n\n"
)

# 1. libtcc1.a binary blob
append_binary_blob_as_c_array("${LIBTCC1_PATH}" "ducktinycc_embedded_libtcc1")

# 2. Collect all assets
set(all_syms "")
set(all_relpaths "")

# Non-Windows: flat TinyCC internal headers -> include/
if(DEFINED HEADERS_DIR)
    collect_assets("${HEADERS_DIR}" "include" "*.h" FALSE unix_syms unix_relpaths)
    list(APPEND all_syms ${unix_syms})
    list(APPEND all_relpaths ${unix_relpaths})
endif()

# Windows: recursive CRT + winapi headers -> include/
if(DEFINED WIN32_INCLUDE_DIR)
    collect_assets("${WIN32_INCLUDE_DIR}" "include" "*.h" TRUE win_inc_syms win_inc_relpaths)
    list(APPEND all_syms ${win_inc_syms})
    list(APPEND all_relpaths ${win_inc_relpaths})
endif()

# Windows: import library .def files -> lib/
if(DEFINED WIN32_LIB_DIR)
    collect_assets("${WIN32_LIB_DIR}" "lib" "*.def" FALSE win_lib_syms win_lib_relpaths)
    list(APPEND all_syms ${win_lib_syms})
    list(APPEND all_relpaths ${win_lib_relpaths})
endif()

# 3. Manifest table: {name (relpath), data, size} entries terminated by a NULL entry
file(APPEND "${OUTPUT_FILE}"
    "typedef struct {\n"
    "    const char *name;\n"
    "    const unsigned char *data;\n"
    "    size_t size;\n"
    "} ducktinycc_embedded_header_entry_t;\n\n"
    "const ducktinycc_embedded_header_entry_t ducktinycc_embedded_headers[] = {\n"
)

list(LENGTH all_syms n_assets)
set(idx 0)
foreach(sym_and_path ${all_syms})
    # sym_and_path is "symname|/abs/path" — extract just the sym name
    string(REPLACE "|" ";" sym_parts "${sym_and_path}")
    list(GET sym_parts 0 sym)
    list(GET all_relpaths ${idx} relpath)
    file(APPEND "${OUTPUT_FILE}"
        "    { \"${relpath}\", ${sym}_bytes, (size_t)sizeof(${sym}_bytes) },\n"
    )
    math(EXPR idx "${idx} + 1")
endforeach()

file(APPEND "${OUTPUT_FILE}"
    "    { (const char *)0, (const unsigned char *)0, 0 }\n"
    "};\n"
    "const size_t ducktinycc_embedded_headers_count = ${n_assets};\n"
)

message(STATUS "gen_embedded_runtime: wrote ${OUTPUT_FILE} (${n_assets} assets + libtcc1.a)")
