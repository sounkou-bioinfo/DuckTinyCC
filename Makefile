.PHONY: clean clean_all rdm site site-clean \
	test_embedded_debug test_embedded_release \
	community_sim_build community_sim_run community_sim \
	fuzz fuzz-asan fuzz-ubsan fuzz-sql fuzz-all fuzz-clean \
	test-sanitized-extension test-sanitizers

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Main extension configuration
EXTENSION_NAME=ducktinycc

# Set to 1 to enable Unstable API (binaries will only work on TARGET_DUCKDB_VERSION, forwards compatibility will be broken)
# WARNING: When set to 1, the duckdb_extension.h from the TARGET_DUCKDB_VERSION must be used, using any other version of
#          the header is unsafe.
USE_UNSTABLE_C_API=0

# The DuckDB version to target
TARGET_DUCKDB_VERSION=v1.2.0
# The DuckDB release to fetch headers from
DUCKDB_HEADER_VERSION=v1.4.3

all: configure release

# Include makefiles from DuckDB
include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
include extension-ci-tools/makefiles/c_api_extensions/c_cpp.Makefile

configure: venv platform extension_version

debug: build_extension_library_debug build_extension_with_metadata_debug
release: build_extension_library_release build_extension_with_metadata_release

test: test_debug
test_debug: test_extension_debug
test_release: test_extension_release

test_embedded_debug: debug
	bash $(PROJ_DIR)scripts/test_embedded_runtime.sh debug

test_embedded_release: release
	bash $(PROJ_DIR)scripts/test_embedded_runtime.sh release

debug_recover:
	@if [ ! -f "$(PROJ_DIR)cmake_build/debug/tinycc_build/libtcc.a" ]; then \
		echo "Recovering debug build tree (tinycc_build missing)"; \
		rm -rf "$(PROJ_DIR)cmake_build/debug"; \
	fi
	$(MAKE) debug

debug_pruned:
	$(MAKE) EXTRA_CMAKE_FLAGS="$(EXTRA_CMAKE_FLAGS) -DDUCKTINYCC_PRUNE_TINYCC_BUILD_DIR=1" debug

release_pruned:
	$(MAKE) EXTRA_CMAKE_FLAGS="$(EXTRA_CMAKE_FLAGS) -DDUCKTINYCC_PRUNE_TINYCC_BUILD_DIR=1" release

community_sim_build:
	bash $(PROJ_DIR)scripts/community_sim_build.sh

community_sim_run:
	bash $(PROJ_DIR)scripts/community_sim_run.sh

community_sim: community_sim_build
	bash $(PROJ_DIR)scripts/community_sim_run.sh

FUZZ_CC ?= clang
FUZZ_RUNS ?= 100000
FUZZ_MAX_LEN ?= 65536
FUZZ_SEED ?= 3735928559
FUZZ_BUILD_DIR ?= .fuzz
FUZZ_CORPUS := test/fuzz/corpus
FUZZ_DRIVER := test/fuzz/ducktinycc_fuzz.c
FUZZ_SOURCES := src/tcc_module.c $(wildcard src/tcc_module_*.c)
FUZZ_HEADERS := $(wildcard src/include/*.h) $(wildcard duckdb_capi/*.h)
FUZZ_COMMON_CFLAGS := \
	-std=c11 -g -O1 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
	-DDUCKDB_EXTENSION_API_VERSION_MAJOR=1 \
	-DDUCKDB_EXTENSION_API_VERSION_MINOR=4 \
	-DDUCKDB_EXTENSION_API_VERSION_PATCH=3 \
	-DDUCKDB_EXTENSION_API_VERSION_UNSTABLE=v1.4.3 \
	-fvisibility=hidden -ffunction-sections -fdata-sections \
	-Isrc/include -Iduckdb_capi -Ithird_party/tinycc
FUZZ_LIBSTDCXX_DIR := $(shell dirname "$$(cc -print-file-name=libstdc++.so)")
FUZZ_COMMON_LDFLAGS := -Wl,--gc-sections -L$(FUZZ_LIBSTDCXX_DIR) -lstdc++
FUZZ_ASAN_BIN := $(FUZZ_BUILD_DIR)/ducktinycc_fuzz_asan
FUZZ_UBSAN_BIN := $(FUZZ_BUILD_DIR)/ducktinycc_fuzz_ubsan

$(FUZZ_ASAN_BIN): $(FUZZ_DRIVER) $(FUZZ_SOURCES) $(FUZZ_HEADERS) Makefile
	mkdir -p $(FUZZ_BUILD_DIR)
	$(FUZZ_CC) $(FUZZ_COMMON_CFLAGS) -fsanitize=fuzzer,address \
		-fno-omit-frame-pointer $(FUZZ_DRIVER) $(FUZZ_COMMON_LDFLAGS) \
		-fsanitize=fuzzer,address -o $@

$(FUZZ_UBSAN_BIN): $(FUZZ_DRIVER) $(FUZZ_SOURCES) $(FUZZ_HEADERS) Makefile
	mkdir -p $(FUZZ_BUILD_DIR)
	$(FUZZ_CC) $(FUZZ_COMMON_CFLAGS) -fsanitize=fuzzer,undefined \
		-fno-omit-frame-pointer $(FUZZ_DRIVER) $(FUZZ_COMMON_LDFLAGS) \
		-fsanitize=fuzzer,undefined -o $@

fuzz-asan: $(FUZZ_ASAN_BIN)
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	cp $(FUZZ_CORPUS)/* "$$tmp"/; mkdir "$$tmp/artifacts"; \
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	$(FUZZ_ASAN_BIN) "$$tmp" -dict=test/fuzz/ducktinycc.dict \
		-artifact_prefix="$$tmp/artifacts/" -max_len=$(FUZZ_MAX_LEN) \
		-timeout=5 -rss_limit_mb=2048 -print_funcs=0 -seed=$(FUZZ_SEED) -runs=$(FUZZ_RUNS)

fuzz-ubsan: $(FUZZ_UBSAN_BIN)
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	cp $(FUZZ_CORPUS)/* "$$tmp"/; mkdir "$$tmp/artifacts"; \
	UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
	$(FUZZ_UBSAN_BIN) "$$tmp" -dict=test/fuzz/ducktinycc.dict \
		-artifact_prefix="$$tmp/artifacts/" -max_len=$(FUZZ_MAX_LEN) \
		-timeout=5 -rss_limit_mb=2048 -print_funcs=0 -seed=$(FUZZ_SEED) -runs=$(FUZZ_RUNS)

fuzz: fuzz-asan fuzz-ubsan

fuzz-sql: release
	bash scripts/test_sql_fuzz.sh build/release/ducktinycc.duckdb_extension

test-sanitized-extension: configure
	bash scripts/test_sanitized_extension.sh $${SANITIZER:-asan}

test-sanitizers:
	$(MAKE) test-sanitized-extension SANITIZER=asan
	$(MAKE) test-sanitized-extension SANITIZER=ubsan

fuzz-all: fuzz fuzz-sql test-sanitizers

fuzz-clean:
	rm -rf $(FUZZ_BUILD_DIR) cmake_build/sanitizer-* build/sanitizer-*

# Override header fetch to use the actual DuckDB release version, not the C API version
update_duckdb_headers_custom:
	$(PYTHON_VENV_BIN) -c "import urllib.request;urllib.request.urlretrieve('https://raw.githubusercontent.com/duckdb/duckdb/$(DUCKDB_HEADER_VERSION)/src/include/duckdb.h', 'duckdb_capi/duckdb.h')"
	$(PYTHON_VENV_BIN) -c "import urllib.request;urllib.request.urlretrieve('https://raw.githubusercontent.com/duckdb/duckdb/$(DUCKDB_HEADER_VERSION)/src/include/duckdb_extension.h', 'duckdb_capi/duckdb_extension.h')"

clean: clean_build clean_cmake
clean_all: clean clean_configure

rdm: release
	Rscript -e "rmarkdown::render('README.Rmd', quiet = TRUE)"

site:
	Rscript scripts/build_docs_site.R

site-clean:
	rm -rf _site
