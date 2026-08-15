#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rtinycc_host.h>
#include <ducktinycc_host.h>

int runtime_imports(void)
{
    char *p = malloc(64);
    char *q = malloc(64);
    int out;

    memset(p, 0, 64);
    memset(q, 0, 64);
    memcpy(q, p, 64);
    memmove(q, p, 64);

    out = (int)strlen(q);      /* zero-filled by memset */
    out += p != 0;
    out += q != 0;

    free(q);
    free(p);
    return out + 22;           /* total 24 */
}

double math_imports(double x)
{
    return sqrt(x);
}

double math_sin(double x) { return sin(x); }
double math_cos(double x) { return cos(x); }
double math_exp(double x) { return exp(x); }
double math_log(double x) { return log(x); }

uintptr_t host_symbol_imports(uintptr_t chunk, uintptr_t sexp)
{
    uintptr_t n = 0;

    n += (uintptr_t)duckdb_data_chunk_get_size((duckdb_data_chunk)chunk);
    n += (uintptr_t)Rf_length((SEXP)sexp);

    /* Keep the declarations live without forcing a real webR/DuckDB runtime in
       the generic Node smoke test.  Project tests bind these to real imports. */
    if (chunk == 0 && sexp == 0)
        n += (uintptr_t)(R_NilValue() == 0);
    return n;
}
