PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hf
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Two layouts are supported:
#   * CI / standard extension template: ./duckdb and ./extension-ci-tools are checked out next to
#     this Makefile (the distribution workflow does exactly that),
#   * local development: both are vendored one level up in ../vendor (see STATE.md), which keeps the
#     ~1 GB duckdb checkout out of this repository.
# A repo-local checkout always wins; override with DUCKDB_SRCDIR=/path/to/duckdb if you need to.
ifeq ($(wildcard ${PROJ_DIR}duckdb/.),)
DUCKDB_SRCDIR ?= ../vendor/duckdb
else
DUCKDB_SRCDIR ?= ./duckdb
endif

ifeq ($(wildcard ${PROJ_DIR}extension-ci-tools/.),)
CI_TOOLS_DIR := ../vendor/extension-ci-tools
else
CI_TOOLS_DIR := ./extension-ci-tools
endif

# Only the extension itself is built; test deps are pulled in ad hoc
BUILD_EXTENSION_TEST_DEPS ?= none

include $(CI_TOOLS_DIR)/makefiles/duckdb_extension.Makefile
