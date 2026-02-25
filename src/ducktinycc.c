#include "duckdb_extension.h"
#include "tcc_module.h"

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
	(void)info;
	(void)access;

	RegisterTccModuleFunction(connection);
	return true;
}
