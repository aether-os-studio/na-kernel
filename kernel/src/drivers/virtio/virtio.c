// Copyright (C) 2025-2026  lihanrui2913
#include "virtio.h"
#include "mmio.h"
#include "pci.h"

#define VIRTIO_MAX_BUS_DEVICES 32
#define VIRTIO_MAX_DEVICE_DRIVERS 16
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128
#define VIRTIO_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static virtio_driver_t *virtio_bus_devices[VIRTIO_MAX_BUS_DEVICES];
static virtio_device_driver_t *virtio_device_drivers[VIRTIO_MAX_DEVICE_DRIVERS];
static spinlock_t virtio_bus_lock = SPIN_INIT;
static bool virtio_bus_initialized = false;

static void virtio_mark_failed(virtio_driver_t *driver) {
    if (!driver || !driver->op || !driver->op->get_status ||
        !driver->op->set_status) {
        return;
    }

    uint32_t status = driver->op->get_status(driver->data);
    driver->op->set_status(driver->data, status | VIRTIO_STATUS_FAILED);
}

static int virtio_bind_device(virtio_driver_t *device,
                              virtio_device_driver_t *preferred) {
    virtio_device_driver_t *driver = preferred;

    if (!device || device->bound_driver) {
        return 0;
    }

    if (!driver) {
        spin_lock(&virtio_bus_lock);
        for (size_t i = 0; i < VIRTIO_ARRAY_SIZE(virtio_device_drivers); i++) {
            if (virtio_device_drivers[i] &&
                virtio_device_drivers[i]->device_type == device->device_type) {
                driver = virtio_device_drivers[i];
                break;
            }
        }
        spin_unlock(&virtio_bus_lock);
    }

    if (!driver || driver->device_type != device->device_type ||
        !driver->probe) {
        return -ENOENT;
    }

    device->bound_driver = driver;
    int ret = driver->probe(device);
    if (ret == 0) {
        return 0;
    }

    device->bound_driver = NULL;
    virtio_mark_failed(device);
    printk("virtio: %s probe failed for %s transport device type %u (ret=%d)\n",
           driver->name ? driver->name : "unnamed", device->transport_name,
           device->device_type, ret);
    return ret;
}

static int virtio_store_device(virtio_driver_t *device) {
    int slot = -1;

    spin_lock(&virtio_bus_lock);
    for (size_t i = 0; i < VIRTIO_ARRAY_SIZE(virtio_bus_devices); i++) {
        if (virtio_bus_devices[i] == device) {
            spin_unlock(&virtio_bus_lock);
            return 0;
        }
        if (slot < 0 && !virtio_bus_devices[i]) {
            slot = (int)i;
        }
    }
    if (slot >= 0) {
        virtio_bus_devices[slot] = device;
    }
    spin_unlock(&virtio_bus_lock);

    return slot >= 0 ? 0 : -ENOSPC;
}

static void virtio_remove_device(virtio_driver_t *device, bool shutdown) {
    if (!device) {
        return;
    }

    if (device->bound_driver) {
        if (shutdown) {
            if (device->bound_driver->shutdown) {
                device->bound_driver->shutdown(device);
            }
        } else if (device->bound_driver->remove) {
            device->bound_driver->remove(device);
        }
        device->bound_driver = NULL;
    }

    spin_lock(&virtio_bus_lock);
    for (size_t i = 0; i < VIRTIO_ARRAY_SIZE(virtio_bus_devices); i++) {
        if (virtio_bus_devices[i] == device) {
            virtio_bus_devices[i] = NULL;
            break;
        }
    }
    spin_unlock(&virtio_bus_lock);
}

static int virtio_register_transport_device(virtio_driver_t *driver,
                                            const char *transport_name) {
    if (!driver || !driver->op || !driver->op->get_device_type) {
        return -EINVAL;
    }

    driver->device_type = driver->op->get_device_type(driver->data);
    driver->transport_name = transport_name;
    driver->bound_driver = NULL;

    int ret = virtio_store_device(driver);
    if (ret < 0) {
        return ret;
    }

    ret = virtio_bind_device(driver, NULL);
    return ret == -ENOENT ? 0 : ret;
}

uint64_t virtio_begin_init(virtio_driver_t *driver,
                           uint64_t supported_features) {
    driver->op->set_status(driver->data, 0);
    driver->op->set_status(driver->data,
                           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint64_t device_features = driver->op->get_features(driver->data);
    uint64_t features = device_features & supported_features;

    if (!driver->op->requires_legacy_layout(driver->data) &&
        (device_features & VIRTIO_F_VERSION_1)) {
        features |= VIRTIO_F_VERSION_1;
    }

    driver->op->set_features(driver->data, features);

    driver->op->set_status(driver->data, VIRTIO_STATUS_ACKNOWLEDGE |
                                             VIRTIO_STATUS_DRIVER |
                                             VIRTIO_STATUS_FEATURES_OK);

    if (!(driver->op->get_status(driver->data) & VIRTIO_STATUS_FEATURES_OK)) {
        printk("virtio: device rejected negotiated features 0x%llx\n",
               features);
        return 0;
    }

    return features;
}

void virtio_finish_init(virtio_driver_t *driver) {
    driver->op->set_status(
        driver->data, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_DRIVER_OK | VIRTIO_STATUS_FEATURES_OK);
}

void virtio_driver_reset(virtio_driver_t *driver) {
    if (!driver || !driver->op || !driver->op->set_status)
        return;
    driver->op->set_status(driver->data, 0);
}

int virtio_register_device_driver(virtio_device_driver_t *driver) {
    int slot = -1;
    int first_error = 0;

    if (!driver || !driver->probe) {
        return -EINVAL;
    }

    spin_lock(&virtio_bus_lock);
    for (size_t i = 0; i < VIRTIO_ARRAY_SIZE(virtio_device_drivers); i++) {
        if (virtio_device_drivers[i] == driver) {
            spin_unlock(&virtio_bus_lock);
            return 0;
        }
        if (slot < 0 && !virtio_device_drivers[i]) {
            slot = (int)i;
        }
    }
    if (slot >= 0) {
        virtio_device_drivers[slot] = driver;
    }
    spin_unlock(&virtio_bus_lock);

    if (slot < 0) {
        return -ENOSPC;
    }

    for (size_t i = 0; i < VIRTIO_ARRAY_SIZE(virtio_bus_devices); i++) {
        virtio_driver_t *device = NULL;

        spin_lock(&virtio_bus_lock);
        if (virtio_bus_devices[i] &&
            virtio_bus_devices[i]->device_type == driver->device_type &&
            !virtio_bus_devices[i]->bound_driver) {
            device = virtio_bus_devices[i];
        }
        spin_unlock(&virtio_bus_lock);

        if (!device) {
            continue;
        }

        int ret = virtio_bind_device(device, driver);
        if (ret < 0 && ret != -ENOENT && first_error == 0) {
            first_error = ret;
        }
    }

    return first_error;
}

int virtio_unregister_device_driver(virtio_device_driver_t *driver) {
    if (!driver) {
        return -EINVAL;
    }

    spin_lock(&virtio_bus_lock);
    for (size_t i = 0; i < VIRTIO_ARRAY_SIZE(virtio_device_drivers); i++) {
        if (virtio_device_drivers[i] == driver) {
            virtio_device_drivers[i] = NULL;
            break;
        }
    }
    spin_unlock(&virtio_bus_lock);

    return 0;
}

bool virtio_driver_has_feature(virtio_driver_t *driver,
                               unsigned int feature_bit) {
    uint64_t features;

    if (!driver || !driver->op || !driver->op->get_features ||
        feature_bit >= 64)
        return false;

    features = driver->op->get_features(driver->data);
    return (features & (1ULL << feature_bit)) != 0;
}

uint32_t virtio_driver_read_config_u32(virtio_driver_t *driver,
                                       uint32_t offset) {
    if (!driver || !driver->op || !driver->op->read_config_space)
        return 0;
    return driver->op->read_config_space(driver->data, offset);
}

void virtio_driver_write_config_u32(virtio_driver_t *driver, uint32_t offset,
                                    uint32_t value) {
    if (!driver || !driver->op || !driver->op->write_config_space)
        return;
    driver->op->write_config_space(driver->data, offset, value);
}

bool virtio_driver_supports_interrupts(virtio_driver_t *driver) {
    if (!driver || !driver->op || !driver->op->supports_interrupts)
        return false;
    return driver->op->supports_interrupts(driver->data);
}

uint64_t virtio_driver_interrupt_seq(virtio_driver_t *driver) {
    if (!driver || !driver->op || !driver->op->interrupt_seq)
        return 0;
    return driver->op->interrupt_seq(driver->data);
}

int virtio_driver_wait_interrupt(virtio_driver_t *driver, uint64_t observed_seq,
                                 int64_t timeout_ns) {
    if (!driver || !driver->op || !driver->op->wait_interrupt)
        return -ENOTSUP;
    return driver->op->wait_interrupt(driver->data, observed_seq, timeout_ns);
}

void virtio_driver_set_interrupt_handler(virtio_driver_t *driver,
                                         virtio_interrupt_handler_t handler,
                                         void *opaque) {
    if (!driver || !driver->op || !driver->op->set_interrupt_handler)
        return;
    driver->op->set_interrupt_handler(driver->data, handler, opaque);
}

void *virtio_driver_parent_native(virtio_driver_t *driver) {
    if (!driver || !driver->op)
        return NULL;

    if (driver->op == &virtio_pci_driver_op) {
        virtio_pci_device_t *pci = (virtio_pci_device_t *)driver->data;
        return pci ? pci->pci_dev : NULL;
    }

    return NULL;
}

bool virtio_driver_get_shm_region(virtio_driver_t *driver, uint8_t id,
                                  uint64_t *addr, uint64_t *len) {
    if (addr)
        *addr = 0;
    if (len)
        *len = 0;

    if (!driver || !driver->op || id != 1)
        return false;

    if (driver->op == &virtio_pci_driver_op) {
        virtio_pci_device_t *pci = (virtio_pci_device_t *)driver->data;
        if (!pci || !pci->host_visible_shm_size)
            return false;
        if (addr)
            *addr = pci->host_visible_shm_paddr;
        if (len)
            *len = pci->host_visible_shm_size;
        return true;
    }

    return false;
}

virtio_device_type_t virtio_driver_get_device_type(virtio_driver_t *driver) {
    return driver ? driver->device_type : VIRTIO_DEVICE_TYPE_RESERVED;
}

static bool virtio_match(pci_device_t *dev, const pci_driver_t *driver) {
    (void)driver;
    return dev && dev->vendor_id == 0x1AF4;
}

static int virtio_probe(pci_device_t *dev) {
    virtio_driver_t *driver = virtio_pci_driver_op.init(dev);
    if (!driver) {
        printk("virtio: probe init failed for %04x:%02x:%02x.%u\n",
               dev->segment, dev->bus, dev->slot, dev->func);
        return -ENODEV;
    }

    int ret = virtio_register_transport_device(driver, "pci");
    if (ret < 0) {
        virtio_mark_failed(driver);
        return ret;
    }

    dev->desc = driver;
    return 0;
}

static void virtio_remove(pci_device_t *dev) {
    virtio_remove_device(dev ? (virtio_driver_t *)dev->desc : NULL, false);
}

static void virtio_shutdown(pci_device_t *dev) {
    virtio_remove_device(dev ? (virtio_driver_t *)dev->desc : NULL, true);
}

static pci_driver_t virtio_pci_driver = {
    .name = "virtio",
    .class_id = 0,
    .match = virtio_match,
    .probe = virtio_probe,
    .remove = virtio_remove,
    .shutdown = virtio_shutdown,
    .flags = 0,
    .private_data = NULL,
};

#if !defined(__x86_64__)
static int virtio_mmio_fdt_probe(fdt_device_t *fdt_dev,
                                 const char *compatible) {
    (void)compatible;

    int len = 0;
    const uint32_t *reg_prop =
        fdt_getprop(fdt_dev->fdt, fdt_dev->node, "reg", &len);
    if (!reg_prop || len < 8) {
        printk("virtio-mmio: missing reg property\n");
        return -EINVAL;
    }

    uint64_t base_addr = fdt32_to_cpu(reg_prop[0]);
    uint64_t size = fdt32_to_cpu(reg_prop[1]);
    if (len >= 16) {
        base_addr = ((uint64_t)fdt32_to_cpu(reg_prop[0]) << 32) |
                    fdt32_to_cpu(reg_prop[1]);
        size = ((uint64_t)fdt32_to_cpu(reg_prop[2]) << 32) |
               fdt32_to_cpu(reg_prop[3]);
    }

    uint32_t irq = 0;
    const uint32_t *irq_prop =
        fdt_getprop(fdt_dev->fdt, fdt_dev->node, "interrupts", &len);
    if (irq_prop && len >= 4) {
        irq = fdt32_to_cpu(irq_prop[0]);
    }

    virtio_mmio_device_t *mmio_dev = malloc(sizeof(*mmio_dev));
    if (!mmio_dev) {
        return -ENOMEM;
    }
    memset(mmio_dev, 0, sizeof(*mmio_dev));

    volatile uint8_t *virt = (volatile uint8_t *)phys_to_virt(base_addr);
    map_page_range(get_current_page_dir(false), (uint64_t)virt, base_addr, size,
                   PT_FLAG_R | PT_FLAG_W | PT_FLAG_DEVICE);
    mmio_dev->base = virt;
    mmio_dev->irq = irq;

    virtio_driver_t *driver = virtio_mmio_ops.init(mmio_dev);
    if (!driver) {
        free(mmio_dev);
        return -ENODEV;
    }

    int ret = virtio_register_transport_device(driver, "mmio");
    if (ret < 0) {
        virtio_mark_failed(driver);
        return ret;
    }

    fdt_dev->driver_data = driver;
    return 0;
}

static void virtio_mmio_fdt_remove(fdt_device_t *fdt_dev) {
    virtio_driver_t *driver =
        fdt_dev ? (virtio_driver_t *)fdt_dev->driver_data : NULL;
    if (!driver) {
        return;
    }

    virtio_remove_device(driver, false);
    if (driver->op && driver->op->set_status) {
        driver->op->set_status(driver->data, 0);
    }
    fdt_dev->driver_data = NULL;
}

static const char *virtio_mmio_compatible[] = {"virtio,mmio", NULL};

static fdt_driver_t virtio_mmio_driver = {
    .name = "virtio-mmio",
    .compatible = virtio_mmio_compatible,
    .probe = virtio_mmio_fdt_probe,
    .remove = virtio_mmio_fdt_remove,
    .shutdown = NULL,
    .flags = 0,
};
#endif

void virtio_bus_init(void) {
    if (virtio_bus_initialized) {
        return;
    }

    virtio_bus_initialized = true;
    regist_pci_driver(&virtio_pci_driver);
#if !defined(__x86_64__)
    regist_fdt_driver(&virtio_mmio_driver);
#endif
}
