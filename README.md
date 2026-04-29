
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

``` sql
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'const char *hello_from_c(void){ return "hello from C"; }',
  symbol := 'hello_from_c',
  sql_name := 'hello_from_c',
  return_type := 'varchar',
  arg_types := []
);

-- Call the registered C UDF
SELECT hello_from_c() AS msg;
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌──────────────┐
    │     msg      │
    │   varchar    │
    ├──────────────┤
    │ hello from C │
    └──────────────┘

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
can be `row` (default) or `chunk_scalar_loop`.

`chunk_scalar_loop` is intentionally named for what it is: DuckDB invokes the
extension on a data chunk, DuckTinyCC exposes chunk-local column arrays to the
generated wrapper, and that wrapper loops over rows calling the target C scalar
function. It is not an Arrow or whole-table batch ABI.

Scalar UDF stability defaults to `stability := 'consistent'`. Use
`stability := 'volatile'` for functions that must be re-run for every row
(e.g. RNGs, counters, clocks, allocation, I/O, callbacks, or reads from
mutable external memory). `tinycc_bind` can stage stability for a later
`compile`; an explicit `stability` on `compile`, `quick_compile`, or
`codegen_preview` overrides the staged value. Generated C helper modes use
explicit per-helper stability: pure metadata helpers (`sizeof`, `alignof`,
field offsets, enum constants) are consistent; allocation, free, setter,
and mutable-memory getter helpers are volatile.

`decimal` maps to `ducktinycc_decimal_t`, a 128-bit scaled integer
carrying `width` and `scale` metadata. SQL `DECIMAL(18,3)` values are
passed through the bridge and round-tripped faithfully.

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

Registering the same SQL name twice within the same session is rejected
with `E_INIT_FAILED`. Use `tcc_new_state` to reset staged state before
re-registering.

### Embedded runtime

`libtcc1.a` and the TinyCC include headers (`stdarg.h`, `stddef.h`,
`tccdefs.h`, etc.) are baked directly into the extension binary as byte
arrays at build time by `cmake/gen_embedded_runtime.cmake`. On the first
`compile` or `quick_compile` call, `tcc_ensure_embedded_runtime()`
extracts them to a content-hash-keyed temp directory
(e.g. `/tmp/ducktinycc_f4441fa0/`). Subsequent calls within the same
process reuse that directory without re-extracting. This means the
extension is fully self-contained: no separate TinyCC installation or
runtime path configuration is needed after deployment. The
`tcc_system_paths()` table function shows where the runtime was placed.

### Default libc linking policy

DuckTinyCC compiles generated modules with TinyCC’s `-nostdlib` option by
default. This keeps common UDFs self-contained and avoids requiring
system C development files just to compile pure arithmetic or
DuckTinyCC-wrapper code. As a result, ordinary libc symbols are **not**
linked implicitly.

If your C code calls libc functions that DuckTinyCC has not injected as
host symbols, explicitly link libc with `library := 'c'` on `compile` or
`quick_compile`, or stage the same setting with `mode := 'add_library',
library := 'c'`. Including a header or writing an `extern` declaration
only declares the function for C type checking; it does not resolve the
symbol at TinyCC relocation time. Use `tcc_library_probe(library := 'c')`
to check whether the platform libc import/library is discoverable in your
environment.

``` sql
SELECT ok, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'extern int puts(const char *); int hello(void){ return puts("hello"); }',
  symbol := 'hello',
  sql_name := 'hello_from_libc',
  return_type := 'i32',
  arg_types := [],
  library := 'c'
);
```

Be careful with process-control APIs such as `exit`, `_Exit`, `abort`,
`setjmp`, and `longjmp`: when explicitly resolved, they execute inside
the DuckDB process. DuckTinyCC does not currently sandbox or catch native
control-flow escapes from generated UDFs. Generated UDFs are trusted
in-process native code; if you explicitly link libc, inject process-control
symbols, or use inline assembly/syscalls, you are responsible for keeping
that code inside the normal function-return contract.

## Build and Test during development

``` sh
make configure
make debug
make release

make test_debug
make test_release

# Verify the embedded runtime (hides the build-dir and runs the full test suite)
make test_embedded_debug
make test_embedded_release
```

## Examples

### Compile In One Call

This example uses one-shot compile with `libm`, then calls the generated
function.

``` sql
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

``` sql
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
```

    ┌─────────┬───────────────┬─────────┬─────────┬─────────────────────────────────┬────────────┬──────────┬─────────┬─────────────┬──────────────────┐
    │   ok    │     mode      │  phase  │  code   │             message             │   detail   │ sql_name │ symbol  │ artifact_id │ connection_scope │
    │ boolean │    varchar    │ varchar │ varchar │             varchar             │  varchar   │ varchar  │ varchar │   varchar   │     varchar      │
    ├─────────┼───────────────┼─────────┼─────────┼─────────────────────────────────┼────────────┼──────────┼─────────┼─────────────┼──────────────────┤
    │ true    │ tcc_new_state │ state   │ OK      │ new TinyCC build state prepared │ state_id=1 │ NULL     │ NULL    │ NULL        │ database         │
    └─────────┴───────────────┴─────────┴─────────┴─────────────────────────────────┴────────────┴──────────┴─────────┴─────────────┴──────────────────┘
    ┌─────────┬────────────┬─────────┬─────────┬─────────────────┬────────────────────────────────────────────┬──────────┬─────────┬─────────────┬──────────────────┐
    │   ok    │    mode    │  phase  │  code   │     message     │                   detail                   │ sql_name │ symbol  │ artifact_id │ connection_scope │
    │ boolean │  varchar   │ varchar │ varchar │     varchar     │                  varchar                   │ varchar  │ varchar │   varchar   │     varchar      │
    ├─────────┼────────────┼─────────┼─────────┼─────────────────┼────────────────────────────────────────────┼──────────┼─────────┼─────────────┼──────────────────┤
    │ true    │ add_source │ state   │ OK      │ source appended │ long long times2(long long x){return x*2;} │ NULL     │ NULL    │ NULL        │ database         │
    └─────────┴────────────┴─────────┴─────────┴─────────────────┴────────────────────────────────────────────┴──────────┴─────────┴─────────────┴──────────────────┘
    ┌─────────┬─────────────┬─────────┬─────────┬────────────────────────┬────────────┬──────────┬─────────┬─────────────┬──────────────────┐
    │   ok    │    mode     │  phase  │  code   │        message         │   detail   │ sql_name │ symbol  │ artifact_id │ connection_scope │
    │ boolean │   varchar   │ varchar │ varchar │        varchar         │  varchar   │ varchar  │ varchar │   varchar   │     varchar      │
    ├─────────┼─────────────┼─────────┼─────────┼────────────────────────┼────────────┼──────────┼─────────┼─────────────┼──────────────────┤
    │ true    │ tinycc_bind │ bind    │ OK      │ symbol binding updated │ consistent │ times2   │ times2  │ NULL        │ connection       │
    └─────────┴─────────────┴─────────┴─────────┴────────────────────────┴────────────┴──────────┴─────────┴─────────────┴──────────────────┘
    ┌─────────┬─────────┬─────────┬─────────┬──────────────────────────────────────────────────┬──────────────────────────┬──────────┬─────────┬────────────────────┬──────────────────┐
    │   ok    │  mode   │  phase  │  code   │                     message                      │          detail          │ sql_name │ symbol  │    artifact_id     │ connection_scope │
    │ boolean │ varchar │ varchar │ varchar │                     varchar                      │         varchar          │ varchar  │ varchar │      varchar       │     varchar      │
    ├─────────┼─────────┼─────────┼─────────┼──────────────────────────────────────────────────┼──────────────────────────┼──────────┼─────────┼────────────────────┼──────────────────┤
    │ true    │ compile │ load    │ OK      │ compiled and registered SQL function via codegen │ /tmp/ducktinycc_f4441fa0 │ times2   │ times2  │ times2@ffi_state_1 │ database         │
    └─────────┴─────────┴─────────┴─────────┴──────────────────────────────────────────────────┴──────────────────────────┴──────────┴─────────┴────────────────────┴──────────────────┘
    ┌───────┐
    │ value │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘

### Simple STRUCT Argument

This example takes one `STRUCT(a BIGINT, b BIGINT)` argument and sums
valid fields.

``` sql
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

``` sql
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

### DECIMAL Round-Trip

This example echoes a `DECIMAL(18,3)` value through a C function. The
bridge represents decimals as a `ducktinycc_decimal_t` struct (a 128-bit
scaled integer with width and scale metadata).

``` sql
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'ducktinycc_decimal_t decimal_echo(ducktinycc_decimal_t d){ return d; }',
  symbol := 'decimal_echo',
  sql_name := 'decimal_echo',
  return_type := 'decimal',
  arg_types := ['decimal']
);

SELECT decimal_echo(12.345::DECIMAL(18,3)) AS value;
```

    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌───────────────┐
    │     value     │
    │ decimal(18,3) │
    ├───────────────┤
    │        12.345 │
    └───────────────┘

### Inspect Runtime Paths and Library Resolution

This example shows where TinyCC looks for assets and how a library probe
resolves candidates.

``` sql
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT kind, key, value, exists
FROM tcc_system_paths();

SELECT kind, key, value, exists, detail
FROM tcc_library_probe(library := 'libtcc1.a');
```

    ┌──────────────┬──────────────┬──────────────────────────────────────────┬─────────┐
    │     kind     │     key      │                  value                   │ exists  │
    │   varchar    │   varchar    │                 varchar                  │ boolean │
    ├──────────────┼──────────────┼──────────────────────────────────────────┼─────────┤
    │ runtime      │ runtime_path │ /tmp/ducktinycc_f4441fa0                 │ true    │
    │ include_path │ path         │ /tmp/ducktinycc_f4441fa0/include         │ true    │
    │ include_path │ path         │ /tmp/ducktinycc_f4441fa0/lib/tcc/include │ false   │
    │ library_path │ path         │ /tmp/ducktinycc_f4441fa0                 │ true    │
    │ library_path │ path         │ /tmp/ducktinycc_f4441fa0/lib             │ false   │
    │ library_path │ path         │ /tmp/ducktinycc_f4441fa0/lib/tcc         │ false   │
    │ library_path │ path         │ /usr/lib                                 │ true    │
    │ library_path │ path         │ /usr/lib64                               │ true    │
    │ library_path │ path         │ /usr/local/lib                           │ true    │
    │ library_path │ path         │ /lib                                     │ true    │
    │ library_path │ path         │ /lib64                                   │ true    │
    │ library_path │ path         │ /lib32                                   │ true    │
    │ library_path │ path         │ /usr/local/lib64                         │ false   │
    │ library_path │ path         │ /usr/lib/x86_64-linux-gnu                │ true    │
    │ library_path │ path         │ /usr/lib/i386-linux-gnu                  │ false   │
    │ library_path │ path         │ /lib/x86_64-linux-gnu                    │ true    │
    │ library_path │ path         │ /lib32/x86_64-linux-gnu                  │ false   │
    │ library_path │ path         │ /usr/lib/x86_64-linux-musl               │ false   │
    │ library_path │ path         │ /usr/lib/i386-linux-musl                 │ false   │
    │ library_path │ path         │ /lib/x86_64-linux-musl                   │ false   │
    │ library_path │ path         │ /lib32/x86_64-linux-musl                 │ false   │
    │ library_path │ path         │ /usr/lib/amd64-linux-gnu                 │ false   │
    │ library_path │ path         │ /usr/lib/aarch64-linux-gnu               │ false   │
    │ library_path │ path         │ /usr/lib/R/lib                           │ true    │
    │ library_path │ path         │ /usr/lib/jvm/default-java/lib/server     │ true    │
    └──────────────┴──────────────┴──────────────────────────────────────────┴─────────┘
      25 rows                                                                4 columns
    ┌─────────────┬──────────────┬──────────────────────────────────────┬─────────┬──────────────────────────────────┐
    │    kind     │     key      │                value                 │ exists  │              detail              │
    │   varchar   │   varchar    │               varchar                │ boolean │             varchar              │
    ├─────────────┼──────────────┼──────────────────────────────────────┼─────────┼──────────────────────────────────┤
    │ input       │ library      │ libtcc1.a                            │ false   │ library probe request            │
    │ runtime     │ runtime_path │ /tmp/ducktinycc_f4441fa0             │ true    │ effective runtime path           │
    │ search_path │ path         │ /tmp/ducktinycc_f4441fa0             │ true    │ searched path                    │
    │ search_path │ path         │ /tmp/ducktinycc_f4441fa0/lib         │ false   │ searched path                    │
    │ search_path │ path         │ /tmp/ducktinycc_f4441fa0/lib/tcc     │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib                             │ true    │ searched path                    │
    │ search_path │ path         │ /usr/lib64                           │ true    │ searched path                    │
    │ search_path │ path         │ /usr/local/lib                       │ true    │ searched path                    │
    │ search_path │ path         │ /lib                                 │ true    │ searched path                    │
    │ search_path │ path         │ /lib64                               │ true    │ searched path                    │
    │ search_path │ path         │ /lib32                               │ true    │ searched path                    │
    │ search_path │ path         │ /usr/local/lib64                     │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/x86_64-linux-gnu            │ true    │ searched path                    │
    │ search_path │ path         │ /usr/lib/i386-linux-gnu              │ false   │ searched path                    │
    │ search_path │ path         │ /lib/x86_64-linux-gnu                │ true    │ searched path                    │
    │ search_path │ path         │ /lib32/x86_64-linux-gnu              │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/x86_64-linux-musl           │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/i386-linux-musl             │ false   │ searched path                    │
    │ search_path │ path         │ /lib/x86_64-linux-musl               │ false   │ searched path                    │
    │ search_path │ path         │ /lib32/x86_64-linux-musl             │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/amd64-linux-gnu             │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/aarch64-linux-gnu           │ false   │ searched path                    │
    │ search_path │ path         │ /usr/lib/R/lib                       │ true    │ searched path                    │
    │ search_path │ path         │ /usr/lib/jvm/default-java/lib/server │ true    │ searched path                    │
    │ candidate   │ libtcc1.a    │ /tmp/ducktinycc_f4441fa0/libtcc1.a   │ true    │ resolved                         │
    │ resolved    │ path         │ /tmp/ducktinycc_f4441fa0/libtcc1.a   │ true    │ resolved library path            │
    │ resolved    │ link_name    │ tcc1                                 │ true    │ normalized tcc_add_library value │
    └─────────────┴──────────────┴──────────────────────────────────────┴─────────┴──────────────────────────────────┘
      27 rows                                                                                              5 columns

### Inject Symbols and Pass Pointers

`add_symbol` injects arbitrary name/address pairs into the TCC state
before compilation. Compiled C code can reference these symbols
directly. This is the foundation for passing function pointers, data
addresses, or opaque handles to compiled code.

The simplest pattern uses the address itself as the value (no
dereference):

``` sql
LOAD 'build/release/ducktinycc.duckdb_extension';

-- Stage a symbol: the literal 42 becomes the symbol's address
SELECT ok, mode, code
FROM tcc_module(
  mode := 'add_symbol',
  symbol_name := 'MY_MAGIC',
  symbol_ptr := 42::UBIGINT
);

-- C code reads the address back via extern + cast
SELECT ok, mode, code
FROM tcc_module(
  mode := 'quick_compile',
  source := '
extern char MY_MAGIC[];
long long get_magic(void) {
  return (long long)(unsigned long long)(void *)MY_MAGIC;
}',
  symbol := 'get_magic',
  sql_name := 'get_magic',
  return_type := 'i64',
  arg_types := []
);

SELECT get_magic() AS magic;
```

    ┌─────────┬────────────┬─────────┐
    │   ok    │    mode    │  code   │
    │ boolean │  varchar   │ varchar │
    ├─────────┼────────────┼─────────┤
    │ true    │ add_symbol │ OK      │
    └─────────┴────────────┴─────────┘
    ┌─────────┬───────────────┬─────────┐
    │   ok    │     mode      │  code   │
    │ boolean │    varchar    │ varchar │
    ├─────────┼───────────────┼─────────┤
    │ true    │ quick_compile │ OK      │
    └─────────┴───────────────┴─────────┘
    ┌───────┐
    │ magic │
    │ int64 │
    ├───────┤
    │    42 │
    └───────┘

For a real-world use of `add_symbol` to pass function pointers, see
`demo/r_udf_via_ducktinycc.R`, which injects R C API addresses and
compiles a trampoline that calls back into R from a DuckDB scalar UDF.

For a visual callback demo, `demo/ggplot2_via_ducktinycc.R` registers a
volatile SQL UDF whose TinyCC-compiled C trampoline calls a pinned R
function via `R_tryEval`; the SQL call writes a ggplot2 PNG.

``` sh
Rscript demo/ggplot2_via_ducktinycc.R /tmp/ducktinycc_ggplot2_demo.png
```

To make the plot appear directly in the DuckDB CLI, run the embedded-R
CLI variant. It compiles `demo/ggplot2_cli_udf.c`, calls ggplot2 from a
SQL scalar UDF, and returns an ANSI-colored terminal canvas.

``` sh
./demo/ggplot2_cli_demo.sh build/release/ducktinycc.duckdb_extension
```

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

The main outstanding feature is **DuckDB callbacks** — wrapping DuckDB
functions (built-in or user-defined SQL scalars) as C function pointers
so they can be passed to compiled C code that expects callback
signatures like `double (*fn)(double)`. This is the DuckDB analogue of
Rtinycc’s `tcc_callback()` mechanism, which wraps R functions as
C-callable trampolines.

The infrastructure to support this is already in place:

- **`add_symbol`** injects arbitrary host pointers into TCC state before
  compilation. The R UDF demo (`demo/r_udf_via_ducktinycc.R`) proves the
  pattern end-to-end: R C API function pointers are injected, a C
  trampoline is compiled that calls back into R, and the result is a
  working DuckDB scalar UDF.
- **DuckDB callback** would follow the same pattern: resolve a DuckDB
  function by name, generate a C trampoline that invokes it via the
  DuckDB C API (e.g., `duckdb_create_*`, `duckdb_execute_*`, or internal
  function invocation), and inject the trampoline as a standard C
  function pointer. Compiled user code would receive it as
  `double (*fn)(double)` and call it without knowing the implementation
  is a DuckDB function.

## Notes

Generated and helper functions are SQL scalar UDFs; only
`tcc_module(...)`, `tcc_system_paths(...)`, and `tcc_library_probe(...)`
are table functions. For library linking, we can pass short names (`m`,
`z`, `c`), explicit filenames (`libfoo.so`, `foo.dll`, `.a`, `.lib`), or
path-like values. Because DuckTinyCC uses `-nostdlib` by default, use
`library := 'c'` when generated code needs libc symbols that are not
otherwise injected. Pointer helpers are low-level interop tools; for most
workflows, handle-based access is safer than raw `tcc_dataptr`.
