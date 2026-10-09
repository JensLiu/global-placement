.DEFAULT_GOAL := build

CMAKE ?= cmake
BUILD_DIR ?= build
BUILD_TYPE ?= Release
GENERATOR ?= Ninja
JOBS ?= 6
GP_DEPENDENCY_CACHE ?= $(abspath ../../OpenROAD/bazel-OpenROAD/external)
ODB ?= OpenROAD-flow-scripts/flow/results/nangate45/gcd/base/2_floorplan.odb

.PHONY: all configure build run clean help

all: build

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DGP_DEPENDENCY_CACHE="$(GP_DEPENDENCY_CACHE)"

build: configure
	+$(CMAKE) --build "$(BUILD_DIR)" --target odb_hello --parallel "$(JOBS)"

run: build
	"$(BUILD_DIR)/odb_hello" "$(ODB)"

clean:
	@if test -f "$(BUILD_DIR)/CMakeCache.txt"; then \
		$(CMAKE) --build "$(BUILD_DIR)" --target clean; \
	else \
		printf '%s\n' 'No configured build to clean.'; \
	fi

help:
	@printf '%s\n' \
		'make                 Configure and build the OpenDB hello-world program' \
		'make run             Read the default GCD floorplan and print basic information' \
		'make run ODB=path    Read another compatible ODB file' \
		'make clean           Remove compiled outputs; retain configuration' \
		'Overrides: BUILD_DIR, BUILD_TYPE, GENERATOR, JOBS, CMAKE, GP_DEPENDENCY_CACHE, ODB'
