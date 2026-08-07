#include <boot/boot.h>
#include <limine.h>
#include <acpi/uacpi/acpi.h>
#include <mm/hhdm.h>

#define LIMINE_REQUEST __attribute__((used, section(".limine_requests")))

__attribute__((
    used, section(".limine_requests_"
                  "start"))) static volatile uint64_t requests_start_marker[4] =
    LIMINE_REQUESTS_START_MARKER;

LIMINE_REQUEST static volatile uint64_t base_revision_request[4] =
    LIMINE_BASE_REVISION(6);

LIMINE_REQUEST static volatile struct limine_stack_size_request
    stack_size_request = {
        .id = LIMINE_STACK_SIZE_REQUEST_ID,
        .revision = 0,
        .stack_size = STACK_SIZE,
};

LIMINE_REQUEST static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

LIMINE_REQUEST static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

LIMINE_REQUEST volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0, .response = NULL};

LIMINE_REQUEST volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0, .response = NULL};

LIMINE_REQUEST volatile struct limine_date_at_boot_request boot_time_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
    .revision = 0,
};

LIMINE_REQUEST volatile struct limine_framebuffer_request framebuffer_request =
    {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

LIMINE_REQUEST static volatile struct limine_firmware_type_request
    firmware_type_request = {
        .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
        .revision = 0,
};

LIMINE_REQUEST static volatile struct limine_efi_system_table_request
    efi_system_table_request = {
        .id = LIMINE_EFI_SYSTEM_TABLE_REQUEST_ID,
        .revision = 0,
};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
    requests_end_marker[4] = LIMINE_REQUESTS_END_MARKER;

#if defined(__x86_64__)
void apic_handle_lapic(struct acpi_madt_lapic *lapic) { (void)lapic; }
void apic_handle_lx2apic(struct acpi_madt_x2apic *lapic) { (void)lapic; }
#endif

void boot_init() {}

uint64_t boot_get_hhdm_offset() { return hhdm_request.response->offset; };

boot_memory_map_t limine_boot_memory_map;

boot_memory_map_t *boot_get_memory_map() {
    size_t entry_count = 0;
    for (entry_count = 0; entry_count < memmap_request.response->entry_count;
         entry_count++) {
        struct limine_memmap_entry *limine_entry =
            memmap_request.response->entries[entry_count];
        limine_boot_memory_map.entries[entry_count] = (boot_memory_map_entry_t){
            .addr = limine_entry->base,
            .len = limine_entry->length,
            .type = (limine_entry->type == LIMINE_MEMMAP_USABLE) ? USABLE
                                                                 : RESERVED,
        };
    }
    limine_boot_memory_map.entry_count = entry_count;
    return &limine_boot_memory_map;
};

uintptr_t boot_get_acpi_rsdp() {
    return rsdp_request.response ? virt_to_phys(rsdp_request.response->address)
                                 : 0;
}

void boot_get_smbios_entries(void **entry32, void **entry64) {
    if (entry32)
        *entry32 = NULL;
    if (entry64)
        *entry64 = NULL;

    if (!smbios_request.response)
        return;

    if (entry32)
        *entry32 = (void *)smbios_request.response->entry_32;
    if (entry64)
        *entry64 = (void *)smbios_request.response->entry_64;
}

uint64_t boot_get_boottime() { return boot_time_request.response->timestamp; }

__attribute__((
    used, section(".limine_requests"))) static volatile struct limine_mp_request
    mp_request = {
        .id = LIMINE_MP_REQUEST_ID,
        .revision = 0,
#if defined(__x86_64__)
        .flags = LIMINE_MP_REQUEST_X86_64_X2APIC,
#endif
};

#if defined(__riscv__)

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_riscv_bsp_hartid_request bsp_hartid_request = {
        .id = LIMINE_RISCV_BSP_HARTID_REQUEST_ID,
        .revision = 0,
};
#endif

extern uint64_t cpu_count;

extern spinlock_t ap_startup_lock;

void boot_smp_init(uintptr_t entry) {
    struct limine_mp_response *mp_response = mp_request.response;

    cpu_count = mp_response->cpu_count;

    for (uint64_t i = 0; i < cpu_count; i++) {
        struct limine_mp_info *cpu = mp_response->cpus[i];
#if defined(__x86_64__)
        extern uint32_t cpuid_to_lapicid[MAX_CPU_NUM];
        cpuid_to_lapicid[i] = cpu->lapic_id;
#endif
#if defined(__aarch64__)
        extern uint64_t cpuid_to_mpidr[MAX_CPU_NUM];
        cpuid_to_mpidr[i] = cpu->mpidr;

#endif
#if defined(__riscv__)
        extern uint64_t cpuid_to_hartid[MAX_CPU_NUM];
        cpuid_to_hartid[i] = cpu->hartid;
#endif
#if defined(__loongarch64__)
        extern uint64_t cpuid_to_physid[MAX_CPU_NUM];
        cpuid_to_physid[i] = cpu->phys_id;
#endif
    }

    for (uint64_t i = 0; i < cpu_count; i++) {
        struct limine_mp_info *cpu = mp_response->cpus[i];
#if defined(__x86_64__)
        if (cpu->lapic_id == mp_response->bsp_lapic_id)
            continue;
#endif
#if defined(__aarch64__)
        if (cpu->mpidr == mp_request.response->bsp_mpidr)
            continue;

#endif
#if defined(__riscv__)
        if (cpu->hartid == bsp_hartid_request.response->bsp_hartid)
            continue;
#endif
#if defined(__loongarch64__)
        if (cpu->phys_id == mp_response->bsp_phys_id)
            continue;
#endif
        raw_spin_lock(&ap_startup_lock);

        cpu->goto_address = (limine_goto_address)entry;
    }
}

#if defined(__x86_64__)
bool boot_cpu_support_x2apic() {
    return !!(mp_request.response->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC);
}
#endif

boot_framebuffer_t limine_boot_fb;

boot_framebuffer_t *boot_get_framebuffer() {
    if (framebuffer_request.response) {
        limine_boot_fb.address =
            (uintptr_t)framebuffer_request.response->framebuffers[0]->address;
        limine_boot_fb.width =
            framebuffer_request.response->framebuffers[0]->width;
        limine_boot_fb.height =
            framebuffer_request.response->framebuffers[0]->height;
        limine_boot_fb.bpp = framebuffer_request.response->framebuffers[0]->bpp;
        limine_boot_fb.pitch =
            framebuffer_request.response->framebuffers[0]->pitch;
        limine_boot_fb.red_mask_shift =
            framebuffer_request.response->framebuffers[0]->red_mask_shift;
        limine_boot_fb.red_mask_size =
            framebuffer_request.response->framebuffers[0]->red_mask_size;
        limine_boot_fb.blue_mask_shift =
            framebuffer_request.response->framebuffers[0]->blue_mask_shift;
        limine_boot_fb.blue_mask_size =
            framebuffer_request.response->framebuffers[0]->blue_mask_size;
        limine_boot_fb.green_mask_shift =
            framebuffer_request.response->framebuffers[0]->green_mask_shift;
        limine_boot_fb.green_mask_size =
            framebuffer_request.response->framebuffers[0]->green_mask_size;

        return &limine_boot_fb;
    }
    return NULL;
}

LIMINE_REQUEST static volatile struct limine_executable_cmdline_request
    executable_cmdline_request = {
        .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
};

LIMINE_REQUEST static volatile struct limine_executable_file_request
    executable_file_request = {
        .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
};

char *boot_get_cmdline() {
    return executable_cmdline_request.response->cmdline;
}

void *boot_get_executable_file(size_t *size) {
    if (size != NULL) {
        *size = 0;
    }

    if (executable_file_request.response == NULL ||
        executable_file_request.response->executable_file == NULL) {
        return NULL;
    }

    if (size != NULL) {
        *size = executable_file_request.response->executable_file->size;
    }

    return executable_file_request.response->executable_file->address;
}

LIMINE_REQUEST static volatile struct limine_module_request modules_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

boot_module_t limine_boot_modules[MAX_MODULES_NUM];

void boot_get_modules(boot_module_t **modules, size_t *count) {
    *count = 0;
    for (uint64_t i = 0; i < modules_request.response->module_count; i++) {
        strcpy(limine_boot_modules[i].path,
               modules_request.response->modules[i]->path);
        limine_boot_modules[i].data =
            modules_request.response->modules[i]->address;
        limine_boot_modules[i].size =
            modules_request.response->modules[i]->size;
        modules[i] = &limine_boot_modules[i];
        (*count)++;
    }
}

#if defined(__x86_64__)

LIMINE_REQUEST static volatile struct limine_paging_mode_request
    paging_mode_request = {
        .id = LIMINE_PAGING_MODE_REQUEST_ID,
        .revision = 0,
        .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};

#elif defined(__riscv__)

LIMINE_REQUEST static volatile struct limine_paging_mode_request
    paging_mode_request = {
        .id = LIMINE_PAGING_MODE_REQUEST_ID,
        .revision = 0,
        .mode = LIMINE_PAGING_MODE_RISCV_DEFAULT,
};
uint64_t boot_get_paging_mode() {
    return paging_mode_request.response ? paging_mode_request.response->mode
                                        : LIMINE_PAGING_MODE_RISCV_DEFAULT;
}

#endif

uint64_t boot_get_firmware_type() {
    return firmware_type_request.response
               ? firmware_type_request.response->firmware_type
               : 0;
}

uint64_t boot_get_system_table() {
    return efi_system_table_request.response
               ? (uint64_t)efi_system_table_request.response->address
               : 0;
}

LIMINE_REQUEST static volatile struct limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID,
    .revision = 0,
    .response = NULL,
};

uint64_t boot_get_dtb() {
    return dtb_request.response ? (uint64_t)dtb_request.response->dtb_ptr : 0;
}

#if defined(__riscv__)

uint64_t boot_get_bsp_hartid() {
    return bsp_hartid_request.response ? bsp_hartid_request.response->bsp_hartid
                                       : 0;
}
#endif
