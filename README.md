
<!-- README.md is generated from README.Rmd. Please edit that file -->

# DuckTinyCC

DuckTinyCC is a DuckDB C extension with a `tcc_*` SQL surface for
TinyCC-based in-process C scripting workflows.

## Functions

| Function                                                                                   | Description                                                         | Notes                                    |
|--------------------------------------------------------------------------------------------|---------------------------------------------------------------------|------------------------------------------|
| `tcc_module(mode := 'config_get', ...)`                                                    | Show current session configuration                                  | Returns diagnostics table                |
| `tcc_module(mode := 'config_set', runtime_path := ... )`                                   | Set session runtime path                                            | Connection-scoped state                  |
| `tcc_module(mode := 'config_reset', ...)`                                                  | Reset session configuration                                         | Falls back to default runtime path       |
| `tcc_module(mode := 'tcc_new_state', ...)`                                                 | Start a fresh TinyCC build state                                    | Keeps connection/runtime scope           |
| `tcc_module(mode := 'add_include'/'add_sysinclude'/'add_library_path'/'add_library', ...)` | Add TinyCC include/library inputs                                   | Session-scoped build inputs              |
| `tcc_module(mode := 'add_option'/'add_define'/'add_header'/'add_source', ...)`             | Add compile options/defines/code units                              | Session-scoped build inputs              |
| `tcc_module(mode := 'tinycc_bind', symbol := ..., sql_name := ...)`                        | Bind symbol + sql alias for next compile                            | Rtinycc-style bind step                  |
| `tcc_module(mode := 'list', ...)`                                                          | Show current registry/session counters                              | Diagnostics table                        |
| `tcc_module(mode := 'compile'/'tinycc_compile', ...)`                                      | Compile + relocate from current session and store callable artifact | Fresh `tcc_new` per compile              |
| `tcc_module(mode := 'call', source := ..., symbol := ..., arg_bigint := ...)`              | Compile + execute `BIGINT(BIGINT)` symbol                           | One-shot compile+execute                 |
| `tcc_module(mode := 'register', source := ..., symbol := ..., sql_name := ...)`            | Compile unit and store callable artifact in session registry        | Uses fresh TinyCC state per registration |
| `tcc_module(mode := 'call', sql_name := ..., arg_bigint := ...)`                           | Execute previously registered artifact                              | No recompile on call path                |
| `tcc_module(mode := 'unregister', sql_name := ...)`                                        | Remove a registered artifact from session registry                  | Frees TinyCC state                       |

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

The examples below follow the same `DBI` + `duckdb` usage style as
`duckhts`.

``` r
library(DBI)
library(duckdb)

drv <- duckdb::duckdb(config = list(allow_unsigned_extensions = "true"))
con <- dbConnect(drv, dbdir = ":memory:")
ext_path <- normalizePath("build/release/ducktinycc.duckdb_extension", mustWork = FALSE)
dbExecute(con, sprintf("LOAD '%s'", ext_path))

dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(mode := 'tcc_new_state')
")

dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'add_source',
    source := 'long long tcc_add1(long long x){ return x + 1; }'
  )
")

dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(
    mode := 'tinycc_bind',
    symbol := 'tcc_add1',
    sql_name := 'tcc_add1'
  )
")

dbGetQuery(con, "
  SELECT ok, mode, code
  FROM tcc_module(mode := 'tinycc_compile')
")

dbGetQuery(con, "
  SELECT ok, mode, code, detail
  FROM tcc_module(
    mode := 'call',
    sql_name := 'tcc_add1',
    arg_bigint := '41'
  )
")

dbDisconnect(con, shutdown = TRUE)
```

## Notes

- Current callable execution path is `mode := 'call'` with
  `BIGINT(BIGINT)` function signatures.
- Dynamic runtime registration as standalone DuckDB scalar names from
  inside `tcc_module` is not enabled in this build; artifacts are
  session-registered and invoked with `mode := 'call'`.
- All outputs are returned as a diagnostics table for SQL-native
  observability.
