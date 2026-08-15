# Development

DuckTinyCC tests native compiler input, recursive bridge metadata, and artifact
lifetime separately. Changes are accepted with focused evidence, not a claim
that one broad build happened to pass.

## Build and test

```sh
make configure
make debug test_debug
make release test_release
make test_embedded_debug
make test_embedded_release
make community_sim
```

The SQLLogicTests are split by behavior and can run independently. Keep new
regressions in the narrowest file that owns the contract.

The embedded-runtime tests hide the normal build directory before running the
SQL suite. This proves runtime compilation uses extracted in-binary assets
rather than an accidental source-tree path.

Process-control and non-local-exit tests run through the dedicated unsafe-UDF
subprocess script. Never run such probes in the main test process.

## Fuzzing

Native parser and code-generation campaign:

```sh
make fuzz
make fuzz FUZZ_RUNS=1000000 FUZZ_SEED=17
```

Deterministic complete-extension SQL campaign:

```sh
make fuzz-sql
DUCKTINYCC_SQL_FUZZ_SEED=17 \
DUCKTINYCC_SQL_FUZZ_TRIALS=10000 \
make fuzz-sql
```

Complete extension plus vendored TinyCC under both sanitizers:

```sh
make test-sanitizers
# or all native/SQL/sanitizer campaigns
make fuzz-all
```

Failures are saved under `.fuzz/artifacts/` as replayable SQL and captured
output. Promotion is mandatory:

1. reproduce with the printed seed or artifact;
2. minimize while preserving the violated property;
3. add a named tracked corpus seed;
4. add a SQLLogicTest for semantic failures;
5. record user-visible parser, bridge, ABI, or runtime changes in `NEWS.md`.

Harness details and overrides live in
[`test/fuzz/README.md`](https://github.com/sounkou-bioinfo/DuckTinyCC/tree/main/test/fuzz).

## Documentation

```sh
make rdm       # README.Rmd → README.md
make site      # docs/*.md → _site/*.html
make site-clean
```

The rendered `README.md` is the landing page; its executable examples remain
part of documentation QC. The site builder also accepts exactly three curated
reference pages: `reference`, `internals`, and `development`. Add information
to the page that owns it rather than creating another status, roadmap, or
implementation-note document.

## Target for 0.3.0

Version `0.3.0` is the callbacks and Rtinycc-parity release. "Parity" means
every transferable Rtinycc capability has a tested DuckTinyCC equivalent;
R-only facilities receive an explicit not-applicable decision.

| Rtinycc capability group | DuckTinyCC 0.3.0 gate |
|---|---|
| State creation; include, library, option, source, header, and symbol inputs | Existing staged SQL API retained and covered independently. |
| Memory compilation, relocation, symbol lifetime, and typed calls | Existing generated-UDF path plus direct symbol/introspection parity where missing. |
| Scalar, pointer, string, aggregate, and recursive FFI types | Existing bridge retained; parity gaps become named regressions. |
| Managed allocation, byte/typed access, STRUCT/UNION/ENUM helpers | Existing SQL helpers plus any missing ownership operations. |
| Synchronous callbacks | Managed DuckDB-scalar callback handle, exact C pointer/signature, validity query, and deterministic close. |
| Worker-thread/async callbacks | Defined scheduling, result, cancellation, and connection-thread rules matching Rtinycc's capability. |
| Symbol listing, direct calls, recompilation, output modes, and CLI workflows | SQL equivalents or an explicit host-specific not-applicable decision. |
| Header introspection and generated bindings | Equivalent parser/code-generation workflow with tracked fixtures. |
| R `SEXP`, R event loop, BLAS discovery, and knitr integration | Not applicable; DuckDB logical types, execution, and documentation QC are the host equivalents. |

Callback tests must cover overload binding, exact signatures, SQL NULLs, errors,
close/invalidation, artifact ownership, recursive invocation, connection use,
and concurrent calls. A raw address without a retained callback owner does not
satisfy the gate.

The release also requires deterministic embedded assets and green debug,
release, embedded, community, fuzz, ASan, UBSan, subprocess-control-flow, and
supported-platform CI. Windows community targets and true DuckDB-Wasm runtime
compilation remain excluded until they compile **and call** generated SQL UDFs;
stub or static builds are portability checks, not support claims.
