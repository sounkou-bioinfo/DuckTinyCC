#ifdef TARGET_DEFS_ONLY

#ifndef EM_WEBASSEMBLY
# define EM_WEBASSEMBLY 0x4157
#endif
#define EM_TCC_TARGET EM_WEBASSEMBLY

/* These relocation constants are placeholders for TinyCC's generic ELF paths.
   WebAssembly output is produced by wasm32-emit.c, not by the ELF linker. */
#define R_WASM32_NONE       0
#define R_WASM32_ADDR32     1
#define R_WASM32_FUNC_INDEX 2
#define R_WASM32_TABLE_INDEX 3

#define R_DATA_32  R_WASM32_ADDR32
#define R_DATA_PTR R_WASM32_ADDR32
#define R_JMP_SLOT R_WASM32_FUNC_INDEX
#define R_GLOB_DAT R_WASM32_ADDR32
#define R_COPY     R_WASM32_NONE
#define R_RELATIVE R_WASM32_ADDR32
#define R_NUM      4

#define ELF_START_ADDR 0
#define ELF_PAGE_SIZE  0x10000
#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT 0

#else

#include "tcc.h"

ST_FUNC int code_reloc(int reloc_type)
{
    switch (reloc_type) {
    case R_WASM32_FUNC_INDEX:
    case R_WASM32_TABLE_INDEX:
        return 1;
    case R_WASM32_NONE:
    case R_WASM32_ADDR32:
        return 0;
    }
    return -1;
}

ST_FUNC int gotplt_entry_type(int reloc_type)
{
    (void)reloc_type;
    return NO_GOTPLT_ENTRY;
}

ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset,
                                  struct sym_attr *attr)
{
    (void)s1;
    (void)attr;
    return got_offset;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
    (void)s1;
}

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type,
                      unsigned char *ptr, addr_t addr, addr_t val)
{
    (void)s1;
    (void)rel;
    (void)addr;
    switch (type) {
    case R_WASM32_NONE:
        return;
    case R_WASM32_ADDR32:
        write32le(ptr, (uint32_t)val);
        return;
    default:
        tcc_error_noabort("wasm32: unsupported ELF-style relocation %d", type);
        return;
    }
}

#endif
