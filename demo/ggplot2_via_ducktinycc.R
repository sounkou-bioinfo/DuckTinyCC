#!/usr/bin/env Rscript
# ============================================================================
# ggplot2 via DuckTinyCC — SQL -> compiled C trampoline -> R plot
# ============================================================================
#
# This demo registers a DuckDB scalar UDF that calls an R function through a
# TinyCC-compiled C trampoline. The SQL call creates a ggplot2 PNG.
#
# Requirements:
#   - R packages: DBI, duckdb, ggplot2
#   - Built ducktinycc extension (make release)
#
# Usage:
#   Rscript demo/ggplot2_via_ducktinycc.R [output.png]
#
# Notes:
#   - R is single-threaded; the demo sets PRAGMA threads=1.
#   - The R callback is evaluated with R_tryEval, so ordinary R errors are
#     caught instead of long-jumping through DuckDB frames.
#   - This is not a sandbox: generated C still runs in-process.
# ============================================================================

suppressPackageStartupMessages({
  library(DBI)
  library(duckdb)
  library(ggplot2)
})

# ---- Step 0: C helper for R object/function addresses ----------------------
#
# R does not expose arbitrary pointer-to-integer conversion in base R. Compile a
# tiny helper that returns decimal strings for the R C API function addresses we
# inject into TinyCC, plus helpers for SEXP addresses and object preservation.

helper_src <- '
#include <R.h>
#include <Rinternals.h>
#include <stdint.h>
#include <stdio.h>

static SEXP addr_string(uintptr_t ptr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)ptr);
    return Rf_mkString(buf);
}

SEXP C_sexp_addr(SEXP x) {
    return addr_string((uintptr_t)x);
}

SEXP C_preserve(SEXP x) {
    R_PreserveObject(x);
    return R_NilValue;
}

SEXP C_r_api_addrs(void) {
    const char *names[] = {
        "R_tryEval",
        "Rf_lang2",
        "Rf_mkString",
        "Rf_protect",
        "Rf_unprotect"
    };
    uintptr_t addrs[] = {
        (uintptr_t)&R_tryEval,
        (uintptr_t)&Rf_lang2,
        (uintptr_t)&Rf_mkString,
        (uintptr_t)&Rf_protect,
        (uintptr_t)&Rf_unprotect
    };
    const int n = (int)(sizeof(names) / sizeof(names[0]));
    SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, n));
    for (int i = 0; i < n; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)addrs[i]);
        SET_STRING_ELT(out, i, Rf_mkChar(buf));
        SET_STRING_ELT(nms, i, Rf_mkChar(names[i]));
    }
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}
'

tmp_c <- tempfile(fileext = ".c")
tmp_so <- sub("\\.c$", .Platform$dynlib.ext, tmp_c)
writeLines(helper_src, tmp_c)
rc <- system2("R", c("CMD", "SHLIB", "-o", tmp_so, tmp_c), stdout = FALSE, stderr = FALSE)
if (rc != 0) {
  stop("R CMD SHLIB failed — is a C compiler available?")
}
dyn.load(tmp_so)

sexp_addr <- function(x) .Call("C_sexp_addr", x)
preserve <- function(x) invisible(.Call("C_preserve", x))
r_api_addrs <- function() as.list(.Call("C_r_api_addrs"))

cat("=== ggplot2 via DuckTinyCC ===\n\n")

# ---- Step 1: Connect to DuckDB and load ducktinycc -------------------------

ext_path <- Sys.getenv("DUCKTINYCC_EXT", unset = "build/release/ducktinycc.duckdb_extension")
out_path <- if (length(commandArgs(trailingOnly = TRUE)) >= 1) {
  commandArgs(trailingOnly = TRUE)[[1]]
} else {
  file.path(tempdir(), "ducktinycc_ggplot2_demo.png")
}
out_path <- normalizePath(out_path, mustWork = FALSE)

drv <- duckdb(config = list("allow_unsigned_extensions" = "true"))
con <- dbConnect(drv)
on.exit({
  try(dbDisconnect(con, shutdown = TRUE), silent = TRUE)
  try(file.remove(c(tmp_c, tmp_so)), silent = TRUE)
}, add = TRUE)

invisible(dbExecute(con, sprintf("LOAD %s", dbQuoteString(con, ext_path))))
invisible(dbExecute(con, "PRAGMA threads=1"))
cat("[1] DuckDB + ducktinycc loaded\n")

# ---- Step 2: Define and pin the R plotting function ------------------------

plot_fun <- function(path) {
  p <- ggplot(mtcars, aes(wt, mpg, color = factor(cyl))) +
    geom_point(size = 3) +
    labs(
      title = "ggplot2 generated from a DuckDB SQL UDF",
      subtitle = "SQL -> DuckTinyCC C trampoline -> R -> ggplot2",
      x = "Weight (1000 lbs)",
      y = "Miles per gallon",
      color = "cyl"
    ) +
    theme_minimal(base_size = 13)
  ggsave(path, p, width = 7, height = 4.5, dpi = 140)
  invisible(path)
}
preserve(plot_fun)
cat("[2] R plotting callback pinned\n")

# ---- Step 3: Inject R C API symbols and callback object --------------------

symbols <- c(
  r_api_addrs(),
  list(
    R_PLOT_FUN = sexp_addr(plot_fun),
    R_GLOBALENV = sexp_addr(globalenv())
  )
)

invisible(dbGetQuery(con, "SELECT * FROM tcc_module(mode := 'tcc_new_state')"))
for (nm in names(symbols)) {
  sql <- sprintf(
    "SELECT ok, code, message FROM tcc_module(mode := 'add_symbol', symbol_name := %s, symbol_ptr := %s::UBIGINT)",
    dbQuoteString(con, nm),
    symbols[[nm]]
  )
  res <- dbGetQuery(con, sql)
  if (!isTRUE(res$ok[[1]])) {
    stop(sprintf("add_symbol '%s' failed: %s", nm, res$message[[1]]))
  }
}
cat("[3] R C API symbols injected into TinyCC\n")

# ---- Step 4: Compile varchar -> varchar C trampoline -----------------------
#
# No libc calls are used here. The output path is copied into a static buffer by
# hand because DuckTinyCC compiles with -nostdlib unless libraries are requested.

trampoline_source <- "
typedef void *SEXP;

typedef SEXP (*fn_tryEval)(SEXP, SEXP, int *);
typedef SEXP (*fn_lang2)(SEXP, SEXP);
typedef SEXP (*fn_mkString)(const char *);
typedef SEXP (*fn_protect)(SEXP);
typedef void (*fn_unprotect)(int);

extern char R_tryEval[];
extern char Rf_lang2[];
extern char Rf_mkString[];
extern char Rf_protect[];
extern char Rf_unprotect[];
extern char R_PLOT_FUN[];
extern char R_GLOBALENV[];

const char *r_ggplot2_png(const char *path) {
    static char out[1024];
    fn_tryEval p_tryEval = (fn_tryEval)(void *)R_tryEval;
    fn_lang2 p_lang2 = (fn_lang2)(void *)Rf_lang2;
    fn_mkString p_mkString = (fn_mkString)(void *)Rf_mkString;
    fn_protect p_protect = (fn_protect)(void *)Rf_protect;
    fn_unprotect p_unprotect = (fn_unprotect)(void *)Rf_unprotect;
    SEXP fun = (SEXP)(void *)R_PLOT_FUN;
    SEXP env = (SEXP)(void *)R_GLOBALENV;
    int err = 0;
    unsigned long i = 0;

    if (!path) {
        return (const char *)0;
    }
    while (path[i] && i + 1 < sizeof(out)) {
        out[i] = path[i];
        i++;
    }
    out[i] = 0;

    SEXP arg = p_protect(p_mkString(out));
    SEXP call = p_protect(p_lang2(fun, arg));
    (void)p_tryEval(call, env, &err);
    p_unprotect(2);
    if (err) {
        return (const char *)0;
    }
    return out;
}
"

compile_sql <- sprintf(
  "SELECT ok, mode, code, message, detail
   FROM tcc_module(
     mode := 'quick_compile',
     source := %s,
     symbol := 'r_ggplot2_png',
     sql_name := 'r_ggplot2_png',
     return_type := 'varchar',
     arg_types := ['varchar'],
     stability := 'volatile'
   )",
  dbQuoteString(con, trampoline_source)
)
res <- dbGetQuery(con, compile_sql)
if (!isTRUE(res$ok[[1]])) {
  print(res)
  stop("failed to compile r_ggplot2_png")
}
cat("[4] C trampoline compiled -> SQL function 'r_ggplot2_png' registered\n")

# ---- Step 5: Call from SQL and verify output -------------------------------

call_sql <- sprintf("SELECT r_ggplot2_png(%s) AS png", dbQuoteString(con, out_path))
out <- dbGetQuery(con, call_sql)
print(out)

if (is.na(out$png[[1]]) || !file.exists(out_path) || file.info(out_path)$size <= 0) {
  stop("ggplot2 PNG was not created")
}

cat(sprintf("\n✓ ggplot2 PNG created: %s (%d bytes)\n", out_path, file.info(out_path)$size))
