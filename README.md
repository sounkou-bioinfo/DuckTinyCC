
<!-- README.md is generated from README.Rmd. Please edit that file -->

# DuckTinyCC

DuckTinyCC is a DuckDB C extension with a `tcc_*` SQL surface for
TinyCC-based in-process C scripting workflows.

## Functions

| Function                                                                        | Description                                                  | Notes                                    |
|---------------------------------------------------------------------------------|--------------------------------------------------------------|------------------------------------------|
| `tcc_module(mode := 'config_get', ...)`                                         | Show current session configuration                           | Returns diagnostics table                |
| `tcc_module(mode := 'config_set', runtime_path := ... )`                        | Set session runtime path                                     | Connection-scoped state                  |
| `tcc_module(mode := 'config_reset', ...)`                                       | Reset session configuration                                  | Falls back to default runtime path       |
| `tcc_module(mode := 'list', ...)`                                               | Show current registry/session counters                       | Diagnostics table                        |
| `tcc_module(mode := 'compile', source := ..., symbol := ...)`                   | Compile + relocate C source and resolve symbol               | Real libtcc execution                    |
| `tcc_module(mode := 'call', source := ..., symbol := ..., arg_bigint := ...)`   | Compile + execute `BIGINT(BIGINT)` symbol                    | One-shot compile+execute                 |
| `tcc_module(mode := 'register', source := ..., symbol := ..., sql_name := ...)` | Compile unit and store callable artifact in session registry | Uses fresh TinyCC state per registration |
| `tcc_module(mode := 'call', sql_name := ..., arg_bigint := ...)`                | Execute previously registered artifact                       | No recompile on call path                |
| `tcc_module(mode := 'unregister', sql_name := ...)`                             | Remove a registered artifact from session registry           | Frees TinyCC state                       |

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

## SQL Usage

``` sql
-- In local development, unsigned extension loading must be enabled in the client.
LOAD 'build/debug/ducktinycc.duckdb_extension';
```

``` sql
SELECT ok, mode, code, detail
FROM tcc_module(mode := 'config_get');
```

``` sql
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'config_set',
  runtime_path := 'third_party/tinycc'
);
```

``` sql
SELECT ok, mode, code
FROM tcc_module(
  mode := 'compile',
  source := 'long long tcc_tmp(long long x){ return x + 1; }',
  symbol := 'tcc_tmp'
);
```

``` sql
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'call',
  source := 'long long tcc_add1(long long x){ return x + 1; }',
  symbol := 'tcc_add1',
  arg_bigint := '41'
);
```

``` sql
SELECT ok, mode, code
FROM tcc_module(
  mode := 'register',
  source := 'long long tcc_add1(long long x){ return x + 1; }',
  symbol := 'tcc_add1',
  sql_name := 'tcc_add1'
);
```

``` sql
SELECT ok, mode, code, detail
FROM tcc_module(
  mode := 'call',
  sql_name := 'tcc_add1',
  arg_bigint := '41'
);
```

``` sql
-- Naked-style SQL call via a macro wrapper
CREATE OR REPLACE MACRO tcc_add1(x) AS (
  SELECT CAST(detail AS BIGINT)
  FROM tcc_module(
    mode := 'call',
    sql_name := 'tcc_add1',
    arg_bigint := CAST(x AS VARCHAR)
  )
);

SELECT tcc_add1(41) AS answer;
```

``` sql
-- Series/loop-style usage
SELECT i, tcc_add1(i) AS i_plus_1
FROM range(0, 10) AS t(i);
```

``` sql
-- Array-style usage through UNNEST
WITH vals AS (
  SELECT unnest([1, 2, 3, 10, 20]) AS x
)
SELECT x, tcc_add1(x) AS y
FROM vals;
```

## Notes

- Current callable execution path is `mode := 'call'` with
  `BIGINT(BIGINT)` function signatures.
- All outputs are returned as a diagnostics table for SQL-native
  observability.
