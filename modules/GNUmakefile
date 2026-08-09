MAKEFLAGS += -rR --no-print-directory

include ../build/common-env.mk
include build/common-flags.mk

all:
	$(call PRINT_STEP,MKDIR,$(PROJECT_ROOT)/modules-$(ARCH))
	$(Q)mkdir -p $(PROJECT_ROOT)/modules-$(ARCH)

	$(call PRINT_STEP,MAKE,modules/na-std)
	$(Q)$(MAKE) -C na-std

	$(call PRINT_STEP,MAKE,modules/drivers)
	$(Q)$(MAKE) -C drivers
	$(call PRINT_STEP,MAKE,modules/fs)
	$(Q)$(MAKE) -C fs
	$(call PRINT_STEP,MAKE,modules/net)
	$(Q)$(MAKE) -C net

fmt:
	$(call PRINT_STEP,MAKE-FMT,modules/na-std)
	$(Q)$(MAKE) -C na-std fmt

	$(call PRINT_STEP,MAKE-FMT,modules/drivers)
	$(Q)$(MAKE) -C drivers fmt
	$(call PRINT_STEP,MAKE-FMT,modules/fs)
	$(Q)$(MAKE) -C fs fmt
	$(call PRINT_STEP,MAKE-FMT,modules/net)
	$(Q)$(MAKE) -C net fmt
