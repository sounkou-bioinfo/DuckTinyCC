
<!-- README.md is generated from README.Rmd. Please edit that file -->

# DuckTinyCC

DuckTinyCC is a DuckDB C extension with a `tcc_*` SQL surface.

The current implementation provides a session-oriented control plane
through `tcc_module(...)` and returns structured diagnostics rows for
each operation.

## Build

``` sh
make configure
make debug
```

## Test

``` sh
make test_debug
```

## Usage from R (DBI)

``` r
library(DBI)
library(duckdb)
```

``` r
db <- duckdb(config = list(allow_unsigned_extensions = "true"))
con <- dbConnect(
  db
)
dbExecute(con, "LOAD 'build/debug/capi_quack.duckdb_extension'")
```

    ## [1] 0

For convenience in examples:

``` r
q <- function(sql) dbGetQuery(con, sql)
```

### Inspect default session state

``` r
q("SELECT ok, mode, code, detail FROM tcc_module(mode := 'config_get')")
```

    ##     ok       mode code  detail
    ## 1 TRUE config_get   OK (unset)

### Set runtime path for this connection session

``` r
q(
  "SELECT ok, mode, code, detail
   FROM tcc_module(
     mode := 'config_set',
     runtime_path := 'third_party/tinycc'
   )"
)
```

    ##     ok       mode code             detail
    ## 1 TRUE config_set   OK third_party/tinycc

### Verify current session config

``` r
q("SELECT ok, mode, code, detail FROM tcc_module(mode := 'config_get')")
```

    ##     ok       mode code             detail
    ## 1 TRUE config_get   OK third_party/tinycc

### List current artifact registry state

``` r
q("SELECT ok, mode, phase, code, message FROM tcc_module(mode := 'list')")
```

    ##     ok mode    phase code                           message
    ## 1 TRUE list registry   OK no registered tcc_* artifacts yet

### Reset session configuration

``` r
q("SELECT ok, mode, code, detail FROM tcc_module(mode := 'config_reset')")
```

    ##     ok         mode code  detail
    ## 1 TRUE config_reset   OK (unset)

### Error diagnostics example (invalid mode)

``` r
q("SELECT ok, mode, phase, code, message FROM tcc_module(mode := 'unknown')")
```

    ##      ok    mode phase       code      message
    ## 1 FALSE unknown  bind E_BAD_MODE unknown mode

## Notes

- The API is intentionally row-oriented and diagnostic-first, so it fits
  SQL workflows and test assertions.
- `compile/register` execution paths are being wired to TinyCC runtime
  next.

``` r
dbDisconnect(con, shutdown = TRUE)
```
