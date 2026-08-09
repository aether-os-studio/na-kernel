MAKEFLAGS += -rR --no-print-directory

include ../../build/common-env.mk

RUST_TARGET_x86_64 := x86_64-unknown-none
RUST_TARGET_aarch64 := aarch64-unknown-none-softfloat
RUST_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_TARGET_loongarch64 := loongarch64-unknown-none-softfloat
RUST_TARGET := $(RUST_TARGET_$(ARCH))

ifeq ($(RUST_TARGET),)
$(error Rust target for architecture $(ARCH) is not defined)
endif

CARGO_PROFILE := $(if $(filter release,$(BUILD_MODE)),release,dev)
CARGO_TARGET_DIR := $(PROJECT_ROOT)/obj-modules-$(ARCH)/$(BUILD_MODE)/na-std
CARGO ?= cargo
RUSTC ?= rustc
BINDGEN ?= bindgen
CARGO_FLAGS := -Z build-std=core,alloc --target $(RUST_TARGET) \
	--profile $(CARGO_PROFILE)

.PHONY: all check fmt
all:
	$(call PRINT_STEP,CARGO,na-std/$(RUST_TARGET))
	$(Q)CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) \
		RUSTC=$(RUSTC) \
		BINDGEN=$(BINDGEN) \
		RUSTFLAGS='-C relocation-model=pic' \
		$(CARGO) build $(CARGO_FLAGS)

check:
	$(call PRINT_STEP,CARGO-CHECK,na-std/$(RUST_TARGET))
	$(Q)CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) \
		RUSTC=$(RUSTC) \
		BINDGEN=$(BINDGEN) \
		RUSTFLAGS='-C relocation-model=pic' \
		$(CARGO) check $(CARGO_FLAGS)

fmt:
	$(call PRINT_STEP,CARGO-FMT,na-std/$(RUST_TARGET))
	$(Q)$(CARGO) fmt
