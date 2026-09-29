#include "duckdb_extension.h"
DUCKDB_EXTENSION_GLOBAL
#define DUCKTINYCC_WASM_UNSUPPORTED 1
#include "../../src/tcc_module.c"
#undef duckdb_malloc
#undef duckdb_free

#include "greatest.h"
#include "theft.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROP_TRIALS 1000u
#define PROP_SEED UINT64_C(0xd17c0ffee1234567)
#define PROP_TOKEN_CAP 16384u
#define PROP_TEXT_MAX 1024u
#define PROP_GEN_DEPTH 5u

struct prop_type {
	bool composite;
	char text[PROP_TOKEN_CAP];
};

struct prop_bytes {
	size_t len;
	uint8_t data[];
};

struct prop_buf {
	char *data;
	size_t cap;
	size_t len;
	bool failed;
};

static void prop_init_api(void) {
	duckdb_ext_api.duckdb_malloc = malloc;
	duckdb_ext_api.duckdb_free = free;
}

static size_t prop_trial_count(void) {
	const char *value = getenv("DUCKTINYCC_PROP_TRIALS");
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !value[0]) {
		return PROP_TRIALS;
	}
	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno || !end || *end || parsed == 0 || parsed > SIZE_MAX) {
		return PROP_TRIALS;
	}
	return (size_t)parsed;
}

static theft_seed prop_seed(void) {
	const char *value = getenv("DUCKTINYCC_PROP_SEED");
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !value[0]) {
		return (theft_seed)PROP_SEED;
	}
	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno || !end || *end) {
		return (theft_seed)PROP_SEED;
	}
	return (theft_seed)parsed;
}

static bool prop_fork(void) {
	const char *value = getenv("DUCKTINYCC_PROP_FORK");

	return value && value[0] && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

static uint64_t prop_bounded(struct theft *t, uint64_t limit) {
	uint64_t value;

	if (limit <= 1) {
		return 0;
	}
	value = theft_random_bits(t, 16);
	if (limit > UINT64_C(65536)) {
		value |= theft_random_bits(t, 16) << 16;
	}
	return value % limit;
}

static void prop_append(struct prop_buf *buf, const char *format, ...) {
	va_list args;
	int written;
	size_t available;

	if (buf->failed) {
		return;
	}
	available = buf->cap - buf->len;
	va_start(args, format);
	written = vsnprintf(buf->data + buf->len, available, format, args);
	va_end(args);
	if (written < 0 || (size_t)written >= available) {
		buf->failed = true;
		return;
	}
	buf->len += (size_t)written;
}

static void prop_emit_primitive(struct theft *t, struct prop_buf *buf) {
	static const char *const values[] = {
	    "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32",
	    "f64", "ptr", "varchar", "blob", "uuid", "date", "time", "timestamp", "interval", "decimal"};

	prop_append(buf, "%s", values[prop_bounded(t, sizeof(values) / sizeof(values[0]))]);
}

static void prop_emit_type(struct theft *t, struct prop_buf *buf, unsigned int depth, bool force_composite) {
	unsigned int count;
	unsigned int i;

	if (!force_composite && (depth >= PROP_GEN_DEPTH || prop_bounded(t, 4) == 0)) {
		prop_emit_primitive(t, buf);
		return;
	}
	switch (prop_bounded(t, 5)) {
	case 0:
		prop_append(buf, "list<");
		prop_emit_type(t, buf, depth + 1, false);
		prop_append(buf, ">");
		break;
	case 1:
		prop_emit_type(t, buf, depth + 1, false);
		prop_append(buf, "[%u]", 1u + (unsigned int)prop_bounded(t, 16));
		break;
	case 2:
		count = 1u + (unsigned int)prop_bounded(t, 2);
		prop_append(buf, "struct<");
		for (i = 0; i < count; i++) {
			prop_append(buf, "%sf%u:", i ? ";" : "", i);
			prop_emit_type(t, buf, depth + 1, false);
		}
		prop_append(buf, ">");
		break;
	case 3:
		prop_append(buf, "map<");
		prop_emit_primitive(t, buf);
		prop_append(buf, ";");
		prop_emit_type(t, buf, depth + 1, false);
		prop_append(buf, ">");
		break;
	default:
		count = 1u + (unsigned int)prop_bounded(t, 2);
		prop_append(buf, "union<");
		for (i = 0; i < count; i++) {
			prop_append(buf, "%sm%u:", i ? ";" : "", i);
			prop_emit_type(t, buf, depth + 1, false);
		}
		prop_append(buf, ">");
		break;
	}
}

static enum theft_alloc_res prop_type_alloc(struct theft *t, void *env, void **instance) {
	struct prop_type *out;
	struct prop_buf buf;

	(void)env;
	out = calloc(1, sizeof(*out));
	if (!out) {
		return THEFT_ALLOC_ERROR;
	}
	buf.data = out->text;
	buf.cap = sizeof(out->text);
	buf.len = 0;
	buf.failed = false;
	out->composite = prop_bounded(t, 4) != 0;
	prop_emit_type(t, &buf, 0, out->composite);
	if (buf.failed || buf.len == 0) {
		free(out);
		return THEFT_ALLOC_ERROR;
	}
	*instance = out;
	return THEFT_ALLOC_OK;
}

static void prop_free(void *instance, void *env) {
	(void)env;
	free(instance);
}

static void prop_type_print(FILE *file, const void *instance, void *env) {
	const struct prop_type *type = instance;

	(void)env;
	fprintf(file, "%s\n", type->text);
}

static enum theft_alloc_res prop_bytes_alloc(struct theft *t, void *env, void **instance) {
	struct prop_bytes *out;
	size_t len;
	size_t i;

	(void)env;
	len = (size_t)prop_bounded(t, PROP_TEXT_MAX + 1u);
	out = malloc(sizeof(*out) + len);
	if (!out) {
		return THEFT_ALLOC_ERROR;
	}
	out->len = len;
	for (i = 0; i < len; i++) {
		out->data[i] = (uint8_t)theft_random_bits(t, 8);
	}
	*instance = out;
	return THEFT_ALLOC_OK;
}

static void prop_bytes_print(FILE *file, const void *instance, void *env) {
	const struct prop_bytes *bytes = instance;
	size_t i;

	(void)env;
	fprintf(file, "len=%zu", bytes->len);
	for (i = 0; i < bytes->len; i++) {
		fprintf(file, "%s%02x", i % 16 ? " " : "\n", bytes->data[i]);
	}
	fprintf(file, "\n");
}

static struct theft_type_info prop_type_info = {
	.alloc = prop_type_alloc,
	.free = prop_free,
	.print = prop_type_print,
	.autoshrink_config = {.enable = true},
};

static struct theft_type_info prop_bytes_info = {
	.alloc = prop_bytes_alloc,
	.free = prop_free,
	.print = prop_bytes_print,
	.autoshrink_config = {.enable = true},
};

static bool prop_typedesc_equal(const tcc_typedesc_t *left, const tcc_typedesc_t *right, unsigned int depth) {
	idx_t i;

	if (!left || !right || depth > 64 || left->kind != right->kind || left->ffi_type != right->ffi_type ||
	    left->array_size != right->array_size || !left->token || !right->token || strcmp(left->token, right->token) != 0) {
		return false;
	}
	switch (left->kind) {
	case TCC_TYPEDESC_PRIMITIVE:
		return true;
	case TCC_TYPEDESC_LIST:
	case TCC_TYPEDESC_ARRAY:
		return prop_typedesc_equal(left->as.list_like.child, right->as.list_like.child, depth + 1);
	case TCC_TYPEDESC_STRUCT:
		if (left->as.struct_like.count != right->as.struct_like.count) {
			return false;
		}
		for (i = 0; i < left->as.struct_like.count; i++) {
			if (strcmp(left->as.struct_like.fields[i].name, right->as.struct_like.fields[i].name) != 0 ||
			    !prop_typedesc_equal(left->as.struct_like.fields[i].type, right->as.struct_like.fields[i].type,
			                         depth + 1)) {
				return false;
			}
		}
		return true;
	case TCC_TYPEDESC_MAP:
		return prop_typedesc_equal(left->as.map_like.key, right->as.map_like.key, depth + 1) &&
		       prop_typedesc_equal(left->as.map_like.value, right->as.map_like.value, depth + 1);
	case TCC_TYPEDESC_UNION:
		if (left->as.union_like.count != right->as.union_like.count) {
			return false;
		}
		for (i = 0; i < left->as.union_like.count; i++) {
			if (strcmp(left->as.union_like.members[i].name, right->as.union_like.members[i].name) != 0 ||
			    !prop_typedesc_equal(left->as.union_like.members[i].type, right->as.union_like.members[i].type,
			                         depth + 1)) {
				return false;
			}
		}
		return true;
	default:
		return false;
	}
}

static enum theft_trial_res prop_valid_type_roundtrips(struct theft *t, void *arg1) {
	const struct prop_type *type = arg1;
	tcc_typedesc_t *first = NULL;
	tcc_typedesc_t *second = NULL;
	tcc_error_buffer_t error_buf;
	bool equal;

	(void)t;
	memset(&error_buf, 0, sizeof(error_buf));
	if (!tcc_typedesc_parse_token(type->text, true, &first, &error_buf) || !first) {
		return THEFT_TRIAL_FAIL;
	}
	memset(&error_buf, 0, sizeof(error_buf));
	if (!tcc_typedesc_parse_token(first->token, true, &second, &error_buf) || !second) {
		tcc_typedesc_destroy(first);
		return THEFT_TRIAL_FAIL;
	}
	equal = prop_typedesc_equal(first, second, 0);
	tcc_typedesc_destroy(second);
	tcc_typedesc_destroy(first);
	return equal ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_truncated_composite_rejects(struct theft *t, void *arg1) {
	const struct prop_type *type = arg1;
	tcc_typedesc_t *desc = NULL;
	tcc_error_buffer_t error_buf;
	char text[PROP_TOKEN_CAP];
	size_t len;
	bool accepted;

	(void)t;
	if (!type->composite) {
		return THEFT_TRIAL_SKIP;
	}
	len = strlen(type->text);
	if (len < 2 || len >= sizeof(text)) {
		return THEFT_TRIAL_FAIL;
	}
	memcpy(text, type->text, len);
	text[len - 1] = '\0';
	memset(&error_buf, 0, sizeof(error_buf));
	accepted = tcc_typedesc_parse_token(text, true, &desc, &error_buf);
	if (desc) {
		tcc_typedesc_destroy(desc);
	}
	return !accepted && !desc ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_codegen_accepts_valid_type(struct theft *t, void *arg1) {
	const struct prop_type *type = arg1;
	tcc_module_bind_data_t bind;
	tcc_module_state_t state;
	tcc_codegen_signature_ctx_t signature;
	tcc_codegen_source_ctx_t source;
	tcc_error_buffer_t error_buf;
	bool ok;

	(void)t;
	memset(&bind, 0, sizeof(bind));
	bind.return_type = (char *)type->text;
	bind.arg_types = (char *)type->text;
	bind.source = "long long prop_target(long long x){ return x; }";
	bind.symbol = "prop_target";
	bind.sql_name = "prop_target";
	bind.wrapper_mode = "row";
	bind.stability = "consistent";
	memset(&error_buf, 0, sizeof(error_buf));
	tcc_codegen_signature_ctx_init(&signature);
	ok = tcc_codegen_signature_parse_types(&bind, &signature, &error_buf);
	tcc_codegen_signature_ctx_destroy(&signature);
	if (!ok) {
		return THEFT_TRIAL_FAIL;
	}
	memset(&state, 0, sizeof(state));
	state.session.state_id = 1;
	state.session.config_version = 1;
	memset(&error_buf, 0, sizeof(error_buf));
	tcc_codegen_source_ctx_init(&source);
	ok = tcc_codegen_prepare_sources(&state, &bind, bind.sql_name, bind.symbol, &source, &error_buf) &&
	     source.wrapper_loader_source && source.compilation_unit_source && source.module_symbol[0] &&
	     strstr(source.wrapper_loader_source, "ducktinycc_register_signature");
	tcc_codegen_source_ctx_destroy(&source);
	return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_random_text_is_deterministic(struct theft *t, void *arg1) {
	const struct prop_bytes *bytes = arg1;
	tcc_typedesc_t *left = NULL;
	tcc_typedesc_t *right = NULL;
	tcc_string_list_t left_csv;
	tcc_string_list_t right_csv;
	tcc_error_buffer_t error_buf;
	char *text;
	bool left_ok;
	bool right_ok;
	bool equal = true;
	size_t i;

	(void)t;
	text = malloc(bytes->len + 1);
	if (!text) {
		return THEFT_TRIAL_ERROR;
	}
	for (i = 0; i < bytes->len; i++) {
		text[i] = bytes->data[i] ? (char)bytes->data[i] : '?';
	}
	text[bytes->len] = '\0';
	memset(&error_buf, 0, sizeof(error_buf));
	left_ok = tcc_typedesc_parse_token(text, true, &left, &error_buf);
	memset(&error_buf, 0, sizeof(error_buf));
	right_ok = tcc_typedesc_parse_token(text, true, &right, &error_buf);
	if (left_ok != right_ok || (left_ok && !prop_typedesc_equal(left, right, 0)) || (!left_ok && (left || right))) {
		equal = false;
	}
	tcc_typedesc_destroy(left);
	tcc_typedesc_destroy(right);
	memset(&left_csv, 0, sizeof(left_csv));
	memset(&right_csv, 0, sizeof(right_csv));
	memset(&error_buf, 0, sizeof(error_buf));
	left_ok = tcc_split_csv_tokens(text, &left_csv, &error_buf);
	memset(&error_buf, 0, sizeof(error_buf));
	right_ok = tcc_split_csv_tokens(text, &right_csv, &error_buf);
	if (left_ok != right_ok || left_csv.count != right_csv.count) {
		equal = false;
	} else {
		for (i = 0; i < (size_t)left_csv.count; i++) {
			if (strcmp(left_csv.items[i], right_csv.items[i]) != 0) {
				equal = false;
				break;
			}
		}
	}
	tcc_string_list_destroy(&left_csv);
	tcc_string_list_destroy(&right_csv);
	free(text);
	return equal ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_run_res prop_run(const char *name, theft_propfun1 function, const struct theft_type_info *info) {
	struct theft_run_config config;

	memset(&config, 0, sizeof(config));
	config.name = name;
	config.prop1 = function;
	config.type_info[0] = info;
	config.trials = prop_trial_count();
	config.seed = prop_seed();
	config.fork.enable = prop_fork();
	return theft_run(&config);
}

TEST generated_types_roundtrip(void) {
	ASSERT_EQ(THEFT_RUN_PASS, prop_run("generated recursive types round-trip", prop_valid_type_roundtrips,
	                                   &prop_type_info));
	PASS();
}

TEST truncated_composites_reject(void) {
	ASSERT_EQ(THEFT_RUN_PASS, prop_run("truncated composite types reject", prop_truncated_composite_rejects,
	                                   &prop_type_info));
	PASS();
}

TEST generated_types_reach_codegen(void) {
	ASSERT_EQ(THEFT_RUN_PASS, prop_run("generated types reach wrapper codegen", prop_codegen_accepts_valid_type,
	                                   &prop_type_info));
	PASS();
}

TEST random_text_is_deterministic(void) {
	ASSERT_EQ(THEFT_RUN_PASS, prop_run("random parser input is deterministic", prop_random_text_is_deterministic,
	                                   &prop_bytes_info));
	PASS();
}

static bool prop_nested_list(char *out, size_t capacity, unsigned int depth) {
	struct prop_buf buf;
	unsigned int i;

	memset(out, 0, capacity);
	buf.data = out;
	buf.cap = capacity;
	buf.len = 0;
	buf.failed = false;
	for (i = 0; i < depth; i++) {
		prop_append(&buf, "list<");
	}
	prop_append(&buf, "i64");
	for (i = 0; i < depth; i++) {
		prop_append(&buf, ">");
	}
	return !buf.failed;
}

TEST recursive_depth_limit_is_exact(void) {
	char text[1024];
	tcc_typedesc_t *desc = NULL;
	tcc_error_buffer_t error_buf;

	ASSERT(prop_nested_list(text, sizeof(text), 63));
	memset(&error_buf, 0, sizeof(error_buf));
	ASSERT(tcc_typedesc_parse_token(text, true, &desc, &error_buf));
	ASSERT(desc);
	tcc_typedesc_destroy(desc);
	desc = NULL;
	ASSERT(prop_nested_list(text, sizeof(text), 64));
	memset(&error_buf, 0, sizeof(error_buf));
	ASSERT_FALSE(tcc_typedesc_parse_token(text, true, &desc, &error_buf));
	ASSERT_FALSE(desc);
	PASS();
}

SUITE(properties) {
	RUN_TEST(generated_types_roundtrip);
	RUN_TEST(truncated_composites_reject);
	RUN_TEST(generated_types_reach_codegen);
	RUN_TEST(random_text_is_deterministic);
	RUN_TEST(recursive_depth_limit_is_exact);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
	GREATEST_MAIN_BEGIN();
	prop_init_api();
	RUN_SUITE(properties);
	GREATEST_MAIN_END();
}
