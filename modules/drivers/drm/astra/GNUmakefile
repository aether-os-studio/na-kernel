KM_NAME := astra
RUST_MODULE_DIR := $(CURDIR)
include ../../../../build/common-env.mk
include $(PROJECT_ROOT)/modules/build/rust-module.mk

# build.rs generates the register constant modules from the Linux amdgpu
# asic_reg headers; override to point at a different checkout.
export LINUX_SRC_DIR ?= $(PROJECT_ROOT)/../../linux
