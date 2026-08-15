#include "duckdb_extension.h"
DUCKDB_EXTENSION_GLOBAL

/* The harness owns only extension-side parsing/codegen.  It neither links nor
 * executes TinyCC modules; the complete-extension SQL campaign covers that path. */
#define DUCKTINYCC_WASM_UNSUPPORTED 1
#include "../../src/tcc_module.c"

#undef duckdb_malloc
#undef duckdb_free

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_init_api(void) {
	static int initialized;

	if (initialized) {
		return;
	}
	duckdb_ext_api.duckdb_malloc = malloc;
	duckdb_ext_api.duckdb_free = free;
	initialized = 1;
}

static void fuzz_validate_typedesc(const tcc_typedesc_t *root, size_t input_size) {
	const tcc_typedesc_t **stack;
	size_t capacity;
	size_t count = 0;
	size_t nodes = 0;

	if (!root || input_size > SIZE_MAX - 16) {
		abort();
	}
	capacity = input_size + 16;
	stack = (const tcc_typedesc_t **)malloc(sizeof(*stack) * capacity);
	if (!stack) {
		abort();
	}
	stack[count++] = root;
	while (count > 0) {
		const tcc_typedesc_t *desc = stack[--count];
		idx_t i;

		if (!desc || !desc->token || desc->token[0] == '\0' || ++nodes > capacity) {
			abort();
		}
		switch (desc->kind) {
		case TCC_TYPEDESC_PRIMITIVE:
			if (tcc_typedesc_is_composite(desc)) {
				abort();
			}
			break;
		case TCC_TYPEDESC_LIST:
			if (!tcc_ffi_type_is_list(desc->ffi_type) || !desc->as.list_like.child || count == capacity) {
				abort();
			}
			stack[count++] = desc->as.list_like.child;
			break;
		case TCC_TYPEDESC_ARRAY:
			if (!tcc_ffi_type_is_array(desc->ffi_type) || desc->array_size == 0 || !desc->as.list_like.child ||
			    count == capacity) {
				abort();
			}
			stack[count++] = desc->as.list_like.child;
			break;
		case TCC_TYPEDESC_STRUCT:
			if (desc->ffi_type != TCC_FFI_STRUCT || desc->as.struct_like.count == 0 ||
			    !desc->as.struct_like.fields || desc->as.struct_like.count > capacity - count) {
				abort();
			}
			for (i = 0; i < desc->as.struct_like.count; i++) {
				if (!tcc_is_identifier_token(desc->as.struct_like.fields[i].name) ||
				    !desc->as.struct_like.fields[i].type) {
					abort();
				}
				stack[count++] = desc->as.struct_like.fields[i].type;
			}
			break;
		case TCC_TYPEDESC_MAP:
			if (desc->ffi_type != TCC_FFI_MAP || !desc->as.map_like.key || !desc->as.map_like.value ||
			    capacity - count < 2) {
				abort();
			}
			stack[count++] = desc->as.map_like.key;
			stack[count++] = desc->as.map_like.value;
			break;
		case TCC_TYPEDESC_UNION:
			if (desc->ffi_type != TCC_FFI_UNION || desc->as.union_like.count == 0 ||
			    !desc->as.union_like.members || desc->as.union_like.count > capacity - count) {
				abort();
			}
			for (i = 0; i < desc->as.union_like.count; i++) {
				if (!tcc_is_identifier_token(desc->as.union_like.members[i].name) ||
				    !desc->as.union_like.members[i].type) {
					abort();
				}
				stack[count++] = desc->as.union_like.members[i].type;
			}
			break;
		default:
			abort();
		}
	}
	free(stack);
}

static void fuzz_typedesc(const char *text, size_t size) {
	tcc_typedesc_t *desc = NULL;
	tcc_typedesc_t *roundtrip = NULL;
	tcc_error_buffer_t error_buf;
	bool accepted;

	memset(&error_buf, 0, sizeof(error_buf));
	accepted = tcc_typedesc_parse_token(text, true, &desc, &error_buf);
	if (!accepted) {
		if (desc) {
			abort();
		}
		return;
	}
	fuzz_validate_typedesc(desc, size);
	memset(&error_buf, 0, sizeof(error_buf));
	if (!tcc_typedesc_parse_token(desc->token, true, &roundtrip, &error_buf) || !roundtrip) {
		abort();
	}
	fuzz_validate_typedesc(roundtrip, size);
	tcc_typedesc_destroy(roundtrip);
	tcc_typedesc_destroy(desc);
}

static void fuzz_csv_and_helpers(const char *text, bool force_bitfield) {
	tcc_string_list_t tokens;
	tcc_c_field_list_t fields;
	tcc_string_list_t constants;
	tcc_error_buffer_t error_buf;
	idx_t i;

	memset(&tokens, 0, sizeof(tokens));
	memset(&error_buf, 0, sizeof(error_buf));
	if (tcc_split_csv_tokens(text, &tokens, &error_buf)) {
		for (i = 0; i < tokens.count; i++) {
			tcc_typedesc_t *desc = NULL;
			memset(&error_buf, 0, sizeof(error_buf));
			if (tcc_typedesc_parse_token(tokens.items[i], false, &desc, &error_buf)) {
				fuzz_validate_typedesc(desc, strlen(tokens.items[i]));
				tcc_typedesc_destroy(desc);
			}
		}
	}
	tcc_string_list_destroy(&tokens);

	memset(&fields, 0, sizeof(fields));
	memset(&error_buf, 0, sizeof(error_buf));
	(void)tcc_parse_c_field_specs(text, force_bitfield, &fields, &error_buf);
	for (i = 0; i < fields.count; i++) {
		if (!tcc_is_identifier_token(fields.items[i].name) || fields.items[i].type == TCC_FFI_VOID) {
			abort();
		}
	}
	tcc_c_field_list_destroy(&fields);

	memset(&constants, 0, sizeof(constants));
	memset(&error_buf, 0, sizeof(error_buf));
	(void)tcc_parse_c_enum_constants(text, &constants, &error_buf);
	for (i = 0; i < constants.count; i++) {
		if (!tcc_is_identifier_token(constants.items[i])) {
			abort();
		}
	}
	tcc_string_list_destroy(&constants);
}

static void fuzz_signature_and_codegen(char *text, const uint8_t *data, size_t size) {
	tcc_module_bind_data_t bind;
	tcc_module_state_t state;
	tcc_codegen_signature_ctx_t signature;
	tcc_codegen_source_ctx_t source;
	tcc_error_buffer_t error_buf;
	tcc_wrapper_mode_t wrapper_mode;
	tcc_function_stability_t stability;
	char *args = strchr(text, '\n');

	if (args) {
		*args++ = '\0';
	} else {
		args = text + strlen(text);
	}
	memset(&bind, 0, sizeof(bind));
	bind.return_type = text;
	bind.arg_types = args;
	bind.source = "long long fuzz_target(long long x){ return x; }";
	bind.symbol = "fuzz_target";
	bind.sql_name = "fuzz_target";
	bind.wrapper_mode = size > 0 && (data[0] & 1) ? "chunk_scalar_loop" : "row";
	bind.stability = size > 0 && (data[0] & 2) ? "volatile" : "consistent";

	memset(&error_buf, 0, sizeof(error_buf));
	tcc_codegen_signature_ctx_init(&signature);
	if (tcc_codegen_signature_parse_types(&bind, &signature, &error_buf)) {
		if (signature.arg_count < 0 || (signature.arg_count > 0 && (!signature.arg_types || !signature.arg_array_sizes))) {
			abort();
		}
	}
	tcc_codegen_signature_ctx_destroy(&signature);

	memset(&state, 0, sizeof(state));
	state.session.state_id = 1;
	state.session.config_version = 1;
	memset(&error_buf, 0, sizeof(error_buf));
	tcc_codegen_source_ctx_init(&source);
	if (tcc_codegen_prepare_sources(&state, &bind, bind.sql_name, bind.symbol, &source, &error_buf)) {
		if (!source.wrapper_loader_source || !source.compilation_unit_source || source.module_symbol[0] == '\0' ||
		    !strstr(source.wrapper_loader_source, "ducktinycc_register_signature")) {
			abort();
		}
	}
	tcc_codegen_source_ctx_destroy(&source);

	memset(&error_buf, 0, sizeof(error_buf));
	(void)tcc_parse_wrapper_mode(text, &wrapper_mode, &error_buf);
	memset(&error_buf, 0, sizeof(error_buf));
	(void)tcc_parse_function_stability(text, &stability, &error_buf);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	char *text;
	size_t i;

	fuzz_init_api();
	if (size > 65536 || size == SIZE_MAX) {
		return 0;
	}
	text = (char *)malloc(size + 1);
	if (!text) {
		abort();
	}
	for (i = 0; i < size; i++) {
		text[i] = data[i] == 0 ? '?' : (char)data[i];
	}
	text[size] = '\0';

	fuzz_typedesc(text, size);
	fuzz_csv_and_helpers(text, size > 0 && (data[0] & 4));
	fuzz_signature_and_codegen(text, data, size);
	free(text);
	return 0;
}
