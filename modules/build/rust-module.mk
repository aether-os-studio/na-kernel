RUST_MODULE_DIR ?= $(CURDIR)
RUST_MODULE_OBJ_DIR ?= $(PROJECT_ROOT)/obj-modules-$(ARCH)/$(BUILD_MODE)/$(KM_NAME)
RUST_MODULE_OUT_DIR ?= $(PROJECT_ROOT)/modules-$(ARCH)
RUST_MODULE_OUTPUT ?= $(RUST_MODULE_OUT_DIR)/$(KM_NAME).ko

RUST_TARGET_x86_64 := x86_64-unknown-none
RUST_TARGET_aarch64 := aarch64-unknown-none-softfloat
RUST_TARGET_riscv64 := riscv64imac-unknown-none-elf
RUST_TARGET_loongarch64 := loongarch64-unknown-none-softfloat
RUST_TARGET := $(RUST_TARGET_$(ARCH))

ifeq ($(RUST_TARGET),)
$(error Rust target for architecture $(ARCH) is not defined)
endif

CARGO ?= cargo
RUSTC ?= rustc
BINDGEN ?= bindgen
RUST_CRATE_NAME ?= $(subst -,_,$(KM_NAME))
CARGO_PROFILE := $(if $(filter release,$(BUILD_MODE)),release,dev)
CARGO_PROFILE_DIR := $(if $(filter release,$(BUILD_MODE)),release,debug)
CARGO_TARGET_DIR := $(RUST_MODULE_OBJ_DIR)/cargo
RUST_STATICLIB := $(CARGO_TARGET_DIR)/$(RUST_TARGET)/$(CARGO_PROFILE_DIR)/lib$(RUST_CRATE_NAME).a
RUST_META_OBJ := $(RUST_MODULE_OBJ_DIR)/.module-meta.c.o
RUST_DEP_OBJS := $(foreach dep,$(MODULE_DEPS),$(RUST_MODULE_OBJ_DIR)/.module-dep-$(dep).c.o)
RUST_METADATA_OBJS := $(RUST_META_OBJ) $(RUST_DEP_OBJS)
RUST_MODULE_RULES := $(PROJECT_ROOT)/modules/build/rust-module.mk
RUST_SOURCE_DEPENDENCIES ?= $(shell find $(PROJECT_ROOT)/modules \
	\( -path '*/target' -o -path '*/obj-*' \) -prune -o \
	\( -name '*.rs' -o -name Cargo.toml -o -name Cargo.lock \) -print \
	| LC_ALL=C sort)

.PHONY: all
all: $(RUST_MODULE_OUTPUT)

$(RUST_STATICLIB): $(RUST_SOURCE_DEPENDENCIES) \
		$(RUST_MODULE_RULES)
	$(call PRINT_STEP,CARGO,$(KM_NAME))
	$(Q)CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) RUSTC=$(RUSTC) BINDGEN=$(BINDGEN) \
		RUSTFLAGS='-C relocation-model=pic' \
		$(CARGO) build -Z build-std=core --target $(RUST_TARGET) \
			--profile $(CARGO_PROFILE)

$(RUST_META_OBJ): $(PROJECT_ROOT)/kernel/src/mod/module.h GNUmakefile \
		$(RUST_MODULE_RULES)
	$(call PRINT_STEP,CC,$<)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(CC) $(CFLAGS) -I$(PROJECT_ROOT)/kernel/src -x c -include $< \
		-DMODULE_BUILD_NAME='"$(KM_NAME)"' -c /dev/null -o $@

define RUST_DEP_RULE
$(RUST_MODULE_OBJ_DIR)/.module-dep-$(1).c.o: \
		$(PROJECT_ROOT)/kernel/src/mod/module.h GNUmakefile $(RUST_MODULE_RULES)
	$$(call PRINT_STEP,CC,$$<)
	$$(Q)mkdir -p "$$$$(dirname $$@)"
	$$(Q)$$(CC) $$(CFLAGS) -I$$(PROJECT_ROOT)/kernel/src -x c -include $$< \
		-DMODULE_BUILD_DEPENDENCY='"$(1)"' -c /dev/null -o $$@
endef

$(foreach dep,$(MODULE_DEPS),$(eval $(call RUST_DEP_RULE,$(dep))))

$(RUST_MODULE_OUTPUT): $(RUST_STATICLIB) $(RUST_METADATA_OBJS) GNUmakefile \
		$(RUST_MODULE_RULES)
	$(call PRINT_STEP,LD,$@)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(LD) -shared --whole-archive $(RUST_STATICLIB) --no-whole-archive \
		$(RUST_METADATA_OBJS) -o $@
