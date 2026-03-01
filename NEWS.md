# DuckTinyCC Extension News

## ducktinycc 0.0.2.9000 (2026-03-01)

- document the actual SQL signature grammar used by `tcc_module(...)`, including scalar aliases and recursive composite tokens (`list<...>`, `type[]`, `type[N]`, `struct<...>`, `map<...>`, `union<...>`)
- document SQL to C bridge correspondences for scalar and composite signatures, including union support
- clarify runtime asset expectations for compile/codegen flows, including the role of `libtcc1.a`
- correct README "How It Works" details for host symbol injection and recursive bridge marshalling
- update community extension description text and add the public project reference URL

## ducktinycc 0.0.2 (2026-02-27)

- public SQL entrypoint `tcc_module(...)` with session/config, build staging, and compile/codegen modes
- compile paths: `compile`, `quick_compile`, `codegen_preview`, plus helper generators `c_struct`, `c_union`, `c_bitfield`, `c_enum`
- runtime diagnostics helpers: `tcc_system_paths(...)`, `tcc_library_probe(...)`
- pointer and buffer SQL helpers: `tcc_alloc`, `tcc_free_ptr`, `tcc_dataptr`, `tcc_ptr_size`, `tcc_ptr_add`, `tcc_read_*`, `tcc_write_*`
- in-memory TinyCC compile + relocate model with generated wrapper registration in DuckDB
