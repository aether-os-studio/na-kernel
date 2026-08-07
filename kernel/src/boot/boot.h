#pragma once

#include <libs/klibc.h>

typedef struct boot_memory_map_entry {
    uintptr_t addr;
    size_t len;
    enum {
        USABLE,
        RESERVED,
    } type;
} boot_memory_map_entry_t;

typedef struct boot_memory_map {
    boot_memory_map_entry_t entries[8192];
    size_t entry_count;
} boot_memory_map_t;

void boot_init();

uint64_t boot_get_hhdm_offset();
boot_memory_map_t *boot_get_memory_map();

uintptr_t boot_get_acpi_rsdp();
void boot_get_smbios_entries(void **entry32, void **entry64);

uint64_t boot_get_boottime();

void boot_smp_init(uintptr_t entry);

#if defined(__x86_64__)
bool boot_cpu_support_x2apic();
#endif

typedef struct boot_framebuffer {
    uintptr_t address;
    size_t width;
    size_t height;
    size_t bpp;
    size_t pitch;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
} boot_framebuffer_t;

boot_framebuffer_t *boot_get_framebuffer();

char *boot_get_cmdline();
void *boot_get_executable_file(size_t *size);

typedef struct boot_module {
    char path[64];
    void *data;
    size_t size;
} boot_module_t;

#define MAX_MODULES_NUM 512

void boot_get_modules(boot_module_t **modules, size_t *count);

uint64_t boot_get_firmware_type();
uint64_t boot_get_system_table();

uint64_t boot_get_dtb();

#if defined(__riscv__)
uint64_t boot_get_bsp_hartid();
uint64_t boot_get_paging_mode();
#endif
