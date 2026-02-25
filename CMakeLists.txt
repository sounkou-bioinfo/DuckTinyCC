cmake_minimum_required(VERSION 3.5...3.29)
option(DUCKDB_WASM_EXTENSION "Whether compiling for Wasm target" OFF)

###
# Configuration
###
if(NOT DEFINED EXTENSION_NAME)
    message(FATAL_ERROR "DuckDB extension name is required")
endif()
add_definitions(-DDUCKDB_EXTENSION_NAME=${EXTENSION_NAME})

if (DEFINED DUCKDB_EXTENSION_API_VERSION_UNSTABLE)
    add_definitions(-DDUCKDB_EXTENSION_API_VERSION_UNSTABLE=${DUCKDB_EXTENSION_API_VERSION_UNSTABLE})
endif()

###
# Build
###
project(${EXTENSION_NAME} LANGUAGES C)

# Create Extension library
set(EXTENSION_SOURCES
        src/ducktinycc.c
        src/tcc_module.c
)

if (DUCKDB_WASM_EXTENSION)
	add_library(${EXTENSION_NAME} STATIC ${EXTENSION_SOURCES})
else()
	add_library(${EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})
endif()

# Hide symbols
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -s")

# Include own headers
target_include_directories(${EXTENSION_NAME} PRIVATE src/include)

# Include DuckDB C API headers
target_include_directories(${EXTENSION_NAME} PRIVATE duckdb_capi)
target_include_directories(${EXTENSION_NAME} PRIVATE third_party/tinycc)

if (NOT DUCKDB_WASM_EXTENSION)
    include(ExternalProject)
    find_program(MAKE_COMMAND NAMES gmake make REQUIRED)

    set(TINYCC_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/tinycc")
    set(TINYCC_BUILD_DIR "${CMAKE_BINARY_DIR}/tinycc_build")

    ExternalProject_Add(tinycc_build
        SOURCE_DIR        "${TINYCC_SRC_DIR}"
        BINARY_DIR        "${TINYCC_BUILD_DIR}"
        BUILD_IN_SOURCE   FALSE
        CONFIGURE_COMMAND "${TINYCC_SRC_DIR}/configure" --source-path=${TINYCC_SRC_DIR} --extra-cflags=-fPIC
        BUILD_COMMAND     ${MAKE_COMMAND} -j libtcc.a libtcc1.a
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  "${TINYCC_BUILD_DIR}/libtcc.a" "${TINYCC_BUILD_DIR}/libtcc1.a"
        LOG_CONFIGURE     TRUE
        LOG_BUILD         TRUE
    )

    add_library(tccstatic STATIC IMPORTED GLOBAL)
    set_target_properties(tccstatic PROPERTIES
        IMPORTED_LOCATION "${TINYCC_BUILD_DIR}/libtcc.a"
        INTERFACE_INCLUDE_DIRECTORIES "${TINYCC_SRC_DIR}"
    )
    add_dependencies(tccstatic tinycc_build)

    target_link_libraries(${EXTENSION_NAME} PRIVATE tccstatic)
    target_compile_definitions(${EXTENSION_NAME} PRIVATE DUCKTINYCC_DEFAULT_RUNTIME_PATH="${TINYCC_BUILD_DIR}")
    if (NOT APPLE AND NOT WIN32)
        target_link_libraries(${EXTENSION_NAME} PRIVATE dl m pthread)
    endif()
else()
    target_compile_definitions(${EXTENSION_NAME} PRIVATE DUCKTINYCC_WASM_UNSUPPORTED=1)
endif()
