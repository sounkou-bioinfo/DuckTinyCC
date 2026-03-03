#!/usr/bin/env Rscript
# ============================================================================
# R UDF via DuckTinyCC — Proof of Concept
# ============================================================================
#
# Registers an arbitrary R function as a DuckDB scalar UDF by:
#   1. Extracting R C API function pointers from the running R process
#   2. Injecting them into TCC via add_symbol
#   3. Compiling a C trampoline that calls back into R
#
# Unlike demo/embed_r_demo.sh (which links libR), this approach works from
# *inside* a running R session — no library linking required.
#
# Requirements:
#   - R with the 'duckdb' package (install.packages("duckdb"))
#   - Built ducktinycc extension (make release)
#
# Usage:
#   Rscript demo/r_udf_via_ducktinycc.R
#
# Limitations:
#   - R is single-threaded; run with PRAGMA threads=1
#   - The R function SEXP is pinned (R_PreserveObject) for GC safety
#   - Only double→double signature demonstrated; extend the trampoline
#     generator for other types
# ============================================================================

library(DBI)
library(duckdb)

# ---- Step 0: Tiny C helper to extract raw pointer addresses from R ---------
#
# We need to pass R C API addresses as UBIGINT to DuckDB's add_symbol.
# R doesn't expose pointer-to-integer conversion in base R, so we compile
# a minimal helper via R CMD SHLIB and dyn.load it.

helper_src <- '
#include <R.h>
#include <Rinternals.h>
#include <stdint.h>

/* Return the SEXP address of any R object as a decimal string. */
SEXP C_sexp_addr(SEXP x) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(uintptr_t)x);
    return Rf_mkString(buf);
}

/* Return the raw address inside an external pointer as a decimal string. */
SEXP C_extptr_addr(SEXP x) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu",
             (unsigned long long)(uintptr_t)R_ExternalPtrAddr(x));
    return Rf_mkString(buf);
}

/* Pin an R object so GC will not collect it. */
SEXP C_preserve(SEXP x) {
    R_PreserveObject(x);
    return R_NilValue;
}
'

tmp_c <- tempfile(fileext = ".c")
tmp_so <- sub("\\.c$", .Platform$dynlib.ext, tmp_c)
writeLines(helper_src, tmp_c)
rc <- system2("R", c("CMD", "SHLIB", "-o", tmp_so, tmp_c),
    stdout = FALSE, stderr = FALSE
)
if (rc != 0) stop("R CMD SHLIB failed — is a C compiler available?")
dyn.load(tmp_so)

sexp_addr <- function(x) .Call("C_sexp_addr", x)
extptr_addr <- function(x) .Call("C_extptr_addr", x)
preserve <- function(x) invisible(.Call("C_preserve", x))

r_sym_addr <- function(name) {
    info <- tryCatch(getNativeSymbolInfo(name), error = function(e) NULL)
    if (is.null(info)) stop(sprintf("R C API symbol '%s' not found", name))
    extptr_addr(info$address)
}

cat("=== R UDF via DuckTinyCC ===\n\n")

# ---- Step 1: Connect to DuckDB and load ducktinycc ------------------------

ext_path <- Sys.getenv("DUCKTINYCC_EXT",
    unset = "build/release/ducktinycc.duckdb_extension"
)
drv <- duckdb(config = list("allow_unsigned_extensions" = "true"))
con <- dbConnect(drv)
dbExecute(con, sprintf("LOAD '%s'", ext_path))
# R is single-threaded; prevent DuckDB from calling the UDF from worker threads
dbExecute(con, "PRAGMA threads=1")
cat("[1] DuckDB + ducktinycc loaded\n")

# ---- Step 2: Define the R function to expose as a SQL UDF ------------------

my_r_fun <- function(x) {
    x * 2 + 1
}

# Pin so GC cannot collect it while TCC code holds the SEXP pointer
preserve(my_r_fun)
cat("[2] R function defined: f(x) = x * 2 + 1\n")

# ---- Step 3: Gather R C API symbol addresses -------------------------------
#
# We need pointers to:
#   R_tryEval(call, env, &err)  — safe eval (no longjmp on error)
#   Rf_lang2(fn, arg)           — build a 1-arg call: fn(arg)
#   Rf_ScalarReal(double)       — wrap double → SEXP
#   Rf_asReal(SEXP)             — unwrap SEXP → double
#   Rf_protect(SEXP)            — GC protect
#   Rf_unprotect(int)           — GC unprotect
#   R_FUN                       — the R function SEXP itself
#   R_GLOBALENV                 — evaluation environment

symbols <- list(
    R_tryEval     = r_sym_addr("R_tryEval"),
    Rf_lang2      = r_sym_addr("Rf_lang2"),
    Rf_ScalarReal = r_sym_addr("Rf_ScalarReal"),
    Rf_asReal     = r_sym_addr("Rf_asReal"),
    Rf_protect    = r_sym_addr("Rf_protect"),
    Rf_unprotect  = r_sym_addr("Rf_unprotect"),
    R_FUN         = sexp_addr(my_r_fun),
    R_GLOBALENV   = sexp_addr(globalenv())
)

cat("[3] R C API addresses resolved:\n")
for (nm in names(symbols)) {
    cat(sprintf("      %-16s = %s\n", nm, symbols[[nm]]))
}

# ---- Step 4: Inject all symbols into TCC state ----------------------------

dbGetQuery(con, "SELECT * FROM tcc_module(mode := 'tcc_new_state')")

for (nm in names(symbols)) {
    sql <- sprintf(
        "SELECT * FROM tcc_module(mode := 'add_symbol', symbol_name := '%s', symbol_ptr := %s::UBIGINT)",
        nm, symbols[[nm]]
    )
    res <- dbGetQuery(con, sql)
    if (!res$ok) stop(sprintf("add_symbol '%s' failed: %s", nm, res$message))
}
cat("[4] All symbols injected into TCC state\n")

# ---- Step 5: Compile the C trampoline --------------------------------------
#
# The trampoline:
#   1. Receives a double from DuckDB (via the generated wrapper)
#   2. Wraps it as an R SEXP via Rf_ScalarReal
#   3. Builds a call expression via Rf_lang2(R_FUN, arg)
#   4. Evaluates via R_tryEval (safe — no longjmp on error)
#   5. Extracts the result double via Rf_asReal
#   6. Returns it to DuckDB

trampoline_source <- "
typedef void *SEXP;

/* R C API function pointer types */
typedef SEXP   (*fn_tryEval)(SEXP, SEXP, int *);
typedef SEXP   (*fn_lang2)(SEXP, SEXP);
typedef SEXP   (*fn_ScalarReal)(double);
typedef double (*fn_asReal)(SEXP);
typedef SEXP   (*fn_protect)(SEXP);
typedef void   (*fn_unprotect)(int);

/* Injected symbols (address-as-value pattern) */
extern char R_tryEval[];
extern char Rf_lang2[];
extern char Rf_ScalarReal[];
extern char Rf_asReal[];
extern char Rf_protect[];
extern char Rf_unprotect[];
extern char R_FUN[];
extern char R_GLOBALENV[];

double r_udf_trampoline(double x) {
    fn_tryEval    p_tryEval    = (fn_tryEval)(void *)R_tryEval;
    fn_lang2      p_lang2      = (fn_lang2)(void *)Rf_lang2;
    fn_ScalarReal p_ScalarReal = (fn_ScalarReal)(void *)Rf_ScalarReal;
    fn_asReal     p_asReal     = (fn_asReal)(void *)Rf_asReal;
    fn_protect    p_protect    = (fn_protect)(void *)Rf_protect;
    fn_unprotect  p_unprotect  = (fn_unprotect)(void *)Rf_unprotect;

    SEXP fun = (SEXP)(void *)R_FUN;
    SEXP env = (SEXP)(void *)R_GLOBALENV;

    int err = 0;
    SEXP arg    = p_protect(p_ScalarReal(x));
    SEXP call   = p_protect(p_lang2(fun, arg));
    SEXP result = p_protect(p_tryEval(call, env, &err));

    double ret = err ? 0.0 : p_asReal(result);
    p_unprotect(3);
    return ret;
}
"

res <- dbGetQuery(con, sprintf(
    "SELECT ok, mode, code, message, detail
     FROM tcc_module(
         mode := 'quick_compile',
         source := '%s',
         symbol := 'r_udf_trampoline',
         sql_name := 'r_udf',
         return_type := 'f64',
         arg_types := ['f64']
     )",
    gsub("'", "''", trampoline_source)
))

if (!res$ok) {
    cat(sprintf("[5] COMPILE FAILED: %s — %s\n", res$code, res$message))
    if (!is.na(res$detail)) cat(sprintf("    Detail: %s\n", res$detail))
    dbDisconnect(con, shutdown = TRUE)
    quit(status = 1)
}
cat("[5] C trampoline compiled → SQL function 'r_udf' registered\n")

# ---- Step 6: Call the R UDF from SQL! --------------------------------------

cat("\n--- Scalar tests ---\n")
test_values <- c(0, 1, 3.14, -5, 100)
all_ok <- TRUE
for (v in test_values) {
    result <- dbGetQuery(con, sprintf("SELECT r_udf(%s) AS y", v))
    expected <- my_r_fun(v)
    ok <- abs(result$y - expected) < 1e-10
    if (!ok) all_ok <- FALSE
    cat(sprintf(
        "  %s  r_udf(%6g) = %g  (expected %g)\n",
        if (ok) "\u2713" else "\u2717", v, result$y, expected
    ))
}

cat("\n--- Batch test (generate_series) ---\n")
batch <- dbGetQuery(con, "
    SELECT x, r_udf(x) AS y
    FROM generate_series(1, 5) t(x)
")
print(batch)

# ---- Cleanup ---------------------------------------------------------------

dbDisconnect(con, shutdown = TRUE)
file.remove(c(tmp_c, tmp_so))

if (all_ok) {
    cat("\n\u2713 All tests passed — R UDF via DuckTinyCC works!\n")
} else {
    cat("\n\u2717 Some tests failed.\n")
    quit(status = 1)
}
