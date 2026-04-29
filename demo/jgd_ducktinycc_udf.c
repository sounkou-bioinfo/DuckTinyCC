#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>

#include <stdio.h>

/*
 * Open a jgd graphics device from inside DuckDB and draw a base R plot.
 *
 * SQL calls this as r_jgd_plot('tcp://127.0.0.1:<port>'). The jgd R package
 * streams JSON graphics operations to the server at that socket. The demo
 * terminal server in demo/jgd_terminal_server.c records and renders them.
 */
const char *r_jgd_plot(const char *socket_uri) {
    static int r_initialized = 0;
    static char out[512];

    if (!socket_uri || !socket_uri[0]) {
        return "missing jgd socket URI";
    }

    if (!r_initialized) {
        char *argv[] = {(char *)"ducktinycc-jgd", (char *)"--silent", (char *)"--no-save"};
        Rf_initEmbeddedR((int)(sizeof(argv) / sizeof(argv[0])), argv);
        r_initialized = 1;
    }

    /* Avoid interpolating socket_uri into R source. Store it as an R variable. */
    SEXP sock = PROTECT(Rf_mkString(socket_uri));
    Rf_defineVar(Rf_install(".ducktinycc_jgd_socket"), sock, R_GlobalEnv);

    const char *rcode =
        "{\n"
        "  suppressPackageStartupMessages(library(jgd))\n"
        "  jgd::jgd(width = 8, height = 6, dpi = 96, socket = .ducktinycc_jgd_socket)\n"
        "  cyl_cols <- c('4' = '#F8766D', '6' = '#00BA38', '8' = '#619CFF')\n"
        "  plot(mtcars$wt, mtcars$mpg,\n"
        "       pch = 19, cex = 1.4, col = cyl_cols[as.character(mtcars$cyl)],\n"
        "       main = 'jgd device opened from DuckDB via DuckTinyCC',\n"
        "       xlab = 'weight (1000 lbs)', ylab = 'miles per gallon')\n"
        "  abline(lm(mpg ~ wt, data = mtcars), col = '#E41A1C', lwd = 2)\n"
        "  legend('topright', legend = paste(names(cyl_cols), 'cyl'),\n"
        "         col = cyl_cols, pch = 19, bty = 'n')\n"
        "  invisible(dev.off())\n"
        "  'sent jgd frame from DuckDB via DuckTinyCC'\n"
        "}\n";

    ParseStatus status;
    int err = 0;
    SEXP cmd = PROTECT(Rf_mkString(rcode));
    SEXP expr = PROTECT(R_ParseVector(cmd, -1, &status, R_NilValue));

    if (status != PARSE_OK || XLENGTH(expr) < 1) {
        snprintf(out, sizeof(out), "%s", "R parse error while building jgd plot");
        UNPROTECT(3);
        return out;
    }

    SEXP ans = PROTECT(R_tryEval(VECTOR_ELT(expr, 0), R_GlobalEnv, &err));
    if (err || TYPEOF(ans) != STRSXP || XLENGTH(ans) < 1 || STRING_ELT(ans, 0) == NA_STRING) {
        snprintf(out, sizeof(out), "%s", "R evaluation error while building jgd plot; is the jgd package installed and is the server reachable?");
        UNPROTECT(4);
        return out;
    }

    snprintf(out, sizeof(out), "%s", CHAR(STRING_ELT(ans, 0)));
    UNPROTECT(4);
    return out;
}
