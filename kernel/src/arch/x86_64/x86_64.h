#pragma once

#define ARCH_MAX_IRQ_NUM 256
#define SCHED_HZ 250

#include "arch/x86_64/asm.h"
#include "arch/x86_64/core/normal.h"
#include "arch/x86_64/cpu_local.h"
#include "arch/x86_64/mm/arch.h"
#include "arch/x86_64/irq/ptrace.h"
#include "arch/x86_64/irq/gate.h"
#include "arch/x86_64/irq/trap.h"
#include "arch/x86_64/irq/irq.h"
#include "arch/x86_64/drivers/serial.h"
#include "arch/x86_64/drivers/apic_timer.h"
#include "arch/x86_64/drivers/rtc_cmos.h"
#include "arch/x86_64/drivers/chars/ps2.h"
#include "arch/x86_64/drivers/msi.h"
#include "arch/x86_64/task/arch_context.h"
#include "arch/x86_64/task/fsgsbase.h"
#include "arch/x86_64/syscall/nr.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/time/time.h"

void arch_early_init();
void arch_init();
void arch_init_after_thread();
void arch_init_after_acpi_pci();
void arch_input_dev_init();

__attribute__((noreturn)) void arch_shutdown();

void arch_pause();

void arch_before_wait_for_interrupt(void);

static inline void arch_wait_for_interrupt() {
    arch_before_wait_for_interrupt();
    asm volatile("hlt");
}

void dcache_clean_range(void *addr, size_t size);
void dcache_invalidate_range(void *addr, size_t size);
void dcache_flush_range(void *addr, size_t size);
void sync_instruction_memory_range(void *addr, size_t size);

void memory_barrier(void);

void read_barrier(void);

void write_barrier(void);

void arch_enable_user_access();
void arch_disable_user_access();
bool arch_memory_region_usable(uint64_t addr, uint64_t len);
uintptr_t arch_get_return_address(uint32_t level);
