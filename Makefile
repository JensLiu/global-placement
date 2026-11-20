.DEFAULT_GOAL := build

CMAKE ?= cmake
GDB ?= gdb
PYTHON ?= python3
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
JOBS ?= $(shell nproc)
GP_DEPENDENCY_CACHE ?= $(abspath ../../OpenROAD/bazel-OpenROAD/external)
# Design selection: any ORFS design with results up to 3_2 (gcd, picorv32, bp, ariane136, ...).
# Name it as an extra goal, e.g. `make run gcd`, `make bench picorv32`; a unique
# prefix also works (`make bench picorv`). DESIGN=... still works.
PLATFORM ?= nangate45
ORFS_FLOW ?= OpenROAD-flow-scripts/flow
COMMAND_GOALS := all configure build run run-debug clean help bench bench-reference designs full
DESIGNS := $(sort $(notdir $(wildcard $(ORFS_FLOW)/results/$(PLATFORM)/*)))
DESIGN_GOALS := $(filter-out $(COMMAND_GOALS),$(MAKECMDGOALS))
resolve_design = $(or $(filter $(1),$(DESIGNS)),$(filter $(1)%,$(DESIGNS)))
RESOLVED_DESIGNS := $(strip $(foreach goal,$(DESIGN_GOALS),$(call resolve_design,$(goal))))
ifneq ($(DESIGN_GOALS),)
ifneq ($(words $(RESOLVED_DESIGNS)),$(words $(DESIGN_GOALS)))
$(error Unknown or ambiguous design '$(DESIGN_GOALS)'. Available ($(PLATFORM)): $(DESIGNS))
endif
ifneq ($(words $(RESOLVED_DESIGNS)),1)
$(error Name one design per invocation, got: $(RESOLVED_DESIGNS))
endif
DESIGN := $(RESOLVED_DESIGNS)
# The design words are selectors, not real targets.
.PHONY: $(DESIGN_GOALS)
$(DESIGN_GOALS):
	@:
endif
DESIGN ?= picorv32
ODB ?= $(ORFS_FLOW)/results/$(PLATFORM)/$(DESIGN)/base/3_2_place_iop.odb
# Frame image formats: png = preview, pdf = vector (sharp at any zoom), svg also possible.
IMAGE_FORMATS ?= png pdf
# 0 chooses up to eight rendering processes; 1 restores serial export.
RENDER_JOBS ?= 0
# fixed: post-IO placement; skip: preliminary placement without IO endpoints.
IO_MODE ?= fixed
# A fresh directory per invocation: the collector never overwrites a run.
# Set TRACE_DIR= to disable collection and image export, or choose a new path explicitly.
# Named <design>-<unique timestamp>, e.g. build/traces/picorv32-20261120-130501-123456789.
TRACE_DIR ?= build/traces/$(DESIGN)-$(shell date +%Y%m%d-%H%M%S-%N)
BENCH_REF = "$(PYTHON)" scripts/bench.py reference --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)"

.PHONY: all configure build run run-debug clean help bench bench-reference designs full

designs:
	@printf '%s\n' $(DESIGNS)

all: build

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DGP_DEPENDENCY_CACHE="$(GP_DEPENDENCY_CACHE)"

build: configure
	+$(CMAKE) --build "$(BUILD_DIR)" --target odb_hello --parallel "$(JOBS)"

run: build
	@trace_dir="$(TRACE_DIR)"; \
	if test -n "$$trace_dir"; then printf 'Placement trace: %s\n' "$$trace_dir"; fi; \
	GP_TRACE_DIR="$$trace_dir" "$(BUILD_DIR)/odb_hello" "$(ODB)" --io-mode "$(IO_MODE)" || exit $$?; \
	if test -n "$$trace_dir"; then \
		"$(PYTHON)" scripts/generate_placement_images.py "$$trace_dir" --jobs "$(RENDER_JOBS)" --bbox --format $(IMAGE_FORMATS) || exit $$?; \
		"$(PYTHON)" scripts/summarise_trace.py "$$trace_dir" --plot "$$trace_dir/metrics.png"; \
	fi

# OpenROAD references, computed once per design and cached in build/reference/.
bench-reference:
	$(BENCH_REF)

# Place, write the global placement, legalise with OpenROAD, and print the scorecard.
# Everything in one folder: one placer run (trace + written placement), the
# per-frame images and metrics dashboard of `make run`, then the OpenROAD
# legalisation, scorecard and comparison images of `make bench`.
full: build
	@trace_dir="$(TRACE_DIR)"; \
	test -n "$$trace_dir" || { echo "full needs a TRACE_DIR"; exit 1; }; \
	printf 'Run folder: %s\n' "$$trace_dir"; \
	GP_TRACE_DIR="$$trace_dir" "$(BUILD_DIR)/odb_hello" "$(ODB)" --io-mode "$(IO_MODE)" \
		--write-db "$$trace_dir/global_placement.odb" || exit $$?; \
	"$(PYTHON)" scripts/generate_placement_images.py "$$trace_dir" --jobs "$(RENDER_JOBS)" --bbox --format $(IMAGE_FORMATS) > /dev/null || exit $$?; \
	"$(PYTHON)" scripts/summarise_trace.py "$$trace_dir" --plot "$$trace_dir/metrics.png" || exit $$?; \
	$(BENCH_REF) || exit $$?; \
	"$(PYTHON)" scripts/bench.py evaluate "$$trace_dir" --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)" || exit $$?; \
	"$(PYTHON)" scripts/bench.py report "$$trace_dir" --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)" || exit $$?; \
	"$(PYTHON)" scripts/bench.py visualise "$$trace_dir" --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)" > /dev/null || exit $$?; \
	printf '\nRun folder: %s\n  images/          per-frame placement images\n  metrics.png      per-iteration dashboard\n  bench/compare_layout.png, bench/compare_metrics.png  comparison with OpenROAD\n' "$$trace_dir"

bench: build
	@trace_dir="$(TRACE_DIR)"; \
	test -n "$$trace_dir" || { echo "bench needs a TRACE_DIR"; exit 1; }; \
	printf 'Placement trace: %s\n' "$$trace_dir"; \
	GP_TRACE_DIR="$$trace_dir" "$(BUILD_DIR)/odb_hello" "$(ODB)" --io-mode "$(IO_MODE)" \
		--write-db "$$trace_dir/global_placement.odb" || exit $$?; \
	$(BENCH_REF) || exit $$?; \
	"$(PYTHON)" scripts/bench.py evaluate "$$trace_dir" --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)" || exit $$?; \
	"$(PYTHON)" scripts/summarise_trace.py "$$trace_dir" --plot "$$trace_dir/metrics.png" > /dev/null || exit $$?; \
	"$(PYTHON)" scripts/bench.py report "$$trace_dir" --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)" || exit $$?; \
	"$(PYTHON)" scripts/bench.py visualise "$$trace_dir" --design "$(DESIGN)" --platform "$(PLATFORM)" --orfs "$(ORFS_FLOW)"

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
		'make run gcd         Run another design (any of: make designs); prefixes work' \
		'make bench gcd       Place, legalise with OpenROAD, scorecard vs references' \
		'make full gcd        run + bench in one folder (one placer run)' \
		'make bench-reference gcd  Build cached OpenROAD references once' \
		'make designs         List designs with ORFS results' \
		'make run             Run placement, collect JSON and export images in a fresh build/traces/ directory' \
		'make run TRACE_DIR=path  Collect JSON and export images in a new directory of your choice' \
		'make run TRACE_DIR=  Run without collecting JSON or exporting images' \
		'make run PYTHON=path  Select the Python interpreter (requires Matplotlib)' \
		'make run RENDER_JOBS=8  Render up to eight frames concurrently (0 = auto, 1 = serial)' \
		'make run IO_MODE=fixed ODB=path  Run an IO-aware placement input' \
		'make run IO_MODE=skip ODB=path   Run a pre-IO placement input' \
		'make clean           Clean selected configuration; retain the other builds' \
		'Overrides: DEBUG (0/1), BUILD_DIR, BUILD_TYPE, GENERATOR, JOBS, CMAKE, GDB, PYTHON, GP_DEPENDENCY_CACHE, ODB, IO_MODE (fixed|skip), TRACE_DIR, IMAGE_FORMATS, RENDER_JOBS'
