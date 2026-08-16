#pragma once

#include <drivers/bus/pci.h>
#include <libs/klibc.h>
#include <libs/llist.h>

typedef struct usb_driver usb_driver_t;
typedef struct usb_pipe usb_pipe_t;
typedef struct usb_device_interface usb_device_interface_t;
typedef struct usb_device usb_device_t;
typedef struct usb_controller usb_controller_t;
typedef struct usb_hub usb_hub_t;
typedef struct usb_hub_ops usb_hub_ops_t;
typedef struct usb_bus_notifier_ops usb_bus_notifier_ops_t;
typedef struct usb_ctrl_request usb_ctrl_request_t;
typedef struct usb_xfer usb_xfer_t;
typedef struct usb_device_id usb_device_id_t;
typedef struct usb_device_descriptor usb_device_descriptor_t;
typedef struct usb_config_descriptor usb_config_descriptor_t;
typedef struct usb_interface_descriptor usb_interface_descriptor_t;
typedef struct usb_endpoint_descriptor usb_endpoint_descriptor_t;
typedef struct usb_super_speed_endpoint_descriptor
    usb_super_speed_endpoint_descriptor_t;
typedef struct bus_device bus_device_t;

#define EVENT_SUCCESS 0
#define EVENT_SHORT_PACKET 1
#define EVENT_STALL -1
#define EVENT_BABBLE -2
#define EVENT_TIMEOUT -3
#define EVENT_ERROR -4

typedef void (*usb_xfer_cb)(int status, int actual_length, void *user_data);
typedef usb_xfer_cb intr_xfer_cb;

/**
 * Host-controller-owned endpoint pipe descriptor.
 */
struct usb_pipe {
    union {
        usb_controller_t *cntl;
        usb_pipe_t *freenext;
    };
    usb_device_t *usbdev;
    uint8_t type;
    uint8_t ep;
    uint8_t devaddr;
    uint8_t speed;
    uint16_t maxpacket;
    uint8_t eptype;
};

/**
 * Parsed interface view exposed to USB function drivers.
 */
struct usb_device_interface {
    usb_interface_descriptor_t *iface;
    void *end;
    usb_device_t *usbdev;
    bus_device_t *bus_device;
};

#define USB_MAX_BOUND_DRIVERS 8
#define USB_MAX_TOPOLOGY_LEN 32
#define USB_MAX_STRING_LEN 64

/**
 * Root USB controller state registered with the USB core.
 */
struct usb_controller {
    pci_device_t *pci;
    uint8_t type;
    uint8_t maxaddr;
    uint8_t busnum;
    uint8_t next_devnum;
    uint8_t flags;
    usb_hub_t *roothub;
    usb_device_t *rootdev;
};

/**
 * Hub state tracked by the USB core, including root hubs and external hubs.
 */
struct usb_hub {
    usb_hub_ops_t *op;
    usb_device_t *usbdev;
    usb_controller_t *cntl;
    spinlock_t lock;
    uint32_t port;
    uint32_t portcount;
    uint32_t devcount;
    usb_device_t **ports;
    bool registered;
    bool removing;
    bool needs_rescan;
    bool enumerating;
    uint32_t refcount;
    uint64_t next_scan_ns;
    struct llist_header node;
};

/**
 * Hub/controller callbacks required by the generic USB enumeration logic.
 */
struct usb_hub_ops {
    usb_pipe_t *(*realloc_pipe)(
        usb_device_t *usbdev, usb_pipe_t *upipe,
        usb_endpoint_descriptor_t *epdesc,
        usb_super_speed_endpoint_descriptor_t *ss_epdesc);
    int (*submit_xfer)(usb_xfer_t *xfer);

    int (*detect)(usb_hub_t *hub, uint32_t port);
    int (*reset)(usb_hub_t *hub, uint32_t port);
    int (*portmap)(usb_hub_t *hub, uint32_t port);
    void (*disconnect)(usb_hub_t *hub, uint32_t port);
};

/**
 * Optional bus-level notifications for subsystems interested in controller or
 * device add/remove events.
 */
struct usb_bus_notifier_ops {
    void (*controller_add)(usb_controller_t *cntl);
    void (*controller_remove)(usb_controller_t *cntl);
    void (*device_add)(usb_device_t *usbdev);
    void (*device_remove)(usb_device_t *usbdev);
};

#define USB_TYPE_UHCI 1
#define USB_TYPE_OHCI 2
#define USB_TYPE_EHCI 3
#define USB_TYPE_XHCI 4

#define USB_FULLSPEED 0
#define USB_LOWSPEED 1
#define USB_HIGHSPEED 2
#define USB_SUPERSPEED 3

#define USB_MAXADDR 127

/****************************************************************
 * usb structs and flags
 ****************************************************************/

#define USB_TIME_SIGATT 100
#define USB_TIME_ATTDB 100
#define USB_TIME_DRST 10
#define USB_TIME_DRSTR 50
#define USB_TIME_RSTRCY 10

#define USB_TIME_STATUS 50
#define USB_TIME_DATAIN 500
#define USB_TIME_COMMAND 5000

#define USB_TIME_SETADDR_RECOVERY 2

#define USB_PID_OUT 0xe1
#define USB_PID_IN 0x69
#define USB_PID_SETUP 0x2d

#define USB_DIR_OUT 0
#define USB_DIR_IN 0x80

#define USB_TYPE_MASK (0x03 << 5)
#define USB_TYPE_STANDARD (0x00 << 5)
#define USB_TYPE_CLASS (0x01 << 5)
#define USB_TYPE_VENDOR (0x02 << 5)
#define USB_TYPE_RESERVED (0x03 << 5)

#define USB_RECIP_MASK 0x1f
#define USB_RECIP_DEVICE 0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02
#define USB_RECIP_OTHER 0x03

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE 0x0A
#define USB_REQ_SET_INTERFACE 0x0B
#define USB_REQ_SYNCH_FRAME 0x0C

struct usb_ctrl_request {
    uint8_t bRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

#define USB_DT_DEVICE 0x01
#define USB_DT_CONFIG 0x02
#define USB_DT_STRING 0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT 0x05
#define USB_DT_DEVICE_QUALIFIER 0x06
#define USB_DT_OTHER_SPEED_CONFIG 0x07
#define USB_DT_PIPE_USAGE 0x24
#define USB_DT_ENDPOINT_COMPANION 0x30

struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;

    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

#define USB_CLASS_PER_INTERFACE 0
#define USB_CLASS_AUDIO 1
#define USB_CLASS_COMM 2
#define USB_CLASS_HID 3
#define USB_CLASS_PHYSICAL 5
#define USB_CLASS_STILL_IMAGE 6
#define USB_CLASS_PRINTER 7
#define USB_CLASS_MASS_STORAGE 8
#define USB_CLASS_HUB 9
#define USB_CLASS_CDC_DATA 0x0a
#define USB_CLASS_CSCID 0x0b
#define USB_CLASS_CONTENT_SEC 0x0d
#define USB_CLASS_VIDEO 0x0e
#define USB_CLASS_PERSONAL_HEALTHCARE 0x0f
#define USB_CLASS_AUDIO_VIDEO 0x10
#define USB_CLASS_BILLBOARD 0x11
#define USB_CLASS_USB_TYPE_C_BRIDGE 0x12
#define USB_CLASS_MCTP 0x14
#define USB_CLASS_WIRELESS_CONTROLLER 0xe0
#define USB_CLASS_MISC 0xef
#define USB_CLASS_APP_SPEC 0xfe
#define USB_CLASS_VENDOR_SPEC 0xff

#define USB_SUBCLASS_DFU 0x01
#define USB_SUBCLASS_VENDOR_SPEC 0xff

struct usb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;

    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

struct usb_super_speed_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;

    uint8_t bMaxBurst;
    uint8_t bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

/**
 * Enumerated USB device plus the interfaces and strings discovered during the
 * standard USB enumeration flow.
 */
struct usb_device {
    usb_hub_t *hub;
    usb_pipe_t *defpipe;
    bus_device_t *bus_device;
    uint32_t port;
    usb_device_interface_t *ifaces;
    int ifaces_num;
    usb_config_descriptor_t *config;
    uint16_t productid;
    uint16_t vendorid;
    uint8_t speed;
    uint8_t devaddr;
    uint8_t busnum;
    uint8_t devnum;
    uint8_t level;
    bool online;
    bool is_root_hub;
    uint64_t usbfs_devnr;
    void *desc;

    usb_device_descriptor_t device_desc;
    usb_hub_t *childhub;
    usb_driver_t *bound_drivers[USB_MAX_BOUND_DRIVERS];
    uint8_t bound_driver_count;
    char topology[USB_MAX_TOPOLOGY_LEN];
    char manufacturer[USB_MAX_STRING_LEN];
    char product[USB_MAX_STRING_LEN];
    char serial[USB_MAX_STRING_LEN];
};

#define USB_ENDPOINT_NUMBER_MASK 0x0f
#define USB_ENDPOINT_DIR_MASK 0x80

#define USB_ENDPOINT_XFERTYPE_MASK 0x03
#define USB_ENDPOINT_XFER_CONTROL 0
#define USB_ENDPOINT_XFER_ISOC 1
#define USB_ENDPOINT_XFER_BULK 2
#define USB_ENDPOINT_XFER_INT 3
#define USB_ENDPOINT_MAX_ADJUSTABLE 0x80

#define USB_CONTROL_SETUP_SIZE 8

#define US_SC_ATAPI_8020 0x02
#define US_SC_ATAPI_8070 0x05
#define US_SC_SCSI 0x06

#define US_PR_BULK 0x50
#define US_PR_UAS 0x62

enum {
    SCSI_INQUIRY = 0x12,
    SCSI_READ_CAPACITY_10 = 0x25,
    SCSI_READ_CAPACITY_16 = 0x9E,
    SCSI_READ_10 = 0x28,
    SCSI_READ_12 = 0xA8,
    SCSI_READ_16 = 0x88,
    SCSI_WRITE_10 = 0x2A,
    SCSI_WRITE_12 = 0xAA,
    SCSI_WRITE_16 = 0x8A,
};

enum scsi_version {
    SCSI_VERSION_10 = 10,
    SCSI_VERSION_12 = 12,
    SCSI_VERSION_16 = 16,
};

struct usb_xfer {
    usb_pipe_t *pipe;
    int dir;
    const void *cmd;
    void *data;
    int datasize;
    uint64_t timeout_ns;
    usb_xfer_cb cb;
    void *user_data;
    int *actual_length_out;
    uint32_t flags;
};

#define USB_XFER_ASYNC (1U << 0)

struct usb_device_id {
    uint16_t match_flags;
    uint16_t idVendor;
    uint16_t idProduct;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
};

#define USB_DEVICE_ID_MATCH_VENDOR (1U << 0)
#define USB_DEVICE_ID_MATCH_PRODUCT (1U << 1)
#define USB_DEVICE_ID_MATCH_INT_CLASS (1U << 2)
#define USB_DEVICE_ID_MATCH_INT_SUBCLASS (1U << 3)
#define USB_DEVICE_ID_MATCH_INT_PROTOCOL (1U << 4)
#define USB_DEVICE_ID_MATCH_DEVICE                                             \
    (USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT)
#define USB_DEVICE_ID_MATCH_INT_INFO                                           \
    (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS |        \
     USB_DEVICE_ID_MATCH_INT_PROTOCOL)

#define USB_DEVICE(vend, prod)                                                 \
    {                                                                          \
        .match_flags = USB_DEVICE_ID_MATCH_DEVICE,                             \
        .idVendor = (vend),                                                    \
        .idProduct = (prod),                                                   \
    }

#define USB_INTERFACE_INFO(cls, subcls, proto)                                 \
    {                                                                          \
        .match_flags = USB_DEVICE_ID_MATCH_INT_INFO,                           \
        .bInterfaceClass = (cls),                                              \
        .bInterfaceSubClass = (subcls),                                        \
        .bInterfaceProtocol = (proto),                                         \
    }

#define USB_INTERFACE_SUBCLASS_BOOT 1
#define USB_INTERFACE_PROTOCOL_KEYBOARD 1
#define USB_INTERFACE_PROTOCOL_MOUSE 2

#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_GET_IDLE 0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

/**
 * Submit a prepared transfer to the controller that owns xfer->pipe.
 * Notes: ownership of the transfer buffer still belongs to the caller. For
 * asynchronous use, that means both the xfer object and its backing buffers
 * must outlive completion.
 */
int usb_submit_xfer(usb_xfer_t *xfer);
/**
 * Send one generic transfer through a prepared pipe.
 * Notes: this is the synchronous convenience path. Callers that need overlap or
 * completion callbacks should build a usb_xfer and use usb_submit_xfer().
 */
int usb_send_pipe(usb_pipe_t *pipe, int dir, const void *cmd, void *data,
                  int datasize, uint64_t timeout_ns);
/**
 * Send a synchronous bulk transfer.
 * Notes: bulk transfers may legitimately complete short. Treating every short
 * packet as a hard fault is a higher-level protocol decision, not a USB rule.
 */
int usb_send_bulk(usb_pipe_t *pipe, int dir, void *data, int datasize);
/**
 * Queue a non-blocking bulk transfer.
 * Notes: once queued, the caller must keep the payload buffer stable until the
 * controller has consumed it or reported completion through its own path.
 */
int usb_send_bulk_nonblock(usb_pipe_t *pipe, int dir, void *data, int datasize);
/**
 * Queue an interrupt transfer with a completion callback.
 * Notes: the callback runs from the USB completion path, so it should stay
 * short and avoid assuming process context.
 */
int usb_send_intr_pipe(usb_pipe_t *pipe, void *data_ptr, int len,
                       intr_xfer_cb cb, void *user_data);
/**
 * Return non-zero when the pipe address space must be treated as 32-bit.
 */
int usb_32bit_pipe(usb_pipe_t *pipe);
/**
 * Allocate or reconfigure a pipe for one endpoint descriptor.
 * Notes: a pipe is controller-owned transport state, not just a parsed copy of
 * the endpoint descriptor. Reusing a pipe across devices or alternate settings
 * without reallocation is usually wrong.
 */
usb_pipe_t *usb_alloc_pipe(usb_device_t *usbdev,
                           usb_endpoint_descriptor_t *epdesc,
                           usb_super_speed_endpoint_descriptor_t *ss_epdesc);
/**
 * Release a previously allocated pipe.
 * Notes: do not free a pipe while transfers are still in flight on it unless
 * the controller-specific teardown path explicitly supports that.
 */
void usb_free_pipe(usb_device_t *usbdev, usb_pipe_t *pipe);
/**
 * Send a control request over endpoint zero.
 * Notes: this is the path for standard enumeration traffic as well as many
 * class/vendor requests, so callers need to fill bmRequestType/wIndex/wValue
 * carefully; the helper does not reinterpret malformed control packets.
 */
int usb_send_default_control(usb_pipe_t *pipe, const usb_ctrl_request_t *req,
                             void *data);
/**
 * Check whether a pipe currently resides on the controller freelist.
 */
int usb_is_freelist(usb_controller_t *cntl, usb_pipe_t *pipe);
/**
 * Populate pipe metadata directly from an endpoint descriptor.
 * Notes: this copies transport parameters only. It does not allocate schedule
 * state or register the pipe with the controller by itself.
 */
void usb_desc2pipe(usb_pipe_t *pipe, usb_device_t *usbdev,
                   usb_endpoint_descriptor_t *epdesc);
/**
 * Derive the polling period for an endpoint.
 */
int usb_get_period(usb_device_t *usbdev, usb_endpoint_descriptor_t *epdesc);
/**
 * Estimate transfer time for a payload sent through the given pipe.
 */
int usb_xfer_time(usb_pipe_t *pipe, int datalen);
/**
 * Find an endpoint descriptor inside one parsed interface block.
 * Notes: this is a descriptor scan over one interface view, not over the whole
 * device. Drivers that bind by interface should stay at that granularity.
 */
usb_endpoint_descriptor_t *usb_find_desc(usb_device_interface_t *iface,
                                         int type, int dir);
/**
 * Find the SuperSpeed companion descriptor associated with an interface.
 * Notes: not every endpoint has one. Full-/high-speed paths should be prepared
 * for a NULL result rather than treating it as descriptor corruption.
 */
usb_super_speed_endpoint_descriptor_t *
usb_find_ss_desc(usb_device_interface_t *iface);
/**
 * Select an alternate setting for one interface number.
 * Notes: alternate-setting changes can invalidate endpoint assumptions that a
 * driver cached earlier, so callers usually need to rebuild pipes afterwards.
 */
int usb_set_interface(usb_device_t *usbdev, uint8_t iface_num,
                      uint8_t alt_setting);
/**
 * Enumerate devices hanging off one hub.
 * Notes: this is the generic USB discovery engine. It is expected to cope with
 * hotplug churn, debounce, and partial failures; callers should not assume one
 * clean pass means the topology is forever settled.
 */
void usb_enumerate(usb_hub_t *hub);
/**
 * Register a controller and its root hub with the USB core.
 * Notes: root-hub wiring is part of controller registration, not a separate
 * optional convenience feature. Enumeration and driver binding depend on it.
 */
void usb_register_controller(usb_controller_t *cntl, usb_hub_t *hub);
/**
 * Remove a controller from the USB core and tear down attached state.
 * Notes: this is the disruptive path. Callers should expect device removal and
 * callback activity, not just silent list deletion.
 */
void usb_unregister_controller(usb_controller_t *cntl);
/**
 * Mark one hub port dirty so the enumeration worker rescans it.
 * Notes: use this for change notification, not as a direct substitute for
 * immediate re-enumeration in random contexts.
 */
void usb_hub_mark_port_changed(usb_hub_t *hub, uint32_t port);
/**
 * Register callbacks for USB bus add/remove notifications.
 * Notes: notifier callbacks observe controller/device lifetime edges. They are
 * best suited for bookkeeping, not for doing the heavy work of a function
 * driver probe path.
 */
void usb_register_bus_notifier(usb_bus_notifier_ops_t *ops);
/**
 * Remove previously registered USB bus notification callbacks.
 * Notes: callers must ensure the notifier object itself stays alive until after
 * unregistration completes.
 */
void usb_unregister_bus_notifier(usb_bus_notifier_ops_t *ops);
/**
 * Return a human-readable string for one USB speed enum value.
 */
const char *usb_speed_name(uint8_t speed);

#define MAX_USBDEV_NUM 256

struct usb_driver {
    const char *name;
    const usb_device_id_t *id_table;
    int priority;
    int (*probe)(usb_device_t *usbdev, usb_device_interface_t *iface);
    int (*remove)(usb_device_t *usbdev);
};

/**
 * Register a USB function driver with the USB core.
 * Notes: like PCI registration, the driver descriptor must remain valid after
 * registration because future hotplug events may still match against it.
 */
int regist_usb_driver(usb_driver_t *driver);
/**
 * Unregister a USB function driver.
 * Notes: this stops future matches, but callers should still think about
 * already-bound devices and teardown ordering around remove() callbacks.
 */
void unregist_usb_driver(usb_driver_t *driver);
usb_driver_t *usb_get_current_probe_driver(void);
usb_driver_t *usb_get_current_remove_driver(void);
