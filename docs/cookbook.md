# Cookbook

Worked examples of C scalar UDFs. They assume the extension is loaded:

```sql
INSTALL ducktinycc FROM community;
LOAD ducktinycc;
```

`test/sql/tcc_cookbook.test` runs every example except the zlib one, which
needs zlib installed. The outputs shown are from the current build.

## Notes

Integer columns are `BIGINT`, so declare `i64`. DuckDB does not implicitly cast
`BIGINT` to `UBIGINT` (`u64`).

DuckDB calls a UDF from several threads at once. Allocate returned strings,
blobs, and list or struct payloads with `ducktinycc_result_alloc`, not in a
`static` buffer. In one test, rendering integers into a shared static buffer
under 8 threads returned the wrong string for 11.8 million of 20 million rows.
Result memory belongs to the executing chunk and is freed after DuckDB copies
the results.

Generated modules are compiled with `-nostdlib`. libc and other libraries are
linked only when requested with `library := 'c'`, `'m'`, `'z'`, and so on.
TinyCC's compiler-support routines (`libtcc1.a`) are always linked.

The compiled code runs in the DuckDB process with no isolation. Anyone who can
call `tcc_module` can run arbitrary native code.

## Validate card numbers (Luhn)

Checks card numbers and IMEIs. Spaces and dashes are ignored.

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
_Bool luhn_ok(const char *s) {
  const char *p = s;
  int sum = 0, dbl = 0, digits = 0;
  while (*p) p++;
  while (p > s) {
    char c = *--p;
    int d;
    if (c == '' '' || c == ''-'') continue;
    if (c < ''0'' || c > ''9'') return 0;
    d = c - ''0'';
    if (dbl) { d *= 2; if (d > 9) d -= 9; }
    sum += d;
    dbl = !dbl;
    digits++;
  }
  return digits > 1 && sum % 10 == 0;
}',
  symbol := 'luhn_ok', sql_name := 'luhn_ok',
  return_type := 'bool', arg_types := ['varchar']
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
SELECT n, luhn_ok(n) AS valid
FROM (VALUES ('4111 1111 1111 1111'), ('4111 1111 1111 1112'),
             ('490154203237518'), ('79927398713'), ('12a4')) t(n);
```
```text
┌─────────────────────┬───────┐
│          n          │ valid │
├─────────────────────┼───────┤
│ 4111 1111 1111 1111 │ true  │
│ 4111 1111 1111 1112 │ false │
│ 490154203237518     │ true  │
│ 79927398713         │ true  │
│ 12a4                │ false │
└─────────────────────┴───────┘
```

On 5 million 16-digit strings (Intel i5-13500, 20 threads), `luhn_ok` took
0.17 to 0.23 s. The following SQL macro gives the same results in 0.78 to
0.92 s, using about five times the CPU time:

```sql
CREATE MACRO luhn_sql(n) AS (
  list_sum(list_transform(string_split(reverse(n), ''), lambda c, i:
    CASE WHEN i % 2 = 0
         THEN CASE WHEN c::INT * 2 > 9 THEN c::INT * 2 - 9 ELSE c::INT * 2 END
         ELSE c::INT END)) % 10 = 0);
```

## Geohash

The result string is built at run time, so it is allocated with
`ducktinycc_result_alloc`. Returning `NULL` from C gives SQL `NULL`.

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
const char *geohash(double lat, double lon, int32_t precision) {
  static const char base32[] = "0123456789bcdefghjkmnpqrstuvwxyz";
  double la[2] = { -90.0, 90.0 }, lo[2] = { -180.0, 180.0 };
  int i = 0, bit = 0, ch = 0, even = 1;
  char *out;
  if (precision < 1 || precision > 12) return 0;
  out = ducktinycc_result_alloc((uint64_t)precision + 1);
  if (!out) return 0;
  while (i < precision) {
    double *r = even ? lo : la;
    double v = even ? lon : lat;
    double mid = (r[0] + r[1]) / 2;
    ch <<= 1;
    if (v >= mid) { ch |= 1; r[0] = mid; } else { r[1] = mid; }
    even = !even;
    if (++bit == 5) { out[i++] = base32[ch]; bit = 0; ch = 0; }
  }
  out[i] = 0;
  return out;
}',
  symbol := 'geohash', sql_name := 'geohash',
  return_type := 'varchar', arg_types := ['f64', 'f64', 'i32']
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
SELECT geohash(57.64911, 10.40744, 11) AS jutland,
       geohash(48.8584, 2.2945, 7) AS eiffel_tower,
       geohash(0, 0, 13) AS too_long;
```
```text
┌─────────────┬──────────────┬──────────┐
│   jutland   │ eiffel_tower │ too_long │
├─────────────┼──────────────┼──────────┤
│ u4pruydqqvj │ u09tunq      │ NULL     │
└─────────────┴──────────────┴──────────┘
```

## Near-duplicate text with SimHash

`simhash64` computes a 64-bit fingerprint per string, and SQL compares
fingerprints with `bit_count(xor(...))`. Similar texts differ in few bits.

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
int64_t simhash64(const char *s) {
  int32_t acc[64] = { 0 };
  uint64_t h = 0, out = 0;
  int in_word = 0, b;
  const unsigned char *p = (const unsigned char *)s;
  for (;; p++) {
    unsigned char c = *p;
    int word = (c >= ''a'' && c <= ''z'') || (c >= ''A'' && c <= ''Z'') || (c >= ''0'' && c <= ''9'');
    if (word) {
      if (!in_word) { h = 1469598103934665603ULL; in_word = 1; }
      if (c >= ''A'' && c <= ''Z'') c = (unsigned char)(c + 32);
      h = (h ^ c) * 1099511628211ULL;
    } else if (in_word) {
      for (b = 0; b < 64; b++) acc[b] += (h >> b) & 1 ? 1 : -1;
      in_word = 0;
    }
    if (!c) break;
  }
  for (b = 0; b < 64; b++) if (acc[b] > 0) out |= 1ULL << b;
  return (int64_t)out;
}',
  symbol := 'simhash64', sql_name := 'simhash64',
  return_type := 'i64', arg_types := ['varchar']
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
WITH docs(id, body) AS (VALUES
  (1, 'DuckDB is an in-process analytical database'),
  (2, 'DuckDB is an in process analytical database!'),
  (3, 'duckdb is an in-process ANALYTICAL database system'),
  (4, 'TinyCC compiles C code very quickly')),
h AS (SELECT id, simhash64(body) AS sh FROM docs)
SELECT a.id AS a, b.id AS b, bit_count(xor(a.sh, b.sh)) AS distance
FROM h a JOIN h b ON a.id < b.id
ORDER BY distance, a, b;
```
```text
┌───┬───┬──────────┐
│ a │ b │ distance │
├───┼───┼──────────┤
│ 1 │ 2 │ 0        │
│ 1 │ 3 │ 6        │
│ 2 │ 3 │ 6        │
│ 3 │ 4 │ 26       │
│ 1 │ 4 │ 32       │
│ 2 │ 4 │ 32       │
└───┴───┴──────────┘
```

## Normal CDF and p-values from libm

DuckDB has no `erf` or `erfc`, so this declares libm's `erfc` and links `m`.

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
extern double erfc(double);
double pnorm(double z) { return 0.5 * erfc(-z / 1.4142135623730951); }',
  symbol := 'pnorm', sql_name := 'pnorm',
  return_type := 'f64', arg_types := ['f64'],
  library := 'm'
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
SELECT round(pnorm(1.959964), 6) AS p_975,
       round(2 * (1 - pnorm(abs(-3.2))), 6) AS two_sided_p;
```
```text
┌───────┬─────────────┐
│ p_975 │ two_sided_p │
├───────┼─────────────┤
│ 0.975 │ 0.001374    │
└───────┴─────────────┘
```

## CRC-32 with zlib

Other C libraries are linked the same way. This example needs the zlib
development link (`libz.so` or `libz.dylib`), so check that it resolves first.

```sql
SELECT kind, key, value FROM tcc_library_probe(library := 'z') WHERE kind = 'resolved';
```
```text
┌──────────┬───────────┬───────────────────────────────────┐
│   kind   │    key    │               value               │
├──────────┼───────────┼───────────────────────────────────┤
│ resolved │ path      │ /usr/lib/x86_64-linux-gnu/libz.so │
│ resolved │ link_name │ z                                 │
└──────────┴───────────┴───────────────────────────────────┘
```

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
extern unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len);
int64_t crc32_of(ducktinycc_blob_t b) {
  return (int64_t)crc32(0UL, (const unsigned char *)b.ptr, (unsigned int)b.len);
}',
  symbol := 'crc32_of', sql_name := 'crc32_of',
  return_type := 'i64', arg_types := ['blob'],
  library := 'z'
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
SELECT printf('%08x', crc32_of('The quick brown fox jumps over the lazy dog'::BLOB)) AS crc32;
```
```text
┌──────────┐
│  crc32   │
├──────────┤
│ 414fa339 │
└──────────┘
```

## Decode varints from a BLOB into a LIST

Decodes unsigned LEB128 varints, as used by Protocol Buffers. The list
payload is allocated with `ducktinycc_result_alloc`. A `ducktinycc_list_t`
with `len > 0` and `ptr == NULL` is SQL `NULL`, used here for truncated input.

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
ducktinycc_list_t uvarints(ducktinycc_blob_t b) {
  ducktinycc_list_t out = { 0, 0, 0, 0 };
  const unsigned char *p = (const unsigned char *)b.ptr;
  uint64_t acc = 0, n = 0, i;
  int shift = 0;
  int64_t *vals;
  if (b.len == 0) return out;
  vals = ducktinycc_result_alloc(b.len * sizeof(int64_t));
  if (!vals) { out.len = 1; return out; }
  for (i = 0; i < b.len; i++) {
    if (shift > 63) { out.len = 1; return out; }
    acc |= (uint64_t)(p[i] & 0x7f) << shift;
    if (p[i] & 0x80) { shift += 7; continue; }
    vals[n++] = (int64_t)acc;
    acc = 0;
    shift = 0;
  }
  if (shift) { out.len = 1; return out; }
  out.ptr = vals;
  out.len = n;
  return out;
}',
  symbol := 'uvarints', sql_name := 'uvarints',
  return_type := 'i64[]', arg_types := ['blob']
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
SELECT uvarints(unhex('01ac029601')) AS values,
       uvarints(unhex('ff')) AS truncated,
       uvarints(''::BLOB) AS empty;
```
```text
┌───────────────┬───────────┬───────┐
│    values     │ truncated │ empty │
├───────────────┼───────────┼───────┤
│ [1, 300, 150] │ NULL      │ []    │
└───────────────┴───────────┴───────┘
```

## Parse semantic versions into a STRUCT

Each entry of `field_ptrs` points at the value of one field. A result with
`field_ptrs == NULL` is SQL `NULL`, and its fields are `NULL` as well.

```sql
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '
static const char *read_part(const char *p, int32_t *out) {
  int32_t v = 0;
  if (*p < ''0'' || *p > ''9'') return 0;
  while (*p >= ''0'' && *p <= ''9'') {
    if (v > 214748364) return 0;
    v = v * 10 + (*p++ - ''0'');
  }
  *out = v;
  return p;
}
ducktinycc_struct_t semver(const char *s) {
  ducktinycc_struct_t out = { 0, 0, 0, 0 };
  int32_t *v = ducktinycc_result_alloc(3 * sizeof(int32_t));
  const void **f = ducktinycc_result_alloc(3 * sizeof(void *));
  const char *p = s;
  int i;
  if (!v || !f) return out;
  if (*p == ''v'') p++;
  for (i = 0; i < 3; i++) {
    p = read_part(p, &v[i]);
    if (!p) return out;
    if (i < 2 && *p++ != ''.'') return out;
    f[i] = &v[i];
  }
  if (*p && *p != ''-'' && *p != ''+'') return out;
  out.field_ptrs = f;
  out.field_count = 3;
  return out;
}',
  symbol := 'semver', sql_name := 'semver',
  return_type := 'struct<major:i32;minor:i32;patch:i32>', arg_types := ['varchar']
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
SELECT v, semver(v) AS parsed, semver(v).minor AS minor
FROM (VALUES ('1.4.3'), ('v2.10.0-rc1'), ('1.4'), ('x.y.z')) t(v);
```
```text
┌─────────────┬───────────────────────────────────────┬───────┐
│      v      │                parsed                 │ minor │
├─────────────┼───────────────────────────────────────┼───────┤
│ 1.4.3       │ {'major': 1, 'minor': 4, 'patch': 3}  │ 4     │
│ v2.10.0-rc1 │ {'major': 2, 'minor': 10, 'patch': 0} │ 10    │
│ 1.4         │ NULL                                  │ NULL  │
│ x.y.z       │ NULL                                  │ NULL  │
└─────────────┴───────────────────────────────────────┴───────┘
```

## Counters in memory allocated from SQL

`tcc_alloc` allocates a buffer, `add_symbol` passes its address to C under the
name `HIST`, and `tcc_read_i64` reads the counts back. The increments are
atomic because several threads call `hist_add` at once. `stability :=
'volatile'` keeps DuckDB from caching or constant-folding the call.

```sql
SET VARIABLE hist = tcc_alloc(128);
SET VARIABLE hist_addr = tcc_dataptr(getvariable('hist'));
SELECT ok, code FROM tcc_module(
  mode := 'add_symbol', symbol_name := 'HIST', symbol_ptr := getvariable('hist_addr'));
SELECT ok, code FROM tcc_module(
  mode := 'quick_compile',
  source := '#include <stdatomic.h>
extern _Atomic int64_t HIST[16];
int64_t hist_add(int64_t bucket) {
  if (bucket < 0 || bucket > 15) return -1;
  return atomic_fetch_add(&HIST[bucket], 1) + 1;
}',
  symbol := 'hist_add', sql_name := 'hist_add',
  return_type := 'i64', arg_types := ['i64'],
  stability := 'volatile'
);
```
```text
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
┌──────┬──────┐
│  ok  │ code │
├──────┼──────┤
│ true │ OK   │
└──────┴──────┘
```

```sql
CREATE TABLE events AS SELECT (hash(range) % 16)::BIGINT AS bucket FROM range(3000000);
SELECT count(*) FILTER (WHERE hist_add(bucket) < 0) AS rejected FROM events;
SELECT list(tcc_read_i64(getvariable('hist'), (8 * k)::UBIGINT) ORDER BY k)
         = (SELECT list(c ORDER BY bucket) FROM (SELECT bucket, count(*) AS c FROM events GROUP BY bucket))
         AS matches_group_by,
       sum(tcc_read_i64(getvariable('hist'), (8 * k)::UBIGINT)) AS total
FROM range(16) t(k);
SELECT tcc_free_ptr(getvariable('hist')) AS freed;
```
```text
┌──────────┐
│ rejected │
├──────────┤
│ 0        │
└──────────┘
┌──────────────────┬─────────┐
│ matches_group_by │  total  │
├──────────────────┼─────────┤
│ true             │ 3000000 │
└──────────────────┴─────────┘
┌───────┐
│ freed │
├───────┤
│ true  │
└───────┘
```

## Row and chunk wrappers

With `wrapper_mode := 'chunk_scalar_loop'` the generated wrapper loops over a
DuckDB data chunk in C instead of being called once per row. The C function
is the same in both modes. For the Luhn example above, `row` took 0.20 to
0.23 s and `chunk_scalar_loop` 0.17 to 0.18 s.

## Compile time

On the same machine, `quick_compile` compiled and registered 200 small
functions in 130 ms, about 0.65 ms each.
