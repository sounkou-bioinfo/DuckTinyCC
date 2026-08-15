#include "tcc.h"
#include "wasm32-twir.h"

/* Emscripten's dynamic loader recognizes a dylink.0 custom section.  This file
   keeps that knowledge out of the raw wasm emitter; wasm32-emit.c only asks for
   the section bytes when s->wasm_side_module is true.  The first bring-up
   emits zero static memory/table requirements; generated Rtinycc/DuckTinyCC
   code imports the main module memory/table and uses explicit imports for host
   services. */

typedef struct WasmDylinkBuf {
    unsigned char *data;
    unsigned long len;
    unsigned long cap;
} WasmDylinkBuf;

static void dylink_put(WasmDylinkBuf *b, int c)
{
    if (b->len + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->data = tcc_realloc(b->data, b->cap);
    }
    b->data[b->len++] = c & 255;
}

static void dylink_uleb(WasmDylinkBuf *b, uint32_t v)
{
    do {
        int c = v & 0x7f;
        v >>= 7;
        if (v)
            c |= 0x80;
        dylink_put(b, c);
    } while (v);
}

static void dylink_bytes(WasmDylinkBuf *b, const void *p, unsigned long n)
{
    const unsigned char *q = p;
    while (n--)
        dylink_put(b, *q++);
}

ST_FUNC void wasm32_emit_dylink0(unsigned char **out, unsigned long *out_len)
{
    WasmDylinkBuf b = { 0, 0, 0 };
    const char name[] = "dylink.0";

    dylink_uleb(&b, sizeof(name) - 1);
    dylink_bytes(&b, name, sizeof(name) - 1);

    /* WASM_DYLINK_MEM_INFO subsection: memorySize, memoryAlign,
       tableSize, tableAlign.  Zeroes mean the side module itself declares no
       static memory/table footprint yet. */
    dylink_put(&b, 1);
    dylink_uleb(&b, 4);
    dylink_uleb(&b, 0);
    dylink_uleb(&b, 0);
    dylink_uleb(&b, 0);
    dylink_uleb(&b, 0);

    *out = b.data;
    *out_len = b.len;
}
