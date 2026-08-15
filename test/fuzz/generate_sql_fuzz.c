#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fuzz_state;

static uint64_t fuzz_next(void) {
	uint64_t x = fuzz_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	fuzz_state = x;
	return x;
}

static unsigned long parse_ulong(const char *value, const char *label) {
	char *end = NULL;
	unsigned long out;

	errno = 0;
	out = strtoul(value, &end, 0);
	if (errno || !end || *end != '\0') {
		fprintf(stderr, "invalid %s: %s\n", label, value);
		exit(2);
	}
	return out;
}

static void sql_string(const char *value) {
	const unsigned char *p = (const unsigned char *)value;

	putchar('\'');
	while (*p) {
		if (*p == '\'') {
			putchar('\'');
		}
		putchar((int)*p++);
	}
	putchar('\'');
}

static void random_text(char *out, size_t capacity) {
	static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_<>[]:;,(){}+-*/' #\\\t";
	size_t limit = capacity > 1 ? capacity - 1 : 0;
	size_t len = limit ? (size_t)(fuzz_next() % (limit + 1)) : 0;
	size_t i;

	for (i = 0; i < len; i++) {
		out[i] = alphabet[fuzz_next() % (sizeof(alphabet) - 1)];
	}
	out[len] = '\0';
}

static void random_type(char *out, size_t capacity, unsigned long trial) {
	static const char *const valid[] = {
	    "i64", "f64", "varchar", "blob", "i32[]", "list<struct<x:i64;y:f64>>", "i64[3]",
	    "struct<a:i64;b:list<u8>>", "map<i64;varchar>", "union<a:i64;b:struct<x:f64>>"};
	static const char *const malformed[] = {
	    "", "list<>", "list<i64", "i64[0]", "i64[184467440737095516160]", "struct<a:>",
	    "map<i64>", "union<a:i64;a:f64>", "struct<:i64>", "list<list<list<list<list<list<list<list<"};
	uint64_t choice = fuzz_next() % 5;
	const char *src;

	if (trial == 0) {
		size_t i;
		out[0] = '\0';
		for (i = 0; i < 80 && strlen(out) + 6 < capacity; i++) {
			strcat(out, "list<");
		}
		strcat(out, "i32");
		for (i = 0; i < 80 && strlen(out) + 2 < capacity; i++) {
			strcat(out, ">");
		}
		return;
	}
	if (choice <= 1) {
		src = valid[fuzz_next() % (sizeof(valid) / sizeof(valid[0]))];
		snprintf(out, capacity, "%s", src);
	} else if (choice == 2) {
		src = malformed[fuzz_next() % (sizeof(malformed) / sizeof(malformed[0]))];
		snprintf(out, capacity, "%s", src);
	} else {
		random_text(out, capacity);
	}
}

static void emit_preview(unsigned long trial) {
	char return_type[1024];
	char arg_types[3][512];
	char wrapper_mode[128];
	char stability[128];
	unsigned int argc = (unsigned int)(fuzz_next() % 4);
	unsigned int i;

	random_type(return_type, sizeof(return_type), trial);
	for (i = 0; i < argc; i++) {
		random_type(arg_types[i], sizeof(arg_types[i]), trial + i + 1);
	}
	if ((fuzz_next() & 3) != 0) {
		snprintf(wrapper_mode, sizeof(wrapper_mode), "%s", (fuzz_next() & 1) ? "row" : "chunk_scalar_loop");
	} else {
		random_text(wrapper_mode, sizeof(wrapper_mode));
	}
	if ((fuzz_next() & 3) != 0) {
		snprintf(stability, sizeof(stability), "%s", (fuzz_next() & 1) ? "consistent" : "volatile");
	} else {
		random_text(stability, sizeof(stability));
	}

	printf("SELECT count(*) FROM tcc_module(mode := 'codegen_preview', source := ");
	sql_string("long long fuzz_preview(long long x){ return x; }");
	printf(", symbol := 'fuzz_preview', sql_name := 'fuzz_preview', return_type := ");
	sql_string(return_type);
	printf(", arg_types := [");
	for (i = 0; i < argc; i++) {
		if (i) {
			printf(",");
		}
		sql_string(arg_types[i]);
	}
	printf("], wrapper_mode := ");
	sql_string(wrapper_mode);
	printf(", stability := ");
	sql_string(stability);
	printf(");\nSELECT 4242;\n");
}

static void emit_compile(unsigned long trial) {
	char source[1024];
	char symbol[64];

	snprintf(symbol, sizeof(symbol), "fuzz_compile_%lu", trial);
	if (trial % 50 == 0) {
		snprintf(source, sizeof(source), "long long %s(long long x){ return x + %lu; }", symbol, trial);
	} else {
		random_text(source, sizeof(source));
	}
	printf("SELECT count(*) FROM tcc_module(mode := 'quick_compile', source := ");
	sql_string(source);
	printf(", symbol := ");
	sql_string(symbol);
	printf(", sql_name := ");
	sql_string(symbol);
	printf(", return_type := 'i64', arg_types := ['i64']);\n");
	if (trial % 50 == 0) {
		printf("SELECT CASE WHEN %s(7) = %lu THEN 1 ELSE error('valid fuzz UDF mismatch') END;\n", symbol,
		       trial + 7);
	}
	printf("SELECT 4242;\n");
}

static void emit_composite_properties(void) {
	const char *list_source =
	    "long long fuzz_list_sum(ducktinycc_list_t a){ unsigned long long i; long long out=0; for(i=0;i<a.len;i++){ "
	    "const long long *p=(const long long *)ducktinycc_list_elem_ptr(&a,i,sizeof(long long)); if(p && "
	    "ducktinycc_list_is_valid(&a,i)) out+=*p; } return out; }";
	const char *array_source =
	    "long long fuzz_array_sum(ducktinycc_array_t a){ unsigned long long i; long long out=0; for(i=0;i<a.len;i++){ "
	    "const long long *p=(const long long *)ducktinycc_array_elem_ptr(&a,i,sizeof(long long)); if(p && "
	    "ducktinycc_array_is_valid(&a,i)) out+=*p; } return out; }";
	const char *map_source =
	    "long long fuzz_map_sum(ducktinycc_map_t m){ unsigned long long i; long long out=0; for(i=0;i<m.len;i++){ "
	    "const long long *k=(const long long *)ducktinycc_map_key_ptr(&m,i,sizeof(long long)); const long long "
	    "*v=(const long long *)ducktinycc_map_value_ptr(&m,i,sizeof(long long)); if(k && v && "
	    "ducktinycc_map_key_is_valid(&m,i) && ducktinycc_map_value_is_valid(&m,i)) out+=*k+*v; } return out; }";

	printf("SELECT count(*) FROM tcc_module(mode := 'quick_compile', source := ");
	sql_string(list_source);
	printf(", symbol := 'fuzz_list_sum', sql_name := 'fuzz_list_sum', return_type := 'i64', arg_types := ['i64[]']);\n");
	printf("SELECT CASE WHEN list(g ORDER BY n) = [3,7] THEN 1 ELSE error('LIST offset property failed') END FROM (SELECT n, fuzz_list_sum(v) g FROM (VALUES (1,[1::BIGINT,2]),(2,[3::BIGINT,4])) t(n,v));\n");

	printf("SELECT count(*) FROM tcc_module(mode := 'quick_compile', source := ");
	sql_string(array_source);
	printf(", symbol := 'fuzz_array_sum', sql_name := 'fuzz_array_sum', return_type := 'i64', arg_types := ['i64[2]']);\n");
	printf("SELECT CASE WHEN list(g ORDER BY n) = [3,7] THEN 1 ELSE error('ARRAY offset property failed') END FROM (SELECT n, fuzz_array_sum(v) g FROM (VALUES (1,[1::BIGINT,2]::BIGINT[2]),(2,[3::BIGINT,4]::BIGINT[2])) t(n,v));\n");

	printf("SELECT count(*) FROM tcc_module(mode := 'quick_compile', source := ");
	sql_string(map_source);
	printf(", symbol := 'fuzz_map_sum', sql_name := 'fuzz_map_sum', return_type := 'i64', arg_types := ['map<i64;i64>']);\n");
	printf("SELECT CASE WHEN list(g ORDER BY n) = [3,7] THEN 1 ELSE error('MAP offset property failed') END FROM (SELECT n, fuzz_map_sum(v) g FROM (VALUES (1,MAP([1::BIGINT],[2::BIGINT])),(2,MAP([3::BIGINT],[4::BIGINT]))) t(n,v));\n");
	printf("SELECT 4242;\n");
}

int main(int argc, char **argv) {
	unsigned long seed;
	unsigned long trials;
	unsigned long trial;

	if (argc != 4) {
		fprintf(stderr, "usage: %s EXTENSION SEED TRIALS\n", argv[0]);
		return 2;
	}
	seed = parse_ulong(argv[2], "seed");
	trials = parse_ulong(argv[3], "trials");
	fuzz_state = seed ? (uint64_t)seed : UINT64_C(0x9e3779b97f4a7c15);

	printf("LOAD ");
	sql_string(argv[1]);
	printf(";\n");
	emit_composite_properties();
	for (trial = 0; trial < trials; trial++) {
		emit_preview(trial);
		if (trial % 5 == 0) {
			emit_compile(trial);
		}
	}
	printf("SELECT 'DUCKTINYCC_SQL_FUZZ_OK seed=%lu trials=%lu';\n", seed, trials);
	return 0;
}
