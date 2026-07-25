#pragma once

#include <libs/klibc.h>

enum {
    X64_REG_R8 = 0,
    X64_REG_R9,
    X64_REG_R10,
    X64_REG_R11,
    X64_REG_R12,
    X64_REG_R13,
    X64_REG_R14,
    X64_REG_R15,
    X64_REG_RDI,
    X64_REG_RSI,
    X64_REG_RBP,
    X64_REG_RBX,
    X64_REG_RDX,
    X64_REG_RAX,
    X64_REG_RCX,
    X64_REG_RSP,
    X64_REG_RIP,
    X64_REG_EFL,
    X64_REG_CSGSFS,
    X64_REG_ERR,
    X64_REG_TRAPNO,
    X64_REG_OLDMASK,
    X64_REG_CR2,
};

static inline bool signal_x64_is_syscall_context(const struct pt_regs *regs) {
    return ((regs->cs & 0x3) == 0x3) && regs->rip == regs->rcx &&
           regs->rflags == regs->r11;
}

static inline void
signal_x64_prepare_syscall_result(struct pt_regs *saved,
                                  const sigaction_t *action) {
    if (!signal_x64_is_syscall_context(saved)) {
        return;
    }

    int64_t retval = (int64_t)saved->rax;
    if (retval >= 0) {
        return;
    }

    switch (-retval) {
    case ERESTARTNOHAND:
    case ERESTART_RESTARTBLOCK:
        saved->rax = (uint64_t)-EINTR;
        break;
    case ERESTARTSYS:
        if ((action->sa_flags & SA_RESTART) == 0) {
            saved->rax = (uint64_t)-EINTR;
            break;
        }
        __attribute__((fallthrough));
    case ERESTARTNOINTR:
        saved->rax = saved->orig_rax;
        if (saved->rip >= SIGNAL_X64_SYSCALL_INS_LEN)
            saved->rip -= SIGNAL_X64_SYSCALL_INS_LEN;
        break;
    default:
        return;
    }
}

typedef struct signal_x64_frame_layout {
    uint64_t frame_rsp;
    uint64_t ucontext_addr;
    uint64_t siginfo_addr;
    uint64_t fpstate_addr;
    uint64_t fpstate_bytes;
    uint64_t trampoline_addr;
    uint64_t trampoline_bytes;
} signal_x64_frame_layout_t;

static inline uint64_t signal_x64_align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static uint64_t signal_x64_saved_fpstate_bytes(void) {
    uint64_t bytes = x64_fpu_state_size();

    if (bytes < sizeof(fpu_context_t))
        bytes = sizeof(fpu_context_t);

    return bytes;
}

static uint64_t signal_x64_user_fpstate_bytes(void) {
    uint64_t bytes = signal_x64_saved_fpstate_bytes();

    if (x64_fpu_xsave_enabled())
        bytes += X64_FP_XSTATE_MAGIC2_SIZE;

    return bytes;
}

static void signal_x64_build_frame_layout(uint64_t user_rsp,
                                          uint64_t fpstate_bytes,
                                          uint64_t trampoline_bytes,
                                          signal_x64_frame_layout_t *layout) {
    if (!layout)
        return;

    memset(layout, 0, sizeof(*layout));

    uint64_t redzone_top = user_rsp - 128;
    uint64_t fixed_bytes = sizeof(void *) + sizeof(ucontext_t) +
                           sizeof(siginfo_t) + trampoline_bytes;
    uint64_t reserve = fixed_bytes + (X64_FPU_FRAME_ALIGN - 1) + fpstate_bytes;

    if (redzone_top <= reserve)
        return;

    uint64_t frame_rsp = redzone_top - reserve;
    frame_rsp =
        (frame_rsp & ~(uint64_t)(X64_FPU_FRAME_ALIGN - 1)) + sizeof(void *);
    if (frame_rsp > redzone_top - reserve)
        frame_rsp -= X64_FPU_FRAME_ALIGN;

    layout->frame_rsp = frame_rsp;
    layout->ucontext_addr = frame_rsp + sizeof(void *);
    layout->siginfo_addr = layout->ucontext_addr + sizeof(ucontext_t);
    layout->fpstate_addr = signal_x64_align_up(
        layout->siginfo_addr + sizeof(siginfo_t), X64_FPU_FRAME_ALIGN);
    layout->fpstate_bytes = fpstate_bytes;
    layout->trampoline_addr = layout->fpstate_addr + fpstate_bytes;
    layout->trampoline_bytes = trampoline_bytes;
}

static bool signal_x64_copy_fpstate_to_user(task_t *task,
                                            uint64_t user_fpstate_addr,
                                            uint64_t fpstate_bytes) {
    if (!task || !task->arch_context || !task->arch_context->fpu_ctx)
        return false;

    uint64_t saved_bytes = signal_x64_saved_fpstate_bytes();
    void *image = malloc(fpstate_bytes);
    if (!image)
        return false;

    memset(image, 0, fpstate_bytes);
    memcpy(image, task->arch_context->fpu_ctx, saved_bytes);

    fpu_context_t *fp = (fpu_context_t *)image;
    if (x64_fpu_xsave_enabled()) {
        uint64_t xfeatures = X64_XSTATE_X87 | X64_XSTATE_SSE;
        if (saved_bytes > X64_XSAVE_HDR_OFFSET) {
            const x64_xsave_header_t *hdr =
                (const x64_xsave_header_t *)((const uint8_t *)image +
                                             X64_XSAVE_HDR_OFFSET);
            if (hdr->xstate_bv)
                xfeatures = hdr->xstate_bv;
        }

        fp->sw_reserved.magic1 = X64_FP_XSTATE_MAGIC1;
        fp->sw_reserved.extended_size = (uint32_t)fpstate_bytes;
        fp->sw_reserved.xstate_bv = xfeatures;
        fp->sw_reserved.xstate_size = (uint32_t)saved_bytes;
        *(uint32_t *)((uint8_t *)image + fpstate_bytes -
                      X64_FP_XSTATE_MAGIC2_SIZE) = X64_FP_XSTATE_MAGIC2;
    } else {
        memset(&fp->sw_reserved, 0, sizeof(fp->sw_reserved));
    }

    bool ok = !copy_to_user((void *)user_fpstate_addr, image, fpstate_bytes);
    free(image);
    return ok;
}

static uint64_t signal_x64_restore_fpstate_bytes(const ucontext_t *ucontext) {
    if (!ucontext)
        return sizeof(fpu_context_t);

    if ((ucontext->uc_flags & X64_UC_FP_XSTATE) && x64_fpu_xsave_enabled())
        return signal_x64_saved_fpstate_bytes();

    return sizeof(fpu_context_t);
}

static bool signal_x64_fpstate_image_valid(const fpu_context_t *fpu_ctx,
                                           uint64_t copied_bytes) {
    if (!x64_fpu_xsave_enabled())
        return true;
    if (!fpu_ctx || copied_bytes <= X64_XSAVE_HDR_OFFSET)
        return true;

    const x64_xsave_header_t *hdr =
        (const x64_xsave_header_t *)((const uint8_t *)fpu_ctx +
                                     X64_XSAVE_HDR_OFFSET);
    if (hdr->xstate_bv & ~x64_fpu_xsave_supported_mask())
        return false;

    for (size_t i = 0; i < sizeof(hdr->reserved1) / sizeof(hdr->reserved1[0]);
         i++) {
        if (hdr->reserved1[i] != 0)
            return false;
    }
    for (size_t i = 0; i < sizeof(hdr->reserved2) / sizeof(hdr->reserved2[0]);
         i++) {
        if (hdr->reserved2[i] != 0)
            return false;
    }

    return true;
}

static inline void signal_x64_fill_ucontext(ucontext_t *ucontext,
                                            const struct pt_regs *regs,
                                            sigset_t blocked_mask,
                                            const stack_t *altstack,
                                            const fpu_context_t *fpu_ctx) {
    memset(ucontext, 0, sizeof(*ucontext));

    ucontext->uc_flags =
        SIGNAL_X64_UC_SIGCONTEXT_SS | SIGNAL_X64_UC_STRICT_RESTORE_SS;
    if (x64_fpu_xsave_enabled())
        ucontext->uc_flags |= X64_UC_FP_XSTATE;
    ucontext->uc_link = NULL;
    if (altstack)
        ucontext->uc_stack = *altstack;
    if (fpu_ctx) {
        memcpy(&ucontext->__fpregs_mem, fpu_ctx,
               sizeof(ucontext->__fpregs_mem));
    }
    ucontext->uc_mcontext.fpregs = NULL;

    ucontext->uc_mcontext.gregs[X64_REG_R8] = regs->r8;
    ucontext->uc_mcontext.gregs[X64_REG_R9] = regs->r9;
    ucontext->uc_mcontext.gregs[X64_REG_R10] = regs->r10;
    ucontext->uc_mcontext.gregs[X64_REG_R11] = regs->r11;
    ucontext->uc_mcontext.gregs[X64_REG_R12] = regs->r12;
    ucontext->uc_mcontext.gregs[X64_REG_R13] = regs->r13;
    ucontext->uc_mcontext.gregs[X64_REG_R14] = regs->r14;
    ucontext->uc_mcontext.gregs[X64_REG_R15] = regs->r15;
    ucontext->uc_mcontext.gregs[X64_REG_RDI] = regs->rdi;
    ucontext->uc_mcontext.gregs[X64_REG_RSI] = regs->rsi;
    ucontext->uc_mcontext.gregs[X64_REG_RBP] = regs->rbp;
    ucontext->uc_mcontext.gregs[X64_REG_RBX] = regs->rbx;
    ucontext->uc_mcontext.gregs[X64_REG_RDX] = regs->rdx;
    ucontext->uc_mcontext.gregs[X64_REG_RAX] = regs->rax;
    ucontext->uc_mcontext.gregs[X64_REG_RCX] = regs->rcx;
    ucontext->uc_mcontext.gregs[X64_REG_RSP] = regs->rsp;
    ucontext->uc_mcontext.gregs[X64_REG_RIP] = regs->rip;
    ucontext->uc_mcontext.gregs[X64_REG_EFL] = regs->rflags;
    ucontext->uc_mcontext.gregs[X64_REG_CSGSFS] =
        (regs->cs & 0xFFFFULL) | ((uint64_t)0 << 16) | ((uint64_t)0 << 32) |
        ((regs->ss & 0xFFFFULL) << 48);
    ucontext->uc_mcontext.gregs[X64_REG_ERR] = regs->errcode;
    ucontext->uc_mcontext.gregs[X64_REG_TRAPNO] = 0;
    ucontext->uc_mcontext.gregs[X64_REG_OLDMASK] = 0;
    ucontext->uc_mcontext.gregs[X64_REG_CR2] = 0;
    memset(&ucontext->uc_sigmask, 0, sizeof(ucontext->uc_sigmask));
    ucontext->uc_sigmask.__bits[0] = sigset_kernel_to_user(blocked_mask);
}

static inline void signal_x64_restore_ptregs(struct pt_regs *regs,
                                             const ucontext_t *ucontext) {
    regs->r8 = ucontext->uc_mcontext.gregs[X64_REG_R8];
    regs->r9 = ucontext->uc_mcontext.gregs[X64_REG_R9];
    regs->r10 = ucontext->uc_mcontext.gregs[X64_REG_R10];
    regs->r11 = ucontext->uc_mcontext.gregs[X64_REG_R11];
    regs->r12 = ucontext->uc_mcontext.gregs[X64_REG_R12];
    regs->r13 = ucontext->uc_mcontext.gregs[X64_REG_R13];
    regs->r14 = ucontext->uc_mcontext.gregs[X64_REG_R14];
    regs->r15 = ucontext->uc_mcontext.gregs[X64_REG_R15];
    regs->rdi = ucontext->uc_mcontext.gregs[X64_REG_RDI];
    regs->rsi = ucontext->uc_mcontext.gregs[X64_REG_RSI];
    regs->rbp = ucontext->uc_mcontext.gregs[X64_REG_RBP];
    regs->rbx = ucontext->uc_mcontext.gregs[X64_REG_RBX];
    regs->rdx = ucontext->uc_mcontext.gregs[X64_REG_RDX];
    regs->rax = ucontext->uc_mcontext.gregs[X64_REG_RAX];
    regs->rcx = ucontext->uc_mcontext.gregs[X64_REG_RCX];
    regs->rsp = ucontext->uc_mcontext.gregs[X64_REG_RSP];
    regs->rip = ucontext->uc_mcontext.gregs[X64_REG_RIP];
    regs->rflags = ucontext->uc_mcontext.gregs[X64_REG_EFL];
    uint64_t csgsfs = ucontext->uc_mcontext.gregs[X64_REG_CSGSFS];
    regs->cs = csgsfs & 0xFFFFULL;
    regs->ss = (csgsfs >> 48) & 0xFFFFULL;
    regs->errcode = ucontext->uc_mcontext.gregs[X64_REG_ERR];
    regs->func = 0;

    if ((regs->cs & 0x3) != 0x3) {
        regs->cs = SELECTOR_USER_CS;
    }
    if ((regs->ss & 0x3) != 0x3) {
        regs->ss = SELECTOR_USER_DS;
    }
}

static bool signal_x64_setup_frame(task_t *task, struct pt_regs *regs, int sig,
                                   const sigaction_t *action,
                                   const siginfo_t *info,
                                   sigset_t restore_mask) {
    if (!task || !regs || !action || action->sa_handler == SIG_DFL ||
        action->sa_handler == SIG_IGN) {
        return false;
    }

    struct pt_regs saved = *regs;
    signal_x64_prepare_syscall_result(&saved, action);

    stack_t frame_altstack;
    signal_altstack_format_old(&frame_altstack, &task->signal->altstack,
                               saved.rsp);

    uint64_t fpstate_bytes = signal_x64_user_fpstate_bytes();
    uint64_t trampoline_bytes = 0;
    bool use_kernel_restorer =
        !(action->sa_flags & SA_RESTORER) || !action->sa_restorer;
    if (use_kernel_restorer && !signal_ensure_user_trampoline(task)) {
        return false;
    }

    signal_x64_frame_layout_t layout;
    signal_x64_build_frame_layout(saved.rsp, fpstate_bytes, trampoline_bytes,
                                  &layout);
    bool use_altstack =
        (action->sa_flags & SA_ONSTACK) &&
        signal_altstack_config_enabled(&task->signal->altstack) &&
        !signal_altstack_contains_sp(&task->signal->altstack, saved.rsp);
    if (use_altstack) {
        uint64_t alt_top = signal_stack_base(&task->signal->altstack) +
                           task->signal->altstack.ss_size;
        signal_x64_build_frame_layout(alt_top, fpstate_bytes, trampoline_bytes,
                                      &layout);
        if (task->signal->altstack.ss_flags & SS_AUTODISARM)
            signal_altstack_disable(&task->signal->altstack);
    }
    if (!layout.frame_rsp) {
        return false;
    }

    uint64_t ucontext_addr = layout.ucontext_addr;
    uint64_t siginfo_addr = layout.siginfo_addr;

    ucontext_t frame_ucontext;
    x64_fpu_save(task->arch_context->fpu_ctx);
    signal_x64_fill_ucontext(&frame_ucontext, &saved, restore_mask,
                             &frame_altstack, task->arch_context->fpu_ctx);
    if (sig == SIGSEGV && info) {
        frame_ucontext.uc_mcontext.gregs[X64_REG_TRAPNO] =
            SIGNAL_X64_TRAPNO_PAGE_FAULT;
        frame_ucontext.uc_mcontext.gregs[X64_REG_CR2] =
            (uint64_t)info->_sifields._sigfault._addr;
    }
    frame_ucontext.uc_mcontext.fpregs = (fpu_context_t *)layout.fpstate_addr;

    void *frame_restorer = (void *)action->sa_restorer;
    if (use_kernel_restorer)
        frame_restorer = (void *)task_mm_signal_trampoline_start(task->mm);
    if (!signal_x64_copy_fpstate_to_user(task, layout.fpstate_addr,
                                         layout.fpstate_bytes)) {
        return false;
    }

    if (copy_to_user((void *)siginfo_addr, info, sizeof(*info)) ||
        copy_to_user((void *)ucontext_addr, &frame_ucontext,
                     sizeof(frame_ucontext)) ||
        copy_to_user((void *)layout.frame_rsp, &frame_restorer,
                     sizeof(frame_restorer))) {
        return false;
    }

    struct pt_regs delivery;
    memset(&delivery, 0, sizeof(delivery));
    delivery.rip = (uint64_t)action->sa_handler;
    delivery.rdi = sig;
    if (action->sa_flags & SA_SIGINFO) {
        delivery.rsi = siginfo_addr;
        delivery.rdx = ucontext_addr;
    }
    delivery.cs = SELECTOR_USER_CS;
    delivery.ss = SELECTOR_USER_DS;
    delivery.rflags = saved.rflags | (1ULL << 9);
    delivery.rsp = layout.frame_rsp;
    delivery.rbp = layout.frame_rsp;

    /* Never expose a partially constructed return frame to the low-level
     * syscall/interrupt exit path. */
    *regs = delivery;

    return true;
}

static bool signal_arch_setup_frame(task_t *task, struct pt_regs *regs, int sig,
                                    const sigaction_t *action,
                                    const siginfo_t *info,
                                    sigset_t restore_mask) {
    return signal_x64_setup_frame(task, regs, sig, action, info, restore_mask);
}

static uint64_t signal_arch_sigreturn(struct pt_regs *regs) {
    arch_disable_interrupt();

    task_t *self = current_task;
    if (!self || !regs) {
        return (uint64_t)-EFAULT;
    }

    ucontext_t frame_ucontext;
    if (copy_from_user(&frame_ucontext, (void *)regs->rsp,
                       sizeof(frame_ucontext))) {
        task_exit(128 + SIGSEGV);
        return 0;
    }

    if (frame_ucontext.uc_mcontext.fpregs) {
        uint64_t fpstate_bytes =
            signal_x64_restore_fpstate_bytes(&frame_ucontext);

        x64_fpu_state_init(self->arch_context->fpu_ctx);
        if (copy_from_user(self->arch_context->fpu_ctx,
                           frame_ucontext.uc_mcontext.fpregs, fpstate_bytes) ||
            !signal_x64_fpstate_image_valid(self->arch_context->fpu_ctx,
                                            fpstate_bytes)) {
            task_exit(128 + SIGSEGV);
            return 0;
        }
    }
    x64_fpu_restore(self->arch_context->fpu_ctx);

    stack_t restore_altstack = frame_ucontext.uc_stack;
    restore_altstack.ss_flags &= ~SS_ONSTACK;
    if (signal_altstack_validate_new(&restore_altstack) < 0) {
        task_exit(128 + SIGSEGV);
        return 0;
    }

    signal_x64_restore_ptregs(regs, &frame_ucontext);

    spin_lock(&self->signal->sighand->siglock);
    self->signal->blocked =
        sigset_user_to_kernel(frame_ucontext.uc_sigmask.__bits[0]);
    signal_altstack_store(&self->signal->altstack, &restore_altstack);
    spin_unlock(&self->signal->sighand->siglock);

    regs->rflags |= (1ULL << 9);

    return regs->rax;
}
