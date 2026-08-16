#include <drivers/bus/usb.h>
#include <limits.h>
#include <mm/mm.h>
#include <mod/rust/api.h>

typedef struct na_usb_binding {
    usb_device_t *device;
    void *context;
    struct na_usb_binding *next;
} na_usb_binding_t;

typedef struct na_usb_registration {
    usb_driver_t driver;
    na_usb_driver_ops_t ops;
    usb_device_id_t *ids;
    na_usb_binding_t *bindings;
    spinlock_t lock;
} na_usb_registration_t;

struct na_usb_pipe {
    usb_device_t *device;
    usb_pipe_t *pipe;
    uint8_t direction;
};

static na_usb_registration_t *na_usb_current_registration(bool removing) {
    usb_driver_t *driver = removing ? usb_get_current_remove_driver()
                                    : usb_get_current_probe_driver();
    return (na_usb_registration_t *)driver;
}

static bool na_usb_is_bound(na_usb_registration_t *registration,
                            usb_device_t *device) {
    bool found = false;

    spin_lock(&registration->lock);
    for (na_usb_binding_t *binding = registration->bindings; binding;
         binding = binding->next) {
        if (binding->device == device) {
            found = true;
            break;
        }
    }
    spin_unlock(&registration->lock);
    return found;
}

static int na_usb_probe(usb_device_t *device,
                        usb_device_interface_t *interface) {
    na_usb_registration_t *registration = na_usb_current_registration(false);
    na_usb_binding_t *binding;
    int status;

    if (!registration || !device || !interface)
        return -EINVAL;
    if (na_usb_is_bound(registration, device))
        return 0;

    binding = calloc(1, sizeof(*binding));
    if (!binding)
        return -ENOMEM;

    status = registration->ops.probe(
        registration->ops.context, (na_usb_device_t *)device,
        (na_usb_interface_t *)interface, &binding->context);
    if (status < 0 || !binding->context) {
        free(binding);
        return status < 0 ? status : -EINVAL;
    }

    binding->device = device;
    spin_lock(&registration->lock);
    binding->next = registration->bindings;
    registration->bindings = binding;
    spin_unlock(&registration->lock);
    return 0;
}

static int na_usb_remove(usb_device_t *device) {
    na_usb_registration_t *registration = na_usb_current_registration(true);
    na_usb_binding_t *binding = NULL;
    na_usb_binding_t **link;

    if (!registration || !device)
        return -EINVAL;

    spin_lock(&registration->lock);
    for (link = &registration->bindings; *link; link = &(*link)->next) {
        if ((*link)->device != device)
            continue;
        binding = *link;
        *link = binding->next;
        break;
    }
    spin_unlock(&registration->lock);

    if (binding) {
        registration->ops.remove(registration->ops.context, binding->context);
        free(binding);
    }
    return 0;
}

int na_usb_driver_register(const char *name, const na_usb_device_id_t *ids,
                           size_t id_count, int priority,
                           const na_usb_driver_ops_t *ops) {
    na_usb_registration_t *registration;
    int status;

    if (!name || !ids || !id_count || !ops || !ops->probe || !ops->remove ||
        id_count > (SIZE_MAX / sizeof(usb_device_id_t)) - 1)
        return -EINVAL;

    registration = calloc(1, sizeof(*registration));
    if (!registration)
        return -ENOMEM;
    registration->ids = calloc(id_count + 1, sizeof(*registration->ids));
    if (!registration->ids) {
        free(registration);
        return -ENOMEM;
    }

    for (size_t index = 0; index < id_count; index++) {
        registration->ids[index] = (usb_device_id_t){
            .match_flags = ids[index].match_flags,
            .idVendor = ids[index].vendor_id,
            .idProduct = ids[index].product_id,
            .bInterfaceClass = ids[index].interface_class,
            .bInterfaceSubClass = ids[index].interface_subclass,
            .bInterfaceProtocol = ids[index].interface_protocol,
        };
    }

    registration->ops = *ops;
    registration->driver = (usb_driver_t){
        .name = name,
        .id_table = registration->ids,
        .priority = priority,
        .probe = na_usb_probe,
        .remove = na_usb_remove,
    };
    spin_init(&registration->lock);

    status = regist_usb_driver(&registration->driver);
    if (status < 0) {
        free(registration->ids);
        free(registration);
    }
    return status;
}

int na_usb_device_info(const na_usb_device_t *device,
                       na_usb_device_info_t *info) {
    const usb_device_t *usb = (const usb_device_t *)device;

    if (!usb || !info)
        return -EINVAL;
    *info = (na_usb_device_info_t){
        .vendor_id = usb->vendorid,
        .product_id = usb->productid,
        .speed = usb->speed,
        .bus_number = usb->busnum,
        .device_number = usb->devnum,
        .address = usb->devaddr,
    };
    return 0;
}

size_t na_usb_device_interface_count(const na_usb_device_t *device) {
    const usb_device_t *usb = (const usb_device_t *)device;
    return usb && usb->ifaces_num > 0 ? (size_t)usb->ifaces_num : 0;
}

na_usb_interface_t *na_usb_device_interface_at(const na_usb_device_t *device,
                                               size_t index) {
    usb_device_t *usb = (usb_device_t *)device;

    if (!usb || index >= na_usb_device_interface_count(device))
        return NULL;
    return (na_usb_interface_t *)&usb->ifaces[index];
}

int na_usb_interface_info(const na_usb_interface_t *interface,
                          na_usb_interface_info_t *info) {
    const usb_device_interface_t *usb =
        (const usb_device_interface_t *)interface;

    if (!usb || !usb->iface || !info)
        return -EINVAL;
    *info = (na_usb_interface_info_t){
        .number = usb->iface->bInterfaceNumber,
        .alternate_setting = usb->iface->bAlternateSetting,
        .class_code = usb->iface->bInterfaceClass,
        .subclass = usb->iface->bInterfaceSubClass,
        .protocol = usb->iface->bInterfaceProtocol,
    };
    return 0;
}

static usb_endpoint_descriptor_t *
na_usb_find_endpoint(usb_device_interface_t *interface, uint8_t transfer_type,
                     uint8_t direction,
                     usb_super_speed_endpoint_descriptor_t **companion) {
    uint8_t *cursor;
    uint8_t *end;

    if (!interface || !interface->iface || !companion)
        return NULL;
    *companion = NULL;
    cursor = (uint8_t *)interface->iface + interface->iface->bLength;
    end = interface->end;

    while (cursor && end && cursor + 2 <= end) {
        uint8_t length = cursor[0];
        uint8_t descriptor_type = cursor[1];

        if (length < 2 || cursor + length > end ||
            descriptor_type == USB_DT_INTERFACE)
            break;
        if (descriptor_type == USB_DT_ENDPOINT) {
            usb_endpoint_descriptor_t *endpoint = (void *)cursor;
            bool type_matches = (endpoint->bmAttributes &
                                 USB_ENDPOINT_XFERTYPE_MASK) == transfer_type;
            bool direction_matches = (endpoint->bEndpointAddress &
                                      USB_ENDPOINT_DIR_MASK) == direction;
            if (type_matches && direction_matches) {
                uint8_t *next = cursor + length;
                if (next + 2 <= end && next[0] >= 2 && next + next[0] <= end &&
                    next[1] == USB_DT_ENDPOINT_COMPANION)
                    *companion = (void *)next;
                return endpoint;
            }
        }
        cursor += length;
    }
    return NULL;
}

int na_usb_pipe_open(na_usb_interface_t *interface, uint8_t transfer_type,
                     uint8_t direction, na_usb_pipe_t **output) {
    usb_device_interface_t *usb_interface = (usb_device_interface_t *)interface;
    usb_super_speed_endpoint_descriptor_t *companion;
    usb_endpoint_descriptor_t *endpoint;
    na_usb_pipe_t *pipe;

    if (!usb_interface || !usb_interface->usbdev || !output ||
        transfer_type > USB_ENDPOINT_XFER_INT ||
        (direction != USB_DIR_OUT && direction != USB_DIR_IN))
        return -EINVAL;
    *output = NULL;

    endpoint = na_usb_find_endpoint(usb_interface, transfer_type, direction,
                                    &companion);
    if (!endpoint)
        return -ENODEV;

    pipe = calloc(1, sizeof(*pipe));
    if (!pipe)
        return -ENOMEM;
    pipe->device = usb_interface->usbdev;
    pipe->direction = direction;
    pipe->pipe = usb_alloc_pipe(pipe->device, endpoint, companion);
    if (!pipe->pipe) {
        free(pipe);
        return -ENODEV;
    }

    *output = pipe;
    return 0;
}

void na_usb_pipe_close(na_usb_pipe_t *pipe) {
    if (!pipe)
        return;
    if (pipe->pipe)
        usb_free_pipe(pipe->device, pipe->pipe);
    free(pipe);
}

static int na_usb_pipe_transfer(na_usb_pipe_t *pipe, uint8_t direction,
                                void *data, size_t size, size_t *actual_size) {
    int actual = 0;
    int status;

    if (!pipe || !pipe->pipe || !actual_size || pipe->direction != direction ||
        (size && !data) || size > INT_MAX)
        return -EINVAL;
    *actual_size = 0;
    if (!size)
        return 0;

    usb_xfer_t transfer = {
        .pipe = pipe->pipe,
        .dir = direction,
        .data = data,
        .datasize = (int)size,
        .timeout_ns = (uint64_t)-1,
        .actual_length_out = &actual,
    };
    status = usb_submit_xfer(&transfer);
    if (status != 0)
        return status == -1 ? -EIO : status;
    if (actual < 0 || (size_t)actual > size)
        return -EIO;
    *actual_size = (size_t)actual;
    return 0;
}

int na_usb_pipe_read(na_usb_pipe_t *pipe, void *data, size_t size,
                     size_t *actual_size) {
    return na_usb_pipe_transfer(pipe, USB_DIR_IN, data, size, actual_size);
}

int na_usb_pipe_write(na_usb_pipe_t *pipe, const void *data, size_t size,
                      size_t *actual_size) {
    return na_usb_pipe_transfer(pipe, USB_DIR_OUT, (void *)data, size,
                                actual_size);
}

int na_usb_control_transfer(na_usb_device_t *device, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            void *data, size_t size, size_t *actual_size) {
    usb_device_t *usb = (usb_device_t *)device;
    usb_ctrl_request_t control;
    int actual = 0;
    int status;

    if (!usb || !usb->defpipe || !actual_size || (size && !data) ||
        size > UINT16_MAX)
        return -EINVAL;
    *actual_size = 0;
    control = (usb_ctrl_request_t){
        .bRequestType = request_type,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = (uint16_t)size,
    };
    usb_xfer_t transfer = {
        .pipe = usb->defpipe,
        .dir = request_type & USB_DIR_IN,
        .cmd = &control,
        .data = data,
        .datasize = (int)size,
        .timeout_ns = (uint64_t)-1,
        .actual_length_out = &actual,
    };
    status = usb_submit_xfer(&transfer);
    if (status != 0)
        return status == -1 ? -EIO : status;
    if (actual < 0 || (size_t)actual > size)
        return -EIO;
    *actual_size = (size_t)actual;
    return 0;
}
