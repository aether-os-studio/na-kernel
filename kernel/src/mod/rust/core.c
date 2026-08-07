#include <drivers/logger.h>
#include <libs/klibc.h>
#include <mm/hhdm.h>
#include <mm/mm.h>
#include <mm/page_table_flags.h>
#include <mod/rust/api.h>

void na_log(const char *message) {
    if (message)
        serial_fprintk("%s", message);
}

void *na_mmio_map(uint64_t physical_address, size_t size) {
    if (!size || physical_address > UINT64_MAX - size)
        return NULL;

    uint64_t physical_end = physical_address + size;
    uint64_t map_start = PADDING_DOWN(physical_address, PAGE_SIZE);
    uint64_t map_end = PADDING_UP(physical_end, PAGE_SIZE);
    if (map_end < physical_end)
        return NULL;

    uint64_t virtual_start = (uint64_t)phys_to_virt(map_start);
    uint64_t flags = PT_FLAG_R | PT_FLAG_W | PT_FLAG_UNCACHEABLE;
    if (map_page_range(get_kernel_page_dir(), virtual_start, map_start,
                       map_end - map_start, flags) != 0)
        return NULL;

    return phys_to_virt(physical_address);
}
