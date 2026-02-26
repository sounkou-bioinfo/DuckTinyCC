
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

### Compile and Register

| Mode                        | Purpose                                          | Notes                                                               |
|-----------------------------|--------------------------------------------------|---------------------------------------------------------------------|
| `compile`, `tinycc_compile` | Compile staged session code and register SQL UDF | Uses `tinycc_bind` or explicit `symbol`/`sql_name`                  |
| `quick_compile`             | One-shot fast lane compile + register            | Requires `source`, `symbol`, `sql_name`, `return_type`, `arg_types` |

## Supported Types

Type metadata is explicit for `compile`, `tinycc_compile`, and
`quick_compile`:

- `return_type := ...` is required
- `arg_types := [...]` is required (use `[]` for zero arguments)

Current executable SQL registration support:

| Signature Piece          | Accepted Tokens                                                                    | DuckDB Type                                         |
|--------------------------|------------------------------------------------------------------------------------|-----------------------------------------------------|
| `return_type`            | `void`, `bool`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64` | `BIGINT` for `void`; otherwise matching scalar type |
| each `arg_types` element | `bool`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64`         | matching scalar type                                |

Additional limits and behavior:

- Arity: `0..10` arguments
- NULL handling: NULL-in/NULL-out
- `void` return registers as a SQL function that emits `NULL` values

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

### 4) Example B: Fast Lane `quick_compile`

This example compiles and registers directly from one SQL call, without
staged state.

``` r
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'quick_compile',
    source := 'int add_i32(int a, int b){ return a + b; }',
    symbol := 'add_i32',
    sql_name := 'add_i32',
    return_type := 'i32',
    arg_types := ['i32', 'i32']
  )
")
#>     ok          mode code
#> 1 TRUE quick_compile   OK
dbGetQuery(con, "SELECT add_i32(20, 22) AS value")
#>   value
#> 1    42
```

### 5) Example C: Libraries (`add_library`)

This example links `libm`, compiles a function that uses `pow`, then
calls it. The required C header is included in the source unit.

``` r
dbGetQuery(con, "SELECT ok, mode, code FROM tcc_module(mode := 'tcc_new_state')")
#>     ok          mode code
#> 1 TRUE tcc_new_state   OK
dbGetQuery(con, "SELECT ok, mode, code FROM tcc_module(mode := 'add_library', library := 'm')")
#>     ok        mode code
#> 1 TRUE add_library   OK
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'add_source',
    source := '#include <math.h>
double pwr(double x, double y){ return pow(x, y); }'
  )
")
#>     ok       mode code
#> 1 TRUE add_source   OK
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(mode := 'tinycc_bind', symbol := 'pwr', sql_name := 'pwr')
")
#>     ok        mode code
#> 1 TRUE tinycc_bind   OK
dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(mode := 'compile', return_type := 'f64', arg_types := ['f64', 'f64'])
")
#>     ok    mode code
#> 1 TRUE compile   OK
dbGetQuery(con, "SELECT CAST(pwr(2.0, 5.0) AS BIGINT) AS value")
#>   value
#> 1    32
```

### 6) Reset Session

This chunk resets session state and shows summary counters.

``` r
dbGetQuery(con, "SELECT ok, mode, code, detail FROM tcc_module(mode := 'list')")
#>     ok mode code                                                        detail
#> 1 TRUE list   OK registered=4 sources=1 headers=0 includes=0 libs=1 state_id=2
dbGetQuery(con, "SELECT ok, mode, code FROM tcc_module(mode := 'config_reset')")
#>     ok         mode code
#> 1 TRUE config_reset   OK
```

### 7) Cleanup

This chunk closes the database connection.

``` r
dbDisconnect(con, shutdown = TRUE)
```

## Notes

- Function invocation is plain SQL (`SELECT my_udf(...)`) after
  `compile` or `quick_compile`.
- `add_header`/`add_source` are compiled as separate translation units.
  For external library prototypes, include headers directly in the
  relevant `source` unit.
- All outputs are returned as a diagnostics table for SQL-native
  observability.
