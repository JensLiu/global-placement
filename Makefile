.DEFAULT_GOAL := build

CMAKE ?= cmake
GDB ?= gdb
DEBUG ?= 0
ifeq ($(DEBUG),1)
BUILD_TYPE ?= Debug
else
BUILD_TYPE ?= Release
endif
# Keep caches, object files, and executables independent for each configuration.
# Recursive expansion also honors run-debug's target-specific BUILD_TYPE.
BUILD_DIR ?= build/$(BUILD_TYPE)
GENERATOR ?= Ninja
JOBS ?= $(nproc)
GP_DEPENDENCY_CACHE ?= $(abspath ../../OpenROAD/bazel-OpenROAD/external)
ODB ?= OpenROAD-flow-scripts/flow/results/nangate45/gcd/base/3_2_place_iop.odb
# ODB ?= OpenROAD-flow-scripts/flow/results/nangate45/picorv32/base/3_2_place_iop.odb
# fixed: post-IO placement; skip: preliminary placement without IO endpoints.
IO_MODE ?= fixed

.PHONY: all configure build run run-debug clean help

all: build

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DGP_DEPENDENCY_CACHE="$(GP_DEPENDENCY_CACHE)"

build: configure
	+$(CMAKE) --build "$(BUILD_DIR)" --target odb_hello --parallel "$(JOBS)"

run: build
	"$(BUILD_DIR)/odb_hello" "$(ODB)" --io-mode "$(IO_MODE)"

run-debug: BUILD_TYPE = Debug
run-debug: build
	$(GDB) --quiet --args "$(BUILD_DIR)/odb_hello" "$(ODB)" --io-mode "$(IO_MODE)"

clean:
	@if test -f "$(BUILD_DIR)/CMakeCache.txt"; then \
		$(CMAKE) --build "$(BUILD_DIR)" --target clean; \
	else \
		printf '%s\n' 'No configured build to clean.'; \
	fi

help:
	@printf '%s\n' \
		'make                 Configure and build the OpenDB hello-world program' \
		'make DEBUG=1         Build Debug in build/Debug (default: build/Release)' \
		'make run DEBUG=1     Build and run in Debug mode (without a debugger)' \
		'make run-debug       Build Debug and open GDB; type run to start' \
		'make run-debug ODB=path  Debug another compatible ODB file' \
		'make run             Run the default post-IO checkpoint with fixed IO anchors' \
		'make run IO_MODE=fixed ODB=path  Run an IO-aware placement input' \
		'make run IO_MODE=skip ODB=path   Run a pre-IO placement input' \
		'make clean           Clean selected configuration; retain the other builds' \
		'Overrides: DEBUG (0/1), BUILD_DIR, BUILD_TYPE, GENERATOR, JOBS, CMAKE, GDB, GP_DEPENDENCY_CACHE, ODB, IO_MODE (fixed|skip)'
