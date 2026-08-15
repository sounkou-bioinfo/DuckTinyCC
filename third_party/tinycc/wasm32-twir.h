#ifndef TCC_WASM32_TWIR_H
#define TCC_WASM32_TWIR_H

/* TinyCC wasm IR (TWIR)
   ---------------------
   TinyCC's traditional backends stream native machine code while tccgen runs.
   WebAssembly needs function signatures, local declarations, imports and a
   structured module envelope, so the wasm backend streams fixed-size records to
   .text and lets wasm32-emit.c translate the records into a real wasm module at
   tcc_output_file()/tcc_output_wasm_to_memory() time.

   This is deliberately simple rather than clever: each record is independent,
   byte-order stable, and easy to dump while bringing up the backend in webR or
   DuckDB-wasm. */

#define WASM32_TWIR_MAGIC 0x54574952u /* 'TWIR' */

/* WebAssembly value-type bytes. */
#define WASM32_VT_VOID 0x40
#define WASM32_VT_I32  0x7f
#define WASM32_VT_I64  0x7e
#define WASM32_VT_F32  0x7d
#define WASM32_VT_F64  0x7c

/* Record tags.  Values are stable so binary TWIR dumps remain readable. */
enum WasmTwirTag {
    TWIR_FUNC_BEGIN = 1,
    TWIR_FUNC_END,
    TWIR_PARAM,
    TWIR_SET_CONST,
    TWIR_MOV,
    TWIR_GET_SLOT,
    TWIR_SET_SLOT,
    TWIR_LOAD_MEM,
    TWIR_STORE_MEM,
    TWIR_BIN,
    TWIR_UNARY,
    TWIR_RETURN,
    TWIR_JUMP,
    TWIR_JUMP_IF,
    TWIR_LABEL,
    TWIR_CALL_ARG,
    TWIR_CALL,
    TWIR_TRAP
};

enum WasmTwirUnary {
    TWIR_U_NEG = 1,
    TWIR_U_EQZ,
    TWIR_U_EXTEND_S,
    TWIR_U_WRAP,
    TWIR_U_TRUNC_S,
    TWIR_U_TRUNC_U,
    TWIR_U_CONVERT_S,
    TWIR_U_CONVERT_U,
    TWIR_U_PROMOTE,
    TWIR_U_DEMOTE
};

typedef struct WasmTwirRec {
    uint32_t magic;
    uint16_t tag;
    uint16_t vt;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    uint64_t imm;
} WasmTwirRec;

/* a/b/c/d conventions by tag:
   FUNC_BEGIN: a=name token, b=return vt, c=variadic flag
   PARAM:      a=slot key, b=value type, c=parameter ordinal
   SET_CONST:  a=dst reg, b=value type, imm=bits
   MOV:        a=dst reg, b=src reg, c=value type
   GET_SLOT:   a=dst reg, b=slot key, c=value type
   SET_SLOT:   a=slot key, b=src reg, c=value type
   LOAD_MEM:   a=dst reg, b=addr reg, c=value type, d=offset
   STORE_MEM:  a=addr reg, b=src reg, c=value type, d=offset
   BIN:        a=dst reg, b=lhs reg, c=rhs reg, d=C/TCC op token, vt=result/input vt
   UNARY:      a=dst reg, b=src reg, c=unary op, d=destination vt, vt=source vt
   RETURN:     a=src reg, b=value type
   JUMP:       a=next patch-chain link, b=target TWIR offset
   JUMP_IF:    a=next patch-chain link, b=target TWIR offset, c=cond reg, d=condition op
   LABEL:      a=label offset
   CALL_ARG:   a=arg reg, b=value type, c=arg ordinal
   CALL:       a=name token, b=arg count, c=result value type, d=result reg
   TRAP:       a=reason token or 0
 */

#ifdef TCC_TARGET_WASM32
ST_FUNC void wasm32_twir_emit(uint16_t tag, uint16_t vt,
                              int a, int b, int c, int d, uint64_t imm);
ST_FUNC int wasm32_type_to_valtype(CType *type);
ST_FUNC int wasm32_valtype_size(int vt);
ST_FUNC const char *wasm32_valtype_name(int vt);
#endif

#endif /* TCC_WASM32_TWIR_H */
