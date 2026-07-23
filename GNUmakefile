# Nuke built-in rules and variables.
MAKEFLAGS += -rR --no-print-directory
.SUFFIXES:

include build/common-env.mk

ifeq ($(MODULE_VERIFY),0)
MODULES_TARGET := modules
else
MODULES_TARGET := sign-modules
endif

LIBGCC_VERSION ?= 2025-12-08
SOFTFLOAT :=
ifeq ($(ARCH),riscv64)
SOFTFLOAT := -softfloat
endif
ifeq ($(ARCH),loongarch64)
SOFTFLOAT := -softfloat
endif

.PHONY: all prepare kernel modules module-signing-keys sign-modules clippy clean distclean
all: kernel $(MODULES_TARGET)

prepare: libgcc_$(ARCH).a
	$(call PRINT_STEP,PREPARE,kernel/get-deps)
	$(Q)./kernel/get-deps

libgcc_$(ARCH).a:
	$(call PRINT_STEP,GET,$@)
	$(Q)curl -Lo $@ https://github.com/osdev0/libgcc-binaries/releases/download/$(LIBGCC_VERSION)/libgcc-$(ARCH)$(SOFTFLOAT).a

kernel:
	$(call PRINT_STEP,MAKE,kernel)
	$(Q)$(MAKE) -C kernel

modules:
	$(call PRINT_STEP,MAKE,modules)
	$(Q)$(MAKE) -C modules

module-signing-keys:
	$(call PRINT_STEP,GEN,$(MODULE_SIGN_KEY_DIR))
	$(Q)./kernel/scripts/gen_module_signing_keys.sh "$(MODULE_SIGN_KEY_DIR)" naos_signing_key_pub

ifneq ($(MODULE_VERIFY),0)
kernel: module-signing-keys
endif

sign-modules: modules module-signing-keys
	$(call PRINT_STEP,SIGN,modules-$(ARCH))
	$(Q)find modules-$(ARCH) -type f -name '*.ko' \
		-exec ./kernel/scripts/sign_module.py {} "$(MODULE_SIGN_PRIV)" \;

clippy:
	$(MAKE) -C kernel clippy

clean:
	$(MAKE) -C kernel clean
	rm -rf obj-modules-$(ARCH) modules-$(ARCH)

distclean:
	$(MAKE) -C kernel distclean
	rm -rf obj-modules-$(ARCH) modules-$(ARCH)
