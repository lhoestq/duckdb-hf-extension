cmake_minimum_required(VERSION 3.5...3.29)

project(HFExtension)

add_extension_definitions()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wpessimizing-move)
endif()

include_directories(src/include)
add_subdirectory(src)

set(EXTENSION_SOURCES ${ALL_OBJECT_FILES})

build_static_extension(hf ${EXTENSION_SOURCES})

set(PARAMETERS "-warnings")
build_loadable_extension(hf ${PARAMETERS} ${EXTENSION_SOURCES})

install(
  TARGETS hf_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
