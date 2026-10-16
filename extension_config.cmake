# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(hf
	SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
	INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/include
)
