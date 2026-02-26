
<!-- README.md is generated from README.Rmd. Please edit that file -->

# DuckTinyCC

DuckTinyCC is a DuckDB C extension with a `tcc_*` SQL surface for
TinyCC-based in-process C scripting workflows.

## API Surface

All functionality is exposed through `tcc_module(...)` and selected with
`mode := ...`.  
Every mode returns one diagnostics row with `ok`, `code`, `message`, and
context columns.

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

### Compile, Load, and Registry

| Mode                        | Purpose                                          | Notes                                                 |
|-----------------------------|--------------------------------------------------|-------------------------------------------------------|
| `compile`, `tinycc_compile` | Compile staged session code and register SQL UDF | Uses `tinycc_bind` or explicit `symbol`/`sql_name`    |
| `register`                  | Compile provided `source` and register SQL UDF   | One-shot registration                                 |
| `ffi_load`                  | Generate C loader + register SQL UDF             | Rtinycc-style dynamic codegen path                    |
| `load`                      | Compile and execute module init symbol           | Manual dynamic module path                            |
| `unregister`                | Remove artifact metadata from session registry   | Loaded/codegen modules are pinned (`E_UNSAFE_UNLOAD`) |

## Supported Types

Type metadata is explicit for `compile`, `tinycc_compile`, `register`,
and `ffi_load`:

- `return_type := ...` is required
- `arg_types := [...]` is required (use `[]` for zero arguments)

Current executable SQL registration support:

| Signature Piece          | Accepted Tokens             | DuckDB Type |
|--------------------------|-----------------------------|-------------|
| `return_type`            | `i64`, `bigint`, `longlong` | `BIGINT`    |
| each `arg_types` element | `i64`, `bigint`, `longlong` | `BIGINT`    |

Additional limits and behavior:

- Arity: `0..10` arguments
- NULL handling: NULL-in/NULL-out on the generated BIGINT wrappers
- `return_type := 'void'` is parsed but not currently executable in the
  SQL registration path (`E_UNSUPPORTED_SIGNATURE`)

## Build

``` sh
make configure
make debug
```

## Test

``` sh
make test_debug
make test_release
```

## Examples

The examples below use `DBI` + `duckdb` and focus on one workflow at a
time.

### 1) Setup

This chunk connects to DuckDB in-memory and loads the extension
artifact.

``` r
library(DBI)
library(duckdb)

drv <- duckdb::duckdb(config = list(allow_unsigned_extensions = "true"))
con <- dbConnect(drv, dbdir = ":memory:")
ext_path <- normalizePath("build/release/ducktinycc.duckdb_extension", mustWork = FALSE)
dbExecute(con, sprintf("LOAD '%s'", ext_path))
#> [1] 0
```

### 2) Session Configuration

This chunk initializes a clean session and inspects the current state.

``` r
dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(
    mode := 'config_set',
    runtime_path := ''
  )
")
#>     ok       mode code  detail
#> 1 TRUE config_set   OK (empty)

dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(mode := 'config_get')
")
#>     ok       mode code
#> 1 TRUE config_get   OK
#>                                                                                  detail
#> 1 runtime=/root/DuckTinyCC/cmake_build/release/tinycc_build state_id=0 config_version=1

dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(mode := 'list')
")
#>     ok mode code                                                        detail
#> 1 TRUE list   OK registered=0 sources=0 headers=0 includes=0 libs=0 state_id=0
```

### 3) Example A: Staged Build + `tinycc_bind` + `tinycc_compile`

This example stages shared build inputs once, compiles two symbols, and
calls both SQL functions.

``` r
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'tcc_new_state')")
#>     ok          mode code     detail
#> 1 TRUE tcc_new_state   OK state_id=1
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'add_include', include_path := 'third_party/tinycc/include')")
#>     ok        mode code                     detail
#> 1 TRUE add_include   OK third_party/tinycc/include
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'add_sysinclude', sysinclude_path := 'third_party/tinycc/include')")
#>     ok           mode code                     detail
#> 1 TRUE add_sysinclude   OK third_party/tinycc/include
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'add_library_path', library_path := 'third_party/tinycc')")
#>     ok             mode code             detail
#> 1 TRUE add_library_path   OK third_party/tinycc
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'add_option', option := '-O2')")
#>     ok       mode code detail
#> 1 TRUE add_option   OK    -O2
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'add_define', define_name := 'TCC_SHIFT', define_value := '3')")
#>     ok       mode code    detail
#> 1 TRUE add_define   OK TCC_SHIFT

dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(
    mode := 'add_header',
    header := '#include <stdint.h>'
  )
")
#>     ok       mode code          detail
#> 1 TRUE add_header   OK header appended

dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(
    mode := 'add_source',
    source := '
      long long tcc_add_shift(long long x){ return x + TCC_SHIFT; }
      long long tcc_times2(long long x){ return x * 2; }
    '
  )
")
#>     ok       mode code          detail
#> 1 TRUE add_source   OK source appended

dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(mode := 'tinycc_bind', symbol := 'tcc_add_shift', sql_name := 'tcc_add_shift')
")
#>     ok        mode code
#> 1 TRUE tinycc_bind   OK
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'tinycc_compile',
    return_type := 'i64',
    arg_types := ['i64']
  )
")
#>     ok           mode code
#> 1 TRUE tinycc_compile   OK
dbGetQuery(con, "SELECT tcc_add_shift(39) AS value")
#>   value
#> 1    42

dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(mode := 'tinycc_bind', symbol := 'tcc_times2', sql_name := 'tcc_times2')
")
#>     ok        mode code
#> 1 TRUE tinycc_bind   OK
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'compile',
    return_type := 'i64',
    arg_types := ['i64']
  )
")
#>     ok    mode code
#> 1 TRUE compile   OK
dbGetQuery(con, "SELECT tcc_times2(21) AS value")
#>   value
#> 1    42
```

### 4) Example B: One-Shot `register`

This example compiles inline source and registers immediately without
staged state reuse.

``` r
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'register',
    source := 'long long tcc_add3(long long a,long long b,long long c){ return a + b + c; }',
    symbol := 'tcc_add3',
    sql_name := 'tcc_add3',
    return_type := 'i64',
    arg_types := ['i64', 'i64', 'i64']
  )
")
#>     ok     mode code
#> 1 TRUE register   OK
dbGetQuery(con, "SELECT tcc_add3(1, 2, 3) AS value")
#>   value
#> 1     6
```

### 5) Example C: `ffi_load` Codegen Path

This chunk exercises the dynamic C codegen path (`ffi_load`) and then
calls the registered SQL function normally.

``` r
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'ffi_load',
    source := 'long long tcc_add10(long long x){ return x + 10; }',
    symbol := 'tcc_add10',
    sql_name := 'tcc_add10',
    return_type := 'i64',
    arg_types := ['i64']
  )
")
#>     ok     mode code
#> 1 TRUE ffi_load   OK

dbGetQuery(con, "SELECT tcc_add10(32) AS value")
#>   value
#> 1    42
```

### 6) Example D: `load` Module Init Path

This example loads a manual module init symbol that registers a SQL
function through host helpers.

``` r
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'load',
    sql_name := 'jit_module_add100',
    symbol := 'jit_mod_init',
    source := '
      typedef struct _duckdb_connection *duckdb_connection;
      extern _Bool ducktinycc_register_i64_unary(duckdb_connection con, const char *name, void *fn_ptr);
      long long jit_add100_impl(long long x) { return x + 100; }
      _Bool jit_mod_init(duckdb_connection con) {
        return ducktinycc_register_i64_unary(con, \"jit_add100\", (void *)jit_add100_impl);
      }
    '
  )
")
#>     ok mode code
#> 1 TRUE load   OK
dbGetQuery(con, "SELECT jit_add100(7) AS value")
#>   value
#> 1   107
```

### 7) Reset and Unregister Notes

This chunk shows the current registry and reset behavior. Loaded/codegen
modules are pinned for safety, so `unregister` returns
`E_UNSAFE_UNLOAD`.

``` r
dbGetQuery(con, "SELECT ok, mode, code FROM tcc_module(mode := 'unregister', sql_name := 'tcc_add10')")
#>      ok       mode            code
#> 1 FALSE unregister E_UNSAFE_UNLOAD
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'list')")
#>     ok mode code                                                        detail
#> 1 TRUE list   OK registered=5 sources=1 headers=1 includes=1 libs=0 state_id=1
dbGetQuery(con, "SELECT ok, mode, code FROM tcc_module(mode := 'config_reset')")
#>     ok         mode code
#> 1 TRUE config_reset   OK
```

### 8) Cleanup

This chunk closes the database connection.

``` r
dbDisconnect(con, shutdown = TRUE)
```

## Notes

- Function invocation is plain SQL (`SELECT my_udf(...)`) after
  `compile`/`register`/`load`/`ffi_load`.
- Dynamic module mode (`mode := 'load'`) keeps compiled TinyCC state
  alive and can use host helper symbols such as
  `ducktinycc_register_i64_unary`.
- Dynamic FFI codegen mode (`mode := 'ffi_load'`) generates a C loader
  module and registers SQL functions via host helpers.
- All outputs are returned as a diagnostics table for SQL-native
  observability.
