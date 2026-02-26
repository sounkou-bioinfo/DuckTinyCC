#include "duckdb_extension.h"
#include "tcc_module.h"

static duckdb_connection g_ducktinycc_connection = NULL;

DUCKDB_EXTENSION_ENTRYPOINT_CUSTOM(duckdb_extension_info info, struct duckdb_extension_access *access) {
	duckdb_database database = NULL;
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
	if (!g_ducktinycc_connection) {
		if (duckdb_connect(database, &g_ducktinycc_connection) == DuckDBError || !g_ducktinycc_connection) {
			if (access) {
				access->set_error(info, "failed to open persistent extension connection");
			}
			return false;
		}
	}

	RegisterTccModuleFunction(g_ducktinycc_connection, database);
	return true;
}
