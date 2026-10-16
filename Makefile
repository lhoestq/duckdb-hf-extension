PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hf
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# The duckdb checkout and the ci tooling are vendored one level up (see STATE.md)
DUCKDB_SRCDIR ?= ../vendor/duckdb

# Only the extension itself is built; test deps are pulled in ad hoc
BUILD_EXTENSION_TEST_DEPS ?= none

include ../vendor/extension-ci-tools/makefiles/duckdb_extension.Makefile
