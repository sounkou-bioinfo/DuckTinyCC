# Development

DuckTinyCC treats native compiler input, recursive bridge metadata, and artifact
lifetime as trust boundaries. Changes are accepted with focused evidence, not a
claim that one broad build happened to pass.

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

The site builder accepts exactly four source pages: `index`, `reference`,
`internals`, and `development`. Add information to the page that owns it rather
than creating another status, roadmap, or implementation-note document.

## Release boundary

Before 1.0, correctness and a small coherent API take priority over compatibility
shims. A 1.0 release requires:

- documented and versioned SQL/helper ABI;
- explicit duplicate, replacement, DROP, unload, and multi-connection lifetime
  behavior;
- deterministic embedded assets and allocator-domain evidence;
- debug/release, embedded, community, fuzz, ASan, UBSan, subprocess-safety, and
  supported-platform CI gates green;
- no unresolved descriptor offset, NULL, ownership, or artifact-lifetime
  ambiguity.

Windows community targets and true DuckDB-Wasm runtime compilation remain
excluded until they compile **and call** generated SQL UDFs under equivalent
gates. Stub/static builds are useful portability checks, not support claims.

Generic DuckDB-to-C callbacks are not a 1.0 promise. DuckDB's stable C API does
not expose arbitrary SQL scalars as context-free C pointers; such a feature
needs a separate contract for binding, context, NULL/errors, reentrancy,
threading, and lifetime.
