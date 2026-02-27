#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>

#include <stdio.h>

/*
 * Minimal embedded-R helper for DuckTinyCC demo purposes.
 * - Initializes R once per process.
 * - Evaluates a tiny R expression.
 * - Returns a stable C string pointer (static buffer).
 */
const char *r_hello_from_embedded(void) {
    static int r_initialized = 0;
    static char out[256];

    if (!r_initialized) {
        char *argv[] = {(char *)"ducktinycc-embedded-r", (char *)"--silent", (char *)"--no-save"};
        Rf_initEmbeddedR((int)(sizeof(argv) / sizeof(argv[0])), argv);
        r_initialized = 1;
    }

    ParseStatus status;
    SEXP cmd = PROTECT(Rf_mkString("paste('hello from embedded R', getRversion())"));
    SEXP expr = PROTECT(R_ParseVector(cmd, -1, &status, R_NilValue));

    if (status != PARSE_OK || XLENGTH(expr) < 1) {
        UNPROTECT(2);
        snprintf(out, sizeof(out), "%s", "R parse error");
        return out;
    }

    SEXP ans = PROTECT(Rf_eval(VECTOR_ELT(expr, 0), R_GlobalEnv));
    if (TYPEOF(ans) == STRSXP && XLENGTH(ans) > 0 && STRING_ELT(ans, 0) != NA_STRING) {
        snprintf(out, sizeof(out), "%s", CHAR(STRING_ELT(ans, 0)));
    } else {
        snprintf(out, sizeof(out), "%s", "R eval returned non-string");
    }

    UNPROTECT(3);
    return out;
}
