
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

### Discovery Helpers

| Function                                                                    | Purpose                                                        | Notes                                                                              |
|-----------------------------------------------------------------------------|----------------------------------------------------------------|------------------------------------------------------------------------------------|
| `tcc_system_paths(runtime_path := '', library_path := '')`                  | Show include and library search paths used by TinyCC workflows | Includes existence flags; includes Windows/MSYS2/Rtools defaults on Windows builds |
| `tcc_library_probe(library := ..., runtime_path := '', library_path := '')` | Resolve a library short name or full filename/path             | Supports `.a`, `.so*`, `.dylib`, `.dll`, `.lib` style names                        |

## Supported Types

Type metadata is explicit for `compile`, `quick_compile`, and
`codegen_preview`:

- `return_type := ...` is required
- `arg_types := [...]` is required (use `[]` for zero arguments)

Current executable SQL registration support:

| Signature Piece          | Accepted Tokens                                                                                                                                                                                                                                              | DuckDB Type                                                                |
|--------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|
| `return_type`            | `void`, `bool`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `varchar`, `text`, `string`, `cstring`, `blob`, `bytea`, `binary`, `varbinary`, `buffer`, `bytes`, `uuid`, `date`, `time`, `timestamp`, `interval`, `decimal`, `numeric` | `BIGINT` for `void`; otherwise matching scalar type                        |
| each `arg_types` element | same scalar tokens as `return_type`, plus fixed-child list tokens `list_i64` or `i64[]`, fixed-size array tokens like `i64[3]`, `struct<name:type;...>`, and `map<key_type;value_type>`                                                                      | scalar type, `LIST(child)`, `ARRAY(child,N)`, `STRUCT(...)`, or `MAP(...)` |

Additional limits and behavior:

- Arity: `0..N` arguments (no fixed hard-coded cap)
- Wrapper mode: `row` (default) or `batch`
- NULL handling: NULL-in/NULL-out
- `void` return registers as a SQL function that emits `NULL` values
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
- current `struct<...>` and `map<...>` members support fixed-width
  scalar families only
  (`bool/i8/u8/i16/u16/i32/u32/i64/u64/f32/f64/uuid/date/time/timestamp/interval/decimal`)
- `struct<...>` and `map<...>` use `;` separators in tokens to avoid
  ambiguity inside `arg_types := [...]`
- `decimal`/`numeric` currently bind as `DECIMAL(18,3)`

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

## Design Direction (API Still Evolving)

- Host symbol registration mode: feasible and aligned with Rtinycc-style
  embedding; prefer an allow-listed registry (name -\> host address)
  over raw SQL-provided addresses.
- Arrow to reduce marshalling: not used directly; batch wrappers already
  consume DuckDB vectors column-wise and avoid per-row host-wrapper
  crossings.
- DuckDB nested types: `LIST`, `ARRAY`, `STRUCT`, and `MAP` are
  first-class SQL argument/return tokens with C descriptors;
  nested/non-fixed-width struct/map members remain future work.
- Pointer utilities (next): adopt an Rtinycc-like helper layer for typed
  memory I/O (`read_i8/u8/.../f64/ptr`, `write_*`, `read_bytes`,
  `write_bytes`) on opaque pointer handles.
- Pointer representation: use `UBIGINT` handles in SQL mapped to an
  internal pointer registry with ownership tags (`owned`, `borrowed`,
  `external`) and optional type tags.
- Real C structs (not DuckDB `STRUCT`): treat these as pointer-backed
  objects (`ptr<T>` style), with helper UDFs for `malloc/free`, field
  address, `container_of`, and typed field get/set/read/write.
- Safety posture: no raw user-provided addresses by default; pointers
  should enter through controlled constructors/host symbols and be
  validated through the registry.

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
    Run Time (s): real 0.001 user 0.000347 sys 0.000538
    ┌─────────┬────────────┬─────────┬───────────────────────────────────────────────────────────────────────────────────────┐
    │   ok    │    mode    │  code   │                                        detail                                         │
    │ boolean │  varchar   │ varchar │                                        varchar                                        │
    ├─────────┼────────────┼─────────┼───────────────────────────────────────────────────────────────────────────────────────┤
    │ true    │ config_get │ OK      │ runtime=/root/DuckTinyCC/cmake_build/release/tinycc_build state_id=0 config_version=1 │
    └─────────┴────────────┴─────────┴───────────────────────────────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000268 sys 0.000201
    ┌─────────┬─────────┬─────────┬───────────────────────────────────────────────────────────────┐
    │   ok    │  mode   │  code   │                            detail                             │
    │ boolean │ varchar │ varchar │                            varchar                            │
    ├─────────┼─────────┼─────────┼───────────────────────────────────────────────────────────────┤
    │ true    │ list    │ OK      │ registered=0 sources=0 headers=0 includes=0 libs=0 state_id=0 │
    └─────────┴─────────┴─────────┴───────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.001 user 0.000323 sys 0.000000

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
    Run Time (s): real 0.000 user 0.000640 sys 0.000000
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
    Run Time (s): real 0.001 user 0.000626 sys 0.000000

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
    Run Time (s): real 0.001 user 0.000000 sys 0.000362
    ┌─────────┬─────────────┬─────────┬────────────────────────────┐
    │   ok    │    mode     │  code   │           detail           │
    │ boolean │   varchar   │ varchar │          varchar           │
    ├─────────┼─────────────┼─────────┼────────────────────────────┤
    │ true    │ add_include │ OK      │ third_party/tinycc/include │
    └─────────┴─────────────┴─────────┴────────────────────────────┘
    Run Time (s): real 0.000 user 0.000000 sys 0.000552
    ┌─────────┬────────────────┬─────────┬────────────────────────────┐
    │   ok    │      mode      │  code   │           detail           │
    │ boolean │    varchar     │ varchar │          varchar           │
    ├─────────┼────────────────┼─────────┼────────────────────────────┤
    │ true    │ add_sysinclude │ OK      │ third_party/tinycc/include │
    └─────────┴────────────────┴─────────┴────────────────────────────┘
    Run Time (s): real 0.000 user 0.000114 sys 0.000105
    ┌─────────┬──────────────────┬─────────┬────────────────────┐
    │   ok    │       mode       │  code   │       detail       │
    │ boolean │     varchar      │ varchar │      varchar       │
    ├─────────┼──────────────────┼─────────┼────────────────────┤
    │ true    │ add_library_path │ OK      │ third_party/tinycc │
    └─────────┴──────────────────┴─────────┴────────────────────┘
    Run Time (s): real 0.001 user 0.000313 sys 0.000157
    ┌─────────┬────────────┬─────────┬─────────┐
    │   ok    │    mode    │  code   │ detail  │
    │ boolean │  varchar   │ varchar │ varchar │
    ├─────────┼────────────┼─────────┼─────────┤
    │ true    │ add_option │ OK      │ -O2     │
    └─────────┴────────────┴─────────┴─────────┘
    Run Time (s): real 0.000 user 0.000201 sys 0.000000
    ┌─────────┬────────────┬─────────┬───────────┐
    │   ok    │    mode    │  code   │  detail   │
    │ boolean │  varchar   │ varchar │  varchar  │
    ├─────────┼────────────┼─────────┼───────────┤
    │ true    │ add_define │ OK      │ TCC_SHIFT │
    └─────────┴────────────┴─────────┴───────────┘
    Run Time (s): real 0.000 user 0.000281 sys 0.000000
    ┌─────────┬────────────┬─────────┬─────────────────┐
    │   ok    │    mode    │  code   │     detail      │
    │ boolean │  varchar   │ varchar │     varchar     │
    ├─────────┼────────────┼─────────┼─────────────────┤
    │ true    │ add_header │ OK      │ header appended │
    └─────────┴────────────┴─────────┴─────────────────┘
    Run Time (s): real 0.000 user 0.000186 sys 0.000000
    ┌─────────┬────────────┬─────────┬─────────────────┐
    │   ok    │    mode    │  code   │     detail      │
    │ boolean │  varchar   │ varchar │     varchar     │
    ├─────────┼────────────┼─────────┼─────────────────┤
    │ true    │ add_source │ OK      │ source appended │
    └─────────┴────────────┴─────────┴─────────────────┘
    Run Time (s): real 0.000 user 0.000181 sys 0.000000
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000171 sys 0.000004
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.003 user 0.001670 sys 0.000853
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘
    Run Time (s): real 0.000 user 0.000134 sys 0.000060
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000153 sys 0.000070
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.003 user 0.005467 sys 0.000000
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘
    Run Time (s): real 0.000 user 0.000194 sys 0.000000

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
    Run Time (s): real 0.004 user 0.002468 sys 0.002068
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    32 │
    └───────┘
    Run Time (s): real 0.000 user 0.000189 sys 0.000040

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
    Run Time (s): real 0.001 user 0.001129 sys 0.001028
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ add_library │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.001 user 0.001456 sys 0.000000
    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ add_source │ OK      │
    └─────────┴────────────┴─────────┘
    Run Time (s): real 0.001 user 0.000542 sys 0.000093
    ┌─────────┬─────────────┬─────────┐
    │   ok    │    mode     │  code   │
    │ boolean │   varchar   │ varchar │
    ├─────────┼─────────────┼─────────┤
    │ true    │ tinycc_bind │ OK      │
    └─────────┴─────────────┴─────────┘
    Run Time (s): real 0.000 user 0.001102 sys 0.000085
    ┌─────────┬─────────┬─────────┐
    │   ok    │  mode   │  code   │
    │ boolean │ varchar │ varchar │
    ├─────────┼─────────┼─────────┤
    │ true    │ compile │ OK      │
    └─────────┴─────────┴─────────┘
    Run Time (s): real 0.008 user 0.005880 sys 0.001991
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    32 │
    └───────┘
    Run Time (s): real 0.000 user 0.000224 sys 0.000099

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
    Run Time (s): real 0.000 user 0.000478 sys 0.000064
    ┌─────────┬──────────────┬─────────┐
    │   ok    │     mode     │  code   │
    │ boolean │   varchar    │ varchar │
    ├─────────┼──────────────┼─────────┤
    │ true    │ config_reset │ OK      │
    └─────────┴──────────────┴─────────┘
    Run Time (s): real 0.000 user 0.000276 sys 0.000037

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
    Run Time (s): real 0.000 user 0.000617 sys 0.000000

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
    Run Time (s): real 0.003 user 0.004377 sys 0.000180
    ┌──────────────┐
    │      s       │
    │    int64     │
    ├──────────────┤
    │ 500001500000 │
    └──────────────┘
    Run Time (s): real 0.005 user 0.010080 sys 0.000289

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
    Run Time (s): real 0.002 user 0.001987 sys 0.000000
    ┌───────────┐
    │ sum_plain │
    │   int64   │
    ├───────────┤
    │         6 │
    └───────────┘
    Run Time (s): real 0.001 user 0.000516 sys 0.000000
    ┌─────────────────────┐
    │ sum_with_null_child │
    │        int64        │
    ├─────────────────────┤
    │                   4 │
    └─────────────────────┘
    Run Time (s): real 0.000 user 0.000278 sys 0.000000

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
    Run Time (s): real 0.001 user 0.001427 sys 0.000284
    ┌───────────┐
    │ sum_plain │
    │   int64   │
    ├───────────┤
    │         6 │
    └───────────┘
    Run Time (s): real 0.000 user 0.000143 sys 0.000095
    ┌─────────────────────┐
    │ sum_with_null_child │
    │        int64        │
    ├─────────────────────┤
    │                   4 │
    └─────────────────────┘
    Run Time (s): real 0.001 user 0.000146 sys 0.000098

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
    Run Time (s): real 0.002 user 0.002870 sys 0.000940
    ┌───────┐
    │   s   │
    │ int64 │
    ├───────┤
    │     7 │
    └───────┘
    Run Time (s): real 0.000 user 0.000157 sys 0.000069

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
    Run Time (s): real 0.001 user 0.001319 sys 0.000344
    ┌───────┐
    │   s   │
    │ int64 │
    ├───────┤
    │    33 │
    └───────┘
    Run Time (s): real 0.001 user 0.000146 sys 0.000120

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
    Run Time (s): real 0.002 user 0.002729 sys 0.000919
    ┌──────────────┐
    │ before_reset │
    │    int32     │
    ├──────────────┤
    │           42 │
    └──────────────┘
    Run Time (s): real 0.000 user 0.000180 sys 0.000069
    ┌─────────┬──────────────┬─────────┐
    │   ok    │     mode     │  code   │
    │ boolean │   varchar    │ varchar │
    ├─────────┼──────────────┼─────────┤
    │ true    │ config_reset │ OK      │
    └─────────┴──────────────┴─────────┘
    Run Time (s): real 0.001 user 0.000163 sys 0.000063
    ┌─────────┬─────────┬─────────┬───────────────────────────────────────────────────────────────┐
    │   ok    │  mode   │  code   │                            detail                             │
    │ boolean │ varchar │ varchar │                            varchar                            │
    ├─────────┼─────────┼─────────┼───────────────────────────────────────────────────────────────┤
    │ true    │ list    │ OK      │ registered=1 sources=0 headers=0 includes=0 libs=0 state_id=1 │
    └─────────┴─────────┴─────────┴───────────────────────────────────────────────────────────────┘
    Run Time (s): real 0.000 user 0.000229 sys 0.000000
    ┌─────────────┐
    │ after_reset │
    │    int32    │
    ├─────────────┤
    │          42 │
    └─────────────┘
    Run Time (s): real 0.000 user 0.000172 sys 0.000000

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

    Run Time (s): real 0.000 user 0.000279 sys 0.000112
    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    Run Time (s): real 0.002 user 0.002042 sys 0.000000
    ┌─────────────────┐
    │ add_i32(20, 22) │
    │      int32      │
    ├─────────────────┤
    │              42 │
    └─────────────────┘
    Run Time (s): real 0.001 user 0.000081 sys 0.000659
    ┌──────────────┐
    │      s       │
    │    int128    │
    ├──────────────┤
    │ 500041500000 │
    └──────────────┘
    Run Time (s): real 0.014 user 0.014597 sys 0.000192
    ┌──────────────┐
    │      s       │
    │    int128    │
    ├──────────────┤
    │ 500041500000 │
    └──────────────┘
    Run Time (s): real 0.014 user 0.013617 sys 0.000000
    ┌──────────────┐
    │      s       │
    │    int128    │
    ├──────────────┤
    │ 500041500000 │
    └──────────────┘
    Run Time (s): real 0.013 user 0.013120 sys 0.000000

### 13) Cleanup

No explicit cleanup is required in the CLI example flow.

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
