# Native property tests

This suite generates valid recursive DuckTinyCC descriptors and arbitrary parser
input without loading DuckDB or executing TinyCC modules. It uses vendored
[`greatest`](../vendor/greatest/LICENSE) for assertions and
[`theft`](../vendor/theft/LICENSE) for deterministic generation and shrinking.

```sh
make prop
make prop-quick
make prop-asan
make prop-ubsan
make prop-sanitize
```

Replay controls:

```sh
make prop PROP_SEED=0x1234 PROP_TRIALS=10000
DUCKTINYCC_PROP_FORK=1 make prop
```

The generated properties require:

- valid recursive `LIST`, `ARRAY`, `STRUCT`, `MAP`, and `UNION` descriptors to
  parse, survive a structural round trip, and reach wrapper-source generation;
- a generated composite with its final delimiter removed to reject cleanly;
- arbitrary byte-derived text to produce deterministic parser and top-level CSV
  splitter results;
- exactly 63 nested list wrappers to parse and 64 to hit the documented depth
  limit.

A failure reports the suite seed, trial seed, and shrunk input. Re-run the suite
seed locally, add the minimized input to `test/fuzz/corpus/`, and add a named SQL
regression when the failure affects the complete extension.

These properties complement rather than replace libFuzzer and the generated SQL
campaign described in [`../fuzz/README.md`](../fuzz/README.md).
