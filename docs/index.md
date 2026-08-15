<div class="hero">
  <p class="eyebrow">DuckDB C extension · development 0.2.0.9000</p>
  <h1>Compile C. Call it from SQL.</h1>
  <p class="lede">DuckTinyCC embeds TinyCC, generates typed DuckDB bridges, and keeps relocated C modules alive in process—without producing a shared library.</p>
  <p class="hero-actions">
    <a class="button primary" href="#quick-start">Quick start</a>
    <a class="button" href="reference.html">SQL reference</a>
  </p>
</div>

<div class="feature-grid">
  <a class="feature" href="reference.html#signature-grammar">
    <strong>Recursive types</strong>
    <span>Scalars, LIST, ARRAY, STRUCT, MAP, UNION, DECIMAL, and temporal values.</span>
  </a>
  <a class="feature" href="internals.html#artifact-lifetime">
    <strong>In-memory artifacts</strong>
    <span>Compile, relocate, register, and retain one explicitly owned TinyCC state.</span>
  </a>
  <a class="feature" href="development.html#fuzzing">
    <strong>Native hardening</strong>
    <span>Tracked fuzz seeds, deterministic SQL mutation, ASan, UBSan, and subprocess probes.</span>
  </a>
</div>

## Quick start

Build locally and start DuckDB with unsigned extensions enabled:

```sh
git clone --recurse-submodules https://github.com/sounkou-bioinfo/DuckTinyCC.git
cd DuckTinyCC
make configure release
duckdb -unsigned
```

Load the extension and compile a scalar function:

```sql
LOAD 'build/release/ducktinycc.duckdb_extension';

SELECT ok, code
FROM tcc_module(
  mode := 'quick_compile',
  source := 'double c_square(double x) { return x * x; }',
  symbol := 'c_square',
  sql_name := 'c_square',
  return_type := 'f64',
  arg_types := ['f64']
);

SELECT c_square(12.0);
```

`quick_compile` generates the wrapper, invokes the embedded compiler, relocates
the module in memory, and registers `c_square(DOUBLE) → DOUBLE` as a DuckDB
scalar UDF.

## Choose a path

| Goal | Start here |
|---|---|
| Compile or stage a C function | [Control plane and modes](reference.html#tcc_module) |
| Express nested SQL/C types | [Signature grammar](reference.html#signature-grammar) |
| Link a system or vendored library | [Libraries and symbols](reference.html#libraries-and-symbols) |
| Use pointers or generated C helpers | [Pointer and helper API](reference.html#pointer-and-helper-api) |
| Understand ownership and relocation | [Internals](internals.html) |
| Run tests, fuzzers, or sanitizers | [Development](development.html) |

## Support boundary

DuckTinyCC executes generated code as **trusted native code inside DuckDB**. It
validates bridge shapes and bounds; it does not sandbox C, foreign pointers,
explicitly linked process APIs, inline assembly, or syscalls.

Native runtime compilation is the supported path. Windows community targets
and DuckDB-Wasm runtime execution remain excluded until their complete
compile-and-call paths meet the native release gates. A green stub or static
library build is not runtime support.

<div class="callout">
<strong>Embedded runtime.</strong> TinyCC headers and <code>libtcc1.a</code> are content-addressed assets inside the extension. No separate TinyCC installation is required after deployment.
</div>
