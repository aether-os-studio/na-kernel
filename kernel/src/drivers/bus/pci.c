#include <mm/mm.h>
#include <drivers/logger.h>
#include <drivers/bus/pci.h>
#include <drivers/bus/pci-dtb_ecam.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <task/task.h>
#include <task/wait.h>

struct acpi_mcfg_allocation *mcfg_entries[PCI_MCFG_MAX_ENTRIES_LEN];
uint64_t mcfg_entries_len = 0;

void mcfg_addr_to_entries(struct acpi_mcfg *mcfg,
                          struct acpi_mcfg_allocation **entries,
                          uint64_t *num) {
    struct acpi_mcfg_allocation *entry =
        (struct acpi_mcfg_allocation *)((uint64_t)mcfg +
                                        sizeof(struct acpi_mcfg));
    int length = mcfg->hdr.length - sizeof(struct acpi_mcfg);
    *num = length / sizeof(struct acpi_mcfg_allocation);
    for (uint64_t i = 0; i < *num; i++) {
        entries[i] = entry + i;
    }
}

uint64_t get_device_mmio_physical_address(uint16_t segment_group, uint8_t bus,
                                          uint8_t device, uint8_t function) {
    for (uint64_t i = 0; i < mcfg_entries_len; i++) {
        if (mcfg_entries[i]->segment == segment_group) {
            return mcfg_entries[i]->address +
                   (((uint64_t)bus - (uint64_t)mcfg_entries[i]->start_bus)
                    << 20) +
                   ((uint64_t)device << 15) + ((uint64_t)function << 12);
        }
    }
    return 0;
}

uint64_t get_mmio_address(uint32_t pci_address, uint16_t offset) {
    uint16_t segment = (pci_address >> 16) & 0xFFFF;
    uint8_t bus = (pci_address >> 8) & 0xFF;
    uint8_t device = (pci_address >> 3) & 0x1F;
    uint8_t function = pci_address & 0x07;

    uint64_t phys =
        get_device_mmio_physical_address(segment, bus, device, function);
    if (phys == 0) {
        return 0;
    }
    uint64_t virt = (uint64_t)phys_to_virt(phys);

    return virt + offset;
}

uint32_t segment_bus_device_functon_to_pci_address(uint16_t segment,
                                                   uint8_t bus, uint8_t device,
                                                   uint8_t function) {
    return ((uint32_t)(segment & 0xFFFF) << 16) |
           ((uint32_t)(bus & 0xFF) << 8) | ((uint32_t)(device & 0x3F) << 3) |
           (uint32_t)(function & 0xF);
}

uint8_t pci_read8(uint32_t b, uint32_t d, uint32_t f, uint32_t s,
                  uint32_t offset) {
    uint32_t pci_address =
        segment_bus_device_functon_to_pci_address(s, b, d, f);
    uint64_t mmio_address = get_mmio_address(pci_address, offset);
    if (mmio_address == 0) {
        printk("Cannot read pci: failed to get mmio address\n");
        return 0xff;
    }
    return *(volatile uint8_t *)mmio_address;
}

void pci_write8(uint32_t b, uint32_t d, uint32_t f, uint32_t s, uint32_t offset,
                uint8_t value) {
    uint32_t pci_address =
        segment_bus_device_functon_to_pci_address(s, b, d, f);
    uint64_t mmio_address = get_mmio_address(pci_address, offset);
    if (mmio_address == 0) {
        printk("Cannot write pci: failed to get mmio address\n");
        return;
    }
    *(volatile uint8_t *)mmio_address = value;
}

uint16_t pci_read16(uint32_t b, uint32_t d, uint32_t f, uint32_t s,
                    uint32_t offset) {
    uint32_t pci_address =
        segment_bus_device_functon_to_pci_address(s, b, d, f);
    uint64_t mmio_address = get_mmio_address(pci_address, offset);
    if (mmio_address == 0) {
        printk("Cannot read pci: failed to get mmio address\n");
        return 0xffff;
    }
    return *(volatile uint16_t *)mmio_address;
}

void pci_write16(uint32_t b, uint32_t d, uint32_t f, uint32_t s,
                 uint32_t offset, uint16_t value) {
    uint32_t pci_address =
        segment_bus_device_functon_to_pci_address(s, b, d, f);
    uint64_t mmio_address = get_mmio_address(pci_address, offset);
    if (mmio_address == 0) {
        printk("Cannot write pci: failed to get mmio address\n");
        return;
    }
    *(volatile uint16_t *)mmio_address = value;
}

uint32_t pci_read32(uint32_t b, uint32_t d, uint32_t f, uint32_t s,
                    uint32_t offset) {
    uint32_t pci_address =
        segment_bus_device_functon_to_pci_address(s, b, d, f);
    uint64_t mmio_address = get_mmio_address(pci_address, offset);
    if (mmio_address == 0) {
        printk("Cannot read pci: failed to get mmio address\n");
        return 0xffffffffu;
    }
    return *(volatile uint32_t *)mmio_address;
}

void pci_write32(uint32_t b, uint32_t d, uint32_t f, uint32_t s,
                 uint32_t offset, uint32_t value) {
    uint32_t pci_address =
        segment_bus_device_functon_to_pci_address(s, b, d, f);
    uint64_t mmio_address = get_mmio_address(pci_address, offset);
    if (mmio_address == 0) {
        printk("Cannot write pci: failed to get mmio address\n");
        return;
    }
    *(volatile uint32_t *)mmio_address = value;
}

uint32_t pci_enumerate_capability_list(pci_device_t *pci_dev,
                                       uint32_t cap_type) {
    uint32_t cap_offset;
    switch (pci_dev->header_type) {
    case 0x00:
        cap_offset = pci_dev->capability_point;
        break;
    case 0x10:
        cap_offset = pci_dev->capability_point;
        break;
    default:
        // 不支持
        return 0;
    }
    uint32_t tmp;
    while (1) {
        tmp = pci_dev->op->read32(pci_dev->bus, pci_dev->slot, pci_dev->func,
                                  pci_dev->segment, cap_offset);
        if ((tmp & 0xff) != cap_type) {
            if (((tmp & 0xff00) >> 8)) {
                cap_offset = (tmp & 0xff00) >> 8;
                continue;
            } else
                return 0;
        }

        return cap_offset;
    }
}

pci_device_op_t pcie_device_op = {
    .convert_bar_address = NULL,
    .allocate_bar_address = NULL,
    .read8 = pci_read8,
    .write8 = pci_write8,
    .read16 = pci_read16,
    .write16 = pci_write16,
    .read32 = pci_read32,
    .write32 = pci_write32,
};

struct {
    uint32_t classcode;
    const char *name;
} pci_classnames[] = {{0x000000, "Non-VGA-Compatible Unclassified Device"},
                      {0x000100, "VGA-Compatible Unclassified Device"},

                      {0x010000, "SCSI Bus Controller"},
                      {0x010100, "IDE Controller"},
                      {0x010200, "Floppy Disk Controller"},
                      {0x010300, "IPI Bus Controller"},
                      {0x010400, "RAID Controller"},
                      {0x010500, "ATA Controller"},
                      {0x010600, "Serial ATA Controller"},
                      {0x010700, "Serial Attached SCSI Controller"},
                      {0x010802, "NVM Express Controller"},
                      {0x018000, "Other Mass Storage Controller"},

                      {0x020000, "Ethernet Controller"},
                      {0x020100, "Token Ring Controller"},
                      {0x020200, "FDDI Controller"},
                      {0x020300, "ATM Controller"},
                      {0x020400, "ISDN Controller"},
                      {0x020500, "WorldFip Controller"},
                      {0x020600, "PICMG 2.14 Multi Computing Controller"},
                      {0x020700, "Infiniband Controller"},
                      {0x020800, "Fabric Controller"},
                      {0x028000, "Other Network Controller"},

                      {0x030000, "VGA Compatible Controller"},
                      {0x030100, "XGA Controller"},
                      {0x030200, "3D Controller (Not VGA-Compatible)"},
                      {0x038000, "Other Display Controller"},

                      {0x040000, "Multimedia Video Controller"},
                      {0x040100, "Multimedia Audio Controller"},
                      {0x040200, "Computer Telephony Device"},
                      {0x040300, "Audio Device"},
                      {0x048000, "Other Multimedia Controller"},

                      {0x050000, "RAM Controller"},
                      {0x050100, "Flash Controller"},
                      {0x058000, "Other Memory Controller"},

                      {0x060000, "Host Bridge"},
                      {0x060100, "ISA Bridge"},
                      {0x060200, "EISA Bridge"},
                      {0x060300, "MCA Bridge"},
                      {0x060400, "PCI-to-PCI Bridge"},
                      {0x060500, "PCMCIA Bridge"},
                      {0x060600, "NuBus Bridge"},
                      {0x060700, "CardBus Bridge"},
                      {0x060800, "RACEway Bridge"},
                      {0x060900, "PCI-to-PCI Bridge"},
                      {0x060A00, "InfiniBand-to-PCI Host Bridge"},
                      {0x068000, "Other Bridge"},

                      {0x070000, "Serial Controller"},
                      {0x070100, "Parallel Controller"},
                      {0x070200, "Multiport Serial Controller"},
                      {0x070300, "Modem"},
                      {0x070400, "IEEE 488.1/2 (GPIB) Controller"},
                      {0x070500, "Smart Card Controller"},
                      {0x078000, "Other Simple Communication Controller"},

                      {0x080000, "PIC"},
                      {0x080100, "DMA Controller"},
                      {0x080200, "Timer"},
                      {0x080300, "RTC Controller"},
                      {0x080400, "PCI Hot-Plug Controller"},
                      {0x080500, "SD Host controller"},
                      {0x080600, "IOMMU"},
                      {0x088000, "Other Base System Peripheral"},

                      {0x090000, "Keyboard Controller"},
                      {0x090100, "Digitizer Pen"},
                      {0x090200, "Mouse Controller"},
                      {0x090300, "Scanner Controller"},
                      {0x090400, "Gameport Controller"},
                      {0x098000, "Other Input Device Controller"},

                      {0x0A0000, "Generic"},
                      {0x0A8000, "Other Docking Station"},

                      {0x0B0000, "386"},
                      {0x0B0100, "486"},
                      {0x0B0200, "Pentium"},
                      {0x0B0300, "Pentium Pro"},
                      {0x0B1000, "Alpha"},
                      {0x0B2000, "PowerPC"},
                      {0x0B3000, "MIPS"},
                      {0x0B4000, "Co-Processor"},
                      {0x0B8000, "Other Processor"},

                      {0x0C0000, "FireWire (IEEE 1394) Controller"},
                      {0x0C0100, "ACCESS Bus Controller"},
                      {0x0C0200, "SSA"},
                      {0x0C0300, "USB Controller"},
                      {0x0C0400, "Fibre Channel"},
                      {0x0C0500, "SMBus Controller"},
                      {0x0C0600, "InfiniBand Controller"},
                      {0x0C0700, "IPMI Interface"},
                      {0x0C0800, "SERCOS Interface (IEC 61491)"},
                      {0x0C0900, "CANbus Controller"},
                      {0x0C8000, "Other Serial Bus Controller"},

                      {0x0D0000, "iRDA Compatible Controlle"},
                      {0x0D0100, "Consumer IR Controller"},
                      {0x0D1000, "RF Controller"},
                      {0x0D1100, "Bluetooth Controller"},
                      {0x0D1200, "Broadband Controller"},
                      {0x0D2000, "Ethernet Controller (802.1a)"},
                      {0x0D2100, "Ethernet Controller (802.1b)"},
                      {0x0D8000, "Other Wireless Controller"},

                      {0x0E0000, "I20"},

                      {0x0F0000, "Satellite TV Controller"},
                      {0x0F0100, "Satellite Audio Controller"},
                      {0x0F0300, "Satellite Voice Controller"},
                      {0x0F0400, "Satellite Data Controller"},

                      {0x100000, "Network and Computing Encrpytion/Decryption"},
                      {0x101000, "Entertainment Encryption/Decryption"},
                      {0x108000, "Other Encryption Controller"},

                      {0x110000, "DPIO Modules"},
                      {0x110100, "Performance Counters"},
                      {0x111000, "Communication Synchronizer"},
                      {0x112000, "Signal Processing Management"},
                      {0x118000, "Other Signal Processing Controller"},
                      {0x000000, (char *)NULL}};

pci_device_t *pci_devices[PCI_DEVICE_MAX];
uint32_t pci_device_number = 0;

const char *pci_classname(uint32_t classcode) {
    for (size_t i = 0; pci_classnames[i].name != NULL; i++) {
        if (pci_classnames[i].classcode == classcode) {
            return pci_classnames[i].name;
        }
        if (pci_classnames[i].classcode == (classcode & 0xFFFF00)) {
            return pci_classnames[i].name;
        }
    }
    return "Unknown device";
}

void pci_find_vid(pci_device_t **result, uint32_t *n, uint32_t vid) {
    int idx = 0;
    for (uint32_t i = 0; i < pci_device_number; i++) {
        if (pci_devices[i]->vendor_id == vid) {
            result[idx] = pci_devices[i];
            idx++;
            continue;
        }
    }
    *n = idx;
}

void pci_find_class(pci_device_t **result, uint32_t *n, uint32_t class_code) {
    int idx = 0;
    for (uint32_t i = 0; i < pci_device_number; i++) {
        if (pci_devices[i]->class_code == class_code) {
            result[idx] = pci_devices[i];
            idx++;
            continue;
        }
        if (class_code == (pci_devices[i]->class_code & 0xFFFF00)) {
            result[idx] = pci_devices[i];
            idx++;
            continue;
        }
    }
    *n = idx;
}

pci_device_t *pci_find_bdfs(uint8_t bus, uint8_t slot, uint8_t func,
                            uint16_t segment) {
    for (uint32_t i = 0; i < pci_device_number; i++) {
        if ((pci_devices[i]->bus == bus) && (pci_devices[i]->slot == slot) &&
            (pci_devices[i]->func == func) &&
            (pci_devices[i]->segment == segment)) {
            return pci_devices[i];
        }
    }
    return NULL;
}

ssize_t pci_bus_config_read(struct bus_device *dev, struct bin_attribute *attr,
                            char *buf, uint64_t off, size_t count) {
    return 0;
}

ssize_t pci_bus_config_write(struct bus_device *dev, struct bin_attribute *attr,
                             const char *buf, uint64_t off, size_t count) {
    return 0;
}

bin_attribute_t pci_bus_config_bin_attr = {
    .name = "config",
    .read = pci_bus_config_read,
    .write = pci_bus_config_write,
};

static ssize_t pci_sysfs_read_hex(char *buf, uint64_t off, size_t count,
                                  uint32_t value, uint32_t digits) {
    char tmp[16];
    size_t len;

    if (!buf) {
        return -EINVAL;
    }

    len = (size_t)snprintf(tmp, sizeof(tmp), "0x%0*x\n", digits, value);
    if (off >= len) {
        return 0;
    }

    size_t to_copy = MIN(count, len - off);
    memcpy(buf, tmp + off, to_copy);
    return (ssize_t)to_copy;
}

ssize_t pci_bus_class_read(struct bus_device *dev, struct bin_attribute *attr,
                           char *buf, uint64_t off, size_t count) {
    pci_device_t *pci_dev = dev->private_data;
    return pci_sysfs_read_hex(buf, off, count, pci_dev->class_code, 6);
}

ssize_t pci_bus_class_write(struct bus_device *dev, struct bin_attribute *attr,
                            const char *buf, uint64_t off, size_t count) {
    return 0;
}

bin_attribute_t pci_bus_class_bin_attr = {
    .name = "class",
    .read = pci_bus_class_read,
    .write = pci_bus_class_write,
};

ssize_t pci_bus_revision_read(struct bus_device *dev,
                              struct bin_attribute *attr, char *buf,
                              uint64_t off, size_t count) {
    pci_device_t *pci_dev = dev->private_data;
    return pci_sysfs_read_hex(buf, off, count, pci_dev->revision_id, 2);
}

ssize_t pci_bus_revision_write(struct bus_device *dev,
                               struct bin_attribute *attr, const char *buf,
                               uint64_t off, size_t count) {
    return 0;
}

bin_attribute_t pci_bus_revision_bin_attr = {
    .name = "revision",
    .read = pci_bus_revision_read,
    .write = pci_bus_revision_write,
};

ssize_t pci_bus_vendor_read(struct bus_device *dev, struct bin_attribute *attr,
                            char *buf, uint64_t off, size_t count) {
    pci_device_t *pci_dev = dev->private_data;
    return pci_sysfs_read_hex(buf, off, count, pci_dev->vendor_id, 4);
}

ssize_t pci_bus_vendor_write(struct bus_device *dev, struct bin_attribute *attr,
                             const char *buf, uint64_t off, size_t count) {
    return 0;
}

bin_attribute_t pci_bus_vendor_bin_attr = {
    .name = "vendor",
    .read = pci_bus_vendor_read,
    .write = pci_bus_vendor_write,
};

ssize_t pci_bus_device_read(struct bus_device *dev, struct bin_attribute *attr,
                            char *buf, uint64_t off, size_t count) {
    pci_device_t *pci_dev = dev->private_data;
    return pci_sysfs_read_hex(buf, off, count, pci_dev->device_id, 4);
}

ssize_t pci_bus_device_write(struct bus_device *dev, struct bin_attribute *attr,
                             const char *buf, uint64_t off, size_t count) {
    return 0;
}

bin_attribute_t pci_bus_device_bin_attr = {
    .name = "device",
    .read = pci_bus_device_read,
    .write = pci_bus_device_write,
};

ssize_t pci_bus_subsystem_vendor_read(struct bus_device *dev,
                                      struct bin_attribute *attr, char *buf,
                                      uint64_t off, size_t count) {
    pci_device_t *pci_dev = dev->private_data;
    return pci_sysfs_read_hex(buf, off, count, pci_dev->subsystem_vendor_id, 4);
}

ssize_t pci_bus_subsystem_vendor_write(struct bus_device *dev,
                                       struct bin_attribute *attr,
                                       const char *buf, uint64_t off,
                                       size_t count) {
    return 0;
}

bin_attribute_t pci_bus_subsystem_vendor_bin_attr = {
    .name = "subsystem_vendor",
    .read = pci_bus_subsystem_vendor_read,
    .write = pci_bus_subsystem_vendor_write,
};

ssize_t pci_bus_subsystem_device_read(struct bus_device *dev,
                                      struct bin_attribute *attr, char *buf,
                                      uint64_t off, size_t count) {
    pci_device_t *pci_dev = dev->private_data;
    return pci_sysfs_read_hex(buf, off, count, pci_dev->subsystem_device_id, 4);
}

ssize_t pci_bus_subsystem_device_write(struct bus_device *dev,
                                       struct bin_attribute *attr,
                                       const char *buf, uint64_t off,
                                       size_t count) {
    return 0;
}

bin_attribute_t pci_bus_subsystem_device_bin_attr = {
    .name = "subsystem_device",
    .read = pci_bus_subsystem_device_read,
    .write = pci_bus_subsystem_device_write,
};

bin_attribute_t *pci_bus_default_bin_attrs[] = {
    &pci_bus_config_bin_attr,           &pci_bus_class_bin_attr,
    &pci_bus_revision_bin_attr,         &pci_bus_vendor_bin_attr,
    &pci_bus_device_bin_attr,           &pci_bus_subsystem_vendor_bin_attr,
    &pci_bus_subsystem_device_bin_attr,
};

attribute_t pci_bus_subsystem_attr = {
    .name = "SUBSYSTEM",
    .value = "pci",
};

attribute_t *pci_bus_default_attrs[] = {
    &pci_bus_subsystem_attr,
};

bus_t pci_bus = {
    .name = "pci",
    .devices_path = "/sys/bus/pci/devices",
    .drivers_path = "/sys/bus/pci/drivers",
    .bus_default_attrs = pci_bus_default_attrs,
    .bus_default_attrs_count =
        sizeof(pci_bus_default_attrs) / sizeof(pci_bus_default_attrs[0]),
    .bus_default_bin_attrs = pci_bus_default_bin_attrs,
    .bus_default_bin_attrs_count = sizeof(pci_bus_default_bin_attrs) /
                                   sizeof(pci_bus_default_bin_attrs[0]),
};

int pci_get_device_path(bus_device_t *device, char *buf, size_t max) {
    pci_device_t *pci_device = device->private_data;
    snprintf(buf, max, "%04x:%02x:%02x.%01x", pci_device->segment,
             pci_device->bus, pci_device->slot, pci_device->func);
    return 0;
}

bus_device_t *bus_device_install_pci(void *dev_data, attribute_t **extra_attrs,
                                     int extra_attrs_count,
                                     bin_attribute_t **extra_bin_attrs,
                                     int extra_bin_attrs_count) {
    return bus_device_install_internal(
        &pci_bus, dev_data, extra_attrs, extra_attrs_count, extra_bin_attrs,
        extra_bin_attrs_count, pci_get_device_path);
}

void pci_scan_bus(pci_device_op_t *op, uint16_t segment_group, uint8_t bus);

void pci_scan_function(pci_device_op_t *op, uint16_t segment, uint8_t bus,
                       uint8_t device, uint8_t function) {
    if (pci_find_bdfs(bus, device, function, segment)) {
        return;
    }

    // 读取 vendor ID
    uint16_t vendor_id = op->read16(bus, device, function, segment, 0x00);
    if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
        return;
    }

    uint16_t device_id = op->read16(bus, device, function, segment, 0x02);

    // 读取 class code 和 revision
    uint32_t class_rev = op->read32(bus, device, function, segment, 0x08);
    uint8_t revision_id = class_rev & 0xFF;
    uint8_t prog_if = (class_rev >> 8) & 0xFF;
    uint8_t subclass = (class_rev >> 16) & 0xFF;
    uint8_t class_code = (class_rev >> 24) & 0xFF;

    // 读取 header type
    uint8_t header_type =
        op->read8(bus, device, function, segment, 0x0E) & 0x7F;

    // 创建设备结构
    pci_device_t *pci_device = (pci_device_t *)malloc(sizeof(pci_device_t));
    if (!pci_device) {
        printk("PCIe: Failed to allocate device structure\n");
        return;
    }
    memset(pci_device, 0, sizeof(pci_device_t));

    pci_device->header_type = header_type;
    pci_device->op = op;
    pci_device->revision_id = revision_id;
    pci_device->segment = segment;
    pci_device->bus = bus;
    pci_device->slot = device;
    pci_device->func = function;
    pci_device->vendor_id = vendor_id;
    pci_device->device_id = device_id;

    uint32_t class_code_24bit = (class_code << 16) | (subclass << 8) | prog_if;
    pci_device->class_code = class_code_24bit;
    pci_device->name = pci_classname(class_code_24bit);

    switch (header_type) {
    case 0x00: { // Standard device (Endpoint)
        // 读取 subsystem ID
        uint32_t subsystem = op->read32(bus, device, function, segment, 0x2C);
        pci_device->subsystem_vendor_id = subsystem & 0xFFFF;
        pci_device->subsystem_device_id = (subsystem >> 16) & 0xFFFF;

        // 读取中断信息
        uint32_t interrupt = op->read32(bus, device, function, segment, 0x3C);
        pci_device->irq_line = interrupt & 0xFF;
        pci_device->irq_pin = (interrupt >> 8) & 0xFF;

        // 读取 capability pointer
        uint16_t status = op->read16(bus, device, function, segment, 0x06);
        if (status & (1 << 4)) { // Capabilities List present
            pci_device->capability_point =
                op->read8(bus, device, function, segment, 0x34) & 0xFC;
        }

        // 枚举 BARs
        for (int i = 0; i < 6; i++) {
            uint32_t offset = 0x10 + i * 4;
            uint32_t bar = op->read32(bus, device, function, segment, offset);
            uint32_t original_bar = bar;

            if (bar & 0x01) {
                // I/O Space BAR
                uint64_t base = bar & 0xFFFFFFFC;
                pci_device->bars[i].mmio = false;
                pci_device->bars[i].prefetchable = false;

                // Probe size
                op->write32(bus, device, function, segment, offset, 0xFFFFFFFF);
                uint32_t size_mask =
                    op->read32(bus, device, function, segment, offset);
                op->write32(bus, device, function, segment, offset, bar);

                uint64_t size = ~(size_mask & 0xFFFFFFFC) + 1;
                if (size_mask == 0 || size_mask == 0xffffffff || size == 0) {
                    pci_device->bars[i].size = 0;
                    pci_device->bars[i].address = 0;
                    continue;
                }

                pci_device->bars[i].address = base;
                pci_device->bars[i].size = size;
            } else {
                // Memory Space BAR
                uint8_t mem_type = (bar >> 1) & 0x3;
                bool prefetchable = bar & (1 << 3);

                if (mem_type == 0x00) {
                    // 32-bit Memory BAR
                    uint64_t base = bar & 0xFFFFFFF0;

                    // Probe size
                    op->write32(bus, device, function, segment, offset,
                                0xFFFFFFFF);
                    uint32_t size_mask =
                        op->read32(bus, device, function, segment, offset);
                    op->write32(bus, device, function, segment, offset, bar);

                    uint64_t size = ~(size_mask & 0xFFFFFFF0) + 1;
                    if (size_mask == 0 || size_mask == 0xffffffff ||
                        size == 0) {
                        pci_device->bars[i].address = 0;
                        pci_device->bars[i].size = 0;
                        pci_device->bars[i].mmio = true;
                        pci_device->bars[i].prefetchable = prefetchable;
                        continue;
                    }

                    if (base == 0 && op->allocate_bar_address) {
                        base = op->allocate_bar_address(bus, device, function,
                                                        segment, i, size, true,
                                                        prefetchable, false);
                        if (base != 0) {
                            uint32_t new_bar = (uint32_t)(base & 0xFFFFFFF0) |
                                               (original_bar & 0xF);
                            op->write32(bus, device, function, segment, offset,
                                        new_bar);
                        }
                    }

                    if (op->convert_bar_address) {
                        base = op->convert_bar_address(base);
                    }
                    pci_device->bars[i].address = base;
                    pci_device->bars[i].size = size;
                    pci_device->bars[i].mmio = true;
                    pci_device->bars[i].prefetchable = prefetchable;
                } else if (mem_type == 0x02) {
                    // 64-bit Memory BAR
                    if (i >= 5) {
                        printk("  BAR%d: Invalid 64-bit BAR position\n", i);
                        break;
                    }

                    uint32_t bar_hi =
                        op->read32(bus, device, function, segment, offset + 4);
                    uint64_t base =
                        ((uint64_t)bar_hi << 32) | (bar & 0xFFFFFFF0);

                    // Probe size
                    uint32_t orig_lo = bar;
                    uint32_t orig_hi = bar_hi;

                    op->write32(bus, device, function, segment, offset,
                                0xFFFFFFFF);
                    op->write32(bus, device, function, segment, offset + 4,
                                0xFFFFFFFF);

                    uint32_t size_lo =
                        op->read32(bus, device, function, segment, offset);
                    uint32_t size_hi =
                        op->read32(bus, device, function, segment, offset + 4);

                    op->write32(bus, device, function, segment, offset,
                                orig_lo);
                    op->write32(bus, device, function, segment, offset + 4,
                                orig_hi);

                    uint64_t size_mask =
                        ((uint64_t)size_hi << 32) | (size_lo & 0xFFFFFFF0);
                    uint64_t size = ~size_mask + 1;
                    if (size_mask == 0 || size_mask == UINT64_MAX ||
                        size == 0) {
                        pci_device->bars[i].address = 0;
                        pci_device->bars[i].size = 0;
                        pci_device->bars[i].mmio = true;
                        pci_device->bars[i].prefetchable = prefetchable;
                        i++;
                        if (i < 6) {
                            pci_device->bars[i].address = 0;
                            pci_device->bars[i].size = 0;
                            pci_device->bars[i].mmio = true;
                            pci_device->bars[i].prefetchable = prefetchable;
                        }
                        continue;
                    }

                    if (base == 0 && op->allocate_bar_address) {
                        base = op->allocate_bar_address(bus, device, function,
                                                        segment, i, size, true,
                                                        prefetchable, true);
                        if (base != 0) {
                            uint32_t new_lo =
                                (uint32_t)(base & 0xFFFFFFF0) | (orig_lo & 0xF);
                            uint32_t new_hi = (uint32_t)(base >> 32);
                            op->write32(bus, device, function, segment, offset,
                                        new_lo);
                            op->write32(bus, device, function, segment,
                                        offset + 4, new_hi);
                        }
                    }

                    if (op->convert_bar_address) {
                        base = op->convert_bar_address(base);
                    }
                    pci_device->bars[i].address = base;
                    pci_device->bars[i].size = size;
                    pci_device->bars[i].mmio = true;
                    pci_device->bars[i].prefetchable = prefetchable;

                    // 64位BAR占用两个槽位
                    i++;
                    if (i < 6) {
                        pci_device->bars[i].address = 0;
                        pci_device->bars[i].size = 0;
                        pci_device->bars[i].mmio = true;
                        pci_device->bars[i].prefetchable = prefetchable;
                    }
                }
            }
        }

        // 枚举 ROM BAR（config 0x30），为需要 VBIOS 的驱动分配并启用
        {
            uint32_t rom = op->read32(bus, device, function, segment, 0x30);
            uint32_t orig = rom;

            op->write32(bus, device, function, segment, 0x30, 0xFFFFF800);
            uint32_t mask = op->read32(bus, device, function, segment, 0x30);
            op->write32(bus, device, function, segment, 0x30, orig);

            uint64_t size = ~(uint64_t)(mask & 0xFFFFF800) + 1;
            if (size != 0 && (mask & 0xFFFFF800) != 0xFFFFF800 &&
                size <= (16 << 20)) {
                uint64_t pci_base = orig & 0xFFFFF800;
                if (pci_base == 0 && op->allocate_bar_address) {
                    pci_base =
                        op->allocate_bar_address(bus, device, function, segment,
                                                 6, size, true, false, false);
                }
                if (pci_base != 0) {
                    pci_device->rom.address =
                        op->convert_bar_address
                            ? op->convert_bar_address(pci_base)
                            : pci_base;
                    pci_device->rom.size = size;
                    pci_device->rom.mmio = true;
                    pci_device->rom.prefetchable = false;
                    op->write32(bus, device, function, segment, 0x30,
                                (uint32_t)(pci_base & 0xFFFFF800) | 1);
                    printk("PCIe: ROM BAR for %02x:%02x.%u at PCI 0x%llx "
                           "size 0x%llx\n",
                           bus, device, function, pci_base, size);
                }
            }
        }

        // 启用设备命令寄存器
        uint16_t command = op->read16(bus, device, function, segment, 0x04);
        command |= (1 << 0); // I/O Space Enable
        command |= (1 << 1); // Memory Space Enable
        command |= (1 << 2); // Bus Master Enable
        op->write16(bus, device, function, segment, 0x04, command);

        // 添加到设备列表
        if (pci_device_number < PCI_DEVICE_MAX) {
            attributes_builder_t *builder = attributes_builder_new();
            if (!builder) {
                printk("PCIe: failed to allocate bus attributes\n");
                free(pci_device);
                break;
            }
            char value[256];

            sprintf(value, "/devices/pci%04x:%02x/%04x:%02x:%02x.%01x",
                    pci_device->segment, pci_device->bus, pci_device->segment,
                    pci_device->bus, pci_device->slot, pci_device->func);
            if (attributes_builder_append_new(builder, "DEVPATH", value) < 0)
                goto err_bus_attrs;

            sprintf(value, "%06x", pci_device->class_code);
            if (attributes_builder_append_new(builder, "PCI_CLASS", value) < 0)
                goto err_bus_attrs;

            sprintf(value, "%04x:%04x", pci_device->vendor_id,
                    pci_device->device_id);
            if (attributes_builder_append_new(builder, "PCI_ID", value) < 0)
                goto err_bus_attrs;

            sprintf(value, "%04x:%04x", pci_device->subsystem_vendor_id,
                    pci_device->subsystem_device_id);
            if (attributes_builder_append_new(builder, "PCI_SUBSYS_ID", value) <
                0)
                goto err_bus_attrs;

            sprintf(value, "%04x:%02x:%02x:%01x", pci_device->segment,
                    pci_device->bus, pci_device->slot, pci_device->func);
            if (attributes_builder_append_new(builder, "PCI_SLOT_NAME", value) <
                0)
                goto err_bus_attrs;

            attribute_t **attrs = builder->attrs;
            int attrs_count = builder->count;

            pci_device->device =
                bus_device_install_pci(pci_device, attrs, attrs_count, NULL, 0);
            if (!pci_device->device)
                goto err_bus_attrs;

            pci_devices[pci_device_number] = pci_device;
            attributes_builder_free_all(builder);

            pci_device_number++;
            break;

        err_bus_attrs:
            printk("PCIe: failed to create bus attributes\n");
            attributes_builder_free_all(builder);
            free(pci_device);
        } else {
            printk("PCIe: Device list full, dropping device\n");
            free(pci_device);
        }

        break;
    }

    case 0x01: { // PCI-to-PCI Bridge
        uint32_t buses = op->read32(bus, device, function, segment, 0x18);
        uint8_t primary_bus = buses & 0xFF;
        uint8_t secondary_bus = (buses >> 8) & 0xFF;
        uint8_t subordinate_bus = (buses >> 16) & 0xFF;

        // 启用桥接器
        uint16_t bridge_ctrl = op->read16(bus, device, function, segment, 0x3E);
        bridge_ctrl &= ~(1 << 6); // Clear Secondary Bus Reset
        op->write16(bus, device, function, segment, 0x3E, bridge_ctrl);

        uint16_t command = op->read16(bus, device, function, segment, 0x04);
        command |= (1 << 0); // I/O Space Enable
        command |= (1 << 1); // Memory Space Enable
        command |= (1 << 2); // Bus Master Enable
        op->write16(bus, device, function, segment, 0x04, command);

        /* Walk the topology through the bridge's secondary bus.  Scanning the
         * complete secondary..subordinate range here and doing the same again
         * for child bridges visits every endpoint once per ancestor bridge. */
        if (secondary_bus != 0 && secondary_bus <= subordinate_bus &&
            secondary_bus != primary_bus) {
            pci_scan_bus(op, segment, secondary_bus);
        }

        free(pci_device);
        break;
    }

    case 0x02: { // CardBus Bridge
        printk("  CardBus Bridge (not supported)\n");
        free(pci_device);
        break;
    }

    default:
        printk("  Unknown header type: 0x%02x\n", header_type);
        free(pci_device);
        break;
    }
}

static bool pci_function_exists(pci_device_op_t *op, uint16_t segment,
                                uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = op->read16(bus, slot, func, segment, 0x00);

    // Vendor ID 必须有效
    if (vendor == 0xFFFF || vendor == 0x0000) {
        return false;
    }

    // 对于 function > 0，额外检查以防止误读
    if (func > 0) {
        // 读取 Device ID 确保不是全0xFF或全0x00
        uint16_t device = op->read16(bus, slot, func, segment, 0x02);
        if (device == 0xFFFF || device == 0x0000) {
            return false;
        }

        // 读取 class code 再次验证
        uint32_t class_rev = op->read32(bus, slot, func, segment, 0x08);
        if (class_rev == 0xFFFFFFFF || class_rev == 0x00000000) {
            return false;
        }
    }

    return true;
}

void pci_scan_slot(pci_device_op_t *op, uint16_t segment_group, uint8_t bus,
                   uint8_t slot) {
    if (!pci_function_exists(op, segment_group, bus, slot, 0)) {
        return;
    }

    pci_scan_function(op, segment_group, bus, slot, 0);

    uint8_t header_type = op->read8(bus, slot, 0, segment_group, 0x0E);

    if (header_type != 0xFF && header_type & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            if (pci_function_exists(op, segment_group, bus, slot, func)) {
                pci_scan_function(op, segment_group, bus, slot, func);
            }
        }
    }
}

void pci_scan_bus(pci_device_op_t *op, uint16_t segment_group, uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        pci_scan_slot(op, segment_group, bus, slot);
    }
}

void pci_scan_segment(pci_device_op_t *op, uint16_t segment_group,
                      uint8_t start_bus) {
    pci_scan_bus(op, segment_group, start_bus);
}

extern pci_driver_t *pci_drivers[MAX_PCI_DRIVERS];

void pci_controller_init() {
    struct uacpi_table mcfg_table;
    uacpi_status status = uacpi_table_find_by_signature("MCFG", &mcfg_table);

    if (status == UACPI_STATUS_OK) {
        printk("Scanning PCIe bus\n");
        // Scan PCIe bus
        mcfg_addr_to_entries((struct acpi_mcfg *)mcfg_table.ptr, mcfg_entries,
                             &mcfg_entries_len);

        for (uint64_t i = 0; i < mcfg_entries_len; i++) {
            uint64_t region_base_addr = mcfg_entries[i]->address;
            uint64_t bus_count =
                mcfg_entries[i]->end_bus - mcfg_entries[i]->start_bus + 1;
            uint64_t region_size = bus_count * (1 << 20);

            map_page_range(get_kernel_page_dir(),
                           (uint64_t)phys_to_virt(region_base_addr),
                           region_base_addr, region_size,
                           PT_FLAG_R | PT_FLAG_W | PT_FLAG_UNCACHEABLE);

            uint16_t segment_group = mcfg_entries[i]->segment;
            pci_scan_segment(&pcie_device_op, segment_group,
                             mcfg_entries[i]->start_bus);
        }
    }

#ifndef __x86_64__
    pci_dtb_ecam_init();
#endif
}

typedef struct pci_probe_job {
    pci_device_t *dev;
    pci_driver_t *drv;
} pci_probe_job_t;

static spinlock_t pci_current_lock = SPIN_INIT;
static wait_queue_head_t pci_probe_wait;
static bool pci_probe_wait_initialized;
static uint64_t pci_probe_pending;

static void pci_probe_job_complete(void) {
    bool wake = false;

    spin_lock(&pci_current_lock);
    if (pci_probe_pending > 0) {
        pci_probe_pending--;
        wake = pci_probe_pending == 0;
    }
    spin_unlock(&pci_current_lock);

    if (wake)
        wait_queue_wake_all(&pci_probe_wait, 0, EOK);
}

static void pci_probe_device_task(uint64_t arg) {
    pci_probe_job_t *job = (pci_probe_job_t *)(uintptr_t)arg;
    pci_device_t *dev = job->dev;
    pci_driver_t *drv = job->drv;

    int ret = drv->probe(dev);
    if (ret < 0) {
        printk("PCI driver %s probe failed for %04x:%02x:%02x.%u (ret=%d)\n",
               drv->name, dev->segment, dev->bus, dev->slot, dev->func, ret);
    }

    if (ret != 0) {
        spin_lock(&pci_current_lock);
        if (dev->kernel_driver == drv)
            dev->kernel_driver = NULL;
        spin_unlock(&pci_current_lock);
    } else {
        printk("PCI driver %s bound to %04x:%02x:%02x.%u\n", drv->name,
               dev->segment, dev->bus, dev->slot, dev->func);
    }
    free(job);
    pci_probe_job_complete();
}

void pci_init() {
    spin_lock(&pci_current_lock);
    if (!pci_probe_wait_initialized) {
        wait_queue_init(&pci_probe_wait);
        pci_probe_wait_initialized = true;
    }
    spin_unlock(&pci_current_lock);

    for (uint64_t i = 0; i < pci_device_number; i++) {
        pci_device_t *device = pci_devices[i];

        if (!device) {
            continue;
        }

        spin_lock(&pci_current_lock);
        bool already_bound = device->desc || device->kernel_driver;
        spin_unlock(&pci_current_lock);
        if (already_bound)
            continue;

        for (uint64_t d = 0; d < MAX_PCI_DRIVERS; d++) {
            if (!pci_drivers[d]) {
                continue;
            }

            bool matched = false;
            if (pci_drivers[d]->match) {
                matched = pci_drivers[d]->match(device, pci_drivers[d]);
            } else if (pci_drivers[d]->class_id) {
                matched = pci_drivers[d]->class_id == device->class_code;
            }

            if (matched) {
                pci_probe_job_t *job = malloc(sizeof(*job));
                if (!job) {
                    printk("PCI: failed to allocate probe job for "
                           "%04x:%02x:%02x.%u\n",
                           device->segment, device->bus, device->slot,
                           device->func);
                    break;
                }

                spin_lock(&pci_current_lock);
                if (device->desc || device->kernel_driver) {
                    spin_unlock(&pci_current_lock);
                    free(job);
                    break;
                }
                device->kernel_driver = pci_drivers[d];
                spin_unlock(&pci_current_lock);

                job->dev = device;
                job->drv = pci_drivers[d];

                /* Increment before publishing the task: it may run and
                 * finish on another CPU before task_create() returns. */
                spin_lock(&pci_current_lock);
                pci_probe_pending++;
                spin_unlock(&pci_current_lock);

                if (!task_create("dev_probe", pci_probe_device_task,
                                 (uint64_t)(uintptr_t)job, NORMAL_PRIORITY)) {
                    spin_lock(&pci_current_lock);
                    if (device->kernel_driver == job->drv)
                        device->kernel_driver = NULL;
                    spin_unlock(&pci_current_lock);
                    free(job);
                    printk("PCI: failed to create probe task for "
                           "%04x:%02x:%02x.%u\n",
                           device->segment, device->bus, device->slot,
                           device->func);
                    pci_probe_job_complete();
                }
                break; /* first matching driver, like the old synchronous loop
                        */
            }
        }
    }
}

void pci_wait_for_probes() {
    uint64_t pending;

    spin_lock(&pci_current_lock);
    pending = pci_probe_wait_initialized ? pci_probe_pending : 0;
    spin_unlock(&pci_current_lock);
    if (pending == 0)
        return;

    printk("PCI: waiting for %llu driver probe task(s)\n", pending);

    for (;;) {
        wait_queue_entry_t wait;

        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&pci_probe_wait, &wait);

        spin_lock(&pci_current_lock);
        pending = pci_probe_pending;
        spin_unlock(&pci_current_lock);
        if (pending == 0) {
            wait_queue_remove(&pci_probe_wait, &wait);
            task_cancel_block_prepare(current_task);
            break;
        }

        (void)task_block(current_task, TASK_UNINTERRUPTABLE, -1,
                         "pci_probe_wait");
        wait_queue_remove(&pci_probe_wait, &wait);
        task_cancel_block_prepare(current_task);
    }

    printk("PCI: all driver probe tasks completed\n");
}

pci_driver_t *pci_drivers[MAX_PCI_DRIVERS] = {NULL};

int regist_pci_driver(pci_driver_t *driver) {
    if (!driver) {
        return -EINVAL;
    }

    for (int i = 0; i < MAX_PCI_DRIVERS; i++) {
        if (pci_drivers[i] == driver) {
            return 0;
        }
        if (!pci_drivers[i]) {
            pci_drivers[i] = driver;
            return 0;
        }
    }

    return -ENOSPC;
}
