#include "tcc.h"
#include "wasm32-twir.h"

ST_FUNC void wasm32_emit_dylink0(unsigned char **out, unsigned long *out_len);

typedef struct WasmBuf {
    unsigned char *data;
    unsigned long len;
    unsigned long cap;
} WasmBuf;

typedef struct WasmLocal {
    int kind;  /* 0=virtual register, 1=TCC local/param slot */
    int key;
    int vt;
    int index;
    int is_param;
} WasmLocal;

typedef struct WasmFunc {
    int token;
    const char *name;
    int ret_vt;
    int variadic;
    WasmTwirRec *first;
    int nrec;
    int first_index;
    int type_index;
    int params[64];
    int param_slots[64];
    int nparams;
    WasmLocal *locals;
    int nlocals;
    int alocals;
} WasmFunc;

typedef struct WasmImport {
    int token;
    const char *name;
    int ret_vt;
    int argc;
    int argv[32];
    int type_index;
    int func_index;
} WasmImport;

typedef struct WasmModule {
    TCCState *s1;
    WasmFunc *funcs;
    int nfuncs;
    int afuncs;
    WasmImport *imports;
    int nimports;
    int aimports;
} WasmModule;

static void wb_reserve(WasmBuf *b, unsigned long n)
{
    if (b->len + n > b->cap) {
        while (b->len + n > b->cap)
            b->cap = b->cap ? b->cap * 2 : 256;
        b->data = tcc_realloc(b->data, b->cap);
    }
}

static void wb_put(WasmBuf *b, int c)
{
    wb_reserve(b, 1);
    b->data[b->len++] = c & 255;
}

static void wb_bytes(WasmBuf *b, const void *p, unsigned long n)
{
    wb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void wb_u32(WasmBuf *b, uint32_t v)
{
    wb_put(b, v);
    wb_put(b, v >> 8);
    wb_put(b, v >> 16);
    wb_put(b, v >> 24);
}

static void wb_u64(WasmBuf *b, uint64_t v)
{
    wb_u32(b, (uint32_t)v);
    wb_u32(b, (uint32_t)(v >> 32));
}

static void wb_uleb(WasmBuf *b, uint64_t v)
{
    do {
        int c = v & 0x7f;
        v >>= 7;
        if (v)
            c |= 0x80;
        wb_put(b, c);
    } while (v);
}

static void wb_sleb(WasmBuf *b, int64_t v)
{
    int more = 1;
    while (more) {
        int c = v & 0x7f;
        int sign = c & 0x40;
        v >>= 7;
        more = !((v == 0 && !sign) || (v == -1 && sign));
        if (more)
            c |= 0x80;
        wb_put(b, c);
    }
}

static void wb_name(WasmBuf *b, const char *name)
{
    unsigned long n = name ? strlen(name) : 0;
    wb_uleb(b, n);
    wb_bytes(b, name, n);
}

static void wb_section(WasmBuf *m, int id, WasmBuf *payload)
{
    if (!payload->len)
        return;
    wb_put(m, id);
    wb_uleb(m, payload->len);
    wb_bytes(m, payload->data, payload->len);
}

static int is_valid_twir(WasmTwirRec *r)
{
    return r->magic == WASM32_TWIR_MAGIC && r->tag >= TWIR_FUNC_BEGIN && r->tag <= TWIR_TRAP;
}

static WasmFunc *module_add_func(WasmModule *m)
{
    if (m->nfuncs == m->afuncs) {
        m->afuncs = m->afuncs ? m->afuncs * 2 : 8;
        m->funcs = tcc_realloc(m->funcs, m->afuncs * sizeof *m->funcs);
    }
    memset(&m->funcs[m->nfuncs], 0, sizeof m->funcs[m->nfuncs]);
    return &m->funcs[m->nfuncs++];
}

static WasmImport *module_add_import(WasmModule *m)
{
    if (m->nimports == m->aimports) {
        m->aimports = m->aimports ? m->aimports * 2 : 8;
        m->imports = tcc_realloc(m->imports, m->aimports * sizeof *m->imports);
    }
    memset(&m->imports[m->nimports], 0, sizeof m->imports[m->nimports]);
    return &m->imports[m->nimports++];
}

static WasmFunc *find_func_by_token(WasmModule *m, int token)
{
    int i;
    for (i = 0; i < m->nfuncs; ++i)
        if (m->funcs[i].token == token)
            return &m->funcs[i];
    return NULL;
}

static int same_import_sig(WasmImport *im, int token, int argc, int *argv, int ret)
{
    int i;
    if (im->token != token || im->argc != argc || im->ret_vt != ret)
        return 0;
    for (i = 0; i < argc; ++i)
        if (im->argv[i] != argv[i])
            return 0;
    return 1;
}

static const char *wasm32_safe_tok_name(int token, const char *prefix)
{
    char buf[96];
    int base = token & ~SYM_FIELD;
    if (base >= TOK_IDENT && base < tok_ident &&
        table_ident && table_ident[base - TOK_IDENT])
        return get_tok_str(base, NULL);
    snprintf(buf, sizeof buf, "%s_%x", prefix, (unsigned)token);
    return tcc_strdup(buf);
}

static WasmImport *find_or_add_import(WasmModule *m, int token, const char *name,
                                      int argc, int *argv, int ret)
{
    int i;
    WasmImport *im;
    for (i = 0; i < m->nimports; ++i)
        if (same_import_sig(&m->imports[i], token, argc, argv, ret))
            return &m->imports[i];
    im = module_add_import(m);
    im->token = token;
    im->name = name ? name : wasm32_safe_tok_name(token, "import");
    im->argc = argc;
    im->ret_vt = ret;
    for (i = 0; i < argc; ++i)
        im->argv[i] = argv[i];
    return im;
}

static int local_index(WasmFunc *f, int kind, int key, int vt, int is_param)
{
    int i;
    for (i = 0; i < f->nlocals; ++i)
        if (f->locals[i].kind == kind && f->locals[i].key == key && f->locals[i].vt == vt)
            return f->locals[i].index;
    if (f->nlocals == f->alocals) {
        f->alocals = f->alocals ? f->alocals * 2 : 32;
        f->locals = tcc_realloc(f->locals, f->alocals * sizeof *f->locals);
    }
    i = f->nlocals++;
    f->locals[i].kind = kind;
    f->locals[i].key = key;
    f->locals[i].vt = vt;
    f->locals[i].index = i;
    f->locals[i].is_param = is_param;
    return i;
}

static void scan_twir(TCCState *s1, WasmModule *m)
{
    unsigned long off, end = text_section->data_offset;
    WasmFunc *f = NULL;
    int pending_argc = 0, pending_argv[32];

    for (off = 0; off + sizeof(WasmTwirRec) <= end; off += sizeof(WasmTwirRec)) {
        WasmTwirRec *r = (WasmTwirRec *)(text_section->data + off);
        if (!is_valid_twir(r))
            continue;
        switch (r->tag) {
        case TWIR_FUNC_BEGIN:
            f = module_add_func(m);
            f->token = r->a;
            f->name = r->imm ? (const char *)(uintptr_t)r->imm : wasm32_safe_tok_name(r->a, "func");
            f->ret_vt = r->b;
            f->variadic = r->c;
            f->first = r;
            f->first_index = m->nimports + m->nfuncs - 1;
            break;
        case TWIR_FUNC_END:
            f = NULL;
            break;
        case TWIR_PARAM:
            if (f && f->nparams < (int)countof(f->params)) {
                f->param_slots[f->nparams] = r->a;
                f->params[f->nparams++] = r->b;
                local_index(f, 1, r->a, r->b, 1);
            }
            break;
        case TWIR_CALL_ARG:
            if (pending_argc < (int)countof(pending_argv))
                pending_argv[pending_argc++] = r->b;
            break;
        case TWIR_CALL:
            if (!find_func_by_token(m, r->a))
                find_or_add_import(m, r->a,
                                   r->imm ? (const char *)(uintptr_t)r->imm : NULL,
                                   pending_argc, pending_argv, r->c);
            pending_argc = 0;
            break;
        default:
            break;
        }
    }
    (void)s1;
}

static int bin_opcode_i32(int op, int vt, int result_vt)
{
    switch (op) {
    case '+': return vt == WASM32_VT_I64 ? 0x7c : 0x6a;
    case '-': return vt == WASM32_VT_I64 ? 0x7d : 0x6b;
    case '*': return vt == WASM32_VT_I64 ? 0x7e : 0x6c;
    case '/': return vt == WASM32_VT_I64 ? 0x7f : 0x6d;
    case TOK_UDIV: return vt == WASM32_VT_I64 ? 0x80 : 0x6e;
    case '%': return vt == WASM32_VT_I64 ? 0x81 : 0x6f;
    case TOK_UMOD: return vt == WASM32_VT_I64 ? 0x82 : 0x70;
    case '&': return vt == WASM32_VT_I64 ? 0x83 : 0x71;
    case '|': return vt == WASM32_VT_I64 ? 0x84 : 0x72;
    case '^': return vt == WASM32_VT_I64 ? 0x85 : 0x73;
    case TOK_SHL: return vt == WASM32_VT_I64 ? 0x86 : 0x74;
    case TOK_SAR: return vt == WASM32_VT_I64 ? 0x87 : 0x75;
    case TOK_SHR: return vt == WASM32_VT_I64 ? 0x88 : 0x76;
    case TOK_EQ: return vt == WASM32_VT_I64 ? 0x51 : 0x46;
    case TOK_NE: return vt == WASM32_VT_I64 ? 0x52 : 0x47;
    case TOK_LT: return vt == WASM32_VT_I64 ? 0x53 : 0x48;
    case TOK_ULT: return vt == WASM32_VT_I64 ? 0x54 : 0x49;
    case TOK_GT: return vt == WASM32_VT_I64 ? 0x55 : 0x4a;
    case TOK_UGT: return vt == WASM32_VT_I64 ? 0x56 : 0x4b;
    case TOK_LE: return vt == WASM32_VT_I64 ? 0x57 : 0x4c;
    case TOK_ULE: return vt == WASM32_VT_I64 ? 0x58 : 0x4d;
    case TOK_GE: return vt == WASM32_VT_I64 ? 0x59 : 0x4e;
    case TOK_UGE: return vt == WASM32_VT_I64 ? 0x5a : 0x4f;
    default:
        (void)result_vt;
        return -1;
    }
}

static int bin_opcode_float(int op, int vt)
{
    if (vt == WASM32_VT_F32) {
        switch (op) {
        case '+': return 0x92;
        case '-': return 0x93;
        case '*': return 0x94;
        case '/': return 0x95;
        case TOK_EQ: return 0x5b;
        case TOK_NE: return 0x5c;
        case TOK_LT: return 0x5d;
        case TOK_GT: return 0x5e;
        case TOK_LE: return 0x5f;
        case TOK_GE: return 0x60;
        }
    } else if (vt == WASM32_VT_F64) {
        switch (op) {
        case '+': return 0xa0;
        case '-': return 0xa1;
        case '*': return 0xa2;
        case '/': return 0xa3;
        case TOK_EQ: return 0x61;
        case TOK_NE: return 0x62;
        case TOK_LT: return 0x63;
        case TOK_GT: return 0x64;
        case TOK_LE: return 0x65;
        case TOK_GE: return 0x66;
        }
    }
    return -1;
}

static void emit_const(WasmBuf *b, int vt, uint64_t imm)
{
    if (vt == WASM32_VT_I64) {
        wb_put(b, 0x42);
        wb_sleb(b, (int64_t)imm);
    } else if (vt == WASM32_VT_F32) {
        wb_put(b, 0x43);
        wb_u32(b, (uint32_t)imm);
    } else if (vt == WASM32_VT_F64) {
        wb_put(b, 0x44);
        wb_u64(b, imm);
    } else {
        wb_put(b, 0x41);
        wb_sleb(b, (int32_t)imm);
    }
}

static void emit_local_get(WasmBuf *b, int idx) { wb_put(b, 0x20); wb_uleb(b, idx); }
static void emit_local_set(WasmBuf *b, int idx) { wb_put(b, 0x21); wb_uleb(b, idx); }

static int func_index_of(WasmModule *m, int token)
{
    int i;
    for (i = 0; i < m->nimports; ++i)
        if (m->imports[i].token == token)
            return m->imports[i].func_index;
    for (i = 0; i < m->nfuncs; ++i)
        if (m->funcs[i].token == token)
            return m->funcs[i].first_index;
    return -1;
}

static void prescan_locals(WasmFunc *f)
{
    int i;
    for (i = 0; i < f->nrec; ++i) {
        WasmTwirRec *r = f->first + i;
        switch (r->tag) {
        case TWIR_SET_CONST:
            local_index(f, 0, r->a, r->b, 0);
            break;
        case TWIR_MOV:
            local_index(f, 0, r->a, r->c, 0);
            local_index(f, 0, r->b, r->c, 0);
            break;
        case TWIR_GET_SLOT:
            local_index(f, 0, r->a, r->c, 0);
            local_index(f, 1, r->b, r->c, 0);
            break;
        case TWIR_SET_SLOT:
            local_index(f, 1, r->a, r->c, 0);
            local_index(f, 0, r->b, r->c, 0);
            break;
        case TWIR_LOAD_MEM:
            local_index(f, 0, r->a, r->c, 0);
            local_index(f, 0, r->b, WASM32_VT_I32, 0);
            break;
        case TWIR_STORE_MEM:
            local_index(f, 0, r->a, WASM32_VT_I32, 0);
            local_index(f, 0, r->b, r->c, 0);
            break;
        case TWIR_BIN:
            local_index(f, 0, r->a, r->vt, 0);
            local_index(f, 0, r->b, r->vt, 0);
            local_index(f, 0, r->c, r->vt, 0);
            break;
        case TWIR_RETURN:
            if (r->b != WASM32_VT_VOID)
                local_index(f, 0, r->a, r->b, 0);
            break;
        case TWIR_CALL_ARG:
            local_index(f, 0, r->a, r->b, 0);
            break;
        case TWIR_CALL:
            if (r->c != WASM32_VT_VOID)
                local_index(f, 0, r->d, r->c, 0);
            break;
        default:
            break;
        }
    }
}

static void emit_func_body(WasmModule *m, WasmFunc *f, WasmBuf *body)
{
    int i, nonparams = 0;
    prescan_locals(f);
    for (i = 0; i < f->nlocals; ++i)
        if (!f->locals[i].is_param)
            nonparams++;
    wb_uleb(body, nonparams);
    for (i = 0; i < f->nlocals; ++i) {
        if (!f->locals[i].is_param) {
            wb_uleb(body, 1);
            wb_put(body, f->locals[i].vt);
        }
    }

    for (i = 0; i < f->nrec; ++i) {
        WasmTwirRec *r = f->first + i;
        int dst, src, lhs, rhs, opc, idx;
        if (!is_valid_twir(r))
            continue;
        switch (r->tag) {
        case TWIR_FUNC_BEGIN:
        case TWIR_PARAM:
            break;
        case TWIR_SET_CONST:
            dst = local_index(f, 0, r->a, r->b, 0);
            emit_const(body, r->b, r->imm);
            emit_local_set(body, dst);
            break;
        case TWIR_MOV:
            dst = local_index(f, 0, r->a, r->c, 0);
            src = local_index(f, 0, r->b, r->c, 0);
            emit_local_get(body, src);
            emit_local_set(body, dst);
            break;
        case TWIR_GET_SLOT:
            dst = local_index(f, 0, r->a, r->c, 0);
            src = local_index(f, 1, r->b, r->c, 0);
            emit_local_get(body, src);
            emit_local_set(body, dst);
            break;
        case TWIR_SET_SLOT:
            dst = local_index(f, 1, r->a, r->c, 0);
            src = local_index(f, 0, r->b, r->c, 0);
            emit_local_get(body, src);
            emit_local_set(body, dst);
            break;
        case TWIR_LOAD_MEM:
            dst = local_index(f, 0, r->a, r->c, 0);
            src = local_index(f, 0, r->b, WASM32_VT_I32, 0);
            emit_local_get(body, src);
            if (r->d) { emit_const(body, WASM32_VT_I32, r->d); wb_put(body, 0x6a); }
            wb_put(body, r->c == WASM32_VT_I64 ? 0x29 : r->c == WASM32_VT_F32 ? 0x2a : r->c == WASM32_VT_F64 ? 0x2b : 0x28);
            wb_uleb(body, r->c == WASM32_VT_I64 || r->c == WASM32_VT_F64 ? 3 : 2);
            wb_uleb(body, 0);
            emit_local_set(body, dst);
            break;
        case TWIR_STORE_MEM:
            dst = local_index(f, 0, r->a, WASM32_VT_I32, 0);
            src = local_index(f, 0, r->b, r->c, 0);
            emit_local_get(body, dst);
            if (r->d) { emit_const(body, WASM32_VT_I32, r->d); wb_put(body, 0x6a); }
            emit_local_get(body, src);
            wb_put(body, r->c == WASM32_VT_I64 ? 0x37 : r->c == WASM32_VT_F32 ? 0x38 : r->c == WASM32_VT_F64 ? 0x39 : 0x36);
            wb_uleb(body, r->c == WASM32_VT_I64 || r->c == WASM32_VT_F64 ? 3 : 2);
            wb_uleb(body, 0);
            break;
        case TWIR_BIN:
            lhs = local_index(f, 0, r->b, r->vt, 0);
            rhs = local_index(f, 0, r->c, r->vt, 0);
            dst = local_index(f, 0, r->a, r->vt, 0);
            emit_local_get(body, lhs);
            emit_local_get(body, rhs);
            opc = (r->vt == WASM32_VT_F32 || r->vt == WASM32_VT_F64)
                ? bin_opcode_float(r->d, r->vt) : bin_opcode_i32(r->d, r->vt, r->vt);
            if (opc < 0)
                wb_put(body, 0x00); /* unreachable */
            else
                wb_put(body, opc);
            emit_local_set(body, dst);
            break;
        case TWIR_CALL_ARG:
            src = local_index(f, 0, r->a, r->b, 0);
            emit_local_get(body, src);
            break;
        case TWIR_CALL:
            idx = func_index_of(m, r->a);
            if (idx < 0)
                wb_put(body, 0x00);
            else { wb_put(body, 0x10); wb_uleb(body, idx); }
            if (r->c != WASM32_VT_VOID) {
                dst = local_index(f, 0, r->d, r->c, 0);
                emit_local_set(body, dst);
            }
            break;
        case TWIR_RETURN:
            if (r->b != WASM32_VT_VOID) {
                src = local_index(f, 0, r->a, r->b, 0);
                emit_local_get(body, src);
            }
            wb_put(body, 0x0f);
            break;
        case TWIR_TRAP:
            wb_put(body, 0x00);
            break;
        case TWIR_FUNC_END:
            break;
        default:
            /* Control-flow records are kept in TWIR now, but this first binary
               emitter deliberately lowers only straight-line functions.  The
               records are not dropped silently: they trap so failures are
               explicit while the structured-control-flow pass lands. */
            wb_put(body, 0x00);
            break;
        }
    }
    if (f->ret_vt == WASM32_VT_VOID)
        wb_put(body, 0x0f);
    wb_put(body, 0x0b);
}

static void emit_type_sig(WasmBuf *sec, int argc, int *argv, int ret_vt)
{
    int i;
    wb_put(sec, 0x60);
    wb_uleb(sec, argc);
    for (i = 0; i < argc; ++i)
        wb_put(sec, argv[i]);
    if (ret_vt == WASM32_VT_VOID) {
        wb_uleb(sec, 0);
    } else {
        wb_uleb(sec, 1);
        wb_put(sec, ret_vt);
    }
}

static void emit_module(TCCState *s1, WasmModule *m, WasmBuf *out)
{
    WasmBuf sec = {0,0,0}, code = {0,0,0}, tmp = {0,0,0};
    int i, type_count, func_base;

    wb_bytes(out, "\0asm", 4);
    wb_u32(out, 1);

    if (s1->wasm_side_module) {
        unsigned char *d = NULL;
        unsigned long dl = 0;
        wasm32_emit_dylink0(&d, &dl);
        if (dl) {
            wb_put(out, 0);
            wb_uleb(out, dl);
            wb_bytes(out, d, dl);
        }
        tcc_free(d);
    }

    func_base = m->nimports;
    for (i = 0; i < m->nimports; ++i)
        m->imports[i].func_index = i;
    for (i = 0; i < m->nfuncs; ++i)
        m->funcs[i].first_index = func_base + i;

    type_count = m->nimports + m->nfuncs;
    wb_uleb(&sec, type_count);
    for (i = 0; i < m->nimports; ++i) {
        m->imports[i].type_index = i;
        emit_type_sig(&sec, m->imports[i].argc, m->imports[i].argv, m->imports[i].ret_vt);
    }
    for (i = 0; i < m->nfuncs; ++i) {
        m->funcs[i].type_index = m->nimports + i;
        emit_type_sig(&sec, m->funcs[i].nparams, m->funcs[i].params, m->funcs[i].ret_vt);
    }
    wb_section(out, 1, &sec); sec.len = 0;

    if (s1->wasm_side_module || m->nimports) {
        int n = m->nimports + !!s1->wasm_import_memory + !!s1->wasm_import_table;
        wb_uleb(&sec, n);
        if (s1->wasm_import_memory) {
            wb_name(&sec, "env"); wb_name(&sec, "memory");
            wb_put(&sec, 0x02); wb_put(&sec, 0x00); wb_uleb(&sec, 0);
        }
        if (s1->wasm_import_table) {
            wb_name(&sec, "env"); wb_name(&sec, "__indirect_function_table");
            wb_put(&sec, 0x01); wb_put(&sec, 0x70); wb_put(&sec, 0x00); wb_uleb(&sec, 0);
        }
        for (i = 0; i < m->nimports; ++i) {
            wb_name(&sec, "env"); wb_name(&sec, m->imports[i].name);
            wb_put(&sec, 0x00); wb_uleb(&sec, m->imports[i].type_index);
        }
        wb_section(out, 2, &sec); sec.len = 0;
    }

    wb_uleb(&sec, m->nfuncs);
    for (i = 0; i < m->nfuncs; ++i)
        wb_uleb(&sec, m->funcs[i].type_index);
    wb_section(out, 3, &sec); sec.len = 0;

    wb_uleb(&sec, m->nfuncs * 2);
    for (i = 0; i < m->nfuncs; ++i) {
        char underscored[512];
        wb_name(&sec, m->funcs[i].name);
        wb_put(&sec, 0x00);
        wb_uleb(&sec, m->funcs[i].first_index);
        snprintf(underscored, sizeof underscored, "_%s", m->funcs[i].name);
        wb_name(&sec, underscored);
        wb_put(&sec, 0x00);
        wb_uleb(&sec, m->funcs[i].first_index);
    }
    wb_section(out, 7, &sec); sec.len = 0;

    wb_uleb(&code, m->nfuncs);
    for (i = 0; i < m->nfuncs; ++i) {
        tmp.len = 0;
        emit_func_body(m, &m->funcs[i], &tmp);
        wb_uleb(&code, tmp.len);
        wb_bytes(&code, tmp.data, tmp.len);
    }
    wb_section(out, 10, &code);

    tcc_free(sec.data);
    tcc_free(code.data);
    tcc_free(tmp.data);
}

static void prune_resolved_imports(WasmModule *m)
{
    int i, w = 0;
    for (i = 0; i < m->nimports; ++i) {
        if (find_func_by_token(m, m->imports[i].token))
            continue;
        if (w != i)
            m->imports[w] = m->imports[i];
        w++;
    }
    m->nimports = w;
}

static void compute_func_record_counts(WasmModule *m)
{
    int i;
    for (i = 0; i < m->nfuncs; ++i) {
        WasmTwirRec *r = m->funcs[i].first;
        int n = 0;
        while (is_valid_twir(r + n)) {
            n++;
            if ((r + n - 1)->tag == TWIR_FUNC_END)
                break;
        }
        m->funcs[i].nrec = n;
    }
}

ST_FUNC int wasm_output_memory(TCCState *s1, unsigned char **out, unsigned long *out_len)
{
    WasmModule m;
    WasmBuf b = {0,0,0};
    int i;
    memset(&m, 0, sizeof m);
    m.s1 = s1;
    if (!text_section || !text_section->data_offset)
        return tcc_error_noabort("wasm32: no TWIR code was generated");
    scan_twir(s1, &m);
    prune_resolved_imports(&m);
    compute_func_record_counts(&m);
    if (!m.nfuncs)
        return tcc_error_noabort("wasm32: no functions in TWIR stream");
    emit_module(s1, &m, &b);
    for (i = 0; i < m.nfuncs; ++i)
        tcc_free(m.funcs[i].locals);
    tcc_free(m.funcs);
    tcc_free(m.imports);
    *out = b.data;
    *out_len = b.len;
    return s1->nb_errors ? -1 : 0;
}

ST_FUNC int wasm_output_file(TCCState *s1, const char *filename)
{
    unsigned char *buf = NULL;
    unsigned long len = 0;
    int fd, r;
    r = wasm_output_memory(s1, &buf, &len);
    if (r < 0)
        return r;
    unlink(filename);
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd < 0) {
        tcc_free(buf);
        return tcc_error_noabort("could not write '%s': %s", filename, strerror(errno));
    }
    if (write(fd, buf, len) != (ssize_t)len)
        r = tcc_error_noabort("short write to '%s'", filename);
    close(fd);
    if (s1->verbose)
        printf("<- %s\n", filename);
    tcc_free(buf);
    return r;
}

ST_FUNC void wasm_free_buffer(unsigned char *ptr)
{
    tcc_free(ptr);
}
