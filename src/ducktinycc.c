#include "duckdb_extension.h"
#include "tcc_module.h"
#include <stdatomic.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

typedef struct {
	duckdb_database database;
	duckdb_connection connection;
	bool module_registered;
} ducktinycc_registry_entry_t;

static ducktinycc_registry_entry_t *g_registry_entries = NULL;
static idx_t g_registry_count = 0;
static idx_t g_registry_capacity = 0;
static atomic_flag g_registry_lock = ATOMIC_FLAG_INIT;

static void ducktinycc_registry_lock(void) {
	while (atomic_flag_test_and_set_explicit(&g_registry_lock, memory_order_acquire)) {
	}
}

static void ducktinycc_registry_unlock(void) {
	atomic_flag_clear_explicit(&g_registry_lock, memory_order_release);
}

static idx_t ducktinycc_registry_find(duckdb_database database) {
	idx_t i;
	for (i = 0; i < g_registry_count; i++) {
		if (g_registry_entries[i].database == database) {
			return i;
		}
	}
	return (idx_t)-1;
}

static bool ducktinycc_registry_reserve(idx_t wanted) {
	ducktinycc_registry_entry_t *new_entries;
	idx_t new_capacity;
	if (g_registry_capacity >= wanted) {
		return true;
	}
	new_capacity = g_registry_capacity == 0 ? 4 : g_registry_capacity * 2;
	while (new_capacity < wanted) {
		new_capacity *= 2;
	}
	new_entries = (ducktinycc_registry_entry_t *)duckdb_malloc(sizeof(ducktinycc_registry_entry_t) * new_capacity);
	if (!new_entries) {
		return false;
	}
	memset(new_entries, 0, sizeof(ducktinycc_registry_entry_t) * new_capacity);
	if (g_registry_entries && g_registry_count > 0) {
		memcpy(new_entries, g_registry_entries, sizeof(ducktinycc_registry_entry_t) * g_registry_count);
		duckdb_free(g_registry_entries);
	}
	g_registry_entries = new_entries;
	g_registry_capacity = new_capacity;
	return true;
}

DUCKDB_EXTENSION_ENTRYPOINT_CUSTOM(duckdb_extension_info info, struct duckdb_extension_access *access) {
	duckdb_database database = NULL;
	idx_t idx;
	ducktinycc_registry_entry_t *entry;
	if (access && info) {
		duckdb_database *db_ptr = access->get_database(info);
		if (db_ptr) {
			database = *db_ptr;
		}
	}
	if (!database) {
		if (access) {
			access->set_error(info, "failed to get database handle");
		}
		return false;
	}

	ducktinycc_registry_lock();
	idx = ducktinycc_registry_find(database);
	if (idx == (idx_t)-1) {
		if (!ducktinycc_registry_reserve(g_registry_count + 1)) {
			ducktinycc_registry_unlock();
			if (access) {
				access->set_error(info, "failed to grow extension registry");
			}
			return false;
		}
		idx = g_registry_count++;
		memset(&g_registry_entries[idx], 0, sizeof(g_registry_entries[idx]));
		g_registry_entries[idx].database = database;
	}
	entry = &g_registry_entries[idx];
	if (!entry->connection) {
		if (duckdb_connect(database, &entry->connection) == DuckDBError || !entry->connection) {
			ducktinycc_registry_unlock();
			if (access) {
				access->set_error(info, "failed to open persistent extension connection");
			}
			return false;
		}
	}
	if (!entry->module_registered) {
		if (!RegisterTccModuleFunction(entry->connection, database)) {
			ducktinycc_registry_unlock();
			if (access) {
				access->set_error(info, "failed to register ducktinycc module functions");
			}
			return false;
		}
		entry->module_registered = true;
	}
	ducktinycc_registry_unlock();

	return true;
}
