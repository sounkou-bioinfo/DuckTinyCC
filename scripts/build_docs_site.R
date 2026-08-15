#!/usr/bin/env Rscript

if (!requireNamespace("litedown", quietly = TRUE)) {
  stop("Package 'litedown' is required to build the documentation site", call. = FALSE)
}

pages <- c(
  index = "docs/index.md",
  reference = "docs/reference.md",
  internals = "docs/internals.md",
  development = "docs/development.md"
)
titles <- c(
  index = "DuckTinyCC",
  reference = "SQL reference · DuckTinyCC",
  internals = "Internals and ownership · DuckTinyCC",
  development = "Development · DuckTinyCC"
)

actual <- sort(list.files("docs", pattern = "[.]md$", full.names = TRUE))
expected <- sort(unname(pages))
if (!identical(actual, expected)) {
  stop(
    paste0(
      "Documentation sources must remain the four-page curated set.\nExpected: ",
      paste(expected, collapse = ", "),
      "\nActual: ",
      paste(actual, collapse = ", ")
    ),
    call. = FALSE
  )
}

site_dir <- "_site"
unlink(site_dir, recursive = TRUE, force = TRUE)
dir.create(site_dir, recursive = TRUE, showWarnings = FALSE)
invisible(file.create(file.path(site_dir, ".nojekyll")))

css <- normalizePath("tools/site.css", winslash = "/", mustWork = TRUE)
header <- normalizePath("tools/site-header.html", winslash = "/", mustWork = TRUE)

metadata <- c(
  "---",
  "output:",
  "  html:",
  "    options:",
  "      toc: true",
  "    meta:",
  paste0(
    "      css: [\"@default@1.14.69\", \"@article@1.14.69\", ",
    "\"@site@1.14.69\", \"", css, "\"]"
  ),
  paste0("      include_before: \"", header, "\""),
  "---"
)

for (name in names(pages)) {
  source <- pages[[name]]
  markdown <- readLines(source, warn = FALSE, encoding = "UTF-8")
  destination <- file.path(site_dir, paste0(name, ".html"))
  message("rendering ", source, " -> ", destination)
  litedown::mark(
    text = c(metadata, markdown),
    output = destination,
    meta = list("plain-title" = titles[[name]])
  )
}

required <- file.path(site_dir, paste0(names(pages), ".html"))
missing <- required[!file.exists(required) | file.info(required)$size == 0]
if (length(missing) > 0L) {
  stop("Site build did not produce: ", paste(missing, collapse = ", "), call. = FALSE)
}
