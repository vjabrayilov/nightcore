CARGO ?= cargo
PROFILE ?= release

GUEST_MANIFEST := guest/Cargo.toml
HOST_MANIFEST := host/Cargo.toml

# Derive package names without external tools
GUEST_NAME := $(shell sed -n 's/^name = "\(.*\)"/\1/p' $(GUEST_MANIFEST) | head -n1)
HOST_NAME := $(shell sed -n 's/^name = "\(.*\)"/\1/p' $(HOST_MANIFEST) | head -n1)

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

.PHONY: all guest host run clean print

all: host

guest:
	@echo "Building guest ($(GUEST_NAME)) for $(GUEST_TARGET) in $(PROFILE)"
	@CARGO_TARGET_DIR="$(GUEST_TARGET_DIR)" RUSTFLAGS="$(GUEST_RUSTFLAGS)" $(CARGO) build $(CARGO_PROFILE_FLAG) --manifest-path $(GUEST_MANIFEST) --target $(GUEST_TARGET)
	@echo "Guest built at $(GUEST_BIN)"

host: guest
	@echo "Building host ($(HOST_NAME)) in $(PROFILE)"
	@$(CARGO) build $(CARGO_PROFILE_FLAG) --manifest-path $(HOST_MANIFEST)
	@echo "Host built at $(HOST_BIN)"

run: host
	@echo "Running host with guest binary: $(GUEST_BIN)"
	@HYPERLIGHT_GUEST_BIN_PATH="$(GUEST_BIN)" "$(HOST_BIN)"

clean:
	@$(CARGO) clean
	@rm -rf "$(GUEST_TARGET_DIR)"

print:
	@echo CARGO=$(CARGO)
	@echo PROFILE=$(PROFILE)
	@echo GUEST_TARGET=$(GUEST_TARGET)
	@echo GUEST_RUSTFLAGS=$(GUEST_RUSTFLAGS)
	@echo GUEST_NAME=$(GUEST_NAME)
	@echo HOST_NAME=$(HOST_NAME)
	@echo GUEST_BIN=$(GUEST_BIN)
	@echo HOST_BIN=$(HOST_BIN)


