all:
	$(call PRINT_STEP,MAKE,modules/drivers/block/nvme)
	$(Q)$(MAKE) -C nvme

	$(call PRINT_STEP,MAKE,modules/drivers/block/msc)
	$(Q)$(MAKE) -C msc

	$(call PRINT_STEP,MAKE,modules/drivers/block/virtio-blk)
	$(Q)$(MAKE) -C virtio-blk

fmt:
