#ifndef _STDINT_H
#define _STDINT_H

/* TinyCC built-in stdint.h
 * Derived from tccdefs.h predefined type macros so that no system development
 * headers (libc6-dev / glibc-devel) are required at JIT-compile time.
 *
 * tccdefs.h (compiled in via CONFIG_TCC_PREDEFS=1) provides:
 *   __INT32_TYPE__    -> int
 *   __INT64_TYPE__    -> long / long long  (platform-dependent)
 *   __INTPTR_TYPE__   -> __PTRDIFF_TYPE__  (pointer-width signed)
 *   __UINTPTR_TYPE__  -> unsigned __PTRDIFF_TYPE__
 *   __SIZE_TYPE__     -> platform-width unsigned
 *
 * 8-bit and 16-bit types are universally `signed/unsigned char` and
 * `signed/unsigned short` on all platforms TinyCC supports.
 */

/* Exact-width integer types */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef __INT32_TYPE__     int32_t;
typedef unsigned __INT32_TYPE__ uint32_t;
typedef __INT64_TYPE__     int64_t;
typedef unsigned __INT64_TYPE__ uint64_t;

/* Minimum-width integer types */
typedef int8_t   int_least8_t;
typedef uint8_t  uint_least8_t;
typedef int16_t  int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t  int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t  int_least64_t;
typedef uint64_t uint_least64_t;

/* Fastest minimum-width integer types */
typedef int8_t   int_fast8_t;
typedef uint8_t  uint_fast8_t;
typedef int32_t  int_fast16_t;
typedef uint32_t uint_fast16_t;
typedef int32_t  int_fast32_t;
typedef uint32_t uint_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint64_t uint_fast64_t;

/* Pointer-width integer types (from tccdefs.h) */
typedef __INTPTR_TYPE__  intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;

/* Greatest-width integer types */
typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

/* Limits of exact-width types */
#define INT8_MIN   (-128)
#define INT8_MAX   (127)
#define UINT8_MAX  (255U)

#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define UINT16_MAX (65535U)

#define INT32_MIN  (-2147483648)
#define INT32_MAX  (2147483647)
#define UINT32_MAX (4294967295U)

#define INT64_MIN  (-9223372036854775807LL - 1LL)
#define INT64_MAX  (9223372036854775807LL)
#define UINT64_MAX (18446744073709551615ULL)

/* Limits of pointer-width types */
#if __SIZEOF_POINTER__ == 8
# define INTPTR_MIN  INT64_MIN
# define INTPTR_MAX  INT64_MAX
# define UINTPTR_MAX UINT64_MAX
# define SIZE_MAX    UINT64_MAX
# define PTRDIFF_MIN INT64_MIN
# define PTRDIFF_MAX INT64_MAX
#else
# define INTPTR_MIN  INT32_MIN
# define INTPTR_MAX  INT32_MAX
# define UINTPTR_MAX UINT32_MAX
# define SIZE_MAX    UINT32_MAX
# define PTRDIFF_MIN INT32_MIN
# define PTRDIFF_MAX INT32_MAX
#endif

/* Limits of minimum-width types (same as exact-width) */
#define INT_LEAST8_MIN   INT8_MIN
#define INT_LEAST8_MAX   INT8_MAX
#define UINT_LEAST8_MAX  UINT8_MAX
#define INT_LEAST16_MIN  INT16_MIN
#define INT_LEAST16_MAX  INT16_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MIN  INT32_MIN
#define INT_LEAST32_MAX  INT32_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MIN  INT64_MIN
#define INT_LEAST64_MAX  INT64_MAX
#define UINT_LEAST64_MAX UINT64_MAX

/* Limits of fastest minimum-width types */
#define INT_FAST8_MIN    INT8_MIN
#define INT_FAST8_MAX    INT8_MAX
#define UINT_FAST8_MAX   UINT8_MAX
#define INT_FAST16_MIN   INT32_MIN
#define INT_FAST16_MAX   INT32_MAX
#define UINT_FAST16_MAX  UINT32_MAX
#define INT_FAST32_MIN   INT32_MIN
#define INT_FAST32_MAX   INT32_MAX
#define UINT_FAST32_MAX  UINT32_MAX
#define INT_FAST64_MIN   INT64_MIN
#define INT_FAST64_MAX   INT64_MAX
#define UINT_FAST64_MAX  UINT64_MAX

/* Limits of intmax/uintmax */
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

/* Macros for integer constants */
#define INT8_C(c)   (c)
#define INT16_C(c)  (c)
#define INT32_C(c)  (c)
#define INT64_C(c)  (c ## LL)
#define UINT8_C(c)  (c ## U)
#define UINT16_C(c) (c ## U)
#define UINT32_C(c) (c ## U)
#define UINT64_C(c) (c ## ULL)
#define INTMAX_C(c)  INT64_C(c)
#define UINTMAX_C(c) UINT64_C(c)

#endif /* _STDINT_H */
