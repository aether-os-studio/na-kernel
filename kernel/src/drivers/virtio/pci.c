// Copyright (C) 2025-2026  lihanrui2913
#include "pci.h"

#include <task/task.h>

extern virtio_driver_op_t virtio_pci_driver_op;

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

static inline uint8_t virtio_pci_read8(volatile void *addr) {
    return *(volatile uint8_t *)addr;
}

static inline uint16_t virtio_pci_read16(volatile void *addr) {
    return *(volatile uint16_t *)addr;
}

static inline uint32_t virtio_pci_read32_reg(volatile void *addr) {
    return *(volatile uint32_t *)addr;
}

static inline uint64_t virtio_pci_read64(volatile void *addr) {
    return *(volatile uint64_t *)addr;
}

static inline void virtio_pci_write8(volatile void *addr, uint8_t value) {
    *(volatile uint8_t *)addr = value;
}

static inline void virtio_pci_write16(volatile void *addr, uint16_t value) {
    *(volatile uint16_t *)addr = value;
}

static inline void virtio_pci_write32_reg(volatile void *addr, uint32_t value) {
    *(volatile uint32_t *)addr = value;
}

static inline void virtio_pci_write64(volatile void *addr, uint64_t value) {
    *(volatile uint64_t *)addr = value;
}

static void virtio_pci_irq_handler(uint64_t irq_num, void *data,
                                   struct pt_regs *regs) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    uint8_t status = 0;
    virtio_interrupt_handler_t handler;
    void *opaque;

    (void)irq_num;
    (void)regs;

    if (!pci || !pci->isr_status)
        return;

    status = virtio_pci_read8(pci->isr_status);
    if (!status)
        return;

    spin_lock(&pci->irq_lock);
    pci->irq_seq++;
    handler = pci->irq_handler;
    opaque = pci->irq_handler_opaque;
    spin_unlock(&pci->irq_lock);

    wait_queue_wake_all(&pci->irq_wait, 0, EOK);
    if (handler)
        handler(opaque, status);
}

static int virtio_pci_setup_msi(virtio_pci_device_t *pci) {
    int ret;

    if (!pci || !pci->isr_status)
        return -ENOTSUP;

    ret = msi_setup_irq(&pci->msi, pci->pci_dev, 0, true,
                        virtio_pci_irq_handler, pci, "virtio_pci_irq");
    if (ret < 0)
        return ret;

    pci->irq_enabled = true;
    pci->irq_msix = pci->msi.pci.msi_attribute.is_msix;
    if (pci->irq_msix) {
        virtio_pci_write16(&pci->common_cfg_bar->msix_config, 0);
    }

    return 0;
}

static void virtio_pci_configure_queue_irq(virtio_pci_device_t *pci,
                                           uint16_t queue) {
    if (!pci || !pci->irq_enabled || !pci->irq_msix)
        return;

    virtio_pci_write16(&pci->common_cfg_bar->queue_select, queue);
    virtio_pci_write16(&pci->common_cfg_bar->queue_msix_vector, 0);
}

virtio_device_type_t get_device_type(uint16_t device_id) {
    switch (device_id) {
    case PCI_DEVICE_ID_NETWORK:
        return VIRTIO_DEVICE_TYPE_NETWORK;
    case PCI_DEVICE_ID_BLOCK:
        return VIRTIO_DEVICE_TYPE_BLOCK;
    default:
        return (virtio_device_type_t)(device_id - PCI_DEVICE_ID_OFFSET);
    }
}

virtio_driver_t *virtio_pci_init(void *data) {
    pci_device_t *device = (pci_device_t *)data;
    uint16_t vendor_id = device->vendor_id;
    uint16_t device_id = device->device_id;
    virtio_device_type_t device_type = get_device_type(device_id);
    const char *fail_reason = "unknown";

    virtio_cap_info_t *common_cfg = NULL;
    virtio_cap_info_t *notify_cfg = NULL;
    virtio_cap_info_t *isr_cfg = NULL;
    virtio_cap_info_t *device_cfg = NULL;

    uint32_t notify_off_multiplier = 0;
    uint64_t host_visible_shm_paddr = 0;
    uint64_t host_visible_shm_size = 0;

    uint32_t tmp = 0;
    uint32_t cap_offset = device->capability_point;
    while (1) {
        tmp = device->op->read32(device->bus, device->slot, device->func,
                                 device->segment, cap_offset);
        if ((tmp & 0xff) != 0x09) {
            if (((tmp & 0xff00) >> 8)) {
                cap_offset = (tmp & 0xff00) >> 8;
                continue;
            } else
                break;
        }

        uint8_t cfg_type =
            device->op->read8(device->bus, device->slot, device->func,
                              device->segment, cap_offset + 0x03);
        uint8_t bar = device->op->read8(device->bus, device->slot, device->func,
                                        device->segment, cap_offset + 0x04);
        uint8_t cap_id =
            device->op->read8(device->bus, device->slot, device->func,
                              device->segment, cap_offset + 0x05);
        uint32_t cap_offset_lo =
            device->op->read32(device->bus, device->slot, device->func,
                               device->segment, cap_offset + 0x08);
        uint32_t cap_length_lo =
            device->op->read32(device->bus, device->slot, device->func,
                               device->segment, cap_offset + 0x0C);

        virtio_cap_info_t *cap_info = NULL;
        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG ||
            cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG ||
            cfg_type == VIRTIO_PCI_CAP_ISR_CFG ||
            cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
            cap_info = malloc(sizeof(virtio_cap_info_t));
            if (!cap_info) {
                break;
            }
            memset(cap_info, 0, sizeof(virtio_cap_info_t));
            cap_info->bar = bar;
            cap_info->offset = cap_offset_lo;
            cap_info->length = cap_length_lo;
        }

        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
            common_cfg = cap_info;
        } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
            device_cfg = cap_info;
        } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            notify_cfg = cap_info;
            notify_off_multiplier =
                device->op->read32(device->bus, device->slot, device->func,
                                   device->segment, cap_offset + 0x10);
        } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
            isr_cfg = cap_info;
        } else {
            if (cap_info) {
                free(cap_info);
            }
            if (cfg_type == 8 && cap_id == 1 && bar < 6) {
                uint64_t cap_offset_hi =
                    device->op->read32(device->bus, device->slot, device->func,
                                       device->segment, cap_offset + 0x10);
                uint64_t cap_length_hi =
                    device->op->read32(device->bus, device->slot, device->func,
                                       device->segment, cap_offset + 0x14);
                uint64_t shm_offset = cap_offset_lo | (cap_offset_hi << 32);
                uint64_t shm_length = cap_length_lo | (cap_length_hi << 32);
                if (shm_length && device->bars[bar].address) {
                    host_visible_shm_paddr =
                        device->bars[bar].address + shm_offset;
                    host_visible_shm_size = shm_length;
                }
            }
        }

        cap_offset = (tmp & 0xff00) >> 8;
        if (!cap_offset)
            break;
    }

    virtio_pci_device_t *pci = malloc(sizeof(virtio_pci_device_t));
    if (!pci) {
        fail_reason = "failed to allocate virtio_pci_device";
        goto fail;
    }
    memset(pci, 0, sizeof(virtio_pci_device_t));
    pci->pci_dev = device;
    pci->device_type = device_type;
    pci->common_cfg = common_cfg;
    pci->device_cfg = device_cfg;
    spin_init(&pci->irq_lock);
    wait_queue_init(&pci->irq_wait);

    uint64_t common_cfg_vaddr = 0;

    if (common_cfg) {
        pci_bar_t *bar = &device->bars[pci->common_cfg->bar];
        uint64_t bar_paddr = bar->address + pci->common_cfg->offset;
        if (bar_paddr == 0) {
            fail_reason = "common cfg BAR address is zero";
            goto fail_pci;
        }
        uint64_t bar_vaddr = (uint64_t)phys_to_virt(bar_paddr);
        map_page_range(get_current_page_dir(false),
                       bar_vaddr & ~(PAGE_SIZE - 1),
                       bar_paddr & ~(PAGE_SIZE - 1), pci->common_cfg->length,
                       PT_FLAG_R | PT_FLAG_W | PT_FLAG_DEVICE);
        common_cfg_vaddr = bar_vaddr;
    } else {
        fail_reason = "missing common cfg capability";
        goto fail_pci;
    }

    uint64_t notify_cfg_vaddr = 0;

    if (notify_cfg) {
        pci_bar_t *bar = &device->bars[notify_cfg->bar];
        uint64_t bar_paddr = bar->address + notify_cfg->offset;
        if (bar_paddr == 0) {
            fail_reason = "notify cfg BAR address is zero";
            goto fail_pci;
        }
        uint64_t bar_vaddr = (uint64_t)phys_to_virt(bar_paddr);
        map_page_range(get_current_page_dir(false),
                       bar_vaddr & ~(PAGE_SIZE - 1),
                       bar_paddr & ~(PAGE_SIZE - 1), notify_cfg->length,
                       PT_FLAG_R | PT_FLAG_W | PT_FLAG_DEVICE);
        notify_cfg_vaddr = bar_vaddr;
    } else {
        fail_reason = "missing notify cfg capability";
        goto fail_pci;
    }

    uint64_t config_space_vaddr = 0;
    uint64_t isr_status_vaddr = 0;

    if (device_cfg) {
        pci_bar_t *bar = &device->bars[pci->device_cfg->bar];
        uint64_t bar_paddr = bar->address + pci->device_cfg->offset;
        if (bar_paddr == 0) {
            fail_reason = "device cfg BAR address is zero";
            goto fail_pci;
        }
        uint64_t bar_vaddr = (uint64_t)phys_to_virt(bar_paddr);
        map_page_range(get_current_page_dir(false),
                       bar_vaddr & ~(PAGE_SIZE - 1),
                       bar_paddr & ~(PAGE_SIZE - 1), pci->device_cfg->length,
                       PT_FLAG_R | PT_FLAG_W | PT_FLAG_DEVICE);
        config_space_vaddr = bar_vaddr;
    } else {
        fail_reason = "missing device cfg capability";
        goto fail_pci;
    }

    if (isr_cfg) {
        pci_bar_t *bar = &device->bars[isr_cfg->bar];
        uint64_t bar_paddr = bar->address + isr_cfg->offset;
        if (bar_paddr != 0) {
            uint64_t bar_vaddr = (uint64_t)phys_to_virt(bar_paddr);
            map_page_range(get_current_page_dir(false),
                           bar_vaddr & ~(PAGE_SIZE - 1),
                           bar_paddr & ~(PAGE_SIZE - 1), isr_cfg->length,
                           PT_FLAG_R | PT_FLAG_W | PT_FLAG_DEVICE);
            isr_status_vaddr = bar_vaddr;
        }
    }

    pci->notify_off_multiplier = notify_off_multiplier;
    pci->host_visible_shm_paddr = host_visible_shm_paddr;
    pci->host_visible_shm_size = host_visible_shm_size;

    pci->common_cfg_bar = (virtio_pci_common_cfg_t *)common_cfg_vaddr;
    pci->notify_regions = (uint16_t *)notify_cfg_vaddr;
    pci->isr_status = (volatile uint8_t *)isr_status_vaddr;
    pci->config_space_vaddr = config_space_vaddr;

    if (virtio_pci_setup_msi(pci) != 0) {
        pci->irq_enabled = false;
        pci->irq_msix = false;
    }

    virtio_driver_t *driver = malloc(sizeof(virtio_driver_t));
    memset(driver, 0, sizeof(virtio_driver_t));
    driver->data = (void *)pci;
    driver->op = &virtio_pci_driver_op;
    return driver;

fail_pci:
    free(pci);
fail:
    printk("virtio_pci: init failed for %04x:%02x:%02x.%u (%04x:%04x): %s\n",
           device->segment, device->bus, device->slot, device->func, vendor_id,
           device_id, fail_reason);
    if (common_cfg)
        free(common_cfg);
    if (notify_cfg)
        free(notify_cfg);
    if (isr_cfg)
        free(isr_cfg);
    if (device_cfg)
        free(device_cfg);
    return NULL;
}

virtio_device_type_t virtio_pci_get_device_type(void *data) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    return pci->device_type;
}

uint64_t virtio_pci_get_features(void *data) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write32_reg(&pci->common_cfg_bar->device_feature_select, 0);
    uint32_t features_low =
        virtio_pci_read32_reg(&pci->common_cfg_bar->device_feature);
    virtio_pci_write32_reg(&pci->common_cfg_bar->device_feature_select, 1);
    uint32_t features_high =
        virtio_pci_read32_reg(&pci->common_cfg_bar->device_feature);
    return (uint64_t)features_low | ((uint64_t)features_high << 32);
}

void virtio_pci_set_features(void *data, uint64_t features) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    uint32_t features_low = features & 0xFFFFFFFF;
    uint32_t features_high = (features >> 32) & 0xFFFFFFFF;

    virtio_pci_write32_reg(&pci->common_cfg_bar->device_feature_select, 0);
    virtio_pci_write32_reg(&pci->common_cfg_bar->driver_feature_select, 0);
    virtio_pci_write32_reg(&pci->common_cfg_bar->driver_feature, features_low);
    virtio_pci_write32_reg(&pci->common_cfg_bar->device_feature_select, 1);
    virtio_pci_write32_reg(&pci->common_cfg_bar->driver_feature_select, 1);
    virtio_pci_write32_reg(&pci->common_cfg_bar->driver_feature, features_high);
}

uint32_t virtio_pci_get_max_queue_size(void *data, uint16_t queue) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write16(&pci->common_cfg_bar->queue_select, queue);
    return virtio_pci_read16(&pci->common_cfg_bar->queue_size);
}

void virtio_pci_notify(void *data, uint16_t queue) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write16(&pci->common_cfg_bar->queue_select, queue);
    uint16_t notify_off =
        virtio_pci_read16(&pci->common_cfg_bar->queue_notify_off);
    uint64_t offset_bytes = (uint64_t)notify_off * pci->notify_off_multiplier;
    volatile uint16_t *notify_addr =
        (volatile uint16_t *)((uintptr_t)pci->notify_regions + offset_bytes);
    write_barrier();
    *notify_addr = queue;
}

uint32_t virtio_pci_get_status(void *data) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    return virtio_pci_read8(&pci->common_cfg_bar->device_status);
}

void virtio_pci_set_status(void *data, uint32_t status) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write8(&pci->common_cfg_bar->device_status, (uint8_t)status);
}

void virtio_pci_queue_set(void *data, uint16_t queue, uint32_t size,
                          uint64_t descriptors_paddr,
                          uint64_t driver_area_paddr,
                          uint64_t device_area_paddr) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write16(&pci->common_cfg_bar->queue_select, queue);
    virtio_pci_write16(&pci->common_cfg_bar->queue_size, (uint16_t)size);
    virtio_pci_write64(&pci->common_cfg_bar->queue_desc, descriptors_paddr);
    virtio_pci_write64(&pci->common_cfg_bar->queue_driver, driver_area_paddr);
    virtio_pci_write64(&pci->common_cfg_bar->queue_device, device_area_paddr);
    virtio_pci_configure_queue_irq(pci, queue);
    virtio_pci_write16(&pci->common_cfg_bar->queue_enable, 1);
}

bool virtio_pci_queue_used(void *data, uint16_t queue) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write16(&pci->common_cfg_bar->queue_select, queue);
    return virtio_pci_read16(&pci->common_cfg_bar->queue_enable) == 1;
}

bool virtio_pci_requires_legacy_layout(void *data) { return false; }

uint32_t virtio_pci_read_config_space(void *data, uint32_t offset) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    return virtio_pci_read32_reg(
        (volatile void *)(pci->config_space_vaddr + offset));
}

void virtio_pci_write_config_space(void *data, uint32_t offset,
                                   uint32_t value) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    virtio_pci_write32_reg((volatile void *)(pci->config_space_vaddr + offset),
                           value);
}

bool virtio_pci_supports_interrupts(void *data) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;
    return pci && pci->irq_enabled;
}

void virtio_pci_set_interrupt_handler(void *data,
                                      virtio_interrupt_handler_t handler,
                                      void *opaque) {
    virtio_pci_device_t *pci = (virtio_pci_device_t *)data;

    if (!pci)
        return;

    spin_lock(&pci->irq_lock);
    pci->irq_handler = handler;
    pci->irq_handler_opaque = opaque;
    spin_unlock(&pci->irq_lock);
}

static uint64_t virtio_pci_interrupt_seq(void *data) {
    virtio_pci_device_t *pci = data;
    return pci ? __atomic_load_n(&pci->irq_seq, __ATOMIC_ACQUIRE) : 0;
}

static int virtio_pci_wait_interrupt(void *data, uint64_t observed_seq,
                                     int64_t timeout_ns) {
    virtio_pci_device_t *pci = data;
    if (!pci || !pci->irq_enabled || !current_task)
        return -ENOTSUP;

    wait_queue_entry_t wait;
    task_prepare_block(current_task);
    wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
    wait_queue_add(&pci->irq_wait, &wait);

    int result = EOK;
    if (virtio_pci_interrupt_seq(pci) == observed_seq) {
        result = task_block(current_task, TASK_BLOCKING, timeout_ns,
                            "virtio_interrupt");
    } else {
        task_cancel_block_prepare(current_task);
    }

    wait_queue_remove(&pci->irq_wait, &wait);
    task_cancel_block_prepare(current_task);
    return result;
}

virtio_driver_op_t virtio_pci_driver_op = {
    .init = virtio_pci_init,
    .get_device_type = virtio_pci_get_device_type,
    .get_features = virtio_pci_get_features,
    .set_features = virtio_pci_set_features,
    .get_max_queue_size = virtio_pci_get_max_queue_size,
    .notify = virtio_pci_notify,
    .get_status = virtio_pci_get_status,
    .set_status = virtio_pci_set_status,
    .queue_set = virtio_pci_queue_set,
    .queue_used = virtio_pci_queue_used,
    .requires_legacy_layout = virtio_pci_requires_legacy_layout,
    .read_config_space = virtio_pci_read_config_space,
    .write_config_space = virtio_pci_write_config_space,
    .supports_interrupts = virtio_pci_supports_interrupts,
    .set_interrupt_handler = virtio_pci_set_interrupt_handler,
    .interrupt_seq = virtio_pci_interrupt_seq,
    .wait_interrupt = virtio_pci_wait_interrupt,
};
