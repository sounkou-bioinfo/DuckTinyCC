# SQL reference

DuckTinyCC exposes three table functions and a set of scalar memory helpers.
Functions compiled by `tcc_module(...)` are registered as ordinary DuckDB
scalar UDFs.

## Table functions

| Function | Purpose |
|---|---|
| `tcc_module(...)` | Configure a session, stage build inputs, compile/register UDFs, and generate C helpers. |
| `tcc_system_paths(...)` | Report effective embedded runtime, include, and library paths. |
| `tcc_library_probe(...)` | Show search paths and candidate files for a library request. |

`tcc_module(...)` returns one diagnostics row:

```text
ok, mode, phase, code, message, detail,
sql_name, symbol, artifact_id, connection_scope
```

Use `ok` and `code` for control flow; retain `message` and `detail` for
operational diagnostics.

## `tcc_module`

### Session and configuration

| Mode | Effect |
|---|---|
| `config_get` | Return current session configuration. This is the default mode. |
| `config_set` | Set a supported configuration value. |
| `config_reset` | Reset staged configuration and runtime-path state. |
| `list` | List staged build inputs. |
| `tcc_new_state` | Clear staged inputs and increment the logical state ID. It does **not** allocate a `TCCState`. |

### Build staging

| Mode | Stages |
|---|---|
| `add_include` | User include search path. |
| `add_sysinclude` | System include search path. |
| `add_library_path` | Library search directory. |
| `add_library` | Bare name, filename, or path-like library request. |
| `add_option` | TinyCC compiler option. |
| `add_define` | Preprocessor definition. |
| `add_header` | In-memory header name and source. |
| `add_source` | C translation-unit source. |
| `add_symbol` | C identifier and raw host pointer. |
| `tinycc_bind` | Symbol, SQL name, stability, and signature metadata for a later `compile`. |

### Compilation and code generation

| Mode | Effect |
|---|---|
| `quick_compile` | Compile one supplied C source and register one scalar UDF. |
| `compile` | Compile the staged sources and bindings. |
| `codegen_preview` | Validate metadata and generate wrapper code without loading a module. |
| `c_struct` | Generate/register allocation, metadata, field, and array-field helpers. |
| `c_union` | Generate/register union helpers. |
| `c_bitfield` | Generate/register bitfield helpers. |
| `c_enum` | Generate/register enum constant helpers. |

Common compile inputs are `source`, `symbol`, `sql_name`, `return_type`,
`arg_types`, `wrapper_mode`, `stability`, and `library`. Named table-function
arguments must be constants.

`wrapper_mode` accepts:

- `row` — default typed scalar bridge;
- `chunk_scalar_loop` — a generated loop over one DuckDB data chunk. It is not
  an Arrow or whole-table batch ABI.

`stability` accepts `consistent` or `volatile`. Use `volatile` for random
values, clocks, counters, allocation, I/O, callbacks, or reads from mutable
external memory.

## Signature grammar

Scalar tokens:

```text
void  bool
 i8   u8   i16  u16  i32  u32  i64  u64
f32   f64  ptr  varchar  blob  uuid
date  time  timestamp  interval  decimal
```

Recursive forms:

```text
list<T>                    T[]
T[N]
struct<name:T;name:T>
map<K;V>
union<name:T;name:T>
```

Forms may nest recursively, up to the parser's fixed depth limit. `decimal`
uses `ducktinycc_decimal_t`, a signed 128-bit scaled value plus width and scale
metadata.

Composite inputs are borrowed descriptor views. Their C layouts and offset
rules are documented under [descriptor views](internals.html#descriptor-views).

## Libraries and symbols

`library` and `add_library` accept:

- bare names such as `m`, `z`, or `c`;
- platform filenames such as `libfoo.so`, `libfoo.dylib`, `foo.dll`, `.a`, or
  `.lib` files;
- relative or absolute path-like values.

DuckTinyCC compiles generated modules with `-nostdlib`. A header or `extern`
declaration provides C types, not a definition at relocation time. Probe the
effective candidates before relying on a platform library:

```sql
SELECT * FROM tcc_library_probe(library := 'm');
```

`add_symbol` injects a name/pointer pair after DuckTinyCC's host symbols and
before compilation. The name must be a C identifier and the pointer is a raw
`UBIGINT` address. Prefer managed handles; injected addresses are process-local
and valid only while their owner remains alive.

## Pointer and helper API

Managed pointer scalars:

```text
tcc_alloc          tcc_free_ptr       tcc_ptr_size
tcc_dataptr        tcc_ptr_add
tcc_read_*         tcc_write_*
tcc_read_bytes     tcc_write_bytes
```

`tcc_alloc()` returns a managed handle. `tcc_dataptr()` deliberately discards
that ownership information and returns a raw address for C interop.

Generated `c_struct`, `c_union`, and `c_bitfield` helpers follow their supplied
SQL prefix. Typical functions include `{prefix}_new`, `{prefix}_free`,
`{prefix}_sizeof`, `{prefix}_alignof`, field getters/setters, offsets, addresses,
and fixed-array element accessors. `c_enum` registers prefixed constant getters.
Allocation and mutation helpers are volatile; pure metadata helpers are
consistent.
