# Hyperlight + Nightcore Integration Makefile
#
# Targets:
#   make              - Build guest + worker, create artifacts
#   make nightcore    - Same as above
#   make guest        - Build only the guest binary
#   make worker       - Build only the worker library
#   make host         - Build standalone host example
#   make demo         - Build and start Nightcore stack
#   make clean        - Clean build artifacts
#   make clean-all    - Clean everything including outputs/
#   make print        - Display build variables

CARGO ?= cargo
PROFILE ?= release

GUEST_MANIFEST := guest/Cargo.toml
HOST_MANIFEST := host/Cargo.toml
WORKER_MANIFEST := worker/Cargo.toml

# Derive package names without external tools
GUEST_NAME := $(shell sed -n 's/^name = "\(.*\)"/\1/p' $(GUEST_MANIFEST) | head -n1)
HOST_NAME := $(shell sed -n 's/^name = "\(.*\)"/\1/p' $(HOST_MANIFEST) | head -n1)
WORKER_NAME := $(shell sed -n 's/^name = "\(.*\)"/\1/p' $(WORKER_MANIFEST) | head -n1)

# Guest build configuration (overridable)
GUEST_TARGET ?= x86_64-unknown-none
GUEST_RUSTFLAGS ?= -C panic=abort -C code-model=small -C link-args=-eentrypoint

# Use isolated target dir for guest to avoid jobserver/lock conflicts
GUEST_TARGET_DIR ?= $(CURDIR)/target/hyperlight-guest
HOST_TARGET_DIR ?= $(CURDIR)/target

ifeq ($(PROFILE),release)
CARGO_PROFILE_FLAG := --release
else
CARGO_PROFILE_FLAG :=
endif

GUEST_BIN := $(GUEST_TARGET_DIR)/$(GUEST_TARGET)/$(PROFILE)/$(GUEST_NAME)
HOST_BIN := $(HOST_TARGET_DIR)/$(PROFILE)/$(HOST_NAME)
# Cargo converts hyphens to underscores in library names
WORKER_LIB := $(HOST_TARGET_DIR)/$(PROFILE)/lib$(subst -,_,$(WORKER_NAME)).so

# Nightcore integration artifacts
GUEST_ARTIFACT := $(CURDIR)/guest.bin
WORKER_ARTIFACT := $(CURDIR)/libecho.so

.PHONY: all help guest host worker nightcore run demo clean clean-all print

all: nightcore

help:
	@echo "Hyperlight + Nightcore Integration Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build guest + worker, create artifacts (default)"
	@echo "  make nightcore    - Same as above"
	@echo "  make guest        - Build only the guest binary"
	@echo "  make worker       - Build only the worker library"
	@echo "  make host         - Build standalone host example"
	@echo "  make demo         - Build and start Nightcore stack"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make clean-all    - Clean everything including outputs/"
	@echo "  make print        - Display build variables"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Artifacts:"
	@echo "  guest.bin         - Hyperlight guest binary (165KB)"
	@echo "  libecho.so        - Worker library for Nightcore (1.6MB)"

guest:
	@echo "Building guest ($(GUEST_NAME)) for $(GUEST_TARGET) in $(PROFILE)"
	@CARGO_TARGET_DIR="$(GUEST_TARGET_DIR)" RUSTFLAGS="$(GUEST_RUSTFLAGS)" $(CARGO) build $(CARGO_PROFILE_FLAG) --manifest-path $(GUEST_MANIFEST) --target $(GUEST_TARGET)
	@echo "Guest built at $(GUEST_BIN)"

host: guest
	@echo "Building host ($(HOST_NAME)) in $(PROFILE)"
	@$(CARGO) build $(CARGO_PROFILE_FLAG) --manifest-path $(HOST_MANIFEST)
	@echo "Host built at $(HOST_BIN)"

worker:
	@echo "Building worker ($(WORKER_NAME)) in $(PROFILE)"
	@$(CARGO) build $(CARGO_PROFILE_FLAG) --manifest-path $(WORKER_MANIFEST)
	@echo "Worker built at $(WORKER_LIB)"

nightcore: guest worker
	@echo "Copying artifacts for Nightcore integration..."
	@cp "$(GUEST_BIN)" "$(GUEST_ARTIFACT)"
	@cp "$(WORKER_LIB)" "$(WORKER_ARTIFACT)"
	@echo "Build complete!"
	@echo "  Guest binary:   $(GUEST_ARTIFACT)"
	@echo "  Worker library: $(WORKER_ARTIFACT)"

run: host
	@echo "Running host with guest binary: $(GUEST_BIN)"
	@HYPERLIGHT_GUEST_BIN_PATH="$(GUEST_BIN)" "$(HOST_BIN)"

demo: nightcore
	@echo "Starting Nightcore stack with Hyperlight..."
	@./run_stack.sh

clean:
	@echo "Cleaning build artifacts..."
	@$(CARGO) clean
	@rm -rf "$(GUEST_TARGET_DIR)"
	@rm -f "$(GUEST_ARTIFACT)" "$(WORKER_ARTIFACT)"
	@echo "Clean complete"

clean-all: clean
	@echo "Cleaning all residuals including outputs..."
	@rm -rf outputs/
	@echo "Deep clean complete"

print:
	@echo CARGO=$(CARGO)
	@echo PROFILE=$(PROFILE)
	@echo GUEST_TARGET=$(GUEST_TARGET)
	@echo GUEST_RUSTFLAGS=$(GUEST_RUSTFLAGS)
	@echo GUEST_NAME=$(GUEST_NAME)
	@echo HOST_NAME=$(HOST_NAME)
	@echo WORKER_NAME=$(WORKER_NAME)
	@echo GUEST_BIN=$(GUEST_BIN)
	@echo HOST_BIN=$(HOST_BIN)
	@echo WORKER_LIB=$(WORKER_LIB)
	@echo GUEST_ARTIFACT=$(GUEST_ARTIFACT)
	@echo WORKER_ARTIFACT=$(WORKER_ARTIFACT)


