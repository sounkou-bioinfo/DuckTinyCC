export function createWasmHost(options = {}) {
  const memory = options.memory ?? new WebAssembly.Memory({ initial: 2 });
  const table = options.table ?? new WebAssembly.Table({ element: 'anyfunc', initial: 32 });
  let bump = options.heapBase ?? 4096;

  const heap = () => new Uint8Array(memory.buffer);
  const view = () => new DataView(memory.buffer);
  const align = (x, a) => (x + a - 1) & -a;
  const ensure = (end) => {
    const pages = 65536;
    if (end <= memory.buffer.byteLength) return;
    memory.grow(Math.ceil((end - memory.buffer.byteLength) / pages));
  };
  const malloc = (size) => {
    const ptr = align(bump, 8);
    bump = ptr + Number(size || 0);
    ensure(bump + 8);
    return ptr;
  };
  const free = () => {};
  const calloc = (nmemb, size) => {
    const n = Number(nmemb) * Number(size);
    const ptr = malloc(n);
    heap().fill(0, ptr, ptr + n);
    return ptr;
  };
  const realloc = (oldPtr, size) => {
    const ptr = malloc(size);
    if (oldPtr) heap().copyWithin(ptr, oldPtr, oldPtr + Number(size));
    return ptr;
  };
  const memcpy = (dst, src, n) => {
    heap().copyWithin(Number(dst), Number(src), Number(src) + Number(n));
    return dst;
  };
  const memmove = memcpy;
  const memset = (dst, c, n) => {
    heap().fill(Number(c) & 255, Number(dst), Number(dst) + Number(n));
    return dst;
  };
  const memcmp = (a, b, n) => {
    const h = heap();
    for (let i = 0; i < Number(n); ++i) {
      const d = h[Number(a) + i] - h[Number(b) + i];
      if (d) return d;
    }
    return 0;
  };
  const strlen = (ptr) => {
    const h = heap();
    let p = Number(ptr);
    while (h[p]) ++p;
    return p - Number(ptr);
  };
  const strcpy = (dst, src) => {
    const h = heap();
    let i = 0;
    do { h[Number(dst) + i] = h[Number(src) + i]; } while (h[Number(src) + i++]);
    return dst;
  };
  const strncpy = (dst, src, n) => {
    const h = heap();
    let i = 0;
    for (; i < Number(n) && h[Number(src) + i]; ++i) h[Number(dst) + i] = h[Number(src) + i];
    for (; i < Number(n); ++i) h[Number(dst) + i] = 0;
    return dst;
  };
  const abort = () => { throw new Error('wasm abort'); };
  const popcount32 = (x) => {
    x = Number(x) >>> 0;
    x = x - ((x >>> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >>> 2) & 0x33333333);
    return (((x + (x >>> 4)) & 0x0f0f0f0f) * 0x01010101) >>> 24;
  };
  const bswap32 = (x) => {
    x = Number(x) >>> 0;
    return (((x & 0xff) << 24) | ((x & 0xff00) << 8) |
            ((x >>> 8) & 0xff00) | ((x >>> 24) & 0xff)) >>> 0;
  };

  const unbound = (name) => (...args) => {
    throw new Error(`unbound wasm host import ${name}(${args.join(',')})`);
  };

  const env = {
    memory,
    __indirect_function_table: table,
    malloc,
    free,
    calloc,
    realloc,
    memcpy,
    memmove,
    memset,
    memcmp,
    strlen,
    strcpy,
    strncpy,
    abort,
    sqrt: Math.sqrt,
    sin: Math.sin,
    cos: Math.cos,
    exp: Math.exp,
    log: Math.log,
    __divdi3: (a, b) => BigInt.asIntN(64, a / b),
    __udivdi3: (a, b) => BigInt.asUintN(64, BigInt.asUintN(64, a) / BigInt.asUintN(64, b)),
    __moddi3: (a, b) => BigInt.asIntN(64, a % b),
    __umoddi3: (a, b) => BigInt.asUintN(64, BigInt.asUintN(64, a) % BigInt.asUintN(64, b)),
    __builtin_popcount: popcount32,
    __builtin_clz: (x) => Math.clz32(Number(x) >>> 0),
    __bswapsi2: bswap32,
    sqrtf: Math.sqrt,
    sinf: Math.sin,
    cosf: Math.cos,
    R_NilValue: () => 0,
    Rf_length: () => 0,
    Rf_protect: (x) => x,
    Rf_unprotect: () => {},
    Rf_error: unbound('Rf_error'),
    Rf_allocVector: unbound('Rf_allocVector'),
    INTEGER: unbound('INTEGER'),
    REAL: unbound('REAL'),
    LOGICAL: unbound('LOGICAL'),
    CHAR: unbound('CHAR'),
    STRING_ELT: unbound('STRING_ELT'),
    duckdb_malloc: malloc,
    duckdb_free: free,
    duckdb_data_chunk_get_size: () => 0,
    duckdb_data_chunk_get_vector: () => 0,
    duckdb_vector_get_data: () => 0,
    duckdb_scalar_function_set_error: unbound('duckdb_scalar_function_set_error'),
  };

  return { memory, table, imports: { env }, env, view, heap };
}
