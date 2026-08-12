#include <arch/x86_64/x86_64.h>
#include <drivers/deadline.h>
#include <task/task.h>

extern void sse_init();
extern bool task_initialized;

void arch_early_init() {
    close_interrupt;

    init_serial();

    sse_init();
    irq_init();
    generic_interrupt_table_init_early();
    hpet_init();
    apic_init();
    x64_cpu_local_init(get_cpuid_by_lapic_id((uint32_t)lapic_id()),
                       (uint32_t)lapic_id());
    x64_tlb_note_loaded_pgdir();
    tss_init();

    apic_timer_init();
    local_apic_init();
    hpet_clockevent_init();
    rtc_cmos_init();

    apic_ipi_init();

    smp_init();

    fsgsbase_init();
}

void arch_init() {
    syscall_init();
    syscall_handler_init();
}

void arch_init_after_thread() {}

void arch_init_after_acpi_pci() {}

void arch_input_dev_init() {
    bool irq_state = arch_interrupt_enabled();
    if (irq_state)
        arch_disable_interrupt();

    if (ps2_init()) {
        if (!ps2_keyboard_init()) {
            printk("PS/2 keyboard init failed\n");
        }
        if (!ps2_mouse_init()) {
            printk("PS/2 mouse init failed\n");
        }
    }

    if (irq_state)
        arch_enable_interrupt();
}

void arch_pause() { asm volatile("pause"); }

void arch_shutdown() {
    while (1) {
        asm volatile("cli\n\thlt");
    }
}

void arch_enable_user_access() {}
void arch_disable_user_access() {}

void arch_program_timer_deadline_local(uint64_t deadline_ns) {
    if (deadline_ns == UINT64_MAX) {
        apic_timer_set_interval_ns(1000000000ULL / SCHED_HZ);
        return;
    }

    uint64_t now = nano_time();
    uint64_t delta_ns = deadline_ns > now ? deadline_ns - now : 1;
    apic_timer_set_interval_ns(delta_ns);
}

void arch_before_wait_for_interrupt(void) {
    if (!arch_interrupt_enabled())
        return;
    if (!__atomic_load_n(&task_initialized, __ATOMIC_ACQUIRE))
        return;

    sched_check_wakeup();
    deadline_reprogram_local();
}

bool arch_memory_region_usable(uint64_t addr, uint64_t len) {
    (void)len;
    return addr >= 0x100000;
}

uintptr_t arch_get_return_address(uint32_t level) {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
#endif
#define RETURN_ADDRESS(level)                                                  \
    do {                                                                       \
        if (!__builtin_frame_address(level))                                   \
            return 0;                                                          \
        return (uintptr_t)__builtin_return_address(level);                     \
    } while (0)
    switch (level) {
    case 0:
        RETURN_ADDRESS(0);
    case 1:
        RETURN_ADDRESS(1);
    case 2:
        RETURN_ADDRESS(2);
    case 3:
        RETURN_ADDRESS(3);
    case 4:
        RETURN_ADDRESS(4);
    case 5:
        RETURN_ADDRESS(5);
    case 6:
        RETURN_ADDRESS(6);
    case 7:
        RETURN_ADDRESS(7);
    case 8:
        RETURN_ADDRESS(8);
    case 9:
        RETURN_ADDRESS(9);
    case 10:
        RETURN_ADDRESS(10);
    case 11:
        RETURN_ADDRESS(11);
    case 12:
        RETURN_ADDRESS(12);
    case 13:
        RETURN_ADDRESS(13);
    case 14:
        RETURN_ADDRESS(14);
    case 15:
        RETURN_ADDRESS(15);
    case 16:
        RETURN_ADDRESS(16);
    default:
        return 0;
    }
#undef RETURN_ADDRESS
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}
