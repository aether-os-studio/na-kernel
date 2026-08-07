# Shared build defaults for top-level and standalone sub-builds.

NAOS_BUILD_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

PROJECT_ROOT ?= $(abspath $(NAOS_BUILD_DIR)/..)
BUILD_MODE ?= release
BOOT_PROTOCOL ?= limine
MODULE_VERIFY ?= 0
ARCH ?= x86_64
ROOT_DIR ?= "$(PROJECT_ROOT)"

MODULE_SIGN_KEY_DIR ?= $(PROJECT_ROOT)/kernel/obj-$(ARCH)/module-signing
MODULE_SIGN_PRIV ?= $(MODULE_SIGN_KEY_DIR)/module_signing_priv.pem
MODULE_SIGN_PUB_HEADER ?= $(MODULE_SIGN_KEY_DIR)/pubkey.h

ifeq ($(ARCH),x86_64)
ifneq ($(origin CC), command line)
CC := $(ARCH)-linux-gnu-gcc
endif
ifneq ($(origin CXX), command line)
CXX := $(ARCH)-linux-gnu-g++
endif
ifneq ($(origin LD), command line)
LD := $(ARCH)-linux-gnu-ld
endif
ifneq ($(origin NM), command line)
NM := $(ARCH)-linux-gnu-nm
endif
ifneq ($(origin OBJCOPY), command line)
OBJCOPY := $(ARCH)-linux-gnu-objcopy
endif
endif
ifeq ($(ARCH),aarch64)
ifneq ($(origin CC), command line)
CC := $(ARCH)-linux-gnu-gcc
endif
ifneq ($(origin CXX), command line)
CXX := $(ARCH)-linux-gnu-g++
endif
ifneq ($(origin LD), command line)
LD := $(ARCH)-linux-gnu-ld
endif
ifneq ($(origin NM), command line)
NM := $(ARCH)-linux-gnu-nm
endif
ifneq ($(origin OBJCOPY), command line)
OBJCOPY := $(ARCH)-linux-gnu-objcopy
endif
endif
ifeq ($(ARCH),riscv64)
ifneq ($(origin CC), command line)
CC := $(ARCH)-linux-gnu-gcc
endif
ifneq ($(origin CXX), command line)
CXX := $(ARCH)-linux-gnu-g++
endif
ifneq ($(origin LD), command line)
LD := $(ARCH)-linux-gnu-ld
endif
ifneq ($(origin NM), command line)
NM := $(ARCH)-linux-gnu-nm
endif
ifneq ($(origin OBJCOPY), command line)
OBJCOPY := $(ARCH)-linux-gnu-objcopy
endif
endif
ifeq ($(ARCH),loongarch64)
ifneq ($(origin CC), command line)
CC := $(ARCH)-linux-gnu-gcc
endif
ifneq ($(origin CXX), command line)
CXX := $(ARCH)-linux-gnu-g++
endif
ifneq ($(origin LD), command line)
LD := $(ARCH)-linux-gnu-ld
endif
ifneq ($(origin NM), command line)
NM := $(ARCH)-linux-gnu-nm
endif
ifneq ($(origin OBJCOPY), command line)
OBJCOPY := $(ARCH)-linux-gnu-objcopy
endif
endif

CC_IS_CLANG ?= $(shell ! $(CC) --version 2>/dev/null | grep 'clang' >/dev/null 2>&1; echo $$?)
V ?= 0

ifeq ($(V),1)
Q :=
define PRINT_STEP
@true
endef
else
Q := @
define PRINT_STEP
@printf "  %-7s %s\n" "$(1)" "$(2)"
endef
endif

export PROJECT_ROOT BOOT_PROTOCOL BUILD_MODE MODULE_VERIFY
export MODULE_SIGN_KEY_DIR MODULE_SIGN_PRIV MODULE_SIGN_PUB_HEADER
export ARCH ROOT_DIR CC CXX LD NM OBJCOPY CC_IS_CLANG V Q
