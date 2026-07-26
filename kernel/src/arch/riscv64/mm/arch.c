#include "arch.h"
#include <arch/arch.h>
#include <boot/boot.h>
#include <limine.h>
#include <mm/mm.h>

static uint64_t riscv64_pt_levels = 0;
static uint64_t riscv64_satp_mode = 0;

uint64_t arch_page_table_levels() {
    if (riscv64_pt_levels != 0)
        return riscv64_pt_levels;

    switch (boot_get_paging_mode()) {
    case LIMINE_PAGING_MODE_RISCV_SV39:
        riscv64_pt_levels = 3;
        break;
    case LIMINE_PAGING_MODE_RISCV_SV57:
        riscv64_pt_levels = 5;
        break;
    case LIMINE_PAGING_MODE_RISCV_SV48:
    default:
        riscv64_pt_levels = 4;
        break;
    }

    return riscv64_pt_levels;
}

uint64_t arch_user_va_limit(void) {
    uint64_t levels = arch_page_table_levels();
    uint64_t va_bits = ARCH_PT_OFFSET_BASE + levels * ARCH_PT_OFFSET_PER_LEVEL;
    if (va_bits >= 64)
        return UINT64_MAX;
    return (1ULL << (va_bits - 1)) - 1;
}

static uint64_t riscv64_detect_satp_mode() {
    if (riscv64_satp_mode != 0)
        return riscv64_satp_mode;

    switch (boot_get_paging_mode()) {
    case LIMINE_PAGING_MODE_RISCV_SV39:
        riscv64_satp_mode = 8;
        break;
    case LIMINE_PAGING_MODE_RISCV_SV57:
        riscv64_satp_mode = 10;
        break;
    case LIMINE_PAGING_MODE_RISCV_SV48:
    default:
        riscv64_satp_mode = 9;
        break;
    }

    return riscv64_satp_mode;
}

uint64_t riscv64_make_satp(uint64_t root_paddr) {
    return (riscv64_detect_satp_mode() << 60) | (root_paddr >> 12);
}

void riscv64_set_page_table_root(uint64_t root_paddr) {
    uint64_t satp = riscv64_make_satp(root_paddr);
    asm volatile("csrw satp, %0" : : "r"(satp) : "memory");
    arch_flush_tlb_all();
}

uint64_t *get_current_page_dir(bool user) {
    uint64_t satp;
    (void)user;
    asm volatile("csrr %0, satp" : "=r"(satp));
    uint64_t ppn = satp & ((1ULL << 44) - 1);
    return phys_to_virt(ppn << 12);
}

void set_current_page_dir(bool user, uint64_t pgdir) {
    (void)user;
    riscv64_set_page_table_root(pgdir);
}

void arch_page_table_init(void) {
    memset(get_current_page_dir(false), 0, PAGE_SIZE / 2);
}

uint64_t arch_page_table_root_entries(int level) {
    uint64_t entries = (1UL << ARCH_PT_OFFSET_PER_LEVEL);
    if ((uint64_t)level == arch_page_table_levels())
        entries >>= 1;
    return entries;
}

uint64_t arch_make_page_table_entry(uint64_t paddr, uint64_t flags) {
    (void)flags;
    return ARCH_MAKE_PDE(paddr, ARCH_PT_TABLE_FLAGS);
}

void arch_page_table_prepare_new(uint64_t *root) {
    memset(root, 0, PAGE_SIZE);
    arch_page_table_copy_kernel(root, get_kernel_page_dir());
}

void arch_page_table_copy_kernel(uint64_t *dst, uint64_t *src) {
    uint64_t user_entries = (1UL << ARCH_PT_OFFSET_PER_LEVEL) >> 1;
    memcpy(dst + user_entries, src + user_entries, PAGE_SIZE / 2);
}

bool arch_page_table_flags_writable(uint64_t flags) {
    return (flags & ARCH_PT_FLAG_WRITEABLE) != 0;
}

uint64_t arch_page_table_flags_make_cow(uint64_t flags) {
    return (flags | ARCH_PT_FLAG_COW) & ~ARCH_PT_FLAG_WRITEABLE;
}

uint64_t arch_page_table_flags_make_writable(uint64_t flags) {
    return (flags | ARCH_PT_FLAG_WRITEABLE) & ~ARCH_PT_FLAG_COW;
}

uint64_t get_arch_page_table_flags(uint64_t flags) {
    uint64_t attr =
        ARCH_PT_FLAG_VALID | ARCH_PT_FLAG_ACCESS | ARCH_PT_FLAG_DIRTY;

    if (flags & PT_FLAG_R)
        attr |= ARCH_PT_FLAG_READ;
    /* RISC-V 不允许 R=0、W=1 的叶子页表项，写映射必须同时可读。 */
    if (flags & PT_FLAG_W)
        attr |= ARCH_PT_FLAG_READ | ARCH_PT_FLAG_WRITE;
    if (flags & PT_FLAG_X)
        attr |= ARCH_PT_FLAG_EXEC;
    if (flags & PT_FLAG_U)
        attr |= ARCH_PT_FLAG_USER;
    if (flags & PT_FLAG_COW)
        attr |= ARCH_PT_FLAG_COW;

    return attr;
}

void arch_flush_tlb(uint64_t vaddr) {
    asm volatile("sfence.vma %0, zero" : : "r"(vaddr) : "memory");
}

void arch_flush_tlb_all() {
    asm volatile("sfence.vma zero, zero" ::: "memory");
}
