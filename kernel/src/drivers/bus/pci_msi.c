#include <drivers/bus/pci_msi.h>
#include <mm/mm.h>
#include <arch/arch.h>
#include <drivers/logger.h>
#include <irq/irq_manager.h>

static inline struct pci_msi_cap_t
__msi_read_cap_list(struct msi_desc_t *msi_desc, uint32_t cap_off) {
    struct pci_msi_cap_t cap_list = {0};
    pci_device_t *ptr = msi_desc->pci_dev;
    uint32_t dw0;
    dw0 =
        ptr->op->read32(ptr->bus, ptr->slot, ptr->func, ptr->segment, cap_off);
    cap_list.cap_id = dw0 & 0xff;
    cap_list.next_off = (dw0 >> 8) & 0xff;
    cap_list.msg_ctrl = (dw0 >> 16) & 0xffff;

    cap_list.msg_addr_lo = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                           ptr->segment, cap_off + 0x4);
    uint16_t msg_data_off = 0xc;
    if (cap_list.msg_ctrl & (1 << 7)) // 64位
    {
        cap_list.msg_addr_hi = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                               ptr->segment, cap_off + 0x8);
    } else {
        cap_list.msg_addr_hi = 0;
        msg_data_off = 0x8;
    }

    cap_list.msg_data = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                        ptr->segment, cap_off + msg_data_off) &
                        0xffff;

    cap_list.mask = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                    ptr->segment, cap_off + 0x10);
    cap_list.pending = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                       ptr->segment, cap_off + 0x14);

    return cap_list;
}

static inline struct pci_msix_cap_t
__msi_read_msix_cap_list(struct msi_desc_t *msi_desc, uint32_t cap_off) {
    struct pci_msix_cap_t cap_list = {0};
    pci_device_t *ptr = msi_desc->pci_dev;
    uint32_t dw0;
    dw0 =
        ptr->op->read32(ptr->bus, ptr->slot, ptr->func, ptr->segment, cap_off);
    cap_list.cap_id = dw0 & 0xff;
    cap_list.next_off = (dw0 >> 8) & 0xff;
    cap_list.msg_ctrl = (dw0 >> 16) & 0xffff;

    cap_list.dword1 = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                      ptr->segment, cap_off + 0x4);
    cap_list.dword2 = ptr->op->read32(ptr->bus, ptr->slot, ptr->func,
                                      ptr->segment, cap_off + 0x8);
    return cap_list;
}

static inline int __msix_map_table(pci_device_t *pci_dev,
                                   struct pci_msix_cap_t *msix_cap) {
    pci_dev->msix_offset = msix_cap->dword1 & (~0x7);
    pci_dev->msix_table_size = (msix_cap->msg_ctrl & 0x7ff) + 1;
    pci_dev->msix_mmio_size =
        pci_dev->msix_table_size * 16 + pci_dev->msix_offset;

    uint32_t bir = msix_cap->dword1 & 0x7;
    if (bir > 5) {
        printk("MSI-X: Invalid bir %d\n", bir);
        return -EINVAL;
    }

    uint64_t bar_physical_address = pci_dev->bars[bir].address;
    uint64_t bar_size = pci_dev->bars[bir].size;

    if (bar_physical_address == 0 || bar_size == 0) {
        return -ENOMEM;
    }

    pci_dev->msix_mmio_vaddr =
        (uint64_t)phys_to_virt((uint64_t)bar_physical_address);
    if (map_page_range(get_kernel_page_dir(), pci_dev->msix_mmio_vaddr,
                       bar_physical_address, bar_size,
                       PT_FLAG_R | PT_FLAG_W | PT_FLAG_DEVICE) != 0) {
        pci_dev->msix_mmio_vaddr = 0;
        return -ENOMEM;
    }

    return 0;
}

static inline void __msix_set_entry(struct msi_desc_t *msi_desc) {
    uint64_t table_base =
        msi_desc->pci_dev->msix_mmio_vaddr + msi_desc->pci_dev->msix_offset;
    volatile uint32_t *entry_ptr =
        (volatile uint32_t *)(table_base + msi_desc->msi_index * 16);
    uint32_t vector_control = msi_desc->msg.vector_control;

    entry_ptr[3] = vector_control | 1U;
    dma_mb();
    entry_ptr[0] = msi_desc->msg.address_lo;
    entry_ptr[1] = msi_desc->msg.address_hi;
    entry_ptr[2] = msi_desc->msg.data;
    dma_mb();
    entry_ptr[3] = vector_control & ~1U;
    dma_mb();
}

static inline void __msix_clear_entry(pci_device_t *pci_dev,
                                      uint16_t msi_index) {
    uint64_t table_base = pci_dev->msix_mmio_vaddr + pci_dev->msix_offset;
    volatile uint64_t *entry_ptr =
        (volatile uint64_t *)(table_base + msi_index * 16);

    // 清除MSI-X表项
    entry_ptr[0] = 0;
    entry_ptr[1] = 0;
}

int msi_prepare_desc(struct msi_desc_t *msi_desc, pci_device_t *pci_dev,
                     uint16_t msi_index, bool prefer_msix) {
    if (!msi_desc || !pci_dev)
        return -EINVAL;

    memset(msi_desc, 0, sizeof(*msi_desc));
    msi_desc->pci_dev = pci_dev;
    msi_desc->msi_index = msi_index;
    msi_desc->edge_trigger = true;
    msi_desc->assert = false;
    msi_desc->pci.msi_attribute.is_msix = prefer_msix;

    return arch_pci_msi_prepare(msi_desc, msi_index);
}

void msi_release_desc(struct msi_desc_t *msi_desc) {
    if (!msi_desc)
        return;

    if (msi_desc->pci_dev && msi_desc->pci.msi_attribute.is_msix &&
        msi_desc->pci_dev->msix_mmio_vaddr &&
        msi_desc->msi_index < msi_desc->pci_dev->msix_table_size) {
        __msix_clear_entry(msi_desc->pci_dev, msi_desc->msi_index);
    }

    arch_pci_msi_free(msi_desc);
    memset(msi_desc, 0, sizeof(*msi_desc));
}

int msi_setup_irq(struct msi_desc_t *msi_desc, pci_device_t *pci_dev,
                  uint16_t msi_index, bool prefer_msix,
                  void (*handler)(uint64_t irq_num, void *data,
                                  struct pt_regs *regs),
                  void *handler_data, char *name) {
    int ret = msi_prepare_desc(msi_desc, pci_dev, msi_index, prefer_msix);
    if (ret < 0)
        return ret;

    ret = msi_enable(msi_desc);
    if (ret < 0) {
        msi_release_desc(msi_desc);
        return ret;
    }

    irq_regist_irq(msi_desc->irq_num, handler, 0, handler_data,
                   arch_pci_msi_controller(), name,
                   IRQ_FLAGS_MSIX | IRQ_FLAGS_EDGE);
    return 0;
}

int msi_enable(struct msi_desc_t *msi_desc) {
    pci_device_t *ptr;
    uint32_t cap_ptr;
    uint16_t command;
    uint16_t message_control;

    if (!msi_desc || !msi_desc->pci_dev || !msi_desc->pci_dev->op)
        return -EINVAL;

    ptr = msi_desc->pci_dev;

    if (msi_desc->pci.msi_attribute.is_msix) {
        cap_ptr = pci_enumerate_capability_list(ptr, 0x11);
        if (cap_ptr == 0) {
            cap_ptr = pci_enumerate_capability_list(ptr, 0x05);
            if (cap_ptr == 0)
                return -ENOSYS;
            msi_desc->pci.msi_attribute.is_msix = 0;
        }
    } else {
        cap_ptr = pci_enumerate_capability_list(ptr, 0x05);
        if (cap_ptr == 0)
            return -ENOSYS;
        msi_desc->pci.msi_attribute.is_msix = 0;
    }

    arch_pci_msi_compose_msg(msi_desc);

    command = ptr->op->read16(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                              PCI_CONF_COMMAND);
    command |= (1U << 1) | (1U << 2) | (1U << 10);
    ptr->op->write16(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                     PCI_CONF_COMMAND, command);

    if (msi_desc->pci.msi_attribute.is_msix) {
        struct pci_msix_cap_t cap = __msi_read_msix_cap_list(msi_desc, cap_ptr);

        if (ptr->msix_mmio_vaddr == 0) {
            int ret = __msix_map_table(ptr, &cap);
            if (ret < 0)
                return ret;
        }

        if (msi_desc->msi_index >= ptr->msix_table_size)
            return -EINVAL;

        __msix_set_entry(msi_desc);

        message_control = ptr->op->read16(ptr->bus, ptr->slot, ptr->func,
                                          ptr->segment, cap_ptr + 0x2);
        message_control &= ~(1U << 14);
        message_control |= (1U << 15);
        ptr->op->write16(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                         cap_ptr + 0x2, message_control);
        return 0;
    }

    message_control = ptr->op->read16(ptr->bus, ptr->slot, ptr->func,
                                      ptr->segment, cap_ptr + 0x2);
    ptr->op->write32(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                     cap_ptr + 0x4, msi_desc->msg.address_lo);

    if (message_control & (1U << 7)) {
        ptr->op->write32(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                         cap_ptr + 0x8, msi_desc->msg.address_hi);
        ptr->op->write16(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                         cap_ptr + 0xC, (uint16_t)msi_desc->msg.data);
    } else {
        ptr->op->write16(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                         cap_ptr + 0x8, (uint16_t)msi_desc->msg.data);
    }

    message_control |= 1U;
    message_control &= ~(7U << 4);
    ptr->op->write16(ptr->bus, ptr->slot, ptr->func, ptr->segment,
                     cap_ptr + 0x2, message_control);

    return 0;
}
