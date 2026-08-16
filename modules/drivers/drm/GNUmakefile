all:
	$(call PRINT_STEP,MAKE,modules/drivers/virtio-gpu)
	$(Q)$(MAKE) -C virtio-gpu
	$(call PRINT_STEP,MAKE,modules/drivers/astra)
	$(Q)$(MAKE) -C astra

fmt:
	$(call PRINT_STEP,MAKE-FMT,modules/drivers/virtio-gpu)
	$(Q)$(MAKE) -C virtio-gpu fmt
	$(call PRINT_STEP,MAKE-FMT,modules/drivers/astra)
	$(Q)$(MAKE) -C astra fmt
