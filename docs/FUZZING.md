---
layout: page
title: Fuzzing and sanitizers
---

# Fuzzing and sanitizers

DuckTinyCC borrows two complementary disciplines from DuckNNG and DuckHTS:
small native trust-boundary harnesses with tracked corpora, and replayable SQL
mutation against the complete sanitized extension.

## Native campaign

`test/fuzz/ducktinycc_fuzz.c` exercises extension-owned grammar and wrapper
assembly without loading generated code. Accepted descriptors are traversed and
reparsed. ASan and UBSan campaigns use the same tracked seeds and dictionary but
write mutations and failures only to temporary directories.

```sh
make fuzz
make fuzz FUZZ_RUNS=1000000 FUZZ_SEED=17
```

## SQL boundary campaign

`test/fuzz/generate_sql_fuzz.c` generates deterministic SQL; no Python driver is
used. One DuckDB CLI process receives malformed signatures, malformed C,
known-valid compile/call cases, and randomized composite properties. The final
marker proves the connection survived the entire campaign.

```sh
make fuzz-sql
DUCKTINYCC_SQL_FUZZ_SEED=17 DUCKTINYCC_SQL_FUZZ_TRIALS=10000 make fuzz-sql
```

Failures are saved under `.fuzz/artifacts/` as directly replayable SQL plus
stdout/stderr.

## Full-extension sanitizers

```sh
make test-sanitizers
```

Both the extension and vendored TinyCC static objects receive sanitizer flags.
The SQL campaign then runs with the relevant runtime preloaded.

## Failure promotion

1. Reproduce with the printed seed or saved SQL.
2. Minimize without changing the violated property.
3. Add a named corpus seed.
4. Add a SQLLogicTest when the failure is semantic.
5. Record user-visible bridge, parser, or runtime changes in `NEWS.md`.

See [`test/fuzz/README.md`](https://github.com/sounkou-bioinfo/DuckTinyCC/tree/main/test/fuzz)
for harness details and overrides.
