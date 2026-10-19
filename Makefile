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

# The DuckDB version this extension is built *for*. It has to be stated out loud: the loader compares a
# version string baked into the artifact with its own, and the alpha tags are not published upstream, so a
# checkout of the right commit cannot describe itself as that release (the CI-side of the same argument, plus
# its workaround, is in .github/workflows/build.yml). Left unset when CI provides its own pin - CI is the one
# that knows which duckdb it built with, and an env var here would outrank its OVERRIDE_GIT_DESCRIBE.
#
# Without it a local build produces an artifact stamped with the duckdb commit sha, which the pre-release
# duckdb refuses to load - and the failure is silent in the wrong direction: `make` succeeds.
ifeq ($(origin DUCKDB_VERSION),undefined)
ifeq ($(origin OVERRIDE_GIT_DESCRIBE),undefined)
DUCKDB_VERSION = v2.0.0-alpha47600
export DUCKDB_VERSION
endif
endif

# Only the extension itself is built; test deps are pulled in ad hoc
BUILD_EXTENSION_TEST_DEPS ?= none

include $(CI_TOOLS_DIR)/makefiles/duckdb_extension.Makefile
