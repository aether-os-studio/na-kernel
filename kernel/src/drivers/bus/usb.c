// Rewrite version from seabios usb.c

#include <arch/arch.h>
#include <drivers/bus/bus.h>
#include <drivers/logger.h>
#include <drivers/bus/usb.h>
#include <mm/mm.h>
#include <task/task.h>
#include <task/wait.h>

attribute_t usb_bus_subsystem_attr = {
    .name = "SUBSYSTEM",
    .value = "usb",
};

attribute_t *usb_bus_default_attrs[] = {
    &usb_bus_subsystem_attr,
};

bus_t usb_bus = {
    .name = "usb",
    .devices_path = "/sys/bus/usb/devices",
    .drivers_path = "/sys/bus/usb/drivers",
    .bus_default_attrs = usb_bus_default_attrs,
    .bus_default_attrs_count =
        sizeof(usb_bus_default_attrs) / sizeof(usb_bus_default_attrs[0]),
    .bus_default_bin_attrs = NULL,
    .bus_default_bin_attrs_count = 0,
};

int usb_get_device_path(bus_device_t *device, char *buf, size_t max) {
    usb_device_t *usb_device = device->private_data;
    snprintf(buf, max, "%s", usb_device->topology);
    return 0;
}

static int usb_get_interface_path(bus_device_t *device, char *buf, size_t max) {
    usb_device_interface_t *iface = device->private_data;
    snprintf(buf, max, "%s:1.%u", iface->usbdev->topology,
             iface->iface->bInterfaceNumber);
    return 0;
}

bus_device_t *bus_device_install_usb(void *dev_data, attribute_t **extra_attrs,
                                     int extra_attrs_count,
                                     bin_attribute_t **extra_bin_attrs,
                                     int extra_bin_attrs_count) {
    return bus_device_install_internal(
        &usb_bus, dev_data, extra_attrs, extra_attrs_count, extra_bin_attrs,
        extra_bin_attrs_count, usb_get_device_path);
}

#define USB_HUB_SNAPSHOT_MAX 64

usb_driver_t *usb_drivers[MAX_USBDEV_NUM] = {NULL};
static usb_driver_t *usb_current_probe_driver;
static usb_driver_t *usb_current_remove_driver;

static usb_bus_notifier_ops_t *usb_bus_notifier;
static spinlock_t usb_notifier_lock = SPIN_INIT;

static struct llist_header usb_hub_list = {
    .prev = &usb_hub_list,
    .next = &usb_hub_list,
};
static spinlock_t usb_hub_list_lock = SPIN_INIT;
static task_t *usb_hotplug_task;
static spinlock_t usb_hotplug_lock = SPIN_INIT;
static uint8_t usb_next_busnum = 1;

static void usb_register_devnode(usb_device_t *usbdev);
static void usb_unregister_devnode(usb_device_t *usbdev);

typedef struct usbdevfs_ctrltransfer {
    uint8_t bRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint32_t timeout;
    void *data;
} __attribute__((packed)) usbdevfs_ctrltransfer_t;

typedef struct usbdevfs_bulktransfer {
    unsigned int ep;
    unsigned int len;
    unsigned int timeout;
    void *data;
} usbdevfs_bulktransfer_t;

typedef struct usbdevfs_setinterface {
    unsigned int interface;
    unsigned int altsetting;
} usbdevfs_setinterface_t;

typedef struct usbdevfs_getdriver {
    unsigned int interface;
    char driver[256];
} usbdevfs_getdriver_t;

typedef struct usbdevfs_connectinfo {
    unsigned int devnum;
    unsigned char slow;
} usbdevfs_connectinfo_t;

typedef struct usbdevfs_conninfo_ex {
    uint32_t size;
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint8_t num_ports;
    uint8_t ports[7];
} usbdevfs_conninfo_ex_t;

typedef struct usbdevfs_hub_portinfo {
    char nports;
    char port[127];
} usbdevfs_hub_portinfo_t;

typedef struct usb_root_hub_config {
    usb_config_descriptor_t config;
    usb_interface_descriptor_t iface;
    usb_endpoint_descriptor_t ep;
    usb_super_speed_endpoint_descriptor_t ss_ep;
} __attribute__((packed)) usb_root_hub_config_t;

#define USBDEVFS_CONTROL _IOWR('U', 0, usbdevfs_ctrltransfer_t)
#define USBDEVFS_BULK _IOWR('U', 2, usbdevfs_bulktransfer_t)
#define USBDEVFS_SETINTERFACE _IOR('U', 4, usbdevfs_setinterface_t)
#define USBDEVFS_SETCONFIGURATION _IOR('U', 5, unsigned int)
#define USBDEVFS_GETDRIVER _IOW('U', 8, usbdevfs_getdriver_t)
#define USBDEVFS_CLAIMINTERFACE _IOR('U', 15, unsigned int)
#define USBDEVFS_RELEASEINTERFACE _IOR('U', 16, unsigned int)
#define USBDEVFS_CONNECTINFO _IOW('U', 17, usbdevfs_connectinfo_t)
#define USBDEVFS_HUB_PORTINFO _IOR('U', 19, usbdevfs_hub_portinfo_t)
#define USBDEVFS_GET_CAPABILITIES _IOR('U', 26, uint32_t)
#define USBDEVFS_GET_SPEED _IO('U', 31)

#define USBDEVFS_CAP_ZERO_PACKET 0x01
#define USBDEVFS_CAP_CONNINFO_EX 0x80

#define USB_SPEED_UNKNOWN 0
#define USB_SPEED_LOW 1
#define USB_SPEED_FULL 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_WIRELESS 4
#define USB_SPEED_SUPER 5
#define USB_SPEED_SUPER_PLUS 6

static const char *usb_root_hub_product_name(uint8_t type) {
    switch (type) {
    case USB_TYPE_XHCI:
        return "xHCI Root Hub";
    case USB_TYPE_EHCI:
        return "EHCI Root Hub";
    case USB_TYPE_OHCI:
        return "OHCI Root Hub";
    case USB_TYPE_UHCI:
        return "UHCI Root Hub";
    default:
        return "USB Root Hub";
    }
}

static const char *usb_speed_sysfs_name(uint8_t speed) {
    switch (speed) {
    case USB_LOWSPEED:
        return "1.5";
    case USB_FULLSPEED:
        return "12";
    case USB_HIGHSPEED:
        return "480";
    case USB_SUPERSPEED:
        return "5000";
    default:
        return "0";
    }
}

static void usb_format_bcd(char *buf, size_t size, uint16_t value) {
    snprintf(buf, size, "%x.%02x", (value >> 8) & 0xff, value & 0xff);
}

static void usb_register_bus_device(usb_device_t *usbdev) {
    if (!usbdev || usbdev->bus_device)
        return;

    attributes_builder_t *builder = attributes_builder_new();
    if (!builder)
        return;
    char value[256];

    snprintf(value, sizeof(value), "/devices/usb/%s", usbdev->topology);
    if (attributes_builder_append_new(builder, "DEVPATH", value) < 0)
        goto err_attrs;
    if (attributes_builder_append_new(builder, "DEVTYPE", "usb_device") < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%u", usbdev->busnum);
    if (attributes_builder_append_new(builder, "busnum", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%u", usbdev->devnum);
    if (attributes_builder_append_new(builder, "devnum", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%s", usbdev->topology);
    if (attributes_builder_append_new(builder, "devpath", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%04x", usbdev->vendorid);
    if (attributes_builder_append_new(builder, "idVendor", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%04x", usbdev->productid);
    if (attributes_builder_append_new(builder, "idProduct", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%02x", usbdev->device_desc.bDeviceClass);
    if (attributes_builder_append_new(builder, "bDeviceClass", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%02x", usbdev->device_desc.bDeviceSubClass);
    if (attributes_builder_append_new(builder, "bDeviceSubClass", value) < 0)
        goto err_attrs;

    snprintf(value, sizeof(value), "%02x", usbdev->device_desc.bDeviceProtocol);
    if (attributes_builder_append_new(builder, "bDeviceProtocol", value) < 0)
        goto err_attrs;

    usb_format_bcd(value, sizeof(value), usbdev->device_desc.bcdUSB);
    if (attributes_builder_append_new(builder, "version", value) < 0)
        goto err_attrs;

    usb_format_bcd(value, sizeof(value), usbdev->device_desc.bcdDevice);
    if (attributes_builder_append_new(builder, "bcdDevice", value) < 0)
        goto err_attrs;

    if (attributes_builder_append_new(builder, "speed",
                                      usb_speed_sysfs_name(usbdev->speed)) < 0)
        goto err_attrs;

    if (usbdev->manufacturer[0])
        if (attributes_builder_append_new(builder, "manufacturer",
                                          usbdev->manufacturer) < 0)
            goto err_attrs;
    if (usbdev->product[0])
        if (attributes_builder_append_new(builder, "product", usbdev->product) <
            0)
            goto err_attrs;
    if (usbdev->serial[0])
        if (attributes_builder_append_new(builder, "serial", usbdev->serial) <
            0)
            goto err_attrs;

    usbdev->bus_device =
        bus_device_install_usb(usbdev, builder->attrs, builder->count, NULL, 0);
    if (!usbdev->bus_device)
        goto err_attrs;

    attributes_builder_free_all(builder);
    return;

err_attrs:
    attributes_builder_free_all(builder);
}

static void usb_register_interface_bus_devices(usb_device_t *usbdev) {
    if (!usbdev || !usbdev->ifaces || usbdev->ifaces_num <= 0)
        return;

    for (int i = 0; i < usbdev->ifaces_num; i++) {
        usb_device_interface_t *iface = &usbdev->ifaces[i];
        if (iface->bus_device)
            continue;

        attributes_builder_t *builder = attributes_builder_new();
        if (!builder)
            return;
        char value[256];

        snprintf(value, sizeof(value), "/devices/usb/%s/%s:1.%u",
                 usbdev->topology, usbdev->topology,
                 iface->iface->bInterfaceNumber);
        if (attributes_builder_append_new(builder, "DEVPATH", value) < 0)
            goto err_attrs;
        if (attributes_builder_append_new(builder, "DEVTYPE", "usb_interface") <
            0)
            goto err_attrs;

        snprintf(value, sizeof(value), "%02x", iface->iface->bInterfaceNumber);
        if (attributes_builder_append_new(builder, "bInterfaceNumber", value) <
            0)
            goto err_attrs;

        snprintf(value, sizeof(value), "%02x", iface->iface->bAlternateSetting);
        if (attributes_builder_append_new(builder, "bAlternateSetting", value) <
            0)
            goto err_attrs;

        snprintf(value, sizeof(value), "%02x", iface->iface->bNumEndpoints);
        if (attributes_builder_append_new(builder, "bNumEndpoints", value) < 0)
            goto err_attrs;

        snprintf(value, sizeof(value), "%02x", iface->iface->bInterfaceClass);
        if (attributes_builder_append_new(builder, "bInterfaceClass", value) <
            0)
            goto err_attrs;

        snprintf(value, sizeof(value), "%02x",
                 iface->iface->bInterfaceSubClass);
        if (attributes_builder_append_new(builder, "bInterfaceSubClass",
                                          value) < 0)
            goto err_attrs;

        snprintf(value, sizeof(value), "%02x",
                 iface->iface->bInterfaceProtocol);
        if (attributes_builder_append_new(builder, "bInterfaceProtocol",
                                          value) < 0)
            goto err_attrs;

        iface->bus_device = bus_device_install_internal(
            &usb_bus, iface, builder->attrs, builder->count, NULL, 0,
            usb_get_interface_path);
        if (!iface->bus_device)
            goto err_attrs;

        attributes_builder_free_all(builder);
        continue;

    err_attrs:
        attributes_builder_free_all(builder);
    }
}

static uint16_t usb_root_hub_product_id(uint8_t type) {
    switch (type) {
    case USB_TYPE_XHCI:
        return 0x0003;
    case USB_TYPE_EHCI:
        return 0x0002;
    case USB_TYPE_OHCI:
    case USB_TYPE_UHCI:
    default:
        return 0x0001;
    }
}

static uint16_t usb_root_hub_bcd(uint8_t type) {
    switch (type) {
    case USB_TYPE_XHCI:
        return 0x0310;
    case USB_TYPE_EHCI:
        return 0x0200;
    default:
        return 0x0110;
    }
}

static uint32_t usb_linux_speed(uint8_t speed) {
    switch (speed) {
    case USB_LOWSPEED:
        return USB_SPEED_LOW;
    case USB_FULLSPEED:
        return USB_SPEED_FULL;
    case USB_HIGHSPEED:
        return USB_SPEED_HIGH;
    case USB_SUPERSPEED:
        return USB_SPEED_SUPER;
    default:
        return USB_SPEED_UNKNOWN;
    }
}

static void usb_format_devnode_name(const usb_device_t *usbdev, char *name,
                                    size_t size) {
    snprintf(name, size, "bus/usb/%03u/%03u", usbdev->busnum, usbdev->devnum);
}

static uint64_t usb_timeout_ns(uint32_t timeout_ms) {
    if (!timeout_ms)
        return (uint64_t)-1;
    return (uint64_t)timeout_ms * 1000000ULL;
}

static size_t usb_root_hub_num_ports(const usb_device_t *usbdev) {
    if (!usbdev || !usbdev->childhub)
        return 0;
    return usbdev->childhub->portcount;
}

static size_t usb_root_hub_status_bytes(size_t portcount) {
    return 1 + ((portcount + 7) / 8);
}

static size_t usb_root_hub_config_size(const usb_device_t *usbdev) {
    size_t size = sizeof(usb_config_descriptor_t) +
                  sizeof(usb_interface_descriptor_t) +
                  sizeof(usb_endpoint_descriptor_t);
    if (usbdev && usbdev->speed == USB_SUPERSPEED)
        size += sizeof(usb_super_speed_endpoint_descriptor_t);
    return size;
}

static size_t usb_root_hub_fill_config(const usb_device_t *usbdev, void *buf,
                                       size_t len) {
    usb_root_hub_config_t desc;
    size_t total = usb_root_hub_config_size(usbdev);
    uint16_t maxpacket =
        (uint16_t)usb_root_hub_status_bytes(usb_root_hub_num_ports(usbdev));

    memset(&desc, 0, sizeof(desc));

    desc.config.bLength = sizeof(desc.config);
    desc.config.bDescriptorType = USB_DT_CONFIG;
    desc.config.wTotalLength = total;
    desc.config.bNumInterfaces = 1;
    desc.config.bConfigurationValue = 1;
    desc.config.bmAttributes = 0x40;
    desc.config.bMaxPower = 0;

    desc.iface.bLength = sizeof(desc.iface);
    desc.iface.bDescriptorType = USB_DT_INTERFACE;
    desc.iface.bInterfaceNumber = 0;
    desc.iface.bAlternateSetting = 0;
    desc.iface.bNumEndpoints = 1;
    desc.iface.bInterfaceClass = USB_CLASS_HUB;
    desc.iface.bInterfaceSubClass = 0;
    desc.iface.bInterfaceProtocol = usbdev->speed == USB_SUPERSPEED ? 1 : 0;

    desc.ep.bLength = sizeof(desc.ep);
    desc.ep.bDescriptorType = USB_DT_ENDPOINT;
    desc.ep.bEndpointAddress = USB_DIR_IN | 1;
    desc.ep.bmAttributes = USB_ENDPOINT_XFER_INT;
    desc.ep.wMaxPacketSize = maxpacket;
    desc.ep.bInterval = usbdev->speed == USB_SUPERSPEED ? 12 : 255;

    desc.ss_ep.bLength = sizeof(desc.ss_ep);
    desc.ss_ep.bDescriptorType = USB_DT_ENDPOINT_COMPANION;
    desc.ss_ep.bMaxBurst = 0;
    desc.ss_ep.bmAttributes = 0;
    desc.ss_ep.wBytesPerInterval = maxpacket;

    if (!buf || !len)
        return total;

    size_t copied = 0;
    memcpy((uint8_t *)buf + copied, &desc.config,
           MIN(len - copied, sizeof(desc.config)));
    copied += MIN(len - copied, sizeof(desc.config));
    if (copied < len) {
        memcpy((uint8_t *)buf + copied, &desc.iface,
               MIN(len - copied, sizeof(desc.iface)));
        copied += MIN(len - copied, sizeof(desc.iface));
    }
    if (copied < len) {
        memcpy((uint8_t *)buf + copied, &desc.ep,
               MIN(len - copied, sizeof(desc.ep)));
        copied += MIN(len - copied, sizeof(desc.ep));
    }
    if (usbdev->speed == USB_SUPERSPEED && copied < len) {
        memcpy((uint8_t *)buf + copied, &desc.ss_ep,
               MIN(len - copied, sizeof(desc.ss_ep)));
    }

    return total;
}

static size_t usb_fill_string_descriptor(const char *src, void *buf,
                                         size_t len) {
    size_t slen = src ? strlen(src) : 0;
    size_t total = 2 + slen * 2;
    uint8_t *dst = buf;

    if (!buf || !len)
        return total;

    if (len >= 1)
        dst[0] = MIN(total, 255);
    if (len >= 2)
        dst[1] = USB_DT_STRING;

    for (size_t i = 0; i < slen && 2 + i * 2 + 1 < len; i++) {
        dst[2 + i * 2] = (uint8_t)src[i];
        dst[2 + i * 2 + 1] = 0;
    }

    return total;
}

static int usb_root_hub_control(usb_device_t *usbdev,
                                const usb_ctrl_request_t *req, void *data) {
    uint8_t desc_type = (req->wValue >> 8) & 0xff;
    uint8_t desc_index = req->wValue & 0xff;

    if (!req)
        return -EINVAL;

    switch (req->bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
        if (desc_type == USB_DT_DEVICE) {
            if (data && req->wLength)
                memcpy(data, &usbdev->device_desc,
                       MIN((size_t)req->wLength, sizeof(usbdev->device_desc)));
            return 0;
        }
        if (desc_type == USB_DT_CONFIG) {
            if (data && req->wLength)
                usb_root_hub_fill_config(usbdev, data, req->wLength);
            return 0;
        }
        if (desc_type == USB_DT_STRING) {
            if (!data || !req->wLength)
                return 0;

            if (desc_index == 0) {
                uint8_t langid[4] = {4, USB_DT_STRING, 0x09, 0x04};
                memcpy(data, langid, MIN((size_t)req->wLength, sizeof(langid)));
                return 0;
            }
            if (desc_index == usbdev->device_desc.iManufacturer)
                return usb_fill_string_descriptor(usbdev->manufacturer, data,
                                                  req->wLength),
                       0;
            if (desc_index == usbdev->device_desc.iProduct)
                return usb_fill_string_descriptor(usbdev->product, data,
                                                  req->wLength),
                       0;
            if (desc_index == usbdev->device_desc.iSerialNumber)
                return usb_fill_string_descriptor(usbdev->serial, data,
                                                  req->wLength),
                       0;
        }
        return -ENOSYS;
    case USB_REQ_GET_CONFIGURATION:
        if (data && req->wLength)
            *(uint8_t *)data = 1;
        return 0;
    case USB_REQ_SET_CONFIGURATION:
    case USB_REQ_GET_STATUS:
    case USB_REQ_SET_INTERFACE:
        if (data && req->wLength)
            memset(data, 0, req->wLength);
        return 0;
    default:
        return -ENOSYS;
    }
}

static int usb_device_control(usb_device_t *usbdev,
                              const usb_ctrl_request_t *req, void *data,
                              uint32_t timeout_ms) {
    if (!usbdev || !req)
        return -EINVAL;
    if (usbdev->is_root_hub)
        return usb_root_hub_control(usbdev, req, data);
    if (!usbdev->defpipe)
        return -ENODEV;

    return usb_send_pipe(usbdev->defpipe, req->bRequestType & USB_DIR_IN, req,
                         data, req->wLength, usb_timeout_ns(timeout_ms), NULL);
}

static int usb_find_endpoint(usb_device_t *usbdev, uint8_t epaddr,
                             usb_device_interface_t **iface_out,
                             usb_endpoint_descriptor_t **ep_out,
                             usb_super_speed_endpoint_descriptor_t **ss_out) {
    if (!usbdev)
        return -ENODEV;

    for (int i = 0; i < usbdev->ifaces_num; i++) {
        usb_device_interface_t *iface = &usbdev->ifaces[i];
        uint8_t *ptr = (uint8_t *)iface->iface + iface->iface->bLength;
        uint8_t *end = iface->end;
        usb_endpoint_descriptor_t *last_ep = NULL;

        while (ptr && end && ptr + 2 <= end) {
            uint8_t len = ptr[0];
            uint8_t type = ptr[1];

            if (len < 2)
                break;
            if (type == USB_DT_INTERFACE)
                break;

            if (type == USB_DT_ENDPOINT) {
                usb_endpoint_descriptor_t *ep = (void *)ptr;
                last_ep = ep;
                if (ep->bEndpointAddress == epaddr) {
                    if (iface_out)
                        *iface_out = iface;
                    if (ep_out)
                        *ep_out = ep;
                    if (ss_out)
                        *ss_out = NULL;
                }
            } else if (type == USB_DT_ENDPOINT_COMPANION && last_ep &&
                       last_ep->bEndpointAddress == epaddr) {
                if (ss_out)
                    *ss_out = (void *)ptr;
                return 0;
            }

            if (last_ep && last_ep->bEndpointAddress == epaddr &&
                type != USB_DT_ENDPOINT_COMPANION)
                return 0;

            ptr += len;
        }
    }

    return ep_out && *ep_out ? 0 : -ENOENT;
}

static size_t usb_descriptor_blob_size(usb_device_t *usbdev) {
    size_t size = sizeof(usbdev->device_desc);
    if (usbdev->is_root_hub)
        return size + usb_root_hub_config_size(usbdev);
    if (usbdev->config)
        size += usbdev->config->wTotalLength;
    return size;
}

static usb_endpoint_descriptor_t *
usb_find_endpoint_by_address(usb_device_t *usbdev, uint8_t epaddr,
                             usb_device_interface_t **iface_out,
                             usb_super_speed_endpoint_descriptor_t **ss_out) {
    int type = (epaddr & USB_ENDPOINT_DIR_MASK) ? USB_DIR_IN : USB_DIR_OUT;

    for (int i = 0; i < usbdev->ifaces_num; i++) {
        usb_device_interface_t *iface = &usbdev->ifaces[i];
        usb_endpoint_descriptor_t *ep =
            usb_find_desc(iface, USB_ENDPOINT_XFER_BULK, type);
        if (!ep)
            continue;
        if (ep->bEndpointAddress != epaddr)
            continue;
        if (iface_out)
            *iface_out = iface;
        if (ss_out)
            *ss_out = usb_find_ss_desc(iface);
        return ep;
    }

    return NULL;
}

static ssize_t usbfs_read(void *dev, void *buf, uint64_t offset, size_t size,
                          fd_t *fd) {
    usb_device_t *usbdev = dev;
    size_t total;
    size_t copied = 0;

    if (!usbdev || !buf)
        return -EINVAL;

    total = usb_descriptor_blob_size(usbdev);
    if (offset >= total)
        return 0;
    size = MIN(size, total - offset);

    if (offset < sizeof(usbdev->device_desc)) {
        size_t chunk = MIN(size, sizeof(usbdev->device_desc) - offset);
        memcpy(buf, (uint8_t *)&usbdev->device_desc + offset, chunk);
        copied += chunk;
        offset += chunk;
    }

    if (copied >= size)
        return copied;

    if (usbdev->is_root_hub) {
        usb_root_hub_config_t config;
        size_t cfg_size = usb_root_hub_config_size(usbdev);
        memset(&config, 0, sizeof(config));
        usb_root_hub_fill_config(usbdev, &config, cfg_size);
        memcpy((uint8_t *)buf + copied,
               (uint8_t *)&config + (offset - sizeof(usbdev->device_desc)),
               size - copied);
        return size;
    }

    if (!usbdev->config)
        return copied;

    memcpy((uint8_t *)buf + copied,
           (uint8_t *)usbdev->config + (offset - sizeof(usbdev->device_desc)),
           size - copied);
    return size;
}

static ssize_t usbfs_write(void *dev, void *buf, uint64_t offset, size_t size,
                           fd_t *fd) {
    return -EINVAL;
}

static int usb_topology_ports(const usb_device_t *usbdev, uint8_t *ports,
                              size_t max_ports) {
    const char *dash;
    const char *p;
    int count = 0;

    if (!usbdev || usbdev->is_root_hub)
        return 0;

    dash = strchr(usbdev->topology, '-');
    if (!dash)
        return 0;

    p = dash + 1;
    while (*p && count < (int)max_ports) {
        unsigned int port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned int)(*p - '0');
            p++;
        }
        ports[count++] = (uint8_t)port;
        if (*p != '.')
            break;
        p++;
    }

    return count;
}

static ssize_t usbfs_ioctl(void *dev, int cmd, void *args, fd_t *fd) {
    usb_device_t *usbdev = dev;

    if (!usbdev)
        return -ENODEV;

    switch ((uint32_t)cmd) {
    case USBDEVFS_CONTROL: {
        usbdevfs_ctrltransfer_t ctrl;
        usb_ctrl_request_t req;
        void *kbuf = NULL;
        int ret;

        if (!args)
            return -EFAULT;
        if (copy_from_user(&ctrl, args, sizeof(ctrl)))
            return -EFAULT;

        memset(&req, 0, sizeof(req));
        req.bRequestType = ctrl.bRequestType;
        req.bRequest = ctrl.bRequest;
        req.wValue = ctrl.wValue;
        req.wIndex = ctrl.wIndex;
        req.wLength = ctrl.wLength;

        if (ctrl.wLength) {
            kbuf = malloc(ctrl.wLength);
            if (!kbuf)
                return -ENOMEM;
            if (!(ctrl.bRequestType & USB_DIR_IN) &&
                copy_from_user(kbuf, ctrl.data, ctrl.wLength)) {
                free(kbuf);
                return -EFAULT;
            }
        }

        ret = usb_device_control(usbdev, &req, kbuf, ctrl.timeout);
        if (ret == 0 && (ctrl.bRequestType & USB_DIR_IN) && ctrl.wLength &&
            copy_to_user(ctrl.data, kbuf, ctrl.wLength)) {
            ret = -EFAULT;
        }

        free(kbuf);
        return ret;
    }
    case USBDEVFS_BULK: {
        usbdevfs_bulktransfer_t bulk;
        usb_endpoint_descriptor_t *ep;
        usb_super_speed_endpoint_descriptor_t *ss_ep = NULL;
        usb_pipe_t *pipe;
        void *kbuf = NULL;
        int actual = 0;
        int dir;
        int ret;

        if (!args)
            return -EFAULT;
        if (copy_from_user(&bulk, args, sizeof(bulk)))
            return -EFAULT;
        if (usbdev->bound_driver_count > 0)
            return -EBUSY;

        ep = usb_find_endpoint_by_address(usbdev, (uint8_t)bulk.ep, NULL,
                                          &ss_ep);
        if (!ep)
            return -ENOENT;

        pipe = usb_alloc_pipe(usbdev, ep, ss_ep);
        if (!pipe)
            return -ENOMEM;

        if (bulk.len) {
            kbuf = malloc(bulk.len);
            if (!kbuf) {
                usb_free_pipe(usbdev, pipe);
                return -ENOMEM;
            }
            if (!(bulk.ep & USB_DIR_IN) &&
                copy_from_user(kbuf, bulk.data, bulk.len)) {
                free(kbuf);
                usb_free_pipe(usbdev, pipe);
                return -EFAULT;
            }
        }

        dir = (bulk.ep & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
        ret = usb_send_pipe(pipe, dir, NULL, kbuf, (int)bulk.len,
                            usb_timeout_ns(bulk.timeout), &actual);
        if (ret == 0 && (bulk.ep & USB_DIR_IN) && actual > 0 &&
            copy_to_user(bulk.data, kbuf, (size_t)actual)) {
            ret = -EFAULT;
        }
        if (ret == 0)
            ret = actual;

        free(kbuf);
        usb_free_pipe(usbdev, pipe);
        return ret;
    }
    case USBDEVFS_CONNECTINFO: {
        usbdevfs_connectinfo_t info = {
            .devnum = usbdev->devnum,
            .slow = usbdev->speed == USB_LOWSPEED,
        };
        if (!args || copy_to_user(args, &info, sizeof(info)))
            return -EFAULT;
        return 0;
    }
    case USBDEVFS_GET_CAPABILITIES: {
        uint32_t caps = USBDEVFS_CAP_ZERO_PACKET | USBDEVFS_CAP_CONNINFO_EX;
        if (!args || copy_to_user(args, &caps, sizeof(caps)))
            return -EFAULT;
        return 0;
    }
    case USBDEVFS_GET_SPEED:
        return usb_linux_speed(usbdev->speed);
    case USBDEVFS_CLAIMINTERFACE:
    case USBDEVFS_RELEASEINTERFACE:
        return 0;
    case USBDEVFS_SETCONFIGURATION: {
        unsigned int config = 0;
        usb_ctrl_request_t req;
        if (!args || copy_from_user(&config, args, sizeof(config)))
            return -EFAULT;
        memset(&req, 0, sizeof(req));
        req.bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
        req.bRequest = USB_REQ_SET_CONFIGURATION;
        req.wValue = (uint16_t)config;
        return usb_device_control(usbdev, &req, NULL, 1000);
    }
    case USBDEVFS_SETINTERFACE: {
        usbdevfs_setinterface_t setif;
        usb_ctrl_request_t req;
        if (!args || copy_from_user(&setif, args, sizeof(setif)))
            return -EFAULT;
        memset(&req, 0, sizeof(req));
        req.bRequestType =
            USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
        req.bRequest = USB_REQ_SET_INTERFACE;
        req.wIndex = (uint16_t)setif.interface;
        req.wValue = (uint16_t)setif.altsetting;
        return usb_device_control(usbdev, &req, NULL, 1000);
    }
    case USBDEVFS_GETDRIVER: {
        usbdevfs_getdriver_t driver;
        memset(&driver, 0, sizeof(driver));
        if (!args || copy_from_user(&driver, args, sizeof(driver)))
            return -EFAULT;
        snprintf(driver.driver, sizeof(driver.driver), "%s",
                 usbdev->is_root_hub ? "hub" : "usb");
        if (copy_to_user(args, &driver, sizeof(driver)))
            return -EFAULT;
        return 0;
    }
    case USBDEVFS_HUB_PORTINFO: {
        usbdevfs_hub_portinfo_t info;
        usb_hub_t *hub = usbdev->childhub;
        memset(&info, 0, sizeof(info));
        if (!hub)
            return -ENODEV;
        info.nports = (char)MIN(hub->portcount, (uint32_t)127);
        for (uint32_t i = 0; i < hub->portcount && i < 127; i++) {
            usb_device_t *child = hub->ports ? hub->ports[i] : NULL;
            info.port[i] = child ? (char)child->devnum : 0;
        }
        if (!args || copy_to_user(args, &info, sizeof(info)))
            return -EFAULT;
        return 0;
    }
    default:
        if (_IOC_TYPE((uint32_t)cmd) == 'U' && _IOC_NR((uint32_t)cmd) == 32) {
            usbdevfs_conninfo_ex_t info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            info.busnum = usbdev->busnum;
            info.devnum = usbdev->devnum;
            info.speed = usb_linux_speed(usbdev->speed);
            info.num_ports = (uint8_t)usb_topology_ports(usbdev, info.ports,
                                                         sizeof(info.ports));
            size_t len = _IOC_SIZE((uint32_t)cmd);
            len = MIN(len, sizeof(info));
            if (!args || copy_to_user(args, &info, len))
                return -EFAULT;
            return 0;
        }
        return -ENOTTY;
    }
}

static ssize_t usbfs_poll(void *dev, int events) { return 0; }

static void usb_register_devnode(usb_device_t *usbdev) {
    char name[64];

    if (!usbdev || usbdev->usbfs_devnr)
        return;

    usb_format_devnode_name(usbdev, name, sizeof(name));
    // usbdev->usbfs_devnr = device_install_usb(
    //     DEV_CHAR, DEV_USB, usbdev, name, 0, NULL, NULL, usbfs_ioctl,
    //     usbfs_poll, usbfs_read, usbfs_write, NULL);
}

static void usb_unregister_devnode(usb_device_t *usbdev) {
    if (!usbdev || !usbdev->usbfs_devnr)
        return;
    // device_uninstall(usbdev->usbfs_devnr);
    usbdev->usbfs_devnr = 0;
}

int regist_usb_driver(usb_driver_t *driver) {
    if (!driver)
        return -EINVAL;

    for (int i = 0; i < MAX_USBDEV_NUM; i++) {
        if (!usb_drivers[i]) {
            usb_drivers[i] = driver;
            return 0;
        }
    }

    return -ENOSPC;
}

void unregist_usb_driver(usb_driver_t *driver) {
    if (!driver)
        return;

    for (int i = 0; i < MAX_USBDEV_NUM; i++) {
        if (usb_drivers[i] == driver) {
            usb_drivers[i] = NULL;
            return;
        }
    }
}

void usb_register_bus_notifier(usb_bus_notifier_ops_t *ops) {
    spin_lock(&usb_notifier_lock);
    usb_bus_notifier = ops;
    spin_unlock(&usb_notifier_lock);
}

void usb_unregister_bus_notifier(usb_bus_notifier_ops_t *ops) {
    spin_lock(&usb_notifier_lock);
    if (usb_bus_notifier == ops)
        usb_bus_notifier = NULL;
    spin_unlock(&usb_notifier_lock);
}

const char *usb_speed_name(uint8_t speed) {
    switch (speed) {
    case USB_LOWSPEED:
        return "low";
    case USB_FULLSPEED:
        return "full";
    case USB_HIGHSPEED:
        return "high";
    case USB_SUPERSPEED:
        return "super";
    default:
        return "unknown";
    }
}

usb_driver_t *usb_get_current_probe_driver(void) {
    return usb_current_probe_driver;
}

usb_driver_t *usb_get_current_remove_driver(void) {
    return usb_current_remove_driver;
}

static inline void usb_delay_ms(uint64_t ms) {
    uint64_t timeout = nano_time() + ms * 1000000ULL;
    while (nano_time() < timeout) {
        arch_pause();
    }
}

static inline void usb_notify_controller_add(usb_controller_t *cntl) {
    usb_bus_notifier_ops_t *ops;

    spin_lock(&usb_notifier_lock);
    ops = usb_bus_notifier;
    spin_unlock(&usb_notifier_lock);

    if (ops && ops->controller_add)
        ops->controller_add(cntl);
}

static inline void usb_notify_controller_remove(usb_controller_t *cntl) {
    usb_bus_notifier_ops_t *ops;

    spin_lock(&usb_notifier_lock);
    ops = usb_bus_notifier;
    spin_unlock(&usb_notifier_lock);

    if (ops && ops->controller_remove)
        ops->controller_remove(cntl);
}

static inline void usb_notify_device_add(usb_device_t *usbdev) {
    usb_bus_notifier_ops_t *ops;

    usb_register_devnode(usbdev);

    spin_lock(&usb_notifier_lock);
    ops = usb_bus_notifier;
    spin_unlock(&usb_notifier_lock);

    if (ops && ops->device_add)
        ops->device_add(usbdev);
}

static inline void usb_notify_device_remove(usb_device_t *usbdev) {
    usb_bus_notifier_ops_t *ops;

    spin_lock(&usb_notifier_lock);
    ops = usb_bus_notifier;
    spin_unlock(&usb_notifier_lock);

    if (ops && ops->device_remove)
        ops->device_remove(usbdev);

    usb_unregister_devnode(usbdev);
}

static void usb_hotplug_wake(void) {
    spin_lock(&usb_hotplug_lock);
    task_t *task = usb_hotplug_task;
    spin_unlock(&usb_hotplug_lock);

    if (task)
        task_unblock(task, EOK);
}

static usb_pipe_t *
usb_realloc_pipe(usb_device_t *usbdev, usb_pipe_t *pipe,
                 usb_endpoint_descriptor_t *epdesc,
                 usb_super_speed_endpoint_descriptor_t *ss_epdesc) {
    return usbdev->hub->op->realloc_pipe(usbdev, pipe, epdesc, ss_epdesc);
}

int usb_submit_xfer(usb_xfer_t *xfer) {
    if (!xfer || !xfer->pipe || !xfer->pipe->usbdev ||
        !xfer->pipe->usbdev->hub || !xfer->pipe->usbdev->hub->op ||
        !xfer->pipe->usbdev->hub->op->submit_xfer || !xfer->cb) {
        return -EINVAL;
    }

    return xfer->pipe->usbdev->hub->op->submit_xfer(xfer);
}

typedef struct usb_sync_xfer {
    wait_mutex_t lock;
    wait_queue_head_t wait;
    volatile int refs;
    bool completed;
    bool abandoned;
    int status;
    int actual_length;
    int direction;
    int data_size;
    void *caller_data;
    void *dma_data;
    usb_ctrl_request_t command;
    bool has_command;
} usb_sync_xfer_t;

static void usb_sync_xfer_put(usb_sync_xfer_t *sync) {
    if (!sync || __atomic_sub_fetch(&sync->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;

    if (sync->dma_data)
        free_frames_bytes(sync->dma_data, (size_t)sync->data_size);
    free(sync);
}

static void usb_sync_xfer_complete(int status, int actual_length,
                                   void *user_data) {
    usb_sync_xfer_t *sync = user_data;

    if (!sync)
        return;

    wait_mutex_lock(&sync->lock);
    if (actual_length < 0 || actual_length > sync->data_size) {
        status = EVENT_ERROR;
        actual_length = 0;
    }
    if (!sync->abandoned && sync->direction == USB_DIR_IN &&
        sync->caller_data && sync->dma_data && actual_length > 0) {
        memcpy(sync->caller_data, sync->dma_data, (size_t)actual_length);
    }
    sync->status = status;
    sync->actual_length = actual_length;
    sync->completed = true;
    wait_mutex_unlock(&sync->lock);

    wait_queue_wake_all(&sync->wait, 0, EOK);
    usb_sync_xfer_put(sync);
}

static int usb_sync_status(int status) {
    switch (status) {
    case EVENT_SUCCESS:
    case EVENT_SHORT_PACKET:
        return 0;
    case EVENT_STALL:
        return -EPIPE;
    case EVENT_BABBLE:
        return -EOVERFLOW;
    case EVENT_TIMEOUT:
        return -ETIMEDOUT;
    default:
        return -EIO;
    }
}

static int usb_sync_xfer_finish(usb_sync_xfer_t *sync, int *actual_length_out) {
    int ret;

    wait_mutex_lock(&sync->lock);
    ret = usb_sync_status(sync->status);
    if (actual_length_out)
        *actual_length_out = sync->actual_length;
    wait_mutex_unlock(&sync->lock);
    usb_sync_xfer_put(sync);
    return ret;
}

static int usb_sync_xfer_abandon(usb_sync_xfer_t *sync, int ret,
                                 int *actual_length_out) {
    wait_mutex_lock(&sync->lock);
    if (sync->completed) {
        wait_mutex_unlock(&sync->lock);
        return usb_sync_xfer_finish(sync, actual_length_out);
    }
    sync->abandoned = true;
    sync->caller_data = NULL;
    wait_mutex_unlock(&sync->lock);
    usb_sync_xfer_put(sync);
    return ret;
}

int usb_send_pipe(usb_pipe_t *pipe_fl, int dir, const void *cmd, void *data,
                  int datasize, uint64_t timeout_ns, int *actual_length_out) {
    usb_sync_xfer_t *sync;
    uint64_t deadline;
    int ret;

    if (!pipe_fl || datasize < 0 || (datasize > 0 && !data))
        return -EINVAL;
    if (!current_task || current_task->preempt_count)
        return -EDEADLK;
    if (actual_length_out)
        *actual_length_out = 0;

    sync = calloc(1, sizeof(*sync));
    if (!sync)
        return -ENOMEM;
    wait_mutex_init(&sync->lock);
    wait_queue_init(&sync->wait);
    sync->refs = 2;
    sync->direction = dir;
    sync->data_size = datasize;
    sync->caller_data = data;
    if (cmd) {
        memcpy(&sync->command, cmd, sizeof(sync->command));
        sync->has_command = true;
    }
    if (datasize > 0) {
        sync->dma_data = alloc_frames_bytes((size_t)datasize);
        if (!sync->dma_data) {
            usb_sync_xfer_put(sync);
            usb_sync_xfer_put(sync);
            return -ENOMEM;
        }
        if (dir != USB_DIR_IN)
            memcpy(sync->dma_data, data, (size_t)datasize);
    }

    usb_xfer_t xfer = {
        .pipe = pipe_fl,
        .dir = dir,
        .cmd = sync->has_command ? &sync->command : NULL,
        .data = sync->dma_data,
        .datasize = datasize,
        .cb = usb_sync_xfer_complete,
        .user_data = sync,
    };

    ret = usb_submit_xfer(&xfer);
    if (ret < 0) {
        usb_sync_xfer_put(sync);
        usb_sync_xfer_put(sync);
        return ret;
    }

    if (timeout_ns == (uint64_t)-1)
        timeout_ns = (uint64_t)usb_xfer_time(pipe_fl, datasize) * 1000000ULL;
    uint64_t now = nano_time();
    deadline = timeout_ns > UINT64_MAX - now ? UINT64_MAX : now + timeout_ns;

    for (;;) {
        wait_queue_entry_t wait;
        int64_t remain_ns;

        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&sync->wait, &wait);

        wait_mutex_lock(&sync->lock);
        bool completed = sync->completed;
        wait_mutex_unlock(&sync->lock);
        if (completed) {
            wait_queue_remove(&sync->wait, &wait);
            task_cancel_block_prepare(current_task);
            return usb_sync_xfer_finish(sync, actual_length_out);
        }

        uint64_t now = nano_time();
        if (now >= deadline) {
            wait_queue_remove(&sync->wait, &wait);
            task_cancel_block_prepare(current_task);
            return usb_sync_xfer_abandon(sync, -ETIMEDOUT, actual_length_out);
        }
        remain_ns = deadline == UINT64_MAX ? -1 : (int64_t)(deadline - now);
        ret = task_block(current_task, TASK_BLOCKING, remain_ns, "usb_xfer");
        wait_queue_remove(&sync->wait, &wait);
        task_cancel_block_prepare(current_task);

        if (ret == ETIMEDOUT)
            return usb_sync_xfer_abandon(sync, -ETIMEDOUT, actual_length_out);
        if (ret < 0)
            return usb_sync_xfer_abandon(sync, ret, actual_length_out);
    }
}

int usb_send_intr_pipe(usb_pipe_t *pipe_fl, void *data_ptr, int len,
                       intr_xfer_cb cb, void *user_data) {
    usb_xfer_t xfer = {
        .pipe = pipe_fl,
        .dir = USB_DIR_IN,
        .cmd = NULL,
        .data = data_ptr,
        .datasize = len,
        .cb = cb,
        .user_data = user_data,
    };

    return usb_submit_xfer(&xfer);
}

int usb_32bit_pipe(usb_pipe_t *pipe_fl) { return 1; }

usb_pipe_t *usb_alloc_pipe(usb_device_t *usbdev,
                           usb_endpoint_descriptor_t *epdesc,
                           usb_super_speed_endpoint_descriptor_t *ss_epdesc) {
    return usb_realloc_pipe(usbdev, NULL, epdesc, ss_epdesc);
}

void usb_free_pipe(usb_device_t *usbdev, usb_pipe_t *pipe) {
    if (!pipe)
        return;
    usb_realloc_pipe(usbdev, pipe, NULL, NULL);
}

int usb_send_default_control(usb_pipe_t *pipe, const usb_ctrl_request_t *req,
                             void *data) {
    return usb_send_pipe(pipe, req->bRequestType & USB_DIR_IN, req, data,
                         req->wLength, (uint64_t)-1, NULL);
}

int usb_send_bulk(usb_pipe_t *pipe_fl, int dir, void *data, int datasize) {
    return usb_send_pipe(pipe_fl, dir, NULL, data, datasize, (uint64_t)-1,
                         NULL);
}

int usb_is_freelist(usb_controller_t *cntl, usb_pipe_t *pipe) {
    return pipe->cntl != cntl;
}

void usb_desc2pipe(usb_pipe_t *pipe, usb_device_t *usbdev,
                   usb_endpoint_descriptor_t *epdesc) {
    pipe->cntl = usbdev->hub->cntl;
    pipe->type = usbdev->hub->cntl->type;
    pipe->ep = epdesc->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
    pipe->devaddr = usbdev->devaddr;
    pipe->speed = usbdev->speed;
    pipe->maxpacket = epdesc->wMaxPacketSize;
    pipe->eptype = epdesc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
    pipe->usbdev = usbdev;
}

static inline int __fls(unsigned int x) {
    if (x == 0)
        return 0;
    return 32 - __builtin_clz(x);
}

int usb_get_period(usb_device_t *usbdev, usb_endpoint_descriptor_t *epdesc) {
    int period = epdesc->bInterval;
    if (usbdev->speed != USB_HIGHSPEED)
        return period <= 0 ? 0 : __fls(period);
    return period <= 4 ? 0 : period - 4;
}

int usb_xfer_time(usb_pipe_t *pipe, int datalen) {
    if (!pipe->devaddr)
        return USB_TIME_STATUS + 100;
    return USB_TIME_COMMAND + 100;
}

usb_endpoint_descriptor_t *usb_find_desc(usb_device_interface_t *iface,
                                         int type, int dir) {
    uint8_t *ptr = (uint8_t *)iface->iface + iface->iface->bLength;
    uint8_t *end = iface->end;

    while (ptr && end && ptr + 2 <= end) {
        uint8_t len = ptr[0];
        uint8_t desc_type = ptr[1];

        if (len < 2)
            break;
        if (desc_type == USB_DT_INTERFACE)
            break;

        if (desc_type == USB_DT_ENDPOINT) {
            usb_endpoint_descriptor_t *epdesc = (void *)ptr;
            if ((epdesc->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == dir &&
                (epdesc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == type)
                return epdesc;
        }

        ptr += len;
    }

    return NULL;
}

usb_super_speed_endpoint_descriptor_t *
usb_find_ss_desc(usb_device_interface_t *iface) {
    uint8_t *ptr = (uint8_t *)iface->iface + iface->iface->bLength;
    uint8_t *end = iface->end;
    bool saw_endpoint = false;

    while (ptr && end && ptr + 2 <= end) {
        uint8_t len = ptr[0];
        uint8_t desc_type = ptr[1];

        if (len < 2)
            break;
        if (desc_type == USB_DT_INTERFACE)
            break;

        if (desc_type == USB_DT_ENDPOINT)
            saw_endpoint = true;
        else if (saw_endpoint && desc_type == USB_DT_ENDPOINT_COMPANION)
            return (void *)ptr;

        ptr += len;
    }

    return NULL;
}

int usb_set_interface(usb_device_t *usbdev, uint8_t iface_num,
                      uint8_t alt_setting) {
    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
        .bRequest = USB_REQ_SET_INTERFACE,
        .wValue = alt_setting,
        .wIndex = iface_num,
        .wLength = 0,
    };

    return usb_device_control(usbdev, &req, NULL, 1000);
}

static int get_device_info8(usb_pipe_t *pipe, usb_device_descriptor_t *dinfo) {
    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = USB_DT_DEVICE << 8,
        .wIndex = 0,
        .wLength = 8,
    };
    return usb_send_default_control(pipe, &req, dinfo);
}

static int get_device_info_full(usb_pipe_t *pipe,
                                usb_device_descriptor_t *dinfo) {
    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = USB_DT_DEVICE << 8,
        .wIndex = 0,
        .wLength = sizeof(*dinfo),
    };
    return usb_send_default_control(pipe, &req, dinfo);
}

static usb_config_descriptor_t *get_device_config(usb_pipe_t *pipe) {
    usb_config_descriptor_t cfg;
    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = USB_DT_CONFIG << 8,
        .wIndex = 0,
        .wLength = sizeof(cfg),
    };
    int ret = usb_send_default_control(pipe, &req, &cfg);
    if (ret)
        return NULL;

    usb_config_descriptor_t *config = malloc(cfg.wTotalLength);
    if (!config)
        return NULL;

    req.wLength = cfg.wTotalLength;
    ret = usb_send_default_control(pipe, &req, config);
    if (ret || config->wTotalLength != cfg.wTotalLength) {
        free(config);
        return NULL;
    }

    return config;
}

static int set_configuration(usb_pipe_t *pipe, uint16_t val) {
    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_SET_CONFIGURATION,
        .wValue = val,
        .wIndex = 0,
        .wLength = 0,
    };
    return usb_send_default_control(pipe, &req, NULL);
}

static int usb_get_string_descriptor(usb_pipe_t *pipe, uint8_t index,
                                     uint16_t langid, void *buf, size_t len) {
    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (USB_DT_STRING << 8) | index,
        .wIndex = langid,
        .wLength = len,
    };
    return usb_send_default_control(pipe, &req, buf);
}

static uint16_t usb_get_string_langid(usb_pipe_t *pipe) {
    uint8_t buf[8];

    memset(buf, 0, sizeof(buf));
    if (usb_get_string_descriptor(pipe, 0, 0, buf, sizeof(buf)) != 0)
        return 0x0409;
    if (buf[0] < 4 || buf[1] != USB_DT_STRING)
        return 0x0409;
    return (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
}

static void usb_decode_string(char *dst, size_t dst_len, const uint8_t *src,
                              size_t src_len) {
    size_t out = 0;

    if (!dst_len)
        return;
    dst[0] = '\0';

    if (src_len < 2 || src[1] != USB_DT_STRING)
        return;

    for (size_t i = 2; i + 1 < src_len && out + 1 < dst_len; i += 2) {
        uint16_t ch = (uint16_t)src[i] | ((uint16_t)src[i + 1] << 8);
        if (!ch)
            break;
        dst[out++] = ch < 0x80 && ch >= 0x20 ? (char)ch : '?';
    }

    dst[out] = '\0';
}

static void usb_read_string(usb_device_t *usbdev, uint8_t index, char *dst,
                            size_t dst_len) {
    uint8_t buf[256];
    uint16_t langid;

    if (!index || !usbdev->defpipe || !dst || !dst_len) {
        if (dst && dst_len)
            dst[0] = '\0';
        return;
    }

    memset(buf, 0, sizeof(buf));
    langid = usb_get_string_langid(usbdev->defpipe);
    if (usb_get_string_descriptor(usbdev->defpipe, index, langid, buf,
                                  sizeof(buf)) != 0) {
        dst[0] = '\0';
        return;
    }

    usb_decode_string(dst, dst_len, buf, MIN((size_t)buf[0], sizeof(buf)));
}

static void usb_fill_topology_name(usb_device_t *usbdev) {
    if (!usbdev || !usbdev->hub)
        return;

    if (!usbdev->hub->usbdev || usbdev->hub->usbdev->is_root_hub) {
        sprintf(usbdev->topology, "%u-%u", usbdev->busnum, usbdev->port + 1);
        usbdev->level = 0;
        return;
    }

    snprintf(usbdev->topology, sizeof(usbdev->topology), "%s.%u",
             usbdev->hub->usbdev->topology, usbdev->port + 1);
    usbdev->level = usbdev->hub->usbdev->level + 1;
}

static usb_device_t *usb_create_root_hub_device(usb_controller_t *cntl,
                                                usb_hub_t *hub) {
    usb_device_t *rootdev = calloc(1, sizeof(*rootdev));
    if (!rootdev)
        return NULL;

    rootdev->hub = NULL;
    rootdev->busnum = cntl->busnum;
    rootdev->devnum = 1;
    rootdev->devaddr = 1;
    rootdev->speed =
        cntl->type == USB_TYPE_XHCI ? USB_SUPERSPEED : USB_HIGHSPEED;
    rootdev->online = true;
    rootdev->is_root_hub = true;
    rootdev->vendorid = 0x1d6b;
    rootdev->productid = usb_root_hub_product_id(cntl->type);
    rootdev->childhub = hub;
    rootdev->device_desc.bLength = sizeof(rootdev->device_desc);
    rootdev->device_desc.bDescriptorType = USB_DT_DEVICE;
    rootdev->device_desc.bcdUSB = usb_root_hub_bcd(cntl->type);
    rootdev->device_desc.bDeviceClass = USB_CLASS_HUB;
    rootdev->device_desc.bDeviceSubClass = 0;
    rootdev->device_desc.bDeviceProtocol = cntl->type == USB_TYPE_XHCI ? 3 : 1;
    rootdev->device_desc.bMaxPacketSize0 = 64;
    rootdev->device_desc.idVendor = rootdev->vendorid;
    rootdev->device_desc.idProduct = rootdev->productid;
    rootdev->device_desc.bcdDevice = 0x0001;
    rootdev->device_desc.iManufacturer = 1;
    rootdev->device_desc.iProduct = 2;
    rootdev->device_desc.bNumConfigurations = 1;
    snprintf(rootdev->topology, sizeof(rootdev->topology), "usb%u",
             cntl->busnum);
    snprintf(rootdev->manufacturer, sizeof(rootdev->manufacturer),
             "NeoAetherOS");
    snprintf(rootdev->product, sizeof(rootdev->product), "%s",
             usb_root_hub_product_name(cntl->type));

    return rootdev;
}

static uint8_t usb_alloc_devnum(usb_controller_t *cntl) {
    if (cntl->next_devnum == 0)
        cntl->next_devnum = 1;
    return cntl->next_devnum++;
}

static int usb_match_id(const usb_device_id_t *id, usb_device_t *usbdev,
                        usb_device_interface_t *iface) {
    int score = 0;

    if (!id || !id->match_flags)
        return -1;

    if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
        id->idVendor != usbdev->vendorid)
        return -1;
    if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) &&
        id->idProduct != usbdev->productid)
        return -1;
    if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) &&
        id->bInterfaceClass != iface->iface->bInterfaceClass)
        return -1;
    if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) &&
        id->bInterfaceSubClass != iface->iface->bInterfaceSubClass)
        return -1;
    if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) &&
        id->bInterfaceProtocol != iface->iface->bInterfaceProtocol)
        return -1;

    if (id->match_flags & USB_DEVICE_ID_MATCH_VENDOR)
        score++;
    if (id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT)
        score++;
    if (id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS)
        score++;
    if (id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS)
        score++;
    if (id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL)
        score++;

    return score;
}

static const usb_device_id_t *usb_match_driver(usb_driver_t *driver,
                                               usb_device_t *usbdev,
                                               usb_device_interface_t *iface,
                                               int *score_out) {
    const usb_device_id_t *id;
    int best_score = -1;
    const usb_device_id_t *best = NULL;

    if (!driver || !driver->id_table)
        return NULL;

    for (id = driver->id_table; id->match_flags; id++) {
        int score = usb_match_id(id, usbdev, iface);
        if (score < 0)
            continue;
        if (score > best_score) {
            best_score = score;
            best = id;
        }
    }

    if (score_out)
        *score_out = best_score;
    return best;
}

static int usb_bind_driver(usb_device_t *usbdev, usb_driver_t *driver) {
    for (uint8_t i = 0; i < usbdev->bound_driver_count; i++) {
        if (usbdev->bound_drivers[i] == driver)
            return 0;
    }

    if (usbdev->bound_driver_count >= USB_MAX_BOUND_DRIVERS)
        return -ENOMEM;

    usbdev->bound_drivers[usbdev->bound_driver_count++] = driver;
    return 0;
}

int usb_init_driver(usb_device_t *usbdev) {
    typedef struct usb_probe_candidate {
        usb_driver_t *driver;
        usb_device_interface_t *iface;
        int score;
    } usb_probe_candidate_t;

    for (int iface_n = 0; iface_n < usbdev->ifaces_num; iface_n++) {
        usb_device_interface_t *base = &usbdev->ifaces[iface_n];
        usb_probe_candidate_t *candidates = NULL;
        int candidate_count = 0;
        uint8_t iface_number;

        if (!base->iface)
            continue;

        iface_number = base->iface->bInterfaceNumber;
        for (int prev = 0; prev < iface_n; prev++) {
            if (usbdev->ifaces[prev].iface &&
                usbdev->ifaces[prev].iface->bInterfaceNumber == iface_number) {
                iface_number = 0xff;
                break;
            }
        }

        if (iface_number == 0xff)
            continue;

        for (int j = 0; j < usbdev->ifaces_num; j++) {
            usb_device_interface_t *iface = &usbdev->ifaces[j];

            if (!iface->iface ||
                iface->iface->bInterfaceNumber != base->iface->bInterfaceNumber)
                continue;

            for (int i = 0; i < MAX_USBDEV_NUM; i++) {
                usb_driver_t *driver = usb_drivers[i];
                int score = -1;

                if (!driver)
                    continue;
                if (!usb_match_driver(driver, usbdev, iface, &score))
                    continue;

                usb_probe_candidate_t *new_candidates = realloc(
                    candidates, sizeof(*candidates) * (candidate_count + 1));
                if (!new_candidates) {
                    free(candidates);
                    return -ENOMEM;
                }

                candidates = new_candidates;
                candidates[candidate_count].driver = driver;
                candidates[candidate_count].iface = iface;
                candidates[candidate_count].score =
                    score + driver->priority * 16;
                candidate_count++;
            }
        }

        for (int a = 0; a < candidate_count; a++) {
            for (int b = a + 1; b < candidate_count; b++) {
                if (candidates[b].score > candidates[a].score) {
                    usb_probe_candidate_t tmp = candidates[a];
                    candidates[a] = candidates[b];
                    candidates[b] = tmp;
                }
            }
        }

        for (int i = 0; i < candidate_count; i++) {
            usb_current_probe_driver = candidates[i].driver;
            if (candidates[i].driver->probe(usbdev, candidates[i].iface) == 0) {
                usb_current_probe_driver = NULL;
                usb_bind_driver(usbdev, candidates[i].driver);
                break;
            }
            usb_current_probe_driver = NULL;
        }

        free(candidates);
    }

    return 0;
}

static int usb_parse_config(usb_device_t *usbdev,
                            usb_config_descriptor_t *config) {
    uint8_t *ptr = (uint8_t *)&config[1];
    uint8_t *end = (uint8_t *)config + config->wTotalLength;
    int count = 0;
    usb_device_interface_t *curr = NULL;

    for (uint8_t *scan = ptr; scan + 2 <= end;) {
        uint8_t len = scan[0];
        uint8_t type = scan[1];

        if (len < 2)
            break;
        if (type == USB_DT_INTERFACE)
            count++;
        scan += len;
    }

    if (!count) {
        usbdev->ifaces = NULL;
        usbdev->ifaces_num = 0;
        return 0;
    }

    usbdev->ifaces = calloc(count, sizeof(*usbdev->ifaces));
    if (!usbdev->ifaces)
        return -ENOMEM;

    for (; ptr + 2 <= end;) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];

        if (len < 2)
            break;

        if (type == USB_DT_INTERFACE) {
            if (curr)
                curr->end = ptr;

            curr = &usbdev->ifaces[usbdev->ifaces_num++];
            curr->usbdev = usbdev;
            curr->bus_device = NULL;
            curr->iface = (void *)ptr;
            curr->end = end;
        }

        ptr += len;
    }

    if (curr)
        curr->end = end;

    return 0;
}

static const int speed_to_ctlsize[] = {
    [USB_FULLSPEED] = 8,
    [USB_LOWSPEED] = 8,
    [USB_HIGHSPEED] = 64,
    [USB_SUPERSPEED] = 512,
};

static int usb_set_address(usb_device_t *usbdev) {
    usb_controller_t *cntl = usbdev->hub->cntl;
    usb_endpoint_descriptor_t epdesc = {
        .bEndpointAddress = 0,
        .wMaxPacketSize = speed_to_ctlsize[usbdev->speed],
        .bmAttributes = USB_ENDPOINT_XFER_CONTROL,
    };

    usb_delay_ms(USB_TIME_RSTRCY);

    usbdev->defpipe = usb_alloc_pipe(usbdev, &epdesc, NULL);
    if (!usbdev->defpipe) {
        printk("USB: device allocate pipe failed\n");
        return -1;
    }

    if (cntl->maxaddr >= USB_MAXADDR)
        return -1;

    usb_ctrl_request_t req = {
        .bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_SET_ADDRESS,
        .wValue = cntl->maxaddr + 1,
        .wIndex = 0,
        .wLength = 0,
    };
    int ret = usb_send_default_control(usbdev->defpipe, &req, NULL);
    if (ret) {
        usb_free_pipe(usbdev, usbdev->defpipe);
        usbdev->defpipe = NULL;
        return -1;
    }

    usb_delay_ms(USB_TIME_SETADDR_RECOVERY);

    cntl->maxaddr++;
    usbdev->devaddr = cntl->maxaddr;
    usbdev->defpipe = usb_realloc_pipe(usbdev, usbdev->defpipe, &epdesc, NULL);
    return usbdev->defpipe ? 0 : -1;
}

static int configure_usb_device(usb_device_t *usbdev) {
    usb_device_descriptor_t dinfo;
    usb_config_descriptor_t *config;
    uint16_t maxpacket;

    memset(&dinfo, 0, sizeof(dinfo));
    if (get_device_info8(usbdev->defpipe, &dinfo) != 0) {
        printk("USB: failed to read first 8 bytes of device descriptor\n");
        return 0;
    }

    printk("USB device descriptor:\n");
    printk("  bLength = %d\n", dinfo.bLength);
    printk("  bDescriptorType = %d\n", dinfo.bDescriptorType);
    printk("  bcdUSB = %#06lx\n", dinfo.bcdUSB);
    printk("  bDeviceClass = %#04lx\n", dinfo.bDeviceClass);
    printk("  bDeviceSubClass = %#04lx\n", dinfo.bDeviceSubClass);
    printk("  bMaxPacketSize0 = %#04lx\n", dinfo.bMaxPacketSize0);

    maxpacket = dinfo.bMaxPacketSize0;
    if (dinfo.bcdUSB >= 0x0300)
        maxpacket = 1U << dinfo.bMaxPacketSize0;
    if (maxpacket < 8) {
        printk("USB: invalid ep0 maxpacket %u\n", maxpacket);
        return 0;
    }

    usb_endpoint_descriptor_t epdesc = {
        .bEndpointAddress = 0,
        .wMaxPacketSize = maxpacket,
        .bmAttributes = USB_ENDPOINT_XFER_CONTROL,
    };

    usbdev->defpipe = usb_realloc_pipe(usbdev, usbdev->defpipe, &epdesc, NULL);
    if (!usbdev->defpipe)
        return 0;

    if (get_device_info_full(usbdev->defpipe, &dinfo) != 0) {
        printk("USB: failed to read full device descriptor\n");
        return 0;
    }

    memcpy(&usbdev->device_desc, &dinfo, sizeof(dinfo));
    usbdev->vendorid = dinfo.idVendor;
    usbdev->productid = dinfo.idProduct;

    config = get_device_config(usbdev->defpipe);
    if (!config) {
        printk("USB: failed to read configuration descriptor\n");
        return 0;
    }

    if (usb_parse_config(usbdev, config) != 0) {
        free(config);
        return 0;
    }

    usbdev->config = config;

    usb_delay_ms(100);
    if (set_configuration(usbdev->defpipe, config->bConfigurationValue) != 0) {
        free(usbdev->ifaces);
        usbdev->ifaces = NULL;
        usbdev->ifaces_num = 0;
        free(config);
        usbdev->config = NULL;
        return 0;
    }

    usb_read_string(usbdev, dinfo.iManufacturer, usbdev->manufacturer,
                    sizeof(usbdev->manufacturer));
    usb_read_string(usbdev, dinfo.iProduct, usbdev->product,
                    sizeof(usbdev->product));
    usb_read_string(usbdev, dinfo.iSerialNumber, usbdev->serial,
                    sizeof(usbdev->serial));

    usb_register_bus_device(usbdev);
    usb_register_interface_bus_devices(usbdev);

    usbdev->online = true;
    usb_notify_device_add(usbdev);

    if (usb_init_driver(usbdev) < 0) {
        usbdev->online = false;
        usb_notify_device_remove(usbdev);
        free(usbdev->ifaces);
        usbdev->ifaces = NULL;
        usbdev->ifaces_num = 0;
        free(config);
        usbdev->config = NULL;
        return 0;
    }

    return 1;
}

static void usb_disconnect_device(usb_device_t *usbdev);

static void usb_hub_disconnect_children(usb_hub_t *hub) {
    if (!hub || !hub->ports)
        return;

    for (uint32_t port = 0; port < hub->portcount; port++) {
        usb_device_t *child = NULL;

        spin_lock(&hub->lock);
        child = hub->ports[port];
        hub->ports[port] = NULL;
        spin_unlock(&hub->lock);

        if (!child)
            continue;

        if (hub->op && hub->op->disconnect)
            hub->op->disconnect(hub, port);
        usb_disconnect_device(child);
    }

    hub->devcount = 0;
}

static void usb_hub_release(usb_hub_t *hub) {
    if (!hub)
        return;
    free(hub->ports);
    free(hub);
}

static void usb_hub_put(usb_hub_t *hub) {
    bool release = false;

    spin_lock(&usb_hub_list_lock);
    if (hub->refcount)
        hub->refcount--;
    release = hub->removing && hub->refcount == 0;
    spin_unlock(&usb_hub_list_lock);

    if (release)
        usb_hub_release(hub);
}

static void usb_hub_unregister(usb_hub_t *hub) {
    bool release = false;

    if (!hub)
        return;

    usb_hub_disconnect_children(hub);

    spin_lock(&usb_hub_list_lock);
    if (hub->registered) {
        llist_delete(&hub->node);
        hub->registered = false;
    }
    hub->removing = true;
    release = hub->refcount == 0;
    spin_unlock(&usb_hub_list_lock);

    if (release)
        usb_hub_release(hub);
}

static void usb_disconnect_device(usb_device_t *usbdev) {
    if (!usbdev)
        return;

    if (usbdev->childhub) {
        usb_hub_unregister(usbdev->childhub);
        usbdev->childhub = NULL;
    }

    for (uint8_t i = 0; i < usbdev->bound_driver_count; i++) {
        usb_driver_t *driver = usbdev->bound_drivers[i];
        if (driver && driver->remove) {
            usb_current_remove_driver = driver;
            driver->remove(usbdev);
            usb_current_remove_driver = NULL;
        }
    }

    if (usbdev->online) {
        usbdev->online = false;
        usb_notify_device_remove(usbdev);
    }

    if (usbdev->defpipe)
        usb_free_pipe(usbdev, usbdev->defpipe);
    free(usbdev->ifaces);
    free(usbdev->config);
    free(usbdev);
}

static bool usb_hub_port_setup(usb_device_t *usbdev) {
    usb_hub_t *hub = usbdev->hub;
    int ret;

    ret = hub->op->detect(hub, usbdev->port);
    if (ret <= 0)
        return false;

    usb_delay_ms(USB_TIME_ATTDB);

    ret = hub->op->reset(hub, usbdev->port);
    if (ret < 0)
        return false;

    usbdev->speed = ret;
    usbdev->busnum = hub->cntl->busnum;
    usbdev->devnum = usb_alloc_devnum(hub->cntl);
    usb_fill_topology_name(usbdev);

    if (usb_set_address(usbdev) != 0) {
        if (hub->op->disconnect)
            hub->op->disconnect(hub, usbdev->port);
        return false;
    }

    if (!configure_usb_device(usbdev)) {
        if (hub->op->disconnect)
            hub->op->disconnect(hub, usbdev->port);
        return false;
    }

    printk("USB: enumerated %s (%04x:%04x, %s-speed)\n", usbdev->topology,
           usbdev->vendorid, usbdev->productid, usb_speed_name(usbdev->speed));
    return true;
}

static void usb_scan_hub(usb_hub_t *hub) {
    if (!hub || !hub->ports)
        return;

    if (hub->enumerating)
        return;

    hub->enumerating = true;

    for (uint32_t port = 0; port < hub->portcount; port++) {
        usb_device_t *child;
        int present;

        present = hub->op->detect(hub, port);

        spin_lock(&hub->lock);
        child = hub->ports[port];
        spin_unlock(&hub->lock);

        if (present > 0) {
            if (child)
                continue;

            usb_device_t *usbdev = calloc(1, sizeof(*usbdev));
            if (!usbdev)
                continue;

            usbdev->hub = hub;
            usbdev->port = port;

            if (!usb_hub_port_setup(usbdev)) {
                usb_disconnect_device(usbdev);
                continue;
            }

            spin_lock(&hub->lock);
            if (!hub->ports[port]) {
                hub->ports[port] = usbdev;
                hub->devcount++;
                usbdev = NULL;
            }
            spin_unlock(&hub->lock);

            if (usbdev)
                usb_disconnect_device(usbdev);
        } else if (present == 0 && child) {
            spin_lock(&hub->lock);
            hub->ports[port] = NULL;
            if (hub->devcount)
                hub->devcount--;
            spin_unlock(&hub->lock);

            printk("USB port %d disconnected.\n", port);
            if (hub->op->disconnect)
                hub->op->disconnect(hub, port);
            usb_disconnect_device(child);
        }
    }

    hub->enumerating = false;
}

static void usb_hotplug_thread(uint64_t arg) {
    (void)arg;

    for (;;) {
        arch_enable_interrupt();

        usb_hub_t *snapshot[USB_HUB_SNAPSHOT_MAX];
        uint32_t snapshot_count = 0;

        spin_lock(&usb_hub_list_lock);
        for (struct llist_header *node = usb_hub_list.next;
             node != &usb_hub_list && snapshot_count < USB_HUB_SNAPSHOT_MAX;
             node = node->next) {
            usb_hub_t *hub = container_of(node, usb_hub_t, node);
            if (hub->removing)
                continue;
            if (!hub->needs_rescan)
                continue;

            hub->refcount++;
            hub->needs_rescan = false;
            snapshot[snapshot_count++] = hub;
        }
        spin_unlock(&usb_hub_list_lock);

        for (uint32_t i = 0; i < snapshot_count; i++) {
            usb_scan_hub(snapshot[i]);
            usb_hub_put(snapshot[i]);
        }

        if (snapshot_count == USB_HUB_SNAPSHOT_MAX)
            continue;

        task_prepare_block(current_task);
        bool have_pending = false;
        spin_lock(&usb_hub_list_lock);
        for (struct llist_header *node = usb_hub_list.next;
             node != &usb_hub_list; node = node->next) {
            usb_hub_t *hub = container_of(node, usb_hub_t, node);
            if (!hub->removing && hub->needs_rescan) {
                have_pending = true;
                break;
            }
        }
        spin_unlock(&usb_hub_list_lock);

        if (have_pending)
            task_cancel_block_prepare(current_task);
        else
            task_block(current_task, TASK_BLOCKING, -1, "usb_hotplug");
    }
}

static void usb_hotplug_start(void) {
    spin_lock(&usb_hotplug_lock);
    if (!usb_hotplug_task)
        usb_hotplug_task =
            task_create("usb_hotplug", usb_hotplug_thread, 0, KTHREAD_PRIORITY);
    spin_unlock(&usb_hotplug_lock);
}

void usb_hub_mark_port_changed(usb_hub_t *hub, uint32_t port) {
    (void)port;
    if (!hub)
        return;

    spin_lock(&usb_hub_list_lock);
    if (!hub->removing)
        hub->needs_rescan = true;
    spin_unlock(&usb_hub_list_lock);

    usb_hotplug_wake();
}

void usb_enumerate(usb_hub_t *hub) {
    if (!hub)
        return;

    if (!hub->ports) {
        hub->ports = calloc(hub->portcount, sizeof(*hub->ports));
        if (!hub->ports)
            return;
    }

    usb_hotplug_start();

    spin_lock(&usb_hub_list_lock);
    if (!hub->registered) {
        spin_init(&hub->lock);
        llist_init_head(&hub->node);
        llist_append(&usb_hub_list, &hub->node);
        hub->registered = true;
        hub->removing = false;
        hub->needs_rescan = true;
        hub->refcount = 0;
        hub->next_scan_ns = 0;
    }
    spin_unlock(&usb_hub_list_lock);

    usb_hub_mark_port_changed(hub, 0);
    usb_scan_hub(hub);
}

void usb_register_controller(usb_controller_t *cntl, usb_hub_t *hub) {
    if (!cntl || !hub)
        return;

    spin_lock(&usb_hub_list_lock);
    if (usb_next_busnum == 0)
        usb_next_busnum = 1;
    cntl->busnum = usb_next_busnum++;
    spin_unlock(&usb_hub_list_lock);

    cntl->maxaddr = 0;
    cntl->next_devnum = 2;
    cntl->roothub = hub;
    cntl->rootdev = usb_create_root_hub_device(cntl, hub);
    hub->cntl = cntl;
    hub->usbdev = cntl->rootdev;
    spin_init(&hub->lock);

    usb_notify_controller_add(cntl);
    if (cntl->rootdev) {
        usb_register_bus_device(cntl->rootdev);
        usb_notify_device_add(cntl->rootdev);
    }
    usb_enumerate(hub);
}

void usb_unregister_controller(usb_controller_t *cntl) {
    if (!cntl)
        return;

    if (cntl->roothub) {
        usb_hub_unregister(cntl->roothub);
        cntl->roothub = NULL;
    }

    if (cntl->rootdev) {
        if (cntl->rootdev->online) {
            cntl->rootdev->online = false;
            usb_notify_device_remove(cntl->rootdev);
        }
        free(cntl->rootdev);
        cntl->rootdev = NULL;
    }

    usb_notify_controller_remove(cntl);
}
