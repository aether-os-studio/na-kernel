#include <arch/x86_64/cpu_local.h>
#include <arch/x86_64/core/normal.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/task/fsgsbase.h>
#include <mm/mm.h>
#include <task/task_struct.h>
#include <mod/dlinker.h>

#define IA32_TSC_AUX 0xc0000103

static x64_cpu_local_t x64_cpu_locals[MAX_CPU_NUM];
static bool x64_cpuid_detected = false;
static bool x64_rdpid_supported;
static bool x64_rdtscp_supported;

static void x64_cpu_local_detect_fast_id(void) {
    uint32_t eax, ebx, ecx, edx;

    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0), "c"(0));
    if (eax >= 7) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
        x64_rdpid_supported = !!(ecx & (1U << 22));
    }

    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0x80000000U), "c"(0));
    if (eax >= 0x80000001U) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001U), "c"(0));
        x64_rdtscp_supported = !!(edx & (1U << 27));
    }
}

static uint32_t x64_cpu_local_fast_id(void) {
    if (x64_rdpid_supported) {
        uint64_t id;
        asm volatile("rdpid %0" : "=r"(id) : : "memory");
        return (uint32_t)id;
    }
    if (x64_rdtscp_supported) {
        uint32_t eax, edx, id;
        asm volatile("rdtscp" : "=a"(eax), "=d"(edx), "=c"(id) : : "memory");
        return id;
    }
    return UINT32_MAX;
}

x64_cpu_local_t *x64_get_cpu_local(void) {
    uint32_t cpu_id = x64_cpu_local_fast_id();

    if (cpu_id < MAX_CPU_NUM)
        return &x64_cpu_locals[cpu_id];

    return (x64_cpu_local_t *)read_kgsbase();
}

x64_cpu_local_t *x64_get_cpu_local_by_id(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPU_NUM)
        return NULL;
    return &x64_cpu_locals[cpu_id];
}

void x64_cpu_local_init(uint32_t cpu_id, uint32_t lapic_id_value) {
    if (cpu_id >= MAX_CPU_NUM)
        return;

    if (!x64_cpuid_detected) {
        x64_cpu_local_detect_fast_id();
        x64_cpuid_detected = true;
    }
    if (x64_rdpid_supported || x64_rdtscp_supported)
        wrmsr(IA32_TSC_AUX, cpu_id);

    x64_cpu_local_t *local = &x64_cpu_locals[cpu_id];
    memset(local, 0, sizeof(*local));
    local->cpu_id = cpu_id;
    local->lapic_id = lapic_id_value;
    write_kgsbase((uint64_t)local);
}

void x64_cpu_local_set_current(task_t *current) {
    x64_cpu_local_t *local = x64_get_cpu_local();
    if (!local) {
        return;
    }

    local->task_ptr = current;
    local->syscall_stack = current ? current->syscall_stack : 0;
}

uint32_t x64_current_cpu_id(void) {
    x64_cpu_local_t *local = x64_get_cpu_local();
    if (local)
        return local->cpu_id;
    return get_cpuid_by_lapic_id((uint32_t)lapic_id());
}

void x64_irq_context_enter(void) {
    x64_cpu_local_t *local = x64_get_cpu_local();
    if (!local)
        return;

    local->irq_nesting++;
}

void x64_irq_context_exit(void) {
    x64_cpu_local_t *local = x64_get_cpu_local();
    if (!local || local->irq_nesting == 0)
        return;

    local->irq_nesting--;
}

bool x64_in_irq_context(void) {
    x64_cpu_local_t *local = x64_get_cpu_local();
    return local && local->irq_nesting != 0;
}
