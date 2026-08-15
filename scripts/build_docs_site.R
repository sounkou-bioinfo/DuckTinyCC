#!/usr/bin/env Rscript

if (!requireNamespace("litedown", quietly = TRUE)) {
  stop("Package 'litedown' is required to build the documentation site", call. = FALSE)
}

pages <- c(
  index = "README.md",
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
expected <- sort(unname(pages[names(pages) != "index"]))
if (!identical(actual, expected)) {
  stop(
    paste0(
      "Documentation sources must remain the three-page curated reference set.\nExpected: ",
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

asset_source <- "man/figures/README-ggplot2-cli.svg"
asset_dir <- file.path(site_dir, "man", "figures")
if (!file.exists(asset_source)) {
  stop("Rendered README asset is missing: ", asset_source, call. = FALSE)
}
dir.create(asset_dir, recursive = TRUE, showWarnings = FALSE)
if (!file.copy(asset_source, asset_dir, overwrite = TRUE)) {
  stop("Could not copy rendered README asset: ", asset_source, call. = FALSE)
}

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

required <- c(
  file.path(site_dir, paste0(names(pages), ".html")),
  file.path(asset_dir, basename(asset_source))
)
missing <- required[!file.exists(required) | file.info(required)$size == 0]
if (length(missing) > 0L) {
  stop("Site build did not produce: ", paste(missing, collapse = ", "), call. = FALSE)
}
