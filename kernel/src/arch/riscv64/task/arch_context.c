#include "arch_context.h"
#include <arch/riscv64/cpu_local.h>
#include <drivers/logger.h>
#include <mm/mm.h>
#include <task/sched.h>
#include <task/task.h>

extern void kernel_thread_func();
extern void ret_from_fork();
extern void riscv64_trap_return();

static inline uint64_t riscv64_sstatus_for_user(void) {
    return (1UL << 18) | (1UL << 13) | (1UL << 5);
}

static inline uint64_t riscv64_sstatus_for_kernel(void) {
    return (1UL << 18) | (1UL << 13) | (1UL << 5) | (1UL << 8);
}

#define RISCV_SSTATUS_SIE (1UL << 1)
#define RISCV_SSTATUS_FS_MASK (3UL << 13)
#define RISCV_SSTATUS_FS_DIRTY (3UL << 13)

static inline uint64_t riscv64_kernel_sstatus_default(void) {
    return RISCV_SSTATUS_SIE;
}

static inline uint64_t riscv64_fpu_enable_kernel(void) {
    uint64_t sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    uint64_t enabled =
        (sstatus & ~RISCV_SSTATUS_FS_MASK) | RISCV_SSTATUS_FS_DIRTY;
    asm volatile("csrw sstatus, %0" : : "r"(enabled) : "memory");
    return sstatus;
}

static inline void riscv64_fpu_restore_kernel_state(uint64_t sstatus) {
    asm volatile("csrw sstatus, %0" : : "r"(sstatus) : "memory");
}

void riscv64_fpu_state_init(fpu_context_t *fpu_ctx) {
    if (!fpu_ctx)
        return;

    memset(fpu_ctx, 0, sizeof(*fpu_ctx));
}

void riscv64_fpu_save(fpu_context_t *fpu_ctx) {
    if (!fpu_ctx)
        return;

    uint64_t sstatus = riscv64_fpu_enable_kernel();

    asm volatile(".option push\n\t"
                 ".option arch, +f\n\t"
                 ".option arch, +d\n\t"
                 "fsd f0,   0(%0)\n\t"
                 "fsd f1,   8(%0)\n\t"
                 "fsd f2,  16(%0)\n\t"
                 "fsd f3,  24(%0)\n\t"
                 "fsd f4,  32(%0)\n\t"
                 "fsd f5,  40(%0)\n\t"
                 "fsd f6,  48(%0)\n\t"
                 "fsd f7,  56(%0)\n\t"
                 "fsd f8,  64(%0)\n\t"
                 "fsd f9,  72(%0)\n\t"
                 "fsd f10, 80(%0)\n\t"
                 "fsd f11, 88(%0)\n\t"
                 "fsd f12, 96(%0)\n\t"
                 "fsd f13, 104(%0)\n\t"
                 "fsd f14, 112(%0)\n\t"
                 "fsd f15, 120(%0)\n\t"
                 "fsd f16, 128(%0)\n\t"
                 "fsd f17, 136(%0)\n\t"
                 "fsd f18, 144(%0)\n\t"
                 "fsd f19, 152(%0)\n\t"
                 "fsd f20, 160(%0)\n\t"
                 "fsd f21, 168(%0)\n\t"
                 "fsd f22, 176(%0)\n\t"
                 "fsd f23, 184(%0)\n\t"
                 "fsd f24, 192(%0)\n\t"
                 "fsd f25, 200(%0)\n\t"
                 "fsd f26, 208(%0)\n\t"
                 "fsd f27, 216(%0)\n\t"
                 "fsd f28, 224(%0)\n\t"
                 "fsd f29, 232(%0)\n\t"
                 "fsd f30, 240(%0)\n\t"
                 "fsd f31, 248(%0)\n\t"
                 ".option pop\n\t"
                 :
                 : "r"(fpu_ctx)
                 : "memory");

    uint64_t fcsr;
    asm volatile(".option push\n\t"
                 ".option arch, +f\n\t"
                 "frcsr %0\n\t"
                 ".option pop"
                 : "=r"(fcsr));
    fpu_ctx->fcsr = (uint32_t)fcsr;

    riscv64_fpu_restore_kernel_state(sstatus);
}

void riscv64_fpu_restore(fpu_context_t *fpu_ctx) {
    if (!fpu_ctx)
        return;

    uint64_t sstatus = riscv64_fpu_enable_kernel();

    asm volatile(".option push\n\t"
                 ".option arch, +f\n\t"
                 ".option arch, +d\n\t"
                 "fld f0,   0(%0)\n\t"
                 "fld f1,   8(%0)\n\t"
                 "fld f2,  16(%0)\n\t"
                 "fld f3,  24(%0)\n\t"
                 "fld f4,  32(%0)\n\t"
                 "fld f5,  40(%0)\n\t"
                 "fld f6,  48(%0)\n\t"
                 "fld f7,  56(%0)\n\t"
                 "fld f8,  64(%0)\n\t"
                 "fld f9,  72(%0)\n\t"
                 "fld f10, 80(%0)\n\t"
                 "fld f11, 88(%0)\n\t"
                 "fld f12, 96(%0)\n\t"
                 "fld f13, 104(%0)\n\t"
                 "fld f14, 112(%0)\n\t"
                 "fld f15, 120(%0)\n\t"
                 "fld f16, 128(%0)\n\t"
                 "fld f17, 136(%0)\n\t"
                 "fld f18, 144(%0)\n\t"
                 "fld f19, 152(%0)\n\t"
                 "fld f20, 160(%0)\n\t"
                 "fld f21, 168(%0)\n\t"
                 "fld f22, 176(%0)\n\t"
                 "fld f23, 184(%0)\n\t"
                 "fld f24, 192(%0)\n\t"
                 "fld f25, 200(%0)\n\t"
                 "fld f26, 208(%0)\n\t"
                 "fld f27, 216(%0)\n\t"
                 "fld f28, 224(%0)\n\t"
                 "fld f29, 232(%0)\n\t"
                 "fld f30, 240(%0)\n\t"
                 "fld f31, 248(%0)\n\t"
                 ".option pop\n\t"
                 :
                 : "r"(fpu_ctx)
                 : "memory");

    uint64_t fcsr = fpu_ctx->fcsr;
    asm volatile(".option push\n\t"
                 ".option arch, +f\n\t"
                 "fscsr %0\n\t"
                 ".option pop"
                 :
                 : "r"(fcsr)
                 : "memory");

    riscv64_fpu_restore_kernel_state(sstatus);
}

void arch_context_init(arch_context_t *context, uint64_t page_table_addr,
                       uint64_t entry, uint64_t stack, bool user_mode,
                       uint64_t initial_arg) {
    memset(context, 0, sizeof(*context));
    context->fpu_ctx = alloc_frames_bytes(sizeof(fpu_context_t));
    riscv64_fpu_state_init(context->fpu_ctx);
    context->satp = riscv64_make_satp(page_table_addr);
    context->ctx = (struct pt_regs *)stack - 1;
    memset(context->ctx, 0, sizeof(struct pt_regs));
    context->sp = (uint64_t)context->ctx;
    context->kernel_sstatus = riscv64_kernel_sstatus_default();

    if (user_mode) {
        context->ra = (uint64_t)riscv64_trap_return;
        context->ctx->sepc = entry;
        context->ctx->sp = stack;
        context->ctx->a0 = initial_arg;
        context->ctx->syscallno = NO_SYSCALL;
        context->ctx->sstatus = riscv64_sstatus_for_user();
    } else {
        context->ra = (uint64_t)kernel_thread_func;
        context->s0 = entry;
        context->s1 = initial_arg;
        context->ctx->syscallno = NO_SYSCALL;
        context->ctx->sstatus = riscv64_sstatus_for_kernel();
    }
}

void arch_context_copy(arch_context_t *dst, arch_context_t *src, uint64_t stack,
                       uint64_t clone_flags) {
    (void)clone_flags;
    memset(dst, 0, sizeof(*dst));
    dst->satp = src->satp;
    dst->ctx = (struct pt_regs *)stack - 1;
    memcpy(dst->ctx, src->ctx, sizeof(struct pt_regs));
    dst->ctx->a0 = 0;
    dst->ctx->syscallno = src->ctx ? src->ctx->syscallno : NO_SYSCALL;
    dst->ctx->sepc += 4;
    dst->ra = (uint64_t)ret_from_fork;
    dst->sp = (uint64_t)dst->ctx;
    dst->kernel_sstatus = src->kernel_sstatus;
    dst->fpu_ctx = alloc_frames_bytes(sizeof(fpu_context_t));
    if (src->fpu_ctx) {
        if (current_task && current_task->arch_context == src)
            riscv64_fpu_save(src->fpu_ctx);
        memcpy(dst->fpu_ctx, src->fpu_ctx, sizeof(fpu_context_t));
    } else {
        riscv64_fpu_state_init(dst->fpu_ctx);
    }
}

void arch_context_free(arch_context_t *context) {
    if (!context)
        return;

    if (context->fpu_ctx) {
        free_frames_bytes(context->fpu_ctx, sizeof(fpu_context_t));
        context->fpu_ctx = NULL;
    }
}

void arch_context_save_interrupt_state(arch_context_t *context, bool enabled) {
    if (!context)
        return;

    uint64_t sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    if (enabled)
        sstatus |= RISCV_SSTATUS_SIE;
    else
        sstatus &= ~RISCV_SSTATUS_SIE;
    context->kernel_sstatus = sstatus;
}

void __switch_to(task_t *prev, task_t *next) {
    riscv64_cpu_local_t *local = riscv64_get_cpu_local();
    if (local) {
        local->task_ptr = next;
        local->syscall_stack = next ? next->syscall_stack : 0;
    }

    riscv64_fpu_save(prev->arch_context->fpu_ctx);
    riscv64_fpu_restore(next->arch_context->fpu_ctx);

    task_mark_on_cpu(prev, false);
    if (prev->state == TASK_DIED && task_is_reaped(prev))
        task_schedule_reap();
    task_mark_on_cpu(next, true);
}

extern bool task_initialized;

task_t *arch_get_current() {
    if (!task_initialized)
        return NULL;
    riscv64_cpu_local_t *local = riscv64_get_cpu_local();
    return local ? local->task_ptr : NULL;
}

void arch_set_current(task_t *current) {
    riscv64_cpu_local_set_current(current);
}

void arch_context_to_user_mode(arch_context_t *context, uint64_t entry,
                               uint64_t stack) {
    if (!context->fpu_ctx) {
        context->fpu_ctx = alloc_frames_bytes(sizeof(fpu_context_t));
    }

    context->ctx = (struct pt_regs *)current_task->kernel_stack - 1;
    memset(context->ctx, 0, sizeof(struct pt_regs));
    context->ra = (uint64_t)riscv64_trap_return;
    context->ctx->sepc = entry;
    context->ctx->sp = stack;
    context->ctx->syscallno = NO_SYSCALL;
    context->ctx->sstatus = riscv64_sstatus_for_user();
    context->sp = (uint64_t)context->ctx;
    context->satp = riscv64_make_satp(current_task->mm->page_table_addr);
    context->kernel_sstatus = riscv64_kernel_sstatus_default();
    riscv64_fpu_state_init(context->fpu_ctx);
}

void arch_to_user_mode(arch_context_t *context, uint64_t entry,
                       uint64_t stack) {
    arch_disable_interrupt();

    arch_context_to_user_mode(context, entry, stack);
    riscv64_set_page_table_root(current_task->mm->page_table_addr);
    riscv64_fpu_restore(context->fpu_ctx);

    asm volatile("mv sp, %0\n\t"
                 "jr %1\n\t" ::"r"(context->ctx),
                 "r"(context->ra)
                 : "memory");
}

bool arch_check_elf(const Elf64_Ehdr *ehdr) {
    if (memcmp((void *)ehdr->e_ident,
               "\x7F"
               "ELF",
               4) != 0) {
        return false;
    }

    if (ehdr->e_ident[4] != 2 || ehdr->e_machine != 0xF3) {
        printk("Unsupported ELF format\n");
        return false;
    }

    return true;
}
