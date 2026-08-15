#ifndef _TCC_WASM32_STDDEF_H
#define _TCC_WASM32_STDDEF_H
#define NULL ((void*)0)
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef unsigned short wchar_t;
#define offsetof(type, field) __builtin_offsetof(type, field)
#endif
