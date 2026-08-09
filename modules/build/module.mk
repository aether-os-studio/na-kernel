MODULE_DIR ?= $(CURDIR)
MODULE_OBJ_DIR ?= $(PROJECT_ROOT)/obj-modules-$(ARCH)/$(BUILD_MODE)/$(KM_NAME)
MODULE_OUT_DIR ?= $(PROJECT_ROOT)/modules-$(ARCH)
MODULE_OUTPUT ?= $(MODULE_OUT_DIR)/$(KM_NAME).ko
MODULE_KIND ?= ko
MODULE_AR ?= ar
MODULE_LINKER ?= $(LD)
MODULE_INCLUDE_DIRS ?=
MODULE_LINK_LIBS ?=
MODULE_EXTRA_DEPS ?=
MODULE_OBJ_DEPS ?= $(MODULE_EXTRA_DEPS)
MODULE_LINK_DEPS ?= $(MODULE_EXTRA_DEPS)
MODULE_BUILD_RULES := $(PROJECT_ROOT)/modules/build/module.mk

override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
override OBJ := $(addprefix $(MODULE_OBJ_DIR)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
ifeq ($(MODULE_KIND),ko)
MODULE_META_OBJ := $(MODULE_OBJ_DIR)/.module-meta.c.o
override OBJ += $(MODULE_META_OBJ)
endif
MODULE_INCLUDE_FLAGS := $(addprefix -I,$(MODULE_INCLUDE_DIRS)) -I$(MODULE_DIR)/

all: $(MODULE_OUTPUT)

ifeq ($(MODULE_KIND),staticlib)
$(MODULE_OUTPUT): $(OBJ) $(MODULE_LINK_DEPS) GNUmakefile $(MODULE_BUILD_RULES)
	$(call PRINT_STEP,AR,$@)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(MODULE_AR) rcs $@ $(OBJ)
else ifeq ($(MODULE_KIND),relocatable)
$(MODULE_OUTPUT): $(OBJ) $(MODULE_LINK_DEPS) GNUmakefile $(MODULE_BUILD_RULES)
	$(call PRINT_STEP,LD,$@)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(LD) -r $(OBJ) -o $@
else
$(MODULE_OUTPUT): $(OBJ) $(MODULE_LINK_DEPS) GNUmakefile $(MODULE_BUILD_RULES)
	$(call PRINT_STEP,LD,$@)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(MODULE_LINKER) $(LDFLAGS) -shared $(OBJ) $(MODULE_LINK_LIBS) -o $@
endif

$(MODULE_OBJ_DIR)/%.c.o: %.c GNUmakefile $(MODULE_BUILD_RULES) $(MODULE_OBJ_DEPS)
	$(call PRINT_STEP,CC,$<)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(CC) $(CFLAGS) $(MODULE_INCLUDE_FLAGS) -c $< -o $@

$(MODULE_OBJ_DIR)/%.S.o: %.S GNUmakefile $(MODULE_BUILD_RULES) $(MODULE_OBJ_DEPS)
	$(call PRINT_STEP,AS,$<)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(CC) $(CFLAGS) $(MODULE_INCLUDE_FLAGS) -DASM_FILE -c $< -o $@

ifeq ($(MODULE_KIND),ko)
$(MODULE_META_OBJ): $(PROJECT_ROOT)/kernel/src/mod/module.h GNUmakefile \
		$(MODULE_BUILD_RULES) $(MODULE_OBJ_DEPS)
	$(call PRINT_STEP,CC,$<)
	$(Q)mkdir -p "$$(dirname $@)"
	$(Q)$(CC) $(CFLAGS) $(MODULE_INCLUDE_FLAGS) -x c -include $< \
		-DMODULE_BUILD_NAME='"$(KM_NAME)"' -c /dev/null -o $@
endif

fmt:
