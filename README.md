
<!-- README.md is generated from README.Rmd. Please edit that file -->

# DuckTinyCC

`DuckTinyCC` is a DuckDB C extension that lets us compile C code at
runtime and register SQL scalar UDFs in-process through TinyCC. We use
`tcc_module(...)` as the control plane for staging inputs, generating
wrappers, compiling, and registering functions. We use
`tcc_system_paths(...)` and `tcc_library_probe(...)` when we need to
debug include/library resolution.

## Quick Start

The fastest path is `quick_compile`: one call compiles C source,
generates a wrapper, relocates in memory, and registers a SQL function.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';

-- Compile and register one scalar UDF.
SELECT ok, mode, phase, code, message
FROM tcc_module(
  mode := 'quick_compile',
  source := 'int add_i32(int a, int b){ return a + b; }',
  symbol := 'add_i32',
  sql_name := 'add_i32',
  return_type := 'i32',
  arg_types := ['i32', 'i32']
);

-- Call the generated function.
SELECT add_i32(20, 22) AS answer;
SQL
```

    ┌─────────┬───────────────┬─────────┬─────────┬──────────────────────────────────────────────────┐
    │   ok    │     mode      │  phase  │  code   │                     message                      │
    │ boolean │    varchar    │ varchar │ varchar │                     varchar                      │
    ├─────────┼───────────────┼─────────┼─────────┼──────────────────────────────────────────────────┤
    │ true    │ quick_compile │ load    │ OK      │ compiled and registered SQL function via codegen │
    └─────────┴───────────────┴─────────┴─────────┴──────────────────────────────────────────────────┘
    ┌────────┐
    │ answer │
    │ int32  │
    ├────────┤
    │     42 │
    └────────┘

## API Overview

`tcc_module(...)` defaults to `mode := 'config_get'` and returns one
diagnostics row with these columns:
`ok, mode, phase, code, message, detail, sql_name, symbol, artifact_id, connection_scope`.

In practice, we use session/config modes first (`config_get`,
`config_set`, `config_reset`, `list`, `tcc_new_state`), then staging
modes (`add_include`, `add_sysinclude`, `add_library_path`,
`add_library`, `add_option`, `add_define`, `add_header`, `add_source`,
`tinycc_bind`), then compile/codegen modes (`compile`, `quick_compile`,
`codegen_preview`). We also use helper-generation modes (`c_struct`,
`c_union`, `c_bitfield`, `c_enum`) when we want auto-generated C
composite helpers.

Outside `tcc_module(...)`, we expose `tcc_system_paths(...)`,
`tcc_library_probe(...)`, and pointer/memory helpers (`tcc_alloc`,
`tcc_free_ptr`, `tcc_ptr_size`, `tcc_dataptr`, `tcc_ptr_add`,
`tcc_read_*`, `tcc_write_*`, `tcc_read_bytes`, `tcc_write_bytes`).

## Signatures and Types

For `compile`, `quick_compile`, and `codegen_preview`, we provide
`return_type` and `arg_types` (`[]` for zero args). The parser accepts
scalar tokens (`void`, `bool`, `i8..u64`, `f32/f64`, `ptr`, `varchar`,
`blob`, `uuid`, `date`, `time`, `timestamp`, `interval`, `decimal`) plus
nested forms (`list<type>`, `type[]`, `type[N]`,
`struct<name:type;...>`, `map<key_type;value_type>`,
`union<name:type;...>`). Nested signatures are recursive. `wrapper_mode`
can be `row` (default) or `batch`.

## How It Works

At compile time, we parse signature tokens into recursive type
descriptors, generate wrapper C source around the target symbol, and
build one in-memory TinyCC artifact
(`tcc_new -> compile -> relocate -> module_init`). No shared library
file is emitted.

The generated module init calls `ducktinycc_register_signature(...)`,
which registers the SQL scalar UDF through the extension-managed DuckDB
connection. During execution, the runtime bridge marshals DuckDB vectors
into C bridge descriptors (including nested
`LIST/ARRAY/STRUCT/MAP/UNION`) and writes return values back into DuckDB
vectors.

`tcc_new_state` resets staged build inputs and increments `state_id`; it
does not allocate a new `TCCState`. `config_reset` clears staged state
and runtime path, but it does not remove already registered internal UDF
entries.

## Build and Test during development

``` sh
make configure
make debug
make release

make test_debug
make test_release
```

## Examples

### Compile In One Call

This example uses one-shot compile with `libm`, then calls the generated
function.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';

-- Compile and register qpow.
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := '#include <math.h>
double qpow(double x, double y){ return pow(x, y); }',
  symbol := 'qpow',
  sql_name := 'qpow',
  return_type := 'f64',
  arg_types := ['f64', 'f64'],
  library := 'm'
);

-- Use the generated function.
SELECT CAST(qpow(2.0, 5.0) AS BIGINT) AS value;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    32 │
    └───────┘

### Stage Then Compile

This example stages source and binding first, then compiles in a
separate call.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';

-- Reset staged state.
SELECT *
FROM tcc_module(
  mode := 'tcc_new_state'
);

-- Stage source text.
SELECT *
FROM tcc_module(
  mode := 'add_source',
  source := 'long long times2(long long x){return x*2;}'
);

-- Stage symbol/sql name.
SELECT *
FROM tcc_module(
  mode := 'tinycc_bind',
  symbol := 'times2',
  sql_name := 'times2'
);

-- Compile and register.
SELECT *
FROM tcc_module(
  mode := 'compile',
  return_type := 'i64',
  arg_types := ['i64']
);

SELECT times2(21) AS value;
SQL
```

    ┌─────────┬───────────────┬─────────┬─────────┬─────────────────────────────────┬────────────┬──────────┬─────────┬─────────────┬──────────────────┐
    │   ok    │     mode      │  phase  │  code   │             message             │   detail   │ sql_name │ symbol  │ artifact_id │ connection_scope │
    │ boolean │    varchar    │ varchar │ varchar │             varchar             │  varchar   │ varchar  │ varchar │   varchar   │     varchar      │
    ├─────────┼───────────────┼─────────┼─────────┼─────────────────────────────────┼────────────┼──────────┼─────────┼─────────────┼──────────────────┤
    │ true    │ tcc_new_state │ state   │ OK      │ new TinyCC build state prepared │ state_id=1 │ NULL     │ NULL    │ NULL        │ database         │
    └─────────┴───────────────┴─────────┴─────────┴─────────────────────────────────┴────────────┴──────────┴─────────┴─────────────┴──────────────────┘
    ┌─────────┬────────────┬─────────┬─────────┬─────────────────┬─────────────────┬──────────┬─────────┬─────────────┬──────────────────┐
    │   ok    │    mode    │  phase  │  code   │     message     │     detail      │ sql_name │ symbol  │ artifact_id │ connection_scope │
    │ boolean │  varchar   │ varchar │ varchar │     varchar     │     varchar     │ varchar  │ varchar │   varchar   │     varchar      │
    ├─────────┼────────────┼─────────┼─────────┼─────────────────┼─────────────────┼──────────┼─────────┼─────────────┼──────────────────┤
    │ true    │ add_source │ state   │ OK      │ source appended │ source appended │ NULL     │ NULL    │ NULL        │ database         │
    └─────────┴────────────┴─────────┴─────────┴─────────────────┴─────────────────┴──────────┴─────────┴─────────────┴──────────────────┘
    ┌─────────┬─────────────┬─────────┬─────────┬────────────────────────┬─────────┬──────────┬─────────┬─────────────┬──────────────────┐
    │   ok    │    mode     │  phase  │  code   │        message         │ detail  │ sql_name │ symbol  │ artifact_id │ connection_scope │
    │ boolean │   varchar   │ varchar │ varchar │        varchar         │ varchar │ varchar  │ varchar │   varchar   │     varchar      │
    ├─────────┼─────────────┼─────────┼─────────┼────────────────────────┼─────────┼──────────┼─────────┼─────────────┼──────────────────┤
    │ true    │ tinycc_bind │ bind    │ OK      │ symbol binding updated │ times2  │ times2   │ times2  │ NULL        │ connection       │
    └─────────┴─────────────┴─────────┴─────────┴────────────────────────┴─────────┴──────────┴─────────┴─────────────┴──────────────────┘
    ┌─────────┬─────────┬─────────┬─────────┬──────────────────────────────────────────────────┬───────────────────────────────────────────────────┬──────────┬─────────┬────────────────────┬──────────────────┐
    │   ok    │  mode   │  phase  │  code   │                     message                      │                      detail                       │ sql_name │ symbol  │    artifact_id     │ connection_scope │
    │ boolean │ varchar │ varchar │ varchar │                     varchar                      │                      varchar                      │ varchar  │ varchar │      varchar       │     varchar      │
    ├─────────┼─────────┼─────────┼─────────┼──────────────────────────────────────────────────┼───────────────────────────────────────────────────┼──────────┼─────────┼────────────────────┼──────────────────┤
    │ true    │ compile │ load    │ OK      │ compiled and registered SQL function via codegen │ /root/DuckTinyCC/cmake_build/release/tinycc_build │ times2   │ times2  │ times2@ffi_state_1 │ database         │
    └─────────┴─────────┴─────────┴─────────┴──────────────────────────────────────────────────┴───────────────────────────────────────────────────┴──────────┴─────────┴────────────────────┴──────────────────┘
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘

### Simple STRUCT Argument

This example takes one `STRUCT(a BIGINT, b BIGINT)` argument and sums
valid fields.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long struct_sum_demo(ducktinycc_struct_t s){
  const long long *a;
  const long long *b;
  long long out = 0;
  if (!s.field_ptrs || s.field_count < 2) return 0;
  a = (const long long *)ducktinycc_struct_field_ptr(&s, 0);
  b = (const long long *)ducktinycc_struct_field_ptr(&s, 1);
  if (a && ducktinycc_struct_field_is_valid(&s, 0)) out += a[s.offset];
  if (b && ducktinycc_struct_field_is_valid(&s, 1)) out += b[s.offset];
  return out;
}',
  symbol := 'struct_sum_demo',
  sql_name := 'struct_sum_demo',
  return_type := 'i64',
  arg_types := ['struct<a:i64;b:i64>']
);

SELECT struct_sum_demo({'a': 2::BIGINT, 'b': 5::BIGINT}::STRUCT(a BIGINT, b BIGINT)) AS both_set;
SELECT struct_sum_demo({'a': 2::BIGINT, 'b': NULL::BIGINT}::STRUCT(a BIGINT, b BIGINT)) AS one_null;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌──────────┐
    │ both_set │
    │  int64   │
    ├──────────┤
    │        7 │
    └──────────┘
    ┌──────────┐
    │ one_null │
    │  int64   │
    ├──────────┤
    │        2 │
    └──────────┘

### Simple LIST and ARRAY Arguments

This example compiles one function for `BIGINT[]` (`i64[]`) and one for
fixed-size `BIGINT[3]` (`i64[3]`).

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long list_sum_demo(ducktinycc_list_t a){
  const long long *p = (const long long *)a.ptr;
  unsigned long long i;
  long long s = 0;
  if (!a.ptr) return 0;
  for (i = 0; i < a.len; i++) {
    if (ducktinycc_list_is_valid(&a, i)) s += p[i];
  }
  return s;
}',
  symbol := 'list_sum_demo',
  sql_name := 'list_sum_demo',
  return_type := 'i64',
  arg_types := ['i64[]']
);

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'long long array_sum3_demo(ducktinycc_array_t a){
  const long long *p = (const long long *)a.ptr;
  unsigned long long i;
  long long s = 0;
  if (!a.ptr) return 0;
  for (i = 0; i < a.len; i++) {
    if (ducktinycc_array_is_valid(&a, i)) s += p[i];
  }
  return s;
}',
  symbol := 'array_sum3_demo',
  sql_name := 'array_sum3_demo',
  return_type := 'i64',
  arg_types := ['i64[3]']
);

SELECT list_sum_demo([1, NULL, 3]::BIGINT[]) AS list_sum;
SELECT array_sum3_demo([1, NULL, 3]::BIGINT[3]) AS array_sum;
SQL
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌──────────┐
    │ list_sum │
    │  int64   │
    ├──────────┤
    │        4 │
    └──────────┘
    ┌───────────┐
    │ array_sum │
    │   int64   │
    ├───────────┤
    │         4 │
    └───────────┘

### Inspect Runtime Paths and Library Resolution

This example shows where TinyCC looks for assets and how a library probe
resolves candidates.

``` bash
duckdb -unsigned <<'SQL'
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT kind, key, value, exists
FROM tcc_system_paths();

SELECT kind, key, value, exists, detail
FROM tcc_library_probe(library := 'libtcc1.a');
SQL
```

    ┌──────────────┬──────────────┬───────────────────────────────────────────────────────────────────┬─────────┐
    │     kind     │     key      │                               value                               │ exists  │
    │   varchar    │   varchar    │                              varchar                              │ boolean │
    ├──────────────┼──────────────┼───────────────────────────────────────────────────────────────────┼─────────┤
    │ runtime      │ runtime_path │ /root/DuckTinyCC/cmake_build/release/tinycc_build                 │ true    │
    │ include_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/include         │ false   │
    │ include_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib/tcc/include │ false   │
    │ library_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build                 │ true    │
    │ library_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib             │ true    │
    │ library_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib/tcc         │ false   │
    │ library_path │ path         │ /usr/lib                                                          │ true    │
    │ library_path │ path         │ /usr/lib64                                                        │ true    │
    │ library_path │ path         │ /usr/local/lib                                                    │ true    │
    │ library_path │ path         │ /lib                                                              │ true    │
    │ library_path │ path         │ /lib64                                                            │ true    │
    │ library_path │ path         │ /lib32                                                            │ true    │
    │ library_path │ path         │ /usr/local/lib64                                                  │ false   │
    │ library_path │ path         │ /usr/lib/x86_64-linux-gnu                                         │ true    │
    │ library_path │ path         │ /usr/lib/i386-linux-gnu                                           │ false   │
    │ library_path │ path         │ /lib/x86_64-linux-gnu                                             │ true    │
    │ library_path │ path         │ /lib32/x86_64-linux-gnu                                           │ false   │
    │ library_path │ path         │ /usr/lib/x86_64-linux-musl                                        │ false   │
    │ library_path │ path         │ /usr/lib/i386-linux-musl                                          │ false   │
    │ library_path │ path         │ /lib/x86_64-linux-musl                                            │ false   │
    │ library_path │ path         │ /lib32/x86_64-linux-musl                                          │ false   │
    │ library_path │ path         │ /usr/lib/amd64-linux-gnu                                          │ false   │
    │ library_path │ path         │ /usr/lib/aarch64-linux-gnu                                        │ false   │
    │ library_path │ path         │ /usr/lib/R/lib                                                    │ true    │
    │ library_path │ path         │ /usr/lib/jvm/default-java/lib/server                              │ true    │
    ├──────────────┴──────────────┴───────────────────────────────────────────────────────────────────┴─────────┤
    │ 25 rows                                                                                         4 columns │
    └───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    ┌─────────────┬──────────────┬─────────────────────────────────────────────────────────────┬─────────┬──────────────────────────────────┐
    │    kind     │     key      │                            value                            │ exists  │              detail              │
    │   varchar   │   varchar    │                           varchar                           │ boolean │             varchar              │
    ├─────────────┼──────────────┼─────────────────────────────────────────────────────────────┼─────────┼──────────────────────────────────┤
    │ input       │ library      │ libtcc1.a                                                   │ false   │ library probe request            │
    │ runtime     │ runtime_path │ /root/DuckTinyCC/cmake_build/release/tinycc_build           │ true    │ effective runtime path           │
    │ search_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build           │ true    │ searched path                    │
    │ search_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib       │ true    │ searched path                    │
    │ search_path │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/lib/tcc   │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib                                                    │ true    │ searched path                    │
    │ search_path │ path         │ /usr/lib64                                                  │ true    │ searched path                    │
    │ search_path │ path         │ /usr/local/lib                                              │ true    │ searched path                    │
    │ search_path │ path         │ /lib                                                        │ true    │ searched path                    │
    │ search_path │ path         │ /lib64                                                      │ true    │ searched path                    │
    │ search_path │ path         │ /lib32                                                      │ true    │ searched path                    │
    │ search_path │ path         │ /usr/local/lib64                                            │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/x86_64-linux-gnu                                   │ true    │ searched path                    │
    │ search_path │ path         │ /usr/lib/i386-linux-gnu                                     │ false   │ searched path                    │
    │ search_path │ path         │ /lib/x86_64-linux-gnu                                       │ true    │ searched path                    │
    │ search_path │ path         │ /lib32/x86_64-linux-gnu                                     │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/x86_64-linux-musl                                  │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/i386-linux-musl                                    │ false   │ searched path                    │
    │ search_path │ path         │ /lib/x86_64-linux-musl                                      │ false   │ searched path                    │
    │ search_path │ path         │ /lib32/x86_64-linux-musl                                    │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/amd64-linux-gnu                                    │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/aarch64-linux-gnu                                  │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/R/lib                                              │ true    │ searched path                    │
    │ search_path │ path         │ /usr/lib/jvm/default-java/lib/server                        │ true    │ searched path                    │
    │ candidate   │ libtcc1.a    │ /root/DuckTinyCC/cmake_build/release/tinycc_build/libtcc1.a │ true    │ resolved                         │
    │ resolved    │ path         │ /root/DuckTinyCC/cmake_build/release/tinycc_build/libtcc1.a │ true    │ resolved library path            │
    │ resolved    │ link_name    │ tcc1                                                        │ true    │ normalized tcc_add_library value │
    ├─────────────┴──────────────┴─────────────────────────────────────────────────────────────┴─────────┴──────────────────────────────────┤
    │ 27 rows                                                                                                                     5 columns │
    └───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

### Embedded R Demo (Unix-like)

The embedded R demo is still part of the repository. We keep it as an
advanced interop example rather than a default workflow. The full source
is inlined below and compiled through `tcc_module(...)`.

``` bash
export R_HOME="${R_HOME:-$(R RHOME)}"
R_INCLUDE_FLAGS="$(R CMD config --cppflags | tr ' ' '\n' | grep '^-I' | tr '\n' ' ')"
R_LIBDIR_FLAGS="$(R CMD config --ldflags | tr ' ' '\n' | grep '^-L' | tr '\n' ' ')"
cat > /tmp/ducktinycc_embed_r_udf.c <<'C'
#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>

#include <stdio.h>

const char *r_hello_from_embedded(void) {
    static int r_initialized = 0;
    static char out[256];

    if (!r_initialized) {
        char *argv[] = {
            (char *)"ducktinycc-embedded-r",
            (char *)"--silent",
            (char *)"--no-save"
        };
        Rf_initEmbeddedR((int)(sizeof(argv) / sizeof(argv[0])), argv);
        r_initialized = 1;
    }

    ParseStatus status;
    SEXP cmd = PROTECT(Rf_mkString("paste('hello from embedded R', getRversion())"));
    SEXP expr = PROTECT(R_ParseVector(cmd, -1, &status, R_NilValue));

    if (status != PARSE_OK || XLENGTH(expr) < 1) {
        UNPROTECT(2);
        snprintf(out, sizeof(out), "%s", "R parse error");
        return out;
    }

    SEXP ans = PROTECT(Rf_eval(VECTOR_ELT(expr, 0), R_GlobalEnv));
    if (TYPEOF(ans) == STRSXP && XLENGTH(ans) > 0 && STRING_ELT(ans, 0) != NA_STRING) {
        snprintf(out, sizeof(out), "%s", CHAR(STRING_ELT(ans, 0)));
    } else {
        snprintf(out, sizeof(out), "%s", "R eval returned non-string");
    }

    UNPROTECT(3);
    return out;
}
C
SOURCE_SQL="$(sed "s/'/''/g" /tmp/ducktinycc_embed_r_udf.c)"
duckdb -unsigned <<SQL
LOAD 'build/release/ducktinycc.duckdb_extension';
PRAGMA threads=1;

-- Stage embedded-R build inputs and source.
SELECT ok, mode, code
FROM tcc_module(
  mode := 'tcc_new_state'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_include',
  include_path := 'third_party/tinycc/include'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_sysinclude',
  sysinclude_path := 'third_party/tinycc/include'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_define',
  define_name := 'R_LEGACY_RCOMPLEX',
  define_value := '1'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_option',
  option := '${R_INCLUDE_FLAGS} ${R_LIBDIR_FLAGS}'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_library',
  library := 'R'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_source',
  source := '${SOURCE_SQL}'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'tinycc_bind',
  symbol := 'r_hello_from_embedded',
  sql_name := 'r_hello_from_embedded'
);
SELECT ok, mode, code
FROM tcc_module(
  mode := 'compile',
  return_type := 'varchar',
  arg_types := []
);
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

## What Remains Before 1.0.0

We have broad nested-type coverage today, including `UNION`, but we
still have focused cleanup work before API freeze. The remaining work is
to harden `UNION` behavior in deeper nested and mixed row/batch edge
cases, continue simplifying and deduplicating internal nested-type
marshalling paths (`LIST/ARRAY/STRUCT/MAP/UNION`), and expose a safer
explicit set of DuckDB functions as C-callable symbols for generated
modules.

## Notes

Generated and helper functions are SQL scalar UDFs; only
`tcc_module(...)`, `tcc_system_paths(...)`, and `tcc_library_probe(...)`
are table functions. For library linking, we can pass short names (`m`,
`z`), explicit filenames (`libfoo.so`, `foo.dll`, `.a`, `.lib`), or
path-like values. Pointer helpers are low-level interop tools; for most
workflows, handle-based access is safer than raw `tcc_dataptr`.
