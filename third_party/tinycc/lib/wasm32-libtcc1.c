/* wasm32 libtcc1 runtime helpers for TinyCC.

   v0.1.0-alpha deliberately keeps this runtime to helpers that the initial
   wasm32 backend can compile and validate correctly: 32-bit bit operations and
   byte swaps.  64-bit compiler-rt helpers are intentionally left undefined for
   now so code that needs them imports/fails explicitly instead of receiving
   wrong generated wasm.  Browser side modules still import malloc/free,
   memcpy/memmove/memset/strlen and math from the host runtime. */

typedef unsigned int u32;
typedef unsigned long ul32;

static u32 tcc_pop32(u32 x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0f0f0f0fu;
    return (x * 0x01010101u) >> 24;
}

static u32 tcc_clz32(u32 x)
{
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return 32u - tcc_pop32(x);
}

static u32 tcc_ctz32(u32 x) { return tcc_pop32((x & (0u - x)) - 1u); }

int __builtin_ffs(int x) { return (int)((tcc_ctz32((u32)x) + 1u) & (0u - (u32)(x != 0))); }
int __builtin_ffsl(long x) { return __builtin_ffs((int)x); }

int __builtin_clz(u32 x) { return (int)tcc_clz32(x); }
int __builtin_clzl(ul32 x) { return (int)tcc_clz32((u32)x); }

int __builtin_ctz(u32 x) { return (int)tcc_ctz32(x); }
int __builtin_ctzl(ul32 x) { return (int)tcc_ctz32((u32)x); }

int __builtin_clrsb(int x) { return (int)tcc_clz32(((u32)(x ^ (x >> 31))) << 1) - 1; }
int __builtin_clrsbl(long x) { return __builtin_clrsb((int)x); }

int __builtin_popcount(u32 x) { return (int)tcc_pop32(x); }
int __builtin_popcountl(ul32 x) { return (int)tcc_pop32((u32)x); }

int __builtin_parity(u32 x) { return (int)(tcc_pop32(x) & 1u); }
int __builtin_parityl(ul32 x) { return (int)(tcc_pop32((u32)x) & 1u); }

int __clzsi2(u32 x) { return __builtin_clz(x); }
int __ctzsi2(u32 x) { return __builtin_ctz(x); }
int __popcountsi2(u32 x) { return __builtin_popcount(x); }
int __paritysi2(u32 x) { return __builtin_parity(x); }

u32 __bswapsi2(u32 x)
{
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8)
         | ((x & 0x00ff0000u) >> 8)  | ((x & 0xff000000u) >> 24);
}
