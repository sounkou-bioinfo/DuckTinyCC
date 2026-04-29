#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>

#include <stdio.h>
#include <string.h>

/*
 * Render a ggplot2-built mtcars scatter plot as terminal text.
 *
 * This is intentionally a CLI/display demo: ggplot2 builds the plot and scale
 * data in embedded R; the final string is an ASCII canvas so DuckDB's CLI can
 * print it directly as a VARCHAR result.
 */
const char *r_ggplot2_cli_plot(void) {
    static int r_initialized = 0;
    static char out[32768];

    if (!r_initialized) {
        char *argv[] = {(char *)"ducktinycc-ggplot2-cli", (char *)"--silent", (char *)"--no-save"};
        Rf_initEmbeddedR((int)(sizeof(argv) / sizeof(argv[0])), argv);
        r_initialized = 1;
    }

    const char *rcode =
        "{\n"
        "  suppressPackageStartupMessages(library(ggplot2))\n"
        "  p <- ggplot(mtcars, aes(wt, mpg, colour = factor(cyl))) +\n"
        "    geom_point(size = 3) +\n"
        "    labs(title = 'ggplot2 in DuckDB CLI', subtitle = 'SQL -> TinyCC C UDF -> embedded R -> ggplot2') +\n"
        "    theme_minimal()\n"
        "  d <- ggplot_build(p)$data[[1]]\n"
        "  w <- 72L; h <- 22L\n"
        "  canvas <- matrix(' ', h, w)\n"
        "  xr <- range(d$x, finite = TRUE); yr <- range(d$y, finite = TRUE)\n"
        "  xi <- pmax(1L, pmin(w, 1L + round((d$x - xr[1]) / diff(xr) * (w - 1L))))\n"
        "  yi <- pmax(1L, pmin(h, h - round((d$y - yr[1]) / diff(yr) * (h - 1L))))\n"
        "  esc <- intToUtf8(27L)\n"
        "  rgb <- grDevices::col2rgb(d$colour)\n"
        "  point <- sprintf('%s[38;2;%d;%d;%dm●%s[0m', esc, rgb[1,], rgb[2,], rgb[3,], esc)\n"
        "  for (i in seq_along(xi)) canvas[yi[i], xi[i]] <- point[i]\n"
        "  # Add simple axes and ticks after plotting.\n"
        "  canvas[h, ] <- '-'\n"
        "  canvas[, 1] <- '|'\n"
        "  canvas[h, 1] <- '+'\n"
        "  for (i in seq_along(xi)) canvas[yi[i], xi[i]] <- point[i]\n"
        "  lines <- apply(canvas, 1L, paste0, collapse = '')\n"
        "  sample_cols <- tapply(d$colour, d$group, function(z) z[1])\n"
        "  sample_rgb <- grDevices::col2rgb(sample_cols)\n"
        "  sample_txt <- sprintf('%s[38;2;%d;%d;%dm● cyl=%s%s[0m', esc, sample_rgb[1,], sample_rgb[2,], sample_rgb[3,], names(sample_cols), esc)\n"
        "  legend <- paste('legend:', paste(sample_txt, collapse = '  '), '; x = wt; y = mpg')\n"
        "  rng <- sprintf('wt %.2f..%.2f    mpg %.1f..%.1f', xr[1], xr[2], yr[1], yr[2])\n"
        "  paste(c('ggplot2 in DuckDB CLI: mtcars mpg vs wt', legend, rng, '', lines), collapse = '\\n')\n"
        "}\n";

    ParseStatus status;
    int err = 0;
    SEXP cmd = PROTECT(Rf_mkString(rcode));
    SEXP expr = PROTECT(R_ParseVector(cmd, -1, &status, R_NilValue));

    if (status != PARSE_OK || XLENGTH(expr) < 1) {
        snprintf(out, sizeof(out), "%s", "R parse error while building ggplot2 CLI plot");
        UNPROTECT(2);
        return out;
    }

    SEXP ans = PROTECT(R_tryEval(VECTOR_ELT(expr, 0), R_GlobalEnv, &err));
    if (err || TYPEOF(ans) != STRSXP || XLENGTH(ans) < 1 || STRING_ELT(ans, 0) == NA_STRING) {
        snprintf(out, sizeof(out), "%s", "R evaluation error while building ggplot2 CLI plot");
        UNPROTECT(3);
        return out;
    }

    snprintf(out, sizeof(out), "%s", CHAR(STRING_ELT(ans, 0)));
    UNPROTECT(3);
    return out;
}
