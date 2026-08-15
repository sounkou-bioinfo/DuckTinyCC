#ifndef TCC_WASM32_RTINYCC_HOST_H
#define TCC_WASM32_RTINYCC_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef uintptr_t SEXP;

SEXP R_NilValue(void);
SEXP Rf_protect(SEXP x);
void Rf_unprotect(int n);
void Rf_error(const char *msg);
SEXP Rf_allocVector(int type, int length);
int Rf_length(SEXP x);
int *INTEGER(SEXP x);
double *REAL(SEXP x);
int *LOGICAL(SEXP x);
const char *CHAR(SEXP x);
SEXP STRING_ELT(SEXP x, int i);

#endif
