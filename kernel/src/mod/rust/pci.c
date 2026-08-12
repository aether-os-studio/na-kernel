#include <drivers/bus/pci.h>
#include <libs/klibc.h>
#include <mod/rust/api.h>

typedef struct na_pci_driver_bridge {
    pci_driver_t driver;
    na_pci_driver_ops_t ops;
} na_pci_driver_bridge_t;

static bool na_pci_match(pci_device_t *device, const pci_driver_t *driver) {
    na_pci_driver_bridge_t *bridge =
        container_of(driver, na_pci_driver_bridge_t, driver);
    return bridge->ops.matches &&
           bridge->ops.matches(bridge->ops.context, device);
}

static int na_pci_probe(pci_device_t *device) {
    pci_driver_t *driver = pci_get_current_probe_driver();
    if (!driver)
        return -ENODEV;

    na_pci_driver_bridge_t *bridge =
        container_of(driver, na_pci_driver_bridge_t, driver);
    return bridge->ops.probe ? bridge->ops.probe(bridge->ops.context, device)
                             : -ENOSYS;
}

int na_pci_device_info(pci_device_t *device, na_pci_device_info_t *info) {
    if (!device || !info)
        return -EINVAL;

    *info = (na_pci_device_info_t){
        .class_code = device->class_code,
        .vendor_id = device->vendor_id,
        .device_id = device->device_id,
        .subsystem_vendor_id = device->subsystem_vendor_id,
        .subsystem_device_id = device->subsystem_device_id,
        .segment = device->segment,
        .revision_id = device->revision_id,
        .bus = device->bus,
        .slot = device->slot,
        .function = device->func,
        .irq_line = device->irq_line,
        .irq_pin = device->irq_pin,
    };
    return 0;
}

int na_pci_bar_info(pci_device_t *device, uint8_t index,
                    na_pci_bar_info_t *info) {
    if (!device || !info || index >= 6)
        return -EINVAL;

    pci_bar_t *bar = &device->bars[index];
    if (!bar->size)
        return -ENOENT;

    *info = (na_pci_bar_info_t){
        .address = bar->address,
        .size = bar->size,
        .is_mmio = bar->mmio,
        .prefetchable = bar->prefetchable,
    };
    return 0;
}

static bool na_pci_config_valid(pci_device_t *device, uint16_t offset,
                                uint8_t width) {
    if (!device || !device->op || offset > 4096 - width)
        return false;
    return width == 1 || width == 2 || width == 4;
}

int na_pci_config_read(pci_device_t *device, uint16_t offset, uint8_t width,
                       uint32_t *value) {
    if (!value || !na_pci_config_valid(device, offset, width))
        return -EINVAL;

    pci_device_op_t *op = device->op;
    switch (width) {
    case 1:
        *value = op->read8(device->bus, device->slot, device->func,
                           device->segment, offset);
        break;
    case 2:
        *value = op->read16(device->bus, device->slot, device->func,
                            device->segment, offset);
        break;
    default:
        *value = op->read32(device->bus, device->slot, device->func,
                            device->segment, offset);
        break;
    }
    return 0;
}

int na_pci_config_write(pci_device_t *device, uint16_t offset, uint8_t width,
                        uint32_t value) {
    if (!na_pci_config_valid(device, offset, width))
        return -EINVAL;

    pci_device_op_t *op = device->op;
    switch (width) {
    case 1:
        op->write8(device->bus, device->slot, device->func, device->segment,
                   offset, (uint8_t)value);
        break;
    case 2:
        op->write16(device->bus, device->slot, device->func, device->segment,
                    offset, (uint16_t)value);
        break;
    default:
        op->write32(device->bus, device->slot, device->func, device->segment,
                    offset, value);
        break;
    }
    return 0;
}

int na_pci_bar_claim(pci_device_t *device, uint8_t index) {
    uint8_t mask;

    if (!device || index >= 6 || !device->bars[index].size)
        return -EINVAL;
    mask = (uint8_t)(1U << index);
    if (__atomic_fetch_or(&device->claimed_bars, mask, __ATOMIC_ACQ_REL) & mask)
        return -EBUSY;
    return 0;
}

void na_pci_bar_release(pci_device_t *device, uint8_t index) {
    if (!device || index >= 6)
        return;
    __atomic_fetch_and(&device->claimed_bars, (uint8_t)~(1U << index),
                       __ATOMIC_ACQ_REL);
}

int na_pci_driver_register(const char *name, uint32_t class_id, int flags,
                           const na_pci_driver_ops_t *ops) {
    if (!name || !ops || !ops->probe)
        return -EINVAL;

    na_pci_driver_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return -ENOMEM;

    bridge->ops = *ops;
    bridge->driver = (pci_driver_t){
        .name = name,
        .class_id = class_id,
        .match = ops->matches ? na_pci_match : NULL,
        .probe = na_pci_probe,
        .flags = flags,
    };

    int result = regist_pci_driver(&bridge->driver);
    if (result < 0)
        free(bridge);
    return result;
}
