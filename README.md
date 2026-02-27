
<!-- README.md is generated from README.Rmd. Please edit that file -->

# DuckTinyCC

`DuckTinyCC` is a `DuckDB` `C` extension for in-process JIT compiled C
UDFs allowing `C` scripting via `TinyCC`. Combined with `C` parsing
facilities offered by for example the
[`sitting duck extension`](https://github.com/teaguesterling/sitting_duck),
we can quickly generate declarative bindings.

## API Surface

All functionality is exposed through `tcc_module(...)` and selected with
`mode := ...`.  
Every mode returns one diagnostics row with `ok`, `code`, `message`, and
context columns. Functions are prefixed by `tcc_`

### Session and Configuration

| Mode            | Purpose                              | Notes                              |
|-----------------|--------------------------------------|------------------------------------|
| `config_get`    | Show current session configuration   | Includes runtime path and counters |
| `config_set`    | Set session runtime path             | Connection-scoped                  |
| `config_reset`  | Reset runtime path and build state   | Keeps extension loaded             |
| `tcc_new_state` | Start a fresh build state            | Drops staged build inputs          |
| `list`          | Show session registry/build counters | Diagnostics only                   |

### Build Inputs

| Mode                              | Purpose                                      |
|-----------------------------------|----------------------------------------------|
| `add_include`, `add_sysinclude`   | Add include paths                            |
| `add_library_path`, `add_library` | Add library paths/libraries                  |
| `add_option`, `add_define`        | Add compiler flags and defines               |
| `add_header`, `add_source`        | Stage C code units in session                |
| `tinycc_bind`                     | Stage `symbol` + `sql_name` for next compile |

### Compile and Register

| Mode              | Purpose                                                       | Notes                                                                                                                                                                                                                              |
|-------------------|---------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `compile`         | Compile staged session code and register SQL UDF              | Uses `tinycc_bind` or explicit `symbol`/`sql_name`                                                                                                                                                                                 |
| `quick_compile`   | One-shot fast lane compile + register                         | Requires `source`, `symbol`, `sql_name`, `return_type`, `arg_types`; optional `wrapper_mode := 'row'|'batch'`; can also pass per-call `include_path`, `sysinclude_path`, `library_path`, `library`, `option`, `define_*`, `header` |
| `codegen_preview` | Generate wrapper C source only (no compile/relocate/register) | Uses same signature inputs as compile path (including `wrapper_mode`); returns generated C in `detail`                                                                                                                             |
| `c_struct`        | Generate and register C struct helper UDFs                    | Uses `symbol := <struct_name>`, `arg_types := ['field:type', ...]`; emits `struct_<name>_*` helpers                                                                                                                                |
| `c_union`         | Generate and register C union helper UDFs                     | Uses `symbol := <union_name>`, `arg_types := ['field:type', ...]`; emits `union_<name>_*` helpers                                                                                                                                  |
| `c_bitfield`      | Generate and register C bitfield struct helper UDFs           | Same as `c_struct`, but marks fields as bitfields for helper generation                                                                                                                                                            |
| `c_enum`          | Generate and register C enum constant helper UDFs             | Uses `symbol := <enum_name>`, `arg_types := ['CONST_A', ...]`; emits `enum_<name>_*` helpers                                                                                                                                       |

### Discovery Helpers

| Function                                                                    | Purpose                                                        | Notes                                                                              |
|-----------------------------------------------------------------------------|----------------------------------------------------------------|------------------------------------------------------------------------------------|
| `tcc_system_paths(runtime_path := '', library_path := '')`                  | Show include and library search paths used by TinyCC workflows | Includes existence flags; includes Windows/MSYS2/Rtools defaults on Windows builds |
| `tcc_library_probe(library := ..., runtime_path := '', library_path := '')` | Resolve a library short name or full filename/path             | Supports `.a`, `.so*`, `.dylib`, `.dll`, `.lib` style names                        |

### Pointer and Buffer Helpers (Direct SQL)

| Function                                                                 | Purpose                                           | Notes                                                                |
|--------------------------------------------------------------------------|---------------------------------------------------|----------------------------------------------------------------------|
| `tcc_alloc(size_ubigint)`                                                | Allocate process-local C memory and return handle | Returns `UBIGINT` handle; zero-initialized; NULL on invalid size/OOM |
| `tcc_free_ptr(handle_ubigint)`                                           | Free an allocated handle                          | Returns `BOOLEAN`; true on success                                   |
| `tcc_ptr_size(handle_ubigint)`                                           | Return allocation size in bytes                   | Returns `UBIGINT`; NULL for invalid handle                           |
| `tcc_dataptr(handle_ubigint)`                                            | Return raw process address for handle             | Returns `UBIGINT`; interop escape hatch                              |
| `tcc_ptr_add(addr_ubigint, offset_ubigint)`                              | Raw address arithmetic helper                     | Returns `UBIGINT` address                                            |
| `tcc_read_i8/u8/i16/u16/i32/u32/i64/u64/f32/f64(handle, offset)`         | Typed reads from handle memory                    | Returns typed value or NULL when out-of-bounds/invalid               |
| `tcc_write_i8/u8/i16/u16/i32/u32/i64/u64/f32/f64(handle, offset, value)` | Typed writes into handle memory                   | Returns `BOOLEAN`; false on out-of-bounds/invalid                    |
| `tcc_read_bytes(handle, offset, width)`                                  | Read byte span as `BLOB`                          | NULL on out-of-bounds/invalid                                        |
| `tcc_write_bytes(handle, offset, blob)`                                  | Write `BLOB` bytes into handle memory             | Returns `BOOLEAN`; false on out-of-bounds/invalid                    |

## Supported Types

Type metadata is explicit for `compile`, `quick_compile`, and
`codegen_preview`:

- `return_type := ...` is required
- `arg_types := [...]` is required (use `[]` for zero arguments)

Current executable SQL registration support:

| Signature Piece          | Accepted Tokens                                                                                                                                                                                                                                                                | DuckDB Type                                                                   |
|--------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------|
| `return_type`            | `void`, `bool`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `ptr`, `pointer`, `f32`, `f64`, `varchar`, `text`, `string`, `cstring`, `blob`, `bytea`, `binary`, `varbinary`, `buffer`, `bytes`, `uuid`, `date`, `time`, `timestamp`, `interval`, `decimal`, `numeric` | `BIGINT` for `void`; otherwise matching scalar type (`ptr` maps to `UBIGINT`) |
| each `arg_types` element | same scalar tokens as `return_type`, plus fixed-child list tokens `list_i64` or `i64[]`, fixed-size array tokens like `i64[3]`, `struct<name:type;...>`, and `map<key_type;value_type>`                                                                                        | scalar type, `LIST(child)`, `ARRAY(child,N)`, `STRUCT(...)`, or `MAP(...)`    |

Additional limits and behavior:

- Arity: `0..N` arguments (no fixed hard-coded cap)
- Wrapper mode: `row` (default) or `batch`
- NULL handling: NULL-in/NULL-out
- `void` return registers as a SQL function that emits `NULL` values
- `ptr`/`pointer` use explicit pointer ABI in generated wrappers and map
  to SQL `UBIGINT`
- `varchar`/`cstring` bridge to C `const char *`; returned `NULL`
  pointers produce SQL `NULL`
- `blob`/`buffer` bridge to C
  `ducktinycc_blob_t { const void *ptr; uint64_t len }`; returned
  `ptr == NULL` produces SQL `NULL`
- easy scalar structs use C aliases: `ducktinycc_hugeint_t` (UUID),
  `ducktinycc_date_t`, `ducktinycc_time_t`, `ducktinycc_timestamp_t`,
  `ducktinycc_interval_t`, `ducktinycc_decimal_t`
- list arguments bridge to C
  `ducktinycc_list_t { const void *ptr; const uint64_t *validity; uint64_t offset; uint64_t len }`
- list returns are supported via `ducktinycc_list_t` (`ptr == NULL`
  yields SQL `NULL`)
- fixed-size array arguments/returns use
  `ducktinycc_array_t { const void *ptr; const uint64_t *validity; uint64_t offset; uint64_t len }`
- struct arguments/returns use
  `ducktinycc_struct_t { const void *const *field_ptrs; const uint64_t *const *field_validity; uint64_t field_count; uint64_t offset }`
- map arguments/returns use
  `ducktinycc_map_t { key_ptr, key_validity, value_ptr, value_validity, offset, len }`
- `i64[]` means variable-length `LIST`; `i64[3]` means fixed-size
  `ARRAY`
- `struct<...>`, `map<...>`, and `union<...>` members are recursively
  typed and can include nested composites and string/blob tokens
- `struct<...>` and `map<...>` use `;` separators in tokens to avoid
  ambiguity inside `arg_types := [...]`
- `decimal`/`numeric` currently bind as `DECIMAL(18,3)` \## Get Started

Minimal end-to-end example: compile a C function and call it from SQL.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on

-- Compile and register a zero-arg C UDF returning VARCHAR
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'const char *hello_from_c_udf(void){ return "hello from C UDF"; }',
  symbol := 'hello_from_c_udf',
  sql_name := 'hello_from_c_udf',
  return_type := 'varchar',
  arg_types := []
);

-- Call it like any SQL scalar function
SELECT hello_from_c_udf() AS msg;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.002052 sys 0.000000
    ┌──────────────────┐
    │       msg        │
    │     varchar      │
    ├──────────────────┤
    │ hello from C UDF │
    └──────────────────┘
    Run Time (s): real 0.000 user 0.000325 sys 0.000000

## How It Works

- `compile`, `quick_compile`, and `codegen_preview` generate C wrapper
  code around the target `symbol`.
- `wrapper_mode := 'row'` generates a row ABI wrapper (`void **args`,
  `out_value`, `out_is_null`).
- `wrapper_mode := 'batch'` generates a chunk ABI wrapper (`arg_data`,
  validity masks, `count`, output buffer/mask) and runs once per DuckDB
  chunk.
- A generated module init function calls
  `ducktinycc_register_signature(...)` to register the SQL scalar UDF in
  the active DuckDB connection.
- TinyCC compile + relocate is fully in-memory (no shared library file
  is emitted).
- Host symbols injected into each TinyCC state are currently fixed:
  `duckdb_ext_api` and `ducktinycc_register_signature`.
- DuckDB still owns all vectors/buffers; wrappers operate on borrowed
  chunk memory and should not retain pointers past the call.
- VARCHAR inputs are read from DuckDB `duckdb_string_t` vectors and
  passed to wrappers as C `const char *`.
- BLOB inputs are presented as `ducktinycc_blob_t` views over DuckDB
  buffers.
- LIST inputs are presented as `ducktinycc_list_t` slices over DuckDB
  child vectors (`ptr` + `len` + validity bitmask + child offset).
- ARRAY inputs are presented as `ducktinycc_array_t` fixed-size slices
  over DuckDB array child vectors.
- STRUCT inputs are presented as `ducktinycc_struct_t` views over struct
  child vectors.
- MAP inputs are presented as `ducktinycc_map_t` slices over map
  key/value child vectors.

## Memory Ownership and Lifecycle

- Input argument memory is borrowed from DuckDB vectors for the duration
  of a scalar callback only.
- Output memory is DuckDB-managed; wrappers write directly into DuckDB
  output buffers.
- VARCHAR outputs are copied into DuckDB-owned storage with
  `duckdb_vector_assign_string_element`.
- BLOB outputs are copied into DuckDB-owned storage with
  `duckdb_vector_assign_string_element_len`.
- Host-side marshalling scratch buffers (`arg_ptrs`, decoded column
  arrays, list descriptors) are allocated with `duckdb_malloc` and freed
  before callback return.
- Compiled code and symbols live inside each TinyCC relocated state
  (`TCCState`) and are released when that artifact is destroyed.
- `config_reset` and `tcc_new_state` clear staged build/session inputs,
  but do not remove already registered SQL UDF catalog entries.
- Current DuckDB C API behavior treats these API-registered scalar
  functions as internal catalog entries, so SQL `DROP FUNCTION` is not
  available for them.

## API WIP (Design Direction)

- Host symbol registration mode: feasible and aligned with Rtinycc-style
  embedding; prefer an allow-listed registry (name -\> host address)
  over raw SQL-provided addresses.
- Arrow to reduce marshalling: not used directly; batch wrappers already
  consume DuckDB vectors column-wise and avoid per-row host-wrapper
  crossings.
- DuckDB nested types: `LIST`, `ARRAY`, `STRUCT`, `MAP`, and `UNION` are
  first-class SQL argument/return tokens with recursive C descriptors
  and recursive bridge marshalling.
- Union bridge note: DuckDB C API exposes union logical/value APIs, but
  no dedicated `union_vector_*` helpers; bridge code uses tag-buffer +
  member child vectors.
- Host helper ABI: wrappers now expose typed/bounds-checked buffer
  helpers (`ducktinycc_read_*`, `ducktinycc_write_*`,
  `ducktinycc_read_bytes`, `ducktinycc_write_bytes`,
  `ducktinycc_span_fits`) in addition to list/array/struct/map
  descriptor helpers.
- SQL pointer helpers: `tcc_alloc`, `tcc_free_ptr`, `tcc_dataptr`,
  `tcc_ptr_size`, `tcc_ptr_add`, typed `tcc_read_*`/`tcc_write_*`, and
  `tcc_read_bytes`/`tcc_write_bytes` are registered at extension load.
- Pointer representation: SQL uses `UBIGINT` handles backed by a
  per-database in-memory pointer registry.
- Real C structs/unions/enums/bitfields: `c_struct`, `c_union`,
  `c_enum`, and `c_bitfield` now auto-generate helper UDFs directly from
  C declarations + field specs.
- For `c_struct`/`c_union`, helpers include `*_new`, `*_free`,
  `*_get_*`, `*_set_*`, `*_off_*`, `*_addr`, `*_sizeof`, `*_alignof`.
- For `c_bitfield`, field helpers are `*_get_*`/`*_set_*`
  (compiler-resolved bitfield layout); per-field `*_off_*`/`*_addr` are
  intentionally omitted.
- Callback signatures (planned): support explicit callback tokens in
  bindings, e.g. `callback:<ret(args...)>` and
  `callback_async:<ret(args...)>`, so wrapper code can model
  function-pointer arguments intentionally.
- Callback registry (planned): map DuckDB-side callable handles to
  registry entries (signature + callable + lifecycle token); compiled C
  receives only a generated trampoline pointer plus opaque callback
  context (`ptr`), not raw SQL/runtime addresses.
- Typed trampoline codegen (planned): generate one trampoline per
  callback argument from the declared signature, then marshal C args -\>
  DuckDB call values -\> C return with explicit conversion rules.
- Rtinycc precedent: mirror the `callback_token` +
  `invoke_callback_id` + trampoline user-data pattern used in
  `.sync/Rtinycc/src/RC_libtcc.c` and `.sync/Rtinycc/R/callbacks.R`,
  adapted to DuckDB callable dispatch.
- Callback failure/default policy (planned): follow Rtinycc-style
  defaults (`0`/`false`/`NULL`/`void`) when callback dispatch fails;
  async mode enqueues work and returns the type-default immediately.
- Host symbol wiring for callbacks (planned): expose callback runtime
  entrypoints as host symbols via `tcc_add_symbol(...)` before
  relocation (same ordering discipline used by Rtinycc for callback
  invoke/schedule hooks).
- Callback feature status: planned only (not yet part of the executable
  SQL API surface).
- Safety posture: prefer handle-based helpers (`tcc_alloc` +
  `tcc_read/write_*`) over raw address arithmetic; `tcc_dataptr` is
  exposed for interop escape hatches.

## Builds

``` sh
make configure
make debug  # debug
make release # release
```

## Test

``` sh
make test_debug
make test_release
```

## Examples

Run these snippets in one DuckDB shell session (`duckdb -unsigned`) with
timing enabled.

### 1) Setup

``` sh
duckdb -unsigned
.timer on
LOAD 'build/release/ducktinycc.duckdb_extension';
```

### 2) Session Configuration

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'config_set',
  runtime_path := ''
);

SELECT ok, mode, code, detail
FROM tcc_module(mode := 'config_get');

SELECT ok, mode, code, detail
FROM tcc_module(mode := 'list');
SQL
```

    ┌─────────┬────────────┬─────────┬─────────┐
    │   ok    │    mode    │  code   │ detail  │
    │ boolean │  varchar   │ varchar │ varchar │
    ├─────────┼────────────┼─────────┼─────────┤
    │ true    │ config_set │ OK      │ (empty) │
    └─────────┴────────────┴─────────┴─────────┘
    Run Time (s): real 0.000 user 0.000296 sys 0.000089
    ┌─────────┬────────────┬─────────┬───────────────────────────────────────────────────────────────────────────────────────┐
    │   ok    │    mode    │  code   │                                        detail                                         │
    │ boolean │  varchar   │ varchar │                                        varchar                                        │
    ├─────────┼────────────┼─────────┼───────────────────────────────────────────────────────────────────────────────────────┤
    │ true    │ config_get │ OK      │ runtime=/root/DuckTinyCC/cmake_build/release/tinycc_build state_id=0 config_version=1 │
    └─────────┴────────────┴─────────┴───────────────────────────────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.001 user 0.000414 sys 0.000124
    ┌─────────┬─────────┬─────────┬───────────────────────────────────────────────────────────────┐
    │   ok    │  mode   │  code   │                            detail                             │
    │ boolean │ varchar │ varchar │                            varchar                            │
    ├─────────┼─────────┼─────────┼───────────────────────────────────────────────────────────────┤
    │ true    │ list    │ OK      │ registered=0 sources=0 headers=0 includes=0 libs=0 state_id=0 │
    └─────────┴─────────┴─────────┴───────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000225 sys 0.000000

### 3) System Paths and Library Probe Helpers

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT kind, key, exists, value
FROM tcc_system_paths()
LIMIT 12;

SELECT kind, key, exists, value, detail
FROM tcc_library_probe(library := 'libtcc1.a');
SQL
```

    ┌──────────────┬──────────────┬─────────┬───────────────────────────────────────────────────────────────────┐
    │     kind     │     key      │ exists  │                               value                               │
    │   varchar    │   varchar    │ boolean │                              varchar                              │
    ├──────────────┼──────────────┼─────────┼───────────────────────────────────────────────────────────────────┤
    │ runtime      │ runtime_path │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build                 │
    │ include_path │ path         │ false   │ /root/DuckTinyCC/cmake_build/release/tinycc_build/include         │
    │ include_path │ path         │ false   │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib/tcc/include │
    │ library_path │ path         │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build                 │
    │ library_path │ path         │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib             │
    │ library_path │ path         │ false   │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib/tcc         │
    │ library_path │ path         │ true    │ /usr/lib                                                          │
    │ library_path │ path         │ true    │ /usr/lib64                                                        │
    │ library_path │ path         │ true    │ /usr/local/lib                                                    │
    │ library_path │ path         │ true    │ /lib                                                              │
    │ library_path │ path         │ true    │ /lib64                                                            │
    │ library_path │ path         │ true    │ /lib32                                                            │
    ├──────────────┴──────────────┴─────────┴───────────────────────────────────────────────────────────────────┤
    │ 12 rows                                                                                         4 columns │
    └───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000447 sys 0.000000
    ┌─────────────┬──────────────┬─────────┬─────────────────────────────────────────────────────────────┬──────────────────────────────────┐
    │    kind     │     key      │ exists  │                            value                            │              detail              │
    │   varchar   │   varchar    │ boolean │                           varchar                           │             varchar              │
    ├─────────────┼──────────────┼─────────┼─────────────────────────────────────────────────────────────┼──────────────────────────────────┤
    │ input       │ library      │ false   │ libtcc1.a                                                   │ library probe request            │
    │ runtime     │ runtime_path │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build           │ effective runtime path           │
    │ search_path │ path         │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build           │ searched path                    │
    │ search_path │ path         │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib       │ searched path                    │
    │ search_path │ path         │ false   │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib/tcc   │ searched path                    │
    │ search_path │ path         │ true    │ /usr/lib                                                    │ searched path                    │
    │ search_path │ path         │ true    │ /usr/lib64                                                  │ searched path                    │
    │ search_path │ path         │ true    │ /usr/local/lib                                              │ searched path                    │
    │ search_path │ path         │ true    │ /lib                                                        │ searched path                    │
    │ search_path │ path         │ true    │ /lib64                                                      │ searched path                    │
    │ search_path │ path         │ true    │ /lib32                                                      │ searched path                    │
    │ search_path │ path         │ false   │ /usr/local/lib64                                            │ searched path                    │
    │ search_path │ path         │ true    │ /usr/lib/x86_64-linux-gnu                                   │ searched path                    │
    │ search_path │ path         │ false   │ /usr/lib/i386-linux-gnu                                     │ searched path                    │
    │ search_path │ path         │ true    │ /lib/x86_64-linux-gnu                                       │ searched path                    │
    │ search_path │ path         │ false   │ /lib32/x86_64-linux-gnu                                     │ searched path                    │
    │ search_path │ path         │ false   │ /usr/lib/x86_64-linux-musl                                  │ searched path                    │
    │ search_path │ path         │ false   │ /usr/lib/i386-linux-musl                                    │ searched path                    │
    │ search_path │ path         │ false   │ /lib/x86_64-linux-musl                                      │ searched path                    │
    │ search_path │ path         │ false   │ /lib32/x86_64-linux-musl                                    │ searched path                    │
    │ search_path │ path         │ false   │ /usr/lib/amd64-linux-gnu                                    │ searched path                    │
    │ search_path │ path         │ false   │ /usr/lib/aarch64-linux-gnu                                  │ searched path                    │
    │ search_path │ path         │ true    │ /usr/lib/R/lib                                              │ searched path                    │
    │ search_path │ path         │ true    │ /usr/lib/jvm/default-java/lib/server                        │ searched path                    │
    │ candidate   │ libtcc1.a    │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build/libtcc1.a │ resolved                         │
    │ resolved    │ path         │ true    │ /root/DuckTinyCC/cmake_build/release/tinycc_build/libtcc1.a │ resolved library path            │
    │ resolved    │ link_name    │ true    │ tcc1                                                        │ normalized tcc_add_library value │
    ├─────────────┴──────────────┴─────────┴─────────────────────────────────────────────────────────────┴──────────────────────────────────┤
    │ 27 rows                                                                                                                     5 columns │
    └───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000721 sys 0.000014

### 4) Example A: Staged Build + `tinycc_bind` + `compile`

This example stages shared build inputs once, compiles two symbols, and
calls both SQL functions.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code, detail
FROM tcc_module(mode := 'tcc_new_state');
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_include',
  include_path := 'third_party/tinycc/include'
);
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_sysinclude',
  sysinclude_path := 'third_party/tinycc/include'
);
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_library_path',
  library_path := 'third_party/tinycc'
);
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_option',
  option := '-O2'
);
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_define',
  define_name := 'TCC_SHIFT',
  define_value := '3'
);

SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_header',
  header := '#include <stdint.h>'
);

SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'add_source',
  source := '
    long long tcc_add_shift(long long x){ return x + TCC_SHIFT; }
    long long tcc_times2(long long x){ return x * 2; }
  '
);

SELECT ok, mode, code
FROM tcc_module(mode := 'tinycc_bind', symbol := 'tcc_add_shift', sql_name := 'tcc_add_shift');
SELECT ok, mode, code
FROM tcc_module(
  mode := 'compile',
  return_type := 'i64',
  arg_types := ['i64']
);
SELECT tcc_add_shift(39) AS value;

SELECT ok, mode, code
FROM tcc_module(mode := 'tinycc_bind', symbol := 'tcc_times2', sql_name := 'tcc_times2');
SELECT ok, mode, code
FROM tcc_module(
  mode := 'compile',
  return_type := 'i64',
  arg_types := ['i64']
);
SELECT tcc_times2(21) AS value;
SQL
```

    ┌─────────┬───────────────┬─────────┬────────────┐
    │   ok    │     mode      │  code   │   detail   │
    │ boolean │    varchar    │ varchar │  varchar   │
    ├─────────┼───────────────┼─────────┼────────────┤
    │ true    │ tcc_new_state │ OK      │ state_id=1 │
    └─────────┴───────────────┴─────────┴────────────┘
    Run Time (s): real 0.000 user 0.000725 sys 0.000227
    ┌─────────┬─────────────┬─────────┬────────────────────────────┐
    │   ok    │    mode     │  code   │           detail           │
    │ boolean │   varchar   │ varchar │          varchar           │
    ├─────────┼─────────────┼─────────┼────────────────────────────┤
    │ true    │ add_include │ OK      │ third_party/tinycc/include │
    └─────────┴─────────────┴─────────┴────────────────────────────┘
    Run Time (s): real 0.001 user 0.000328 sys 0.000000
    ┌─────────┬────────────────┬─────────┬────────────────────────────┐
    │   ok    │      mode      │  code   │           detail           │
    │ boolean │    varchar     │ varchar │          varchar           │
    ├─────────┼────────────────┼─────────┼────────────────────────────┤
    │ true    │ add_sysinclude │ OK      │ third_party/tinycc/include │
    └─────────┴────────────────┴─────────┴────────────────────────────┘
    Run Time (s): real 0.000 user 0.000722 sys 0.000000
    ┌─────────┬──────────────────┬─────────┬────────────────────┐
    │   ok    │       mode       │  code   │       detail       │
    │ boolean │     varchar      │ varchar │      varchar       │
    ├─────────┼──────────────────┼─────────┼────────────────────┤
    │ true    │ add_library_path │ OK      │ third_party/tinycc │
    └─────────┴──────────────────┴─────────┴────────────────────┘
    Run Time (s): real 0.000 user 0.000273 sys 0.000000
    ┌─────────┬────────────┬─────────┬─────────┐
    │   ok    │    mode    │  code   │ detail  │
    │ boolean │  varchar   │ varchar │ varchar │
    ├─────────┼────────────┼─────────┼─────────┤
    │ true    │ add_option │ OK      │ -O2     │
    └─────────┴────────────┴─────────┴─────────┘
    Run Time (s): real 0.001 user 0.000255 sys 0.000000
    ┌─────────┬────────────┬─────────┬───────────┐
    │   ok    │    mode    │  code   │  detail   │
    │ boolean │  varchar   │ varchar │  varchar  │
    ├─────────┼────────────┼─────────┼───────────┤
    │ true    │ add_define │ OK      │ TCC_SHIFT │
    └─────────┴────────────┴─────────┴───────────┘
    Run Time (s): real 0.000 user 0.000273 sys 0.000000
    ┌─────────┬────────────┬─────────┬─────────────────┐
    │   ok    │    mode    │  code   │     detail      │
    │ boolean │  varchar   │ varchar │     varchar     │
    ├─────────┼────────────┼─────────┼─────────────────┤
    │ true    │ add_header │ OK      │ header appended │
    └─────────┴────────────┴─────────┴─────────────────┘
    Run Time (s): real 0.000 user 0.000517 sys 0.000000
    ┌─────────┬────────────┬─────────┬─────────────────┐
    │   ok    │    mode    │  code   │     detail      │
    │ boolean │  varchar   │ varchar │     varchar     │
    ├─────────┼────────────┼─────────┼─────────────────┤
    │ true    │ add_source │ OK      │ source appended │
    └─────────┴────────────┴─────────┴─────────────────┘
    Run Time (s): real 0.000 user 0.000200 sys 0.000000
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.001 user 0.000514 sys 0.000003
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.002 user 0.002494 sys 0.000000
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘
    Run Time (s): real 0.000 user 0.000165 sys 0.000000
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000322 sys 0.000000
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.003 user 0.004927 sys 0.000964
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘
    Run Time (s): real 0.000 user 0.000147 sys 0.000033

### 5) Example B: Fast Lane `quick_compile` (with include/library inputs)

This example compiles and registers directly from one SQL call,
including `include_path`, `library_path`, and `library`.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := '#include <math.h>
double qpow(double x, double y){ return pow(x, y); }',
  symbol := 'qpow',
  sql_name := 'qpow',
  return_type := 'f64',
  arg_types := ['f64', 'f64'],
  include_path := 'third_party/tinycc/include',
  library_path := 'third_party/tinycc',
  library := 'm'
);
SELECT CAST(qpow(2.0, 5.0) AS BIGINT) AS value;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.005 user 0.005171 sys 0.000000
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    32 │
    └───────┘
    Run Time (s): real 0.000 user 0.000259 sys 0.000076

### 6) Example C: Libraries (`add_library`)

This example links `libm`, compiles a function that uses `pow`, then
calls it. The required C header is included in the source unit.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(mode := 'tcc_new_state');
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_library',
  library := 'm'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_source',
  source := '#include <math.h>
double pwr(double x, double y){ return pow(x, y); }'
);
SELECT ok, mode, code
FROM tcc_module(mode := 'tinycc_bind', symbol := 'pwr', sql_name := 'pwr');
SELECT ok, mode, code
FROM tcc_module(mode := 'compile', return_type := 'f64', arg_types := ['f64', 'f64']);
SELECT CAST(pwr(2.0, 5.0) AS BIGINT) AS value;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ tcc_new_state │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000294 sys 0.000126
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ add_library │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000212 sys 0.000091
    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ add_source │ OK      │
    └─────────┴────────────┴─────────┘
    Run Time (s): real 0.001 user 0.000250 sys 0.000000
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000243 sys 0.000000
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.005 user 0.005483 sys 0.000000
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    32 │
    └───────┘
    Run Time (s): real 0.000 user 0.000227 sys 0.000000

### 7) Reset Session

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code, detail
FROM tcc_module(mode := 'list');
SELECT ok, mode, code
FROM tcc_module(mode := 'config_reset');
SQL
```

    ┌─────────┬─────────┬─────────┬───────────────────────────────────────────────────────────────┐
    │   ok    │  mode   │  code   │                            detail                             │
    │ boolean │ varchar │ varchar │                            varchar                            │
    ├─────────┼─────────┼─────────┼───────────────────────────────────────────────────────────────┤
    │ true    │ list    │ OK      │ registered=0 sources=0 headers=0 includes=0 libs=0 state_id=0 │
    └─────────┴─────────┴─────────┴───────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000342 sys 0.000124
    ┌─────────┬──────────────┬─────────┐
    │   ok    │     mode     │  code   │
    │ boolean │   varchar    │ varchar │
    ├─────────┼──────────────┼─────────┤
    │ true    │ config_reset │ OK      │
    └─────────┴──────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000442 sys 0.000160

### 8) Codegen Preview (No Compile)

This shows generated wrapper/module-init source without compiling or
registering the SQL function.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, phase, code, artifact_id, LEFT(detail, 220) AS preview
FROM tcc_module(
  mode := 'codegen_preview',
  source := 'int add_i32(int a, int b){ return a + b; }',
  symbol := 'add_i32',
  sql_name := 'add_i32',
  return_type := 'i32',
  arg_types := ['i32', 'i32'],
  wrapper_mode := 'batch'
);
SQL
```

    ┌─────────┬─────────┬─────────┬───────────────────────────┬─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
    │   ok    │  phase  │  code   │        artifact_id        │                                                                                                                 preview                                                                                                                 │
    │ boolean │ varchar │ varchar │          varchar          │                                                                                                                 varchar                                                                                                                 │
    ├─────────┼─────────┼─────────┼───────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
    │ true    │ codegen │ OK      │ __ducktinycc_ffi_init_0_0 │ #include <stdint.h>\ntypedef struct {\n  uint64_t lower;\n  int64_t upper;\n} ducktinycc_hugeint_t;\ntypedef struct {\n  const void *ptr;\n  uint64_t len;\n} ducktinycc_blob_t;\ntypedef struct {\n  int32_t days;\n} ducktinycc_date_ │
    └─────────┴─────────┴─────────┴───────────────────────────┴─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000559 sys 0.000000

### 9) Batch Wrapper Mode

Use `wrapper_mode := 'batch'` to generate a chunk-oriented wrapper and
invoke it once per DuckDB chunk.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long batch_add(long long a, long long b){ return a + b; }',
  symbol := 'batch_add',
  sql_name := 'batch_add',
  return_type := 'i64',
  arg_types := ['i64', 'i64'],
  wrapper_mode := 'batch'
);
SELECT CAST(SUM(batch_add(i, 2)) AS BIGINT) AS s
FROM range(1000000) AS t(i);
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.002084 sys 0.000000
    ┌──────────────┐
    │      s       │
    │    int64     │
    ├──────────────┤
    │ 500001500000 │
    └──────────────┘
    Run Time (s): real 0.008 user 0.016129 sys 0.000000

### 10) Arrays/Lists as C Slice Arguments

For list arguments, wrappers pass a C slice descriptor:

- `ptr`: pointer to first child element for this row
- `len`: element count
- `offset`: child-vector global offset
- `validity`: child-vector validity mask (or `NULL` when all valid)

`ducktinycc_list_is_valid(&list, i)` is available in generated code for
child NULL checks.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long sum_i64(ducktinycc_list_t a){
  const long long *p = (const long long *)a.ptr;
  unsigned long long i;
  long long s = 0;
  if (!a.ptr) return 0;
  for (i = 0; i < a.len; i++) {
    if (ducktinycc_list_is_valid(&a, i)) s += p[i];
  }
  return s;
}',
  symbol := 'sum_i64',
  sql_name := 'sum_i64',
  return_type := 'i64',
  arg_types := ['i64[]'],
  wrapper_mode := 'batch'
);
SELECT sum_i64([1,2,3]::BIGINT[]) AS sum_plain;
SELECT sum_i64([1,NULL,3]::BIGINT[]) AS sum_with_null_child;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.001253 sys 0.001206
    ┌───────────┐
    │ sum_plain │
    │   int64   │
    ├───────────┤
    │         6 │
    └───────────┘
    Run Time (s): real 0.000 user 0.000230 sys 0.000192
    ┌─────────────────────┐
    │ sum_with_null_child │
    │        int64        │
    ├─────────────────────┤
    │                   4 │
    └─────────────────────┘
    Run Time (s): real 0.001 user 0.000116 sys 0.000251

### SQL Pointer Helpers (`tcc_alloc`/`tcc_read*`/`tcc_write*`)

These helpers are directly callable from SQL and are registered at
extension load. Memory is process-local and tied to a per-database
registry; free handles with `tcc_free_ptr` when done. For real C
composites, `c_struct`/`c_union`/`c_enum`/`c_bitfield` modes
auto-generate accessor UDFs in the same session.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
-- Handle-backed pointer helpers
WITH p AS (SELECT tcc_alloc(16) AS h),
w AS (SELECT h, tcc_write_i64(h, 8, 42) AS ok FROM p),
r AS (SELECT h, ok, tcc_read_i64(h, 8) AS v, tcc_ptr_size(h) AS sz, tcc_dataptr(h) AS addr FROM w),
f AS (SELECT ok, v, sz, addr, tcc_free_ptr(h) AS freed FROM r)
SELECT ok, v, sz, addr > 0 AS addr_nonzero, freed FROM f;

WITH p AS (SELECT tcc_alloc(4) AS h),
w AS (SELECT h, tcc_write_bytes(h, 0, from_hex('DEADBEEF')) AS ok FROM p),
r AS (SELECT h, ok, hex(tcc_read_bytes(h, 0, 4)) AS hx FROM w),
f AS (SELECT ok, hx, tcc_free_ptr(h) AS freed FROM r)
SELECT ok, hx, freed FROM f;

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long read_ptr_i64(void *p){
  if (!p) return -1;
  return *(const long long *)p;
}',
  symbol := 'read_ptr_i64',
  sql_name := 'read_ptr_i64',
  return_type := 'i64',
  arg_types := ['ptr']
);
WITH p AS (SELECT tcc_alloc(8) AS h),
w AS (SELECT h, tcc_write_i64(h, 0, 42) AS ok FROM p),
r AS (SELECT h, ok, read_ptr_i64(tcc_dataptr(h)) AS v FROM w),
f AS (SELECT ok, v, tcc_free_ptr(h) AS freed FROM r)
SELECT ok, v, freed FROM f;

-- Auto-generated C struct helpers; ox/oy are byte offsets (offsetof)
SELECT ok, mode, code
FROM tcc_module(
  mode := 'c_struct',
  source := 'struct point_i32_auto { int x; int y; };',
  symbol := 'point_i32_auto',
  arg_types := ['x:i32','y:i32']
);
WITH
p AS (
  SELECT struct_point_i32_auto_new() AS p
),
w AS (
  SELECT struct_point_i32_auto_set_y(struct_point_i32_auto_set_x(p, 7), 9) AS p
  FROM p
),
r AS (
  SELECT p,
         struct_point_i32_auto_get_x(p) AS x,
         struct_point_i32_auto_get_y(p) AS y,
         struct_point_i32_auto_off_x() AS ox,
         struct_point_i32_auto_off_y() AS oy
  FROM w
),
f AS (
  SELECT x, y, ox, oy, struct_point_i32_auto_free(p) IS NULL AS freed FROM r
)
SELECT x, y, ox, oy, freed FROM f;

-- Auto-generated union helpers
SELECT ok, mode, code
FROM tcc_module(
  mode := 'c_union',
  source := 'union number_auto { int i; float f; };',
  symbol := 'number_auto',
  arg_types := ['i:i32','f:f32']
);
WITH p AS (SELECT union_number_auto_new() AS p),
w AS (SELECT union_number_auto_set_i(p, 77) AS p FROM p),
f AS (SELECT union_number_auto_get_i(p) AS v, union_number_auto_free(p) IS NULL AS freed FROM w)
SELECT v, freed FROM f;

-- Auto-generated enum constant helpers
SELECT ok, mode, code
FROM tcc_module(
  mode := 'c_enum',
  source := 'enum color_auto { RED_AUTO = 0, GREEN_AUTO = 1, BLUE_AUTO = 2 };',
  symbol := 'color_auto',
  arg_types := ['RED_AUTO','GREEN_AUTO','BLUE_AUTO']
);
SELECT enum_color_auto_RED_AUTO() AS red, enum_color_auto_BLUE_AUTO() AS blue;

-- Auto-generated bitfield helpers (portable through compiler-generated accessors)
SELECT ok, mode, code
FROM tcc_module(
  mode := 'c_bitfield',
  source := 'struct flags_auto { unsigned int active : 1; unsigned int level : 4; };',
  symbol := 'flags_auto',
  arg_types := ['active:u8','level:u8']
);
WITH p AS (SELECT struct_flags_auto_new() AS p),
w AS (SELECT struct_flags_auto_set_level(struct_flags_auto_set_active(p, 1), 9) AS p FROM p),
f AS (SELECT struct_flags_auto_get_active(p) AS active, struct_flags_auto_get_level(p) AS lvl,
             struct_flags_auto_free(p) IS NULL AS freed FROM w)
SELECT active, lvl, freed FROM f;
SQL
```

    ┌─────────┬───────┬────────┬──────────────┬─────────┐
    │   ok    │   v   │   sz   │ addr_nonzero │  freed  │
    │ boolean │ int64 │ uint64 │   boolean    │ boolean │
    ├─────────┼───────┼────────┼──────────────┼─────────┤
    │ true    │    42 │     16 │ true         │ true    │
    └─────────┴───────┴────────┴──────────────┴─────────┘
    Run Time (s): real 0.001 user 0.000719 sys 0.000000
    ┌─────────┬──────────┬─────────┐
    │   ok    │    hx    │  freed  │
    │ boolean │ varchar  │ boolean │
    ├─────────┼──────────┼─────────┤
    │ true    │ DEADBEEF │ true    │
    └─────────┴──────────┴─────────┘
    Run Time (s): real 0.000 user 0.000390 sys 0.000039
    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.001 user 0.001404 sys 0.000109
    ┌─────────┬───────┬─────────┐
    │   ok    │   v   │  freed  │
    │ boolean │ int64 │ boolean │
    ├─────────┼───────┼─────────┤
    │ true    │    42 │ true    │
    └─────────┴───────┴─────────┘
    Run Time (s): real 0.001 user 0.000430 sys 0.000000
    ┌─────────┬──────────┬─────────┐
    │   ok    │   mode   │  code   │
    │ boolean │ varchar  │ varchar │
    ├─────────┼──────────┼─────────┤
    │ true    │ c_struct │ OK      │
    └─────────┴──────────┴─────────┘
    Run Time (s): real 0.014 user 0.027002 sys 0.000868
    ┌───────┬───────┬────────┬────────┬─────────┐
    │   x   │   y   │   ox   │   oy   │  freed  │
    │ int32 │ int32 │ uint64 │ uint64 │ boolean │
    ├───────┼───────┼────────┼────────┼─────────┤
    │     7 │     9 │      0 │      4 │ true    │
    └───────┴───────┴────────┴────────┴─────────┘
    Run Time (s): real 0.000 user 0.000449 sys 0.000046
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ c_union │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.012 user 0.011973 sys 0.000000
    ┌───────┬─────────┐
    │   v   │  freed  │
    │ int32 │ boolean │
    ├───────┼─────────┤
    │    77 │ true    │
    └───────┴─────────┘
    Run Time (s): real 0.000 user 0.000310 sys 0.000015
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ c_enum  │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.004 user 0.003953 sys 0.000000
    ┌───────┬───────┐
    │  red  │ blue  │
    │ int64 │ int64 │
    ├───────┼───────┤
    │     0 │     2 │
    └───────┴───────┘
    Run Time (s): real 0.001 user 0.000227 sys 0.000010
    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ c_bitfield │ OK      │
    └─────────┴────────────┴─────────┘
    Run Time (s): real 0.007 user 0.007935 sys 0.000000
    ┌────────┬───────┬─────────┐
    │ active │  lvl  │  freed  │
    │ uint8  │ uint8 │ boolean │
    ├────────┼───────┼─────────┤
    │      1 │     9 │ true    │
    └────────┴───────┴─────────┘
    Run Time (s): real 0.001 user 0.000347 sys 0.000011

Fixed-size arrays use `type[N]` and `ducktinycc_array_t`:

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long sum_array3(ducktinycc_array_t a){
  const long long *p = (const long long *)a.ptr;
  unsigned long long i;
  long long s = 0;
  if (!a.ptr) return 0;
  for (i = 0; i < a.len; i++) {
    if (ducktinycc_array_is_valid(&a, i)) s += p[i];
  }
  return s;
}',
  symbol := 'sum_array3',
  sql_name := 'sum_array3',
  return_type := 'i64',
  arg_types := ['i64[3]']
);
SELECT sum_array3([1,2,3]::BIGINT[3]) AS sum_plain;
SELECT sum_array3([1,NULL,3]::BIGINT[3]) AS sum_with_null_child;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.001 user 0.001593 sys 0.000000
    ┌───────────┐
    │ sum_plain │
    │   int64   │
    ├───────────┤
    │         6 │
    └───────────┘
    Run Time (s): real 0.000 user 0.000241 sys 0.000000
    ┌─────────────────────┐
    │ sum_with_null_child │
    │        int64        │
    ├─────────────────────┤
    │                   4 │
    └─────────────────────┘
    Run Time (s): real 0.000 user 0.000257 sys 0.000000

Structs use `struct<name:type;...>` and `ducktinycc_struct_t`:

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long struct_sum2(ducktinycc_struct_t s){
  const long long *a;
  const long long *b;
  if (!s.field_ptrs || s.field_count < 2) return 0;
  a = (const long long *)ducktinycc_struct_field_ptr(&s, 0);
  b = (const long long *)ducktinycc_struct_field_ptr(&s, 1);
  return (a && ducktinycc_struct_field_is_valid(&s, 0) ? a[s.offset] : 0) +
         (b && ducktinycc_struct_field_is_valid(&s, 1) ? b[s.offset] : 0);
}',
  symbol := 'struct_sum2',
  sql_name := 'struct_sum2',
  return_type := 'i64',
  arg_types := ['struct<a:i64;b:i64>']
);
SELECT struct_sum2({'a': 2::BIGINT, 'b': 5::BIGINT}::STRUCT(a BIGINT, b BIGINT)) AS s;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.003 user 0.002206 sys 0.000106
    ┌───────┐
    │   s   │
    │ int64 │
    ├───────┤
    │     7 │
    └───────┘
    Run Time (s): real 0.000 user 0.000295 sys 0.000000

Maps use `map<key_type;value_type>` and `ducktinycc_map_t`:

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long map_sum(ducktinycc_map_t m){
  const long long *k = (const long long *)m.key_ptr;
  const long long *v = (const long long *)m.value_ptr;
  unsigned long long i;
  long long out = 0;
  if (!m.key_ptr || !m.value_ptr) return 0;
  for (i = 0; i < m.len; i++) {
    if (ducktinycc_map_key_is_valid(&m, i) && ducktinycc_map_value_is_valid(&m, i)) out += k[i] + v[i];
  }
  return out;
}',
  symbol := 'map_sum',
  sql_name := 'map_sum',
  return_type := 'i64',
  arg_types := ['map<i64;i64>']
);
SELECT map_sum(MAP([1::BIGINT,2::BIGINT],[10::BIGINT,20::BIGINT])::MAP(BIGINT, BIGINT)) AS s;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.001666 sys 0.000000
    ┌───────┐
    │   s   │
    │ int64 │
    ├───────┤
    │    33 │
    └───────┘
    Run Time (s): real 0.000 user 0.000266 sys 0.000000

Buffer helpers are available for manual byte-layout work (for local
scratch buffers or BLOB payloads):

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long blob_read_i32(ducktinycc_blob_t b){
  int32_t v = 0;
  if (!b.ptr) return 0;
  if (!ducktinycc_read_i32(b.ptr, b.len, 0, &v)) return -1;
  return (long long)v;
}',
  symbol := 'blob_read_i32',
  sql_name := 'blob_read_i32',
  return_type := 'i64',
  arg_types := ['blob']
);
SELECT blob_read_i32(from_hex('2A000000')) AS ok_value;
SELECT blob_read_i32(from_hex('2A0000')) AS short_blob_error;

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long local_rw_i64(void){
  uint8_t buf[16];
  int64_t out = 0;
  if (!ducktinycc_write_i64(buf, sizeof(buf), 8, 41)) return -1;
  if (!ducktinycc_read_i64(buf, sizeof(buf), 8, &out)) return -2;
  return out + 1;
}',
  symbol := 'local_rw_i64',
  sql_name := 'local_rw_i64',
  return_type := 'i64',
  arg_types := []
);
SELECT local_rw_i64() AS local_roundtrip;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.003 user 0.003037 sys 0.000084
    ┌──────────┐
    │ ok_value │
    │  int64   │
    ├──────────┤
    │       42 │
    └──────────┘
    Run Time (s): real 0.001 user 0.000272 sys 0.000125
    ┌──────────────────┐
    │ short_blob_error │
    │      int64       │
    ├──────────────────┤
    │               -1 │
    └──────────────────┘
    Run Time (s): real 0.000 user 0.000308 sys 0.000000
    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.002017 sys 0.000000
    ┌─────────────────┐
    │ local_roundtrip │
    │      int64      │
    ├─────────────────┤
    │              42 │
    └─────────────────┘
    Run Time (s): real 0.000 user 0.000305 sys 0.000000

### 11) `config_reset` Semantics

`config_reset` clears runtime/build staging state, but does not remove
already registered SQL UDF catalog entries.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';
.timer on
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'int reset_demo(int x){ return x + 1; }',
  symbol := 'reset_demo',
  sql_name := 'reset_demo',
  return_type := 'i32',
  arg_types := ['i32']
);
SELECT reset_demo(41) AS before_reset;

SELECT ok, mode, code
FROM tcc_module(mode := 'config_reset');
SELECT ok, mode, code, detail
FROM tcc_module(mode := 'list');

SELECT reset_demo(41) AS after_reset;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.001 user 0.001361 sys 0.000182
    ┌──────────────┐
    │ before_reset │
    │    int32     │
    ├──────────────┤
    │           42 │
    └──────────────┘
    Run Time (s): real 0.001 user 0.000161 sys 0.000054
    ┌─────────┬──────────────┬─────────┐
    │   ok    │     mode     │  code   │
    │ boolean │   varchar    │ varchar │
    ├─────────┼──────────────┼─────────┤
    │ true    │ config_reset │ OK      │
    └─────────┴──────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000223 sys 0.000000
    ┌─────────┬─────────┬─────────┬───────────────────────────────────────────────────────────────┐
    │   ok    │  mode   │  code   │                            detail                             │
    │ boolean │ varchar │ varchar │                            varchar                            │
    ├─────────┼─────────┼─────────┼───────────────────────────────────────────────────────────────┤
    │ true    │ list    │ OK      │ registered=1 sources=0 headers=0 includes=0 libs=0 state_id=1 │
    └─────────┴─────────┴─────────┴───────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000218 sys 0.000000
    ┌─────────────┐
    │ after_reset │
    │    int32    │
    ├─────────────┤
    │          42 │
    └─────────────┘
    Run Time (s): real 0.000 user 0.000446 sys 0.000000

### 12) CLI Benchmark Snippet

Use the DuckDB CLI timer to benchmark compile and call latency quickly:

``` bash
duckdb -unsigned <<'SQL'
.timer on
LOAD 'build/release/ducktinycc.duckdb_extension';
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'int add_i32(int a, int b){ return a + b; }',
  symbol := 'add_i32',
  sql_name := 'add_i32',
  return_type := 'i32',
  arg_types := ['i32', 'i32']
);
SELECT add_i32(20, 22);
SELECT SUM(add_i32(i::INTEGER, 42::INTEGER)) AS s FROM range(1000000) t(i);
SELECT SUM(add_i32(i::INTEGER, 42::INTEGER)) AS s FROM range(1000000) t(i);
SELECT SUM(add_i32(i::INTEGER, 42::INTEGER)) AS s FROM range(1000000) t(i);
SQL
```

    Run Time (s): real 0.001 user 0.000583 sys 0.000000
    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.001934 sys 0.000000
    ┌─────────────────┐
    │ add_i32(20, 22) │
    │      int32      │
    ├─────────────────┤
    │              42 │
    └─────────────────┘
    Run Time (s): real 0.000 user 0.000512 sys 0.000190
    ┌──────────────┐
    │      s       │
    │    int128    │
    ├──────────────┤
    │ 500041500000 │
    └──────────────┘
    Run Time (s): real 0.018 user 0.034193 sys 0.000199
    ┌──────────────┐
    │      s       │
    │    int128    │
    ├──────────────┤
    │ 500041500000 │
    └──────────────┘
    Run Time (s): real 0.016 user 0.016325 sys 0.000000
    ┌──────────────┐
    │      s       │
    │    int128    │
    ├──────────────┤
    │ 500041500000 │
    └──────────────┘
    Run Time (s): real 0.016 user 0.015795 sys 0.000015

### 13) Advanced Demo: Embed R in DuckDB (Unix-like)

This demo compiles a C UDF that boots embedded R once and evaluates a
tiny R expression.

Requirements:

- Unix-like platform with `R` installed and built with shared library
  support (`--enable-R-shlib`).
- `R CMD config --cppflags` and `R CMD config --ldflags` must resolve
  valid include/link flags on your machine.
- `R_HOME` must be set for embedded startup
  (`export R_HOME="$(R RHOME)"`).
- Demo source lives in `demo/embed_r_udf.c`.

``` bash
export R_HOME="${R_HOME:-$(R RHOME)}"
R_INCLUDE_FLAGS="$(R CMD config --cppflags | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
R_LIBDIR_FLAGS="$(R CMD config --ldflags | tr ' ' '\n' | grep '^-L' | tr '\n' ' ')"
SOURCE_SQL="$(sed "s/'/''/g" demo/embed_r_udf.c)"

duckdb -unsigned <<SQL
LOAD 'build/release/ducktinycc.duckdb_extension';
PRAGMA threads=1;

SELECT ok, mode, code FROM tcc_module(mode := 'tcc_new_state');
SELECT ok, mode, code FROM tcc_module(mode := 'add_include', include_path := 'third_party/tinycc/include');
SELECT ok, mode, code FROM tcc_module(mode := 'add_sysinclude', sysinclude_path := 'third_party/tinycc/include');
SELECT ok, mode, code FROM tcc_module(mode := 'add_define', define_name := 'R_LEGACY_RCOMPLEX', define_value := '1');
SELECT ok, mode, code FROM tcc_module(mode := 'add_option', option := '${R_INCLUDE_FLAGS} ${R_LIBDIR_FLAGS}');
SELECT ok, mode, code FROM tcc_module(mode := 'add_library', library := 'R');
SELECT ok, mode, code FROM tcc_module(mode := 'add_source', source := '${SOURCE_SQL}');
SELECT ok, mode, code FROM tcc_module(mode := 'tinycc_bind', symbol := 'r_hello_from_embedded', sql_name := 'r_hello_from_embedded');
SELECT ok, mode, code FROM tcc_module(mode := 'compile', return_type := 'varchar', arg_types := []);
SELECT r_hello_from_embedded() AS msg;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ tcc_new_state │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ add_include │ OK      │
    └─────────┴─────────────┴─────────┘
    ┌─────────┬────────────────┬─────────┐
    │   ok    │      mode      │  code   │
    │ boolean │    varchar     │ varchar │
    ├─────────┼────────────────┼─────────┤
    │ true    │ add_sysinclude │ OK      │
    └─────────┴────────────────┴─────────┘
    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ add_define │ OK      │
    └─────────┴────────────┴─────────┘
    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ add_option │ OK      │
    └─────────┴────────────┴─────────┘
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ add_library │ OK      │
    └─────────┴─────────────┴─────────┘
    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ add_source │ OK      │
    └─────────┴────────────┴─────────┘
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    ┌─────────────────────────────┐
    │             msg             │
    │           varchar           │
    ├─────────────────────────────┤
    │ hello from embedded R 4.5.2 │
    └─────────────────────────────┘

Notes for this demo:

- `Rf_initEmbeddedR` is one-time per process; do not attempt repeated
  init/shutdown cycles in random query paths.
- Treat this as an advanced interop pattern, not a default production
  setup.
- A ready-to-run wrapper script is included: `demo/embed_r_demo.sh`.

## Notes

- Function invocation is plain SQL (`SELECT my_udf(...)`) after
  `compile` or `quick_compile`.
- `add_header`/`add_source` are compiled as separate translation units.
  For external library prototypes, include headers directly in the
  relevant `source` unit.
- Use `tcc_system_paths()` and `tcc_library_probe()` to diagnose
  platform-specific library resolution (including Windows/Rtools/MSYS2
  layouts).
- All outputs are returned as a diagnostics table for SQL-native
  observability.
