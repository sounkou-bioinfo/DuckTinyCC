# DuckTinyCC fuzzing

DuckTinyCC separately tests extension parser/codegen input handling and complete
TinyCC compile/register/execute behavior.

## Native parser/codegen campaign

`ducktinycc_fuzz.c` includes the same amalgamated extension implementation used
by production builds, installs only `malloc` and `free` in the DuckDB extension
API table, and defines `DUCKTINYCC_WASM_UNSUPPORTED` so no TinyCC module is
linked or executed. Linker section garbage collection retains the reachable
extension-owned surfaces:

- recursive SQL-visible type descriptors;
- top-level CSV splitting;
- C helper field and enum grammars;
- complete signature parsing and cleanup;
- wrapper mode and stability parsing;
- generated wrapper/source assembly.

Accepted recursive descriptors are traversed for shape invariants and reparsed.
The parser caps nesting at 64 levels, so hostile input cannot turn recursive
syntax into an unbounded C stack.

Run bounded ASan and UBSan campaigns with:

```sh
make fuzz-asan
make fuzz-ubsan
make fuzz
```

Useful overrides:

```sh
make fuzz FUZZ_RUNS=1000000 FUZZ_MAX_LEN=65536
make fuzz-asan FUZZ_RUNS=10000 FUZZ_SEED=1234
make fuzz-clean
```

Tracked seeds are copied into a temporary writable corpus. Native failures are
copied from the temporary libFuzzer directory to `.fuzz/artifacts/` before the
runner exits, and CI retains that directory for 14 days. A minimized failure
must be added as a named seed and, when it exposes a semantic invariant, as an
ordinary SQL regression too.

## Shrinking native properties

`test/property/ducktinycc_prop.c` uses vendored `greatest` and `theft` to
generate and shrink valid recursive descriptors as well as arbitrary parser
text. The same properties run normally and under ASan/UBSan:

```sh
make prop
make prop-quick
make prop-sanitize
```

See [`../property/README.md`](../property/README.md) for properties and replay
controls.

## C-generated complete-extension SQL campaign

`generate_sql_fuzz.c` is a deterministic C program that emits a complete SQL
campaign. `scripts/test_sql_fuzz.sh` pipes it into one DuckDB CLI process and
requires a final recovery marker. It covers malformed type/codegen requests,
malformed C compilation, and valid compile-and-call cases. Generated runtime
properties compare row and chunk wrappers with SQL expressions across more than
one DuckDB vector and check NULL/constant inputs plus randomized
LIST/ARRAY/STRUCT/MAP values, empty composites, and nonzero child offsets. It
uses no Python runtime.

```sh
make fuzz-sql
DUCKTINYCC_SQL_FUZZ_SEED=17 DUCKTINYCC_SQL_FUZZ_TRIALS=10000 make fuzz-sql
```

On failure, the replayable SQL, stdout, and stderr are copied to
`.fuzz/artifacts/`. Promote minimized SQL to `test/sql/` before deleting the
artifact.

## Full-extension sanitizers

The complete extension and the vendored TinyCC objects are rebuilt with each
sanitizer before running the SQL campaign:

```sh
make test-sanitized-extension SANITIZER=asan
make test-sanitized-extension SANITIZER=ubsan
make test-sanitizers
```

Fuzz acceptance is not proof that arbitrary generated native code is safe.
DuckTinyCC executes trusted in-process code; subprocess control-flow probes,
SQLLogicTests, embedded-runtime tests, community simulation, compiler-fork CI,
and platform builds remain separate gates.
