#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <libs/klibc.h>
#include <drivers/bus/bus.h>

#define PCI_CONF_VENDOR 0X0   // Vendor ID
#define PCI_CONF_DEVICE 0X2   // Device ID
#define PCI_CONF_COMMAND 0x4  // Command
#define PCI_CONF_STATUS 0x6   // Status
#define PCI_CONF_REVISION 0x8 // revision ID

#define PCI_DEVICE_MAX 2048

#define EXPORT_BYTE(target, first)                                             \
    ((first) ? ((target) & ~0xFF00) : (((target) & ~0x00FF) >> 8))

/**
 * Encode segment/bus/device/function into the implementation-defined address
 * form used by the current PCI host-controller backend.
 * Notes: this is a host-controller-facing encoding helper, not a stable device
 * identity format for drivers or userspace. Use structured B/D/F fields when
 * storing device identity in long-lived state.
 */
uint32_t segment_bus_device_functon_to_pci_address(uint16_t segment,
                                                   uint8_t bus, uint8_t device,
                                                   uint8_t function);

#define PCI_BAR_GET_SIZE 1
#define PCI_BAR_GET_IS_MMIO 2

typedef struct {
    uint64_t address;
    uint64_t size;
    bool mmio;
    bool prefetchable;
} pci_bar_t;

/**
 * Low-level PCI configuration-space accessors supplied by a host-controller
 * implementation such as ECAM or architecture-specific glue.
 */
typedef struct {
    uint64_t (*convert_bar_address)(uint64_t bar_address);
    uint64_t (*allocate_bar_address)(uint32_t bus, uint32_t slot, uint32_t func,
                                     uint32_t segment, uint32_t bar_index,
                                     uint64_t size, bool mmio,
                                     bool prefetchable, bool is_64bit);
    uint8_t (*read8)(uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
                     uint32_t arg5);
    void (*write8)(uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
                   uint32_t arg5, uint8_t value);
    uint16_t (*read16)(uint32_t arg1, uint32_t arg2, uint32_t arg3,
                       uint32_t arg4, uint32_t arg5);
    void (*write16)(uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
                    uint32_t arg5, uint16_t value);
    uint32_t (*read32)(uint32_t arg1, uint32_t arg2, uint32_t arg3,
                       uint32_t arg4, uint32_t arg5);
    void (*write32)(uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
                    uint32_t arg5, uint32_t value);
} pci_device_op_t;

struct pcie_info;

struct pci_device;

struct kernel_pci_driver;
typedef bool (*pci_driver_match_t)(struct pci_device *dev,
                                   const struct kernel_pci_driver *driver);

typedef struct kernel_pci_driver {
    const char *name;
    uint32_t class_id;
    pci_driver_match_t match;
    int (*probe)(struct pci_device *dev);
    void (*remove)(struct pci_device *dev);
    void (*shutdown)(struct pci_device *dev);
    int flags;
    void *private_data;
} pci_driver_t;

/**
 * Enumerated PCI function plus driver/runtime state discovered during probing.
 */
typedef struct pci_device {
    const char *name;
    bus_device_t *device;
    uint32_t class_code;
    uint8_t header_type;

    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint8_t revision_id;
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    pci_bar_t bars[6];
    pci_bar_t rom;

    uint32_t capability_point;
    struct pcie_info *pcie;

    uint64_t msix_mmio_vaddr;
    uint64_t msix_mmio_size;
    uint32_t msix_offset;
    uint16_t msix_table_size;

    uint8_t irq_line;
    uint8_t irq_pin;

    uint8_t claimed_bars;

    pci_device_op_t *op;

    void *desc;
    pci_driver_t *kernel_driver;
} pci_device_t;

extern pci_device_t *pci_devices[PCI_DEVICE_MAX];
extern uint32_t pci_device_number;

/**
 * Search a device's PCI capability list for a capability ID.
 * Notes: this walks the linked capability chain exposed by the device itself.
 * Callers still need to validate per-capability layout before reading deeper
 * fields; finding the capability header is only the first step.
 */
uint32_t pci_enumerate_capability_list(pci_device_t *pci_dev,
                                       uint32_t cap_type);

#define PCI_MCFG_MAX_ENTRIES_LEN 1024

uint64_t get_mmio_address(uint32_t pci_address, uint16_t offset);

/**
 * Return a human-readable class string for a PCI class code.
 */
const char *pci_classname(uint32_t classcode);
/**
 * Collect all enumerated devices with the requested vendor ID.
 * Notes: this is a snapshot over the current enumeration result table, not a
 * live subscription. Callers should size `result` conservatively and inspect
 * `*n` as the actual number of returned entries.
 */
void pci_find_vid(pci_device_t **result, uint32_t *n, uint32_t vid);
/**
 * Collect all enumerated devices with the requested class code.
 * Notes: class matches are broad by design. Drivers that care about interface
 * quirks usually need another layer of filtering after this helper returns.
 */
void pci_find_class(pci_device_t **result, uint32_t *n, uint32_t class_code);
/**
 * Look up a previously enumerated device by bus/device/function/segment.
 * Notes: this only finds devices that have already been discovered by the PCI
 * core. It is not an implicit enumeration trigger.
 */
pci_device_t *pci_find_bdfs(uint8_t bus, uint8_t slot, uint8_t func,
                            uint16_t segment);
/**
 * Enumerate all buses reachable from one PCI segment group.
 * Notes: segment scanning is a topology walk plus device-object creation. It
 * should run before driver probing for that segment, otherwise probe code ends
 * up depending on partially populated global state.
 */
void pci_scan_segment(pci_device_op_t *op, uint16_t segment_group,
                      uint8_t start_bus);
/**
 * Enumerate one PCI bus and the functions reachable beneath it.
 * Notes: bridge recursion lives under this path. Bus scanning is not just a
 * flat loop over slots; callers should assume it may discover subordinate buses
 * and therefore more devices than are directly visible on the input bus.
 */
void pci_scan_bus(pci_device_op_t *op, uint16_t segment_group, uint8_t bus);
/**
 * Enumerate one concrete PCI function and populate pci_device_t state.
 * Notes: this is the point where BAR sizing, capability discovery, and driver
 * matchable identity all become concrete. A partially initialized pci_device_t
 * should not escape from here as if probing had already succeeded.
 */
void pci_scan_function(pci_device_op_t *op, uint16_t segment, uint8_t bus,
                       uint8_t device, uint8_t function);
/**
 * Initialize host-controller access before bus enumeration starts.
 * Notes: architecture or firmware glue should finish ECAM/PIO setup before
 * this runs. Driver code should not treat pci_controller_init() as a late
 * "make PCI usable now" escape hatch.
 */
void pci_controller_init();
/**
 * Enumerate PCI devices and probe registered PCI drivers.
 * Notes: this is the ownership boundary between discovery and driver binding.
 * Code that only needs the device list should prefer the find/scan helpers
 * rather than calling pci_init() a second time.
 */
void pci_init();

/**
 * Wait until every probe task dispatched by pci_init() has returned.
 * This is the device-initialization barrier used before fbdev/DRM fallback
 * registration and before entering userland.
 */
void pci_wait_for_probes();

#define MAX_PCI_DRIVERS 64

/**
 * Register a PCI driver with the PCI core.
 * Notes: registration does not just store metadata; it participates in future
 * probe decisions, so the driver object must remain valid after this call.
 */
int regist_pci_driver(pci_driver_t *driver);
