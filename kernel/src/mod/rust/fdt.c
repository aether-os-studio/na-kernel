#include <drivers/fdt/fdt.h>
#include <libs/klibc.h>
#include <mod/rust/api.h>

typedef struct na_fdt_driver_bridge {
    fdt_driver_t driver;
    na_fdt_driver_ops_t ops;
    const char **compatible;
} na_fdt_driver_bridge_t;

static int na_fdt_probe(fdt_device_t *device, const char *compatible) {
    if (!device || !device->driver)
        return -ENODEV;

    na_fdt_driver_bridge_t *bridge =
        container_of(device->driver, na_fdt_driver_bridge_t, driver);
    return bridge->ops.probe
               ? bridge->ops.probe(bridge->ops.context, device, compatible)
               : -ENOSYS;
}

int na_fdt_driver_register(const char *name, const char *compatible_blob,
                           size_t compatible_blob_size,
                           const na_fdt_driver_ops_t *ops) {
    if (!name || !compatible_blob || !compatible_blob_size ||
        compatible_blob[compatible_blob_size - 1] != '\0' || !ops ||
        !ops->probe)
        return -EINVAL;

    size_t compatible_count = 0;
    for (size_t offset = 0; offset < compatible_blob_size;) {
        size_t length =
            strnlen(compatible_blob + offset, compatible_blob_size - offset);
        if (!length || length == compatible_blob_size - offset)
            return -EINVAL;
        compatible_count++;
        offset += length + 1;
    }

    na_fdt_driver_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return -ENOMEM;

    bridge->compatible =
        calloc(compatible_count + 1, sizeof(*bridge->compatible));
    if (!bridge->compatible) {
        free(bridge);
        return -ENOMEM;
    }

    size_t offset = 0;
    for (size_t i = 0; i < compatible_count; i++) {
        bridge->compatible[i] = compatible_blob + offset;
        offset += strlen(compatible_blob + offset) + 1;
    }

    bridge->ops = *ops;
    bridge->driver = (fdt_driver_t){
        .name = name,
        .compatible = bridge->compatible,
        .probe = na_fdt_probe,
    };

    int result = regist_fdt_driver(&bridge->driver);
    if (result < 0) {
        free(bridge->compatible);
        free(bridge);
    }
    return result;
}

const char *na_fdt_device_name(const fdt_device_t *device) {
    return device ? device->name : NULL;
}

const void *na_fdt_device_property(const fdt_device_t *device, const char *name,
                                   size_t *length) {
    if (!device || !device->fdt || !name || !length)
        return NULL;

    int property_length = 0;
    const void *property =
        fdt_getprop(device->fdt, device->node, name, &property_length);
    if (!property || property_length < 0)
        return NULL;
    *length = (size_t)property_length;
    return property;
}

int na_fdt_device_reg_cells(const fdt_device_t *device, uint32_t *address_cells,
                            uint32_t *size_cells) {
    if (!device || !device->fdt || !address_cells || !size_cells)
        return -EINVAL;

    int parent = fdt_parent_offset(device->fdt, device->node);
    if (parent < 0)
        return parent;
    int address = fdt_address_cells(device->fdt, parent);
    int size = fdt_size_cells(device->fdt, parent);
    if (address < 0)
        return address;
    if (size < 0)
        return size;

    *address_cells = (uint32_t)address;
    *size_cells = (uint32_t)size;
    return 0;
}
