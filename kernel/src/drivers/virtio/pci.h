#pragma once

#include <drivers/bus/pci.h>
#include <drivers/bus/pci_msi.h>
#include <task/wait.h>
#include "virtio.h"

typedef struct virtio_cap_info_t {
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
} virtio_cap_info_t;

#define PCI_DEVICE_ID_NETWORK 0x1000
#define PCI_DEVICE_ID_BLOCK 0x1001
#define PCI_DEVICE_ID_OFFSET 0x1040
#define VIRTIO_PCI_MAX_QUEUES 64

typedef struct virtio_pci_common_cfg_t {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} virtio_pci_common_cfg_t;

typedef struct virtio_pci_device {
    pci_device_t *pci_dev;
    virtio_device_type_t device_type;
    virtio_cap_info_t *common_cfg;
    virtio_pci_common_cfg_t *common_cfg_bar;
    uint16_t *notify_regions;
    uint32_t notify_off_multiplier;
    volatile uint16_t *notify_addr[VIRTIO_PCI_MAX_QUEUES];
    volatile uint8_t *isr_status;
    virtio_cap_info_t *device_cfg;
    uint64_t config_space_vaddr;
    uint64_t host_visible_shm_paddr;
    uint64_t host_visible_shm_size;
    struct msi_desc_t msi;
    wait_queue_head_t irq_wait;
    spinlock_t irq_lock;
    uint32_t irq_seq;
    bool irq_enabled;
    bool irq_msix;
    virtio_interrupt_handler_t irq_handler;
    void *irq_handler_opaque;
} virtio_pci_device_t;

extern virtio_driver_op_t virtio_pci_driver_op;
