#pragma once

#include <arch/riscv64/irq/ptrace.h>
#include <libs/elf.h>
#include <mm/mm.h>
#include <task/task_struct.h>

struct task;
typedef struct task task_t;

typedef struct arch_context {
    uint64_t ra;
    uint64_t sp;
    uint64_t s0;
    uint64_t s1;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t satp;
    uint64_t kernel_sstatus;
    struct pt_regs *ctx;
    struct fpu_context *fpu_ctx;
} arch_context_t;

typedef struct fpu_context {
    uint64_t f[32];
    uint32_t fcsr;
    uint32_t reserved;
} __attribute__((aligned(16))) fpu_context_t;

void riscv64_fpu_state_init(fpu_context_t *fpu_ctx);
void riscv64_fpu_save(fpu_context_t *fpu_ctx);
void riscv64_fpu_restore(fpu_context_t *fpu_ctx);

static inline uint64_t arch_regs_get_user_sp(const struct pt_regs *regs) {
    return regs->sp;
}

static inline void arch_regs_set_user_sp(struct pt_regs *regs, uint64_t sp) {
    regs->sp = sp;
}

static inline void arch_context_set_tls(arch_context_t *context, uint64_t tls) {
    context->ctx->tp = tls;
}

#define switch_mm(prev, next)                                                  \
    do {                                                                       \
        if ((prev)->mm != (next)->mm) {                                        \
            riscv64_set_page_table_root((next)->mm->page_table_addr);          \
        }                                                                      \
    } while (0)

#define switch_to(prev, next)                                                  \
    do {                                                                       \
        asm volatile("mv t3, %0\n\t"                                           \
                     "mv t4, %1\n\t"                                           \
                     "mv t5, %2\n\t"                                           \
                     "mv t6, %3\n\t"                                           \
                     "mv t2, %4\n\t"                                           \
                     "addi sp, sp, -16\n\t"                                    \
                     "sd t2, 0(sp)\n\t"                                        \
                     "la t0, 1f\n\t"                                           \
                     "sd t0, 0(t3)\n\t"                                        \
                     "sd sp, 8(t3)\n\t"                                        \
                     "sd s0, 16(t3)\n\t"                                       \
                     "sd s1, 24(t3)\n\t"                                       \
                     "sd s2, 32(t3)\n\t"                                       \
                     "sd s3, 40(t3)\n\t"                                       \
                     "sd s4, 48(t3)\n\t"                                       \
                     "sd s5, 56(t3)\n\t"                                       \
                     "sd s6, 64(t3)\n\t"                                       \
                     "sd s7, 72(t3)\n\t"                                       \
                     "sd s8, 80(t3)\n\t"                                       \
                     "sd s9, 88(t3)\n\t"                                       \
                     "sd s10, 96(t3)\n\t"                                      \
                     "sd s11, 104(t3)\n\t"                                     \
                     "ld t0, 0(t4)\n\t"                                        \
                     "ld sp, 8(t4)\n\t"                                        \
                     "ld s0, 16(t4)\n\t"                                       \
                     "ld s1, 24(t4)\n\t"                                       \
                     "ld s2, 32(t4)\n\t"                                       \
                     "ld s3, 40(t4)\n\t"                                       \
                     "ld s4, 48(t4)\n\t"                                       \
                     "ld s5, 56(t4)\n\t"                                       \
                     "ld s6, 64(t4)\n\t"                                       \
                     "ld s7, 72(t4)\n\t"                                       \
                     "ld s8, 80(t4)\n\t"                                       \
                     "ld s9, 88(t4)\n\t"                                       \
                     "ld s10, 96(t4)\n\t"                                      \
                     "ld s11, 104(t4)\n\t"                                     \
                     "mv ra, t0\n\t"                                           \
                     "mv a0, t5\n\t"                                           \
                     "mv a1, t6\n\t"                                           \
                     "la t1, __switch_to\n\t"                                  \
                     "jr t1\n\t"                                               \
                     "1:\n\t"                                                  \
                     "ld t0, 0(sp)\n\t"                                        \
                     "addi sp, sp, 16\n\t"                                     \
                     "csrw sstatus, t0\n\t"                                    \
                     :                                                         \
                     : "r"((prev)->arch_context), "r"((next)->arch_context),   \
                       "r"(prev), "r"(next),                                   \
                       "r"((prev)->arch_context->kernel_sstatus)               \
                     : "ra", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",   \
                       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");    \
    } while (0)

void arch_context_init(arch_context_t *context, uint64_t page_table_addr,
                       uint64_t entry, uint64_t stack, bool user_mode,
                       uint64_t initial_arg);
void arch_context_copy(arch_context_t *dst, arch_context_t *src, uint64_t stack,
                       uint64_t clone_flags);
void arch_context_free(arch_context_t *context);
void arch_context_save_interrupt_state(arch_context_t *context, bool enabled);
void __switch_to(task_t *prev, task_t *next);
task_t *arch_get_current();
void arch_set_current(task_t *current);

void arch_context_to_user_mode(arch_context_t *context, uint64_t entry,
                               uint64_t stack);
void arch_to_user_mode(arch_context_t *context, uint64_t entry, uint64_t stack);

bool arch_check_elf(const Elf64_Ehdr *elf);
