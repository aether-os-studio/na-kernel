all:
	$(call PRINT_STEP,MAKE,modules/drivers/net)
	$(Q)$(MAKE) -C net

	$(call PRINT_STEP,MAKE,modules/drivers/block)
	$(Q)$(MAKE) -C block

	$(call PRINT_STEP,MAKE,modules/drivers/sound)
	$(Q)$(MAKE) -C sound

	$(call PRINT_STEP,MAKE,modules/drivers/drm)
	$(Q)$(MAKE) -C drm

	$(call PRINT_STEP,MAKE,modules/drivers/usb)
	$(Q)$(MAKE) -C usb

	$(call PRINT_STEP,MAKE,modules/drivers/hub)
	$(Q)$(MAKE) -C hub
	$(call PRINT_STEP,MAKE,modules/drivers/hid)
	$(Q)$(MAKE) -C hid

fmt:
	$(call PRINT_STEP,MAKE-FMT,modules/drivers/net)
	$(Q)$(MAKE) -C net fmt

	$(call PRINT_STEP,MAKE-FMT,modules/drivers/block)
	$(Q)$(MAKE) -C block fmt

	$(call PRINT_STEP,MAKE-FMT,modules/drivers/sound)
	$(Q)$(MAKE) -C sound fmt

	$(call PRINT_STEP,MAKE-FMT,modules/drivers/drm)
	$(Q)$(MAKE) -C drm fmt

	$(call PRINT_STEP,MAKE-FMT,modules/drivers/usb)
	$(Q)$(MAKE) -C usb fmt

	$(call PRINT_STEP,MAKE-FMT,modules/drivers/hub)
	$(Q)$(MAKE) -C hub fmt
	$(call PRINT_STEP,MAKE-FMT,modules/drivers/hid)
	$(Q)$(MAKE) -C hid fmt
