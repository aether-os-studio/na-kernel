#include <arch/arch.h>
#include <arch/signal.h>
#include <task/ptrace.h>
#include <task/signal.h>

#include <fs/vfs/vfs.h>
#include <task/task.h>
#include <mm/mm.h>
#include <mm/page.h>
#include <irq/irq_manager.h>

#include <init/callbacks.h>

#define SIGNAL_MIN_SIGSET_SIZE sizeof(uint32_t)
#define SIGNAL_MAX_SIGSET_SIZE sizeof(sigset_t)
#define SIGNAL_MAX_MASKED_SIG ((int)(sizeof(sigset_t) * 8 - 1))

#define ERESTARTSYS 512
#define ERESTARTNOINTR 513
#define ERESTARTNOHAND 514
#define ERESTART_RESTARTBLOCK 516

signal_internal_t signal_internal_decisions[MAXSIG] = {0};

extern int signalfdfs_id;

bool signal_sig_in_range(int sig) { return sig >= MINSIG && sig < MAXSIG; }

bool signal_sig_maskable(int sig) {
    return sig >= MINSIG && sig <= SIGNAL_MAX_MASKED_SIG;
}

bool signal_sigset_size_valid(size_t sigsetsize) {
    return sigsetsize >= SIGNAL_MIN_SIGSET_SIZE &&
           sigsetsize <= SIGNAL_MAX_SIGSET_SIZE;
}

sigset_t signal_sigbit(int sig) { return (sigset_t)1ULL << (uint64_t)sig; }

sigset_t sigset_user_to_kernel(sigset_t user_mask) {
    sigset_t k = user_mask << 1;
    if (signal_sig_maskable(SIGKILL)) {
        k &= ~signal_sigbit(SIGKILL);
    }
    if (signal_sig_maskable(SIGSTOP)) {
        k &= ~signal_sigbit(SIGSTOP);
    }
    return k;
}

sigset_t sigset_kernel_to_user(sigset_t kernel_mask) {
    return kernel_mask >> 1;
}

bool signal_sigset_has(sigset_t set, int sig) {
    if (!signal_sig_maskable(sig)) {
        return false;
    }
    return (set & signal_sigbit(sig)) != 0;
}

void signal_altstack_disable(stack_t *stack) {
    if (!stack)
        return;

    stack->ss_sp = NULL;
    stack->ss_size = 0;
    stack->ss_flags = SS_DISABLE;
}

uint64_t signal_stack_base(const stack_t *stack) {
    return stack ? (uint64_t)stack->ss_sp : 0;
}

bool signal_altstack_config_enabled(const stack_t *stack) {
    return stack && (stack->ss_flags & SS_DISABLE) == 0 && stack->ss_size > 0;
}

bool signal_altstack_contains_sp(const stack_t *stack, uint64_t sp) {
    if (!signal_altstack_config_enabled(stack))
        return false;

    uint64_t base = signal_stack_base(stack);
    if (base > UINT64_MAX - stack->ss_size)
        return false;

    uint64_t end = base + stack->ss_size;
    return sp >= base && sp < end;
}

int signal_altstack_status_flags(const stack_t *stack, uint64_t sp) {
    if (!stack || (stack->ss_flags & SS_DISABLE) != 0 || stack->ss_size == 0)
        return SS_DISABLE;

    int flags = stack->ss_flags & SS_AUTODISARM;
    if (signal_altstack_contains_sp(stack, sp))
        flags |= SS_ONSTACK;
    return flags;
}

void signal_altstack_format_old(stack_t *dst, const stack_t *src, uint64_t sp) {
    if (!dst)
        return;

    if (!src) {
        signal_altstack_disable(dst);
        return;
    }

    *dst = *src;
    dst->ss_flags = signal_altstack_status_flags(src, sp);
}

int signal_altstack_validate_new(const stack_t *stack) {
    if (!stack)
        return -EINVAL;

    if (stack->ss_flags & SS_DISABLE)
        return 0;

    int allowed_flags = SS_ONSTACK | SS_AUTODISARM;
    if (stack->ss_flags & ~allowed_flags)
        return -EINVAL;

    if (stack->ss_size < MINSIGSTKSZ)
        return -ENOMEM;

    return 0;
}

void signal_altstack_store(stack_t *dst, const stack_t *src) {
    if (!dst || !src)
        return;

    if (src->ss_flags & SS_DISABLE) {
        signal_altstack_disable(dst);
        return;
    }

    *dst = *src;
    dst->ss_flags &= ~SS_ONSTACK;
    dst->ss_flags &= SS_AUTODISARM;
}

static inline uint64_t signal_current_user_sp(task_t *task) {
    if (!task)
        return 0;

    struct pt_regs *regs = (struct pt_regs *)task->syscall_stack - 1;
    return signal_arch_user_sp_from_regs(regs);
}

static inline bool signal_is_blocked(sigset_t blocked, int sig) {
    if (sig == SIGKILL || sig == SIGSTOP) {
        return false;
    }
    return signal_sigset_has(blocked, sig);
}

static inline sigset_t signal_pending_mask_locked(task_t *task) {
    return task->signal->signal;
}

static inline void signal_fill_kernel_siginfo(siginfo_t *info, int sig,
                                              int code) {
    memset(info, 0, sizeof(*info));
    info->si_signo = sig;
    info->si_errno = 0;
    info->si_code = code;
}

static inline bool signal_action_ignored(int sig, const sigaction_t *action) {
    if (sig == SIGKILL || sig == SIGSTOP) {
        return false;
    }
    if (action->sa_handler == SIG_IGN) {
        return true;
    }
    if (action->sa_handler == SIG_DFL &&
        signal_internal_decisions[sig] == SIGNAL_INTERNAL_IGN) {
        return true;
    }
    return false;
}

static bool signal_should_wake_task_locked(task_t *task, int sig) {
    if (!task || !task->signal || !task->signal->sighand ||
        !signal_sig_in_range(sig)) {
        return false;
    }

    if (signal_is_blocked(task->signal->blocked, sig)) {
        return false;
    }

    sigaction_t *action = &task->signal->sighand->actions[sig];
    return !signal_action_ignored(sig, action);
}

static bool signal_has_signalfd_waiter(task_t *task, int sig) {
    fd_info_t *fd_info;
    bool matched = false;

    if (!task || !signal_sig_in_range(sig))
        return false;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return false;

    with_fd_info_lock(
        fd_info, ({
            struct llist_header *node = fd_info->signalfd_refs.next;
            while (node != &fd_info->signalfd_refs) {
                signalfd_ref_t *ref = list_entry(node, signalfd_ref_t, node);

                node = node->next;
                if (ref->file && signalfd_mask_contains(ref->file, sig)) {
                    matched = true;
                    break;
                }
            }
        }));

    task_fd_info_put(fd_info, task);
    return matched;
}

static void signal_notify_signalfd_waiters(task_t *task, int sig) {
    fd_info_t *fd_info;

    if (!task || !signal_sig_in_range(sig))
        return;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return;

    with_fd_info_lock(
        fd_info, ({
            struct llist_header *node = fd_info->signalfd_refs.next;
            while (node != &fd_info->signalfd_refs) {
                signalfd_ref_t *ref = list_entry(node, signalfd_ref_t, node);

                node = node->next;
                if (ref->file && signalfd_mask_contains(ref->file, sig))
                    vfs_poll_notify_file(ref->file, EPOLLIN | EPOLLRDNORM);
            }
        }));

    task_fd_info_put(fd_info, task);
}

static bool signal_should_wake_task(task_t *task, int sig) {
    if (!task || !task->signal || !task->signal->sighand ||
        !signal_sig_in_range(sig)) {
        return false;
    }

    bool should_wake;
    spin_lock(&task->signal->sighand->siglock);
    should_wake = signal_should_wake_task_locked(task, sig);
    spin_unlock(&task->signal->sighand->siglock);

    return should_wake;
}

static inline void signal_wake_interruptible_task(task_t *task, int sig) {
    if (!task || !signal_sig_in_range(sig))
        return;

    bool signalfd_waiter = signal_has_signalfd_waiter(task, sig);
    if (signalfd_waiter)
        signal_notify_signalfd_waiters(task, sig);
    if (!signal_should_wake_task(task, sig) && !signalfd_waiter)
        return;

    if (task->state == TASK_BLOCKING) {
        task_unblock(task, 128 + sig);
    } else if (signalfd_waiter && task->cpu_id < cpu_count &&
               task->cpu_id != current_cpu_id) {
        irq_trigger_sched_ipi(task->cpu_id);
    }
}

static inline int signal_pick_from_set_locked(task_t *task, sigset_t set) {
    sigset_t pending = signal_pending_mask_locked(task) & set;
    for (int sig = MINSIG; sig <= SIGNAL_MAX_MASKED_SIG; sig++) {
        if (pending & signal_sigbit(sig)) {
            return sig;
        }
    }
    return 0;
}

static inline int signal_pick_deliverable_locked(task_t *task) {
    sigset_t pending = signal_pending_mask_locked(task);
    sigset_t blocked = task->signal->blocked;
    for (int sig = MINSIG; sig <= SIGNAL_MAX_MASKED_SIG; sig++) {
        if (!(pending & signal_sigbit(sig))) {
            continue;
        }
        if (!signal_is_blocked(blocked, sig)) {
            return sig;
        }
    }

    return 0;
}

static inline void signal_take_pending_locked(task_t *task, int sig,
                                              siginfo_t *info) {
    if (signal_sig_maskable(sig)) {
        sigset_t bit = signal_sigbit(sig);
        task->signal->signal &= ~bit;

        if (task->signal->pending_signal.info_mask & bit) {
            memcpy(info, &task->signal->pending_signal.info[sig],
                   sizeof(*info));
            task->signal->pending_signal.info_mask &= ~bit;
            memset(&task->signal->pending_signal.info[sig], 0,
                   sizeof(task->signal->pending_signal.info[sig]));
            return;
        }
    }

    signal_fill_kernel_siginfo(info, sig, SI_KERNEL);
}

static inline bool
signal_has_deliverable_outside_set_locked(task_t *task, sigset_t wait_set) {
    sigset_t pending = signal_pending_mask_locked(task);
    sigset_t blocked = task->signal->blocked;

    for (int sig = MINSIG; sig <= SIGNAL_MAX_MASKED_SIG; sig++) {
        if (!(pending & signal_sigbit(sig))) {
            continue;
        }
        if (signal_is_blocked(blocked, sig)) {
            continue;
        }
        if (wait_set & signal_sigbit(sig)) {
            continue;
        }
        sigaction_t *action = &task->signal->sighand->actions[sig];
        if (signal_action_ignored(sig, action)) {
            continue;
        }
        return true;
    }

    return false;
}

static bool signal_sync_user_instruction_memory(uint64_t user_addr,
                                                size_t code_size) {
    if (!user_addr || code_size == 0)
        return false;
    if (check_user_overflow(user_addr, code_size))
        return false;

    uint64_t *pgdir = get_current_page_dir(true);
    uint64_t uaddr = user_addr;
    size_t remain = code_size;

    while (remain > 0) {
        uint64_t pa = user_translate_or_fault(pgdir, uaddr, false);
        if (!pa)
            return false;

        size_t chunk = MIN(remain, PAGE_SIZE - (uaddr & (PAGE_SIZE - 1)));
        sync_instruction_memory_range(user_virt_from_paddr(pa), chunk);

        uaddr += chunk;
        remain -= chunk;
    }

    return true;
}

static bool signal_ensure_user_trampoline(task_t *task) {
    if (!task || !task->mm)
        return false;

    size_t code_size = 0;
    const uint8_t *code = signal_arch_rt_sigreturn_trampoline(&code_size);
    if (!code || code_size == 0 || code_size > PAGE_SIZE)
        return false;

    vma_manager_t *mgr = &task->mm->task_vma_mgr;
    uint64_t trampoline_start = task_mm_signal_trampoline_start(task->mm);
    uint64_t trampoline_end = task_mm_signal_trampoline_end(task->mm);
    spin_lock(&mgr->lock);
    vma_t *vma = vma_find(mgr, trampoline_start);
    if (vma) {
        bool ok =
            vma->vm_start == trampoline_start &&
            vma->vm_end == trampoline_end &&
            (vma->vm_flags & (VMA_READ | VMA_EXEC)) == (VMA_READ | VMA_EXEC) &&
            !(vma->vm_flags & VMA_WRITE);
        spin_unlock(&mgr->lock);
        return ok;
    }

    if (vma_find_intersection(mgr, trampoline_start, trampoline_end)) {
        spin_unlock(&mgr->lock);
        return false;
    }
    spin_unlock(&mgr->lock);

    uint64_t page_paddr = alloc_frames(1);
    if (!page_paddr)
        return false;

    void *page = (void *)phys_to_virt(page_paddr);
    memset(page, 0, PAGE_SIZE);
    memcpy(page, code, code_size);
    sync_instruction_memory_range(page, code_size);

    vma_t *new_vma = vma_alloc();
    if (!new_vma) {
        address_release(page_paddr);
        return false;
    }

    new_vma->vm_start = trampoline_start;
    new_vma->vm_end = trampoline_end;
    new_vma->vm_flags = VMA_ANON | VMA_READ | VMA_EXEC;
    new_vma->vm_type = VMA_TYPE_ANON;
    new_vma->vm_name = strdup("[sigreturn]");
    if (!new_vma->vm_name) {
        vma_free(new_vma);
        address_release(page_paddr);
        return false;
    }

    bool mapped = false;
    bool inserted = false;
    bool raced_ok = false;

    spin_lock(&mgr->lock);
    vma = vma_find(mgr, trampoline_start);
    if (vma) {
        raced_ok =
            vma->vm_start == trampoline_start &&
            vma->vm_end == trampoline_end &&
            (vma->vm_flags & (VMA_READ | VMA_EXEC)) == (VMA_READ | VMA_EXEC) &&
            !(vma->vm_flags & VMA_WRITE);
        spin_unlock(&mgr->lock);
        vma_free(new_vma);
        address_release(page_paddr);
        return raced_ok;
    }

    if (vma_find_intersection(mgr, trampoline_start, trampoline_end)) {
        spin_unlock(&mgr->lock);
        vma_free(new_vma);
        address_release(page_paddr);
        return false;
    }
    spin_unlock(&mgr->lock);

    spin_lock(&task->mm->lock);
    uint64_t map_ret =
        map_page_range_mm(task->mm, trampoline_start, page_paddr, PAGE_SIZE,
                          PT_FLAG_U | PT_FLAG_R | PT_FLAG_X);
    spin_unlock(&task->mm->lock);
    mapped = map_ret == 0;

    if (mapped) {
        spin_lock(&mgr->lock);
        vma = vma_find(mgr, trampoline_start);
        if (vma) {
            inserted = vma->vm_start == trampoline_start &&
                       vma->vm_end == trampoline_end &&
                       (vma->vm_flags & (VMA_READ | VMA_EXEC)) ==
                           (VMA_READ | VMA_EXEC) &&
                       !(vma->vm_flags & VMA_WRITE);
        } else if (!vma_find_intersection(mgr, trampoline_start,
                                          trampoline_end) &&
                   vma_insert(mgr, new_vma) == 0) {
            inserted = true;
            new_vma = NULL;
        }
        spin_unlock(&mgr->lock);
    }

    if (!inserted && mapped) {
        unmap_page_range_mm_batched(task->mm, trampoline_start, PAGE_SIZE);
    }

    if (new_vma)
        vma_free(new_vma);
    address_release(page_paddr);
    if (!inserted)
        return false;

    return true;
}

static inline void signal_set_default(int sig, signal_internal_t action) {
    if (signal_sig_in_range(sig)) {
        signal_internal_decisions[sig] = action;
    }
}

#include <arch/signal_impl.h>

void signal_init() {
    for (int i = 0; i < MAXSIG; i++) {
        signal_internal_decisions[i] = SIGNAL_INTERNAL_TERM;
    }

    signal_set_default(SIGABRT, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGBUS, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGCHLD, SIGNAL_INTERNAL_IGN);
    signal_set_default(SIGCONT, SIGNAL_INTERNAL_CONT);
    signal_set_default(SIGFPE, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGILL, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGIOT, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGQUIT, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGSEGV, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGSTOP, SIGNAL_INTERNAL_STOP);
    signal_set_default(SIGTSTP, SIGNAL_INTERNAL_STOP);
    signal_set_default(SIGSYS, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGTRAP, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGTTIN, SIGNAL_INTERNAL_STOP);
    signal_set_default(SIGTTOU, SIGNAL_INTERNAL_STOP);
    signal_set_default(SIGUNUSED, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGURG, SIGNAL_INTERNAL_IGN);
    signal_set_default(SIGWINCH, SIGNAL_INTERNAL_IGN);
    signal_set_default(SIGXCPU, SIGNAL_INTERNAL_CORE);
    signal_set_default(SIGXFSZ, SIGNAL_INTERNAL_CORE);
}

void task_commit_signal(task_t *task, int sig, siginfo_t *info) {
    if (!task || !task->signal || task->is_kernel ||
        !signal_sig_in_range(sig) || task->state == TASK_DIED) {
        return;
    }

    siginfo_t kinfo;
    if (info) {
        memcpy(&kinfo, info, sizeof(kinfo));
    } else {
        signal_fill_kernel_siginfo(&kinfo, sig, SI_KERNEL);
    }
    kinfo.si_signo = sig;

    spin_lock(&task->signal->sighand->siglock);

    if (signal_sig_maskable(sig)) {
        sigset_t bit = signal_sigbit(sig);
        if ((task->signal->signal & bit) == 0) {
            memcpy(&task->signal->pending_signal.info[sig], &kinfo,
                   sizeof(kinfo));
            task->signal->pending_signal.info_mask |= bit;
        }
        task->signal->signal |= bit;
    }

    spin_unlock(&task->signal->sighand->siglock);

    on_send_signal_call(task, sig, &kinfo);

    signal_wake_interruptible_task(task, sig);
}

bool task_signal_has_deliverable(task_t *task) {
    if (!task || !task->signal || !task->signal->sighand) {
        return false;
    }

    bool deliverable = false;
    spin_lock(&task->signal->sighand->siglock);
    deliverable = task->signal->sighand->group_exit ||
                  signal_has_deliverable_outside_set_locked(task, 0);
    spin_unlock(&task->signal->sighand->siglock);

    return deliverable;
}

static void signal_fill_signalfd_siginfo(struct signalfd_siginfo *sinfo,
                                         int sig, const siginfo_t *info) {
    memset(sinfo, 0, sizeof(*sinfo));
    sinfo->ssi_signo = info ? info->si_signo : sig;
    sinfo->ssi_errno = info ? info->si_errno : 0;
    sinfo->ssi_code = info ? info->si_code : SI_KERNEL;

    if (!info) {
        sinfo->ssi_pid = current_task ? current_task->pid : 0;
        sinfo->ssi_uid = current_task ? current_task->uid : 0;
        return;
    }

    switch (sig) {
    case SIGCHLD:
        sinfo->ssi_pid = info->_sifields._sigchld._pid;
        sinfo->ssi_uid = info->_sifields._sigchld._uid;
        sinfo->ssi_status = info->_sifields._sigchld._status;
        sinfo->ssi_utime = (uint64_t)info->_sifields._sigchld._utime;
        sinfo->ssi_stime = (uint64_t)info->_sifields._sigchld._stime;
        break;
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGTRAP:
        sinfo->ssi_addr = (uint64_t)info->_sifields._sigfault._addr;
        break;
    default:
        if (info->si_code == SI_QUEUE || info->si_code == SI_TIMER ||
            info->si_code == SI_MESGQ || info->si_code == SI_ASYNCIO) {
            sinfo->ssi_pid = info->_sifields._rt._pid;
            sinfo->ssi_uid = info->_sifields._rt._uid;
            sinfo->ssi_int = info->_sifields._rt._sigval.sival_int;
            sinfo->ssi_ptr = (uint64_t)info->_sifields._rt._sigval.sival_ptr;
        } else {
            sinfo->ssi_pid = info->_sifields._kill._pid;
            sinfo->ssi_uid = info->_sifields._kill._uid;
        }
        break;
    }
}

bool task_signal_has_pending_in_set(task_t *task, sigset_t user_mask) {
    if (!task || !task->signal || !task->signal->sighand) {
        return false;
    }

    sigset_t wait_set = sigset_user_to_kernel(user_mask);
    bool pending = false;
    spin_lock(&task->signal->sighand->siglock);
    pending = signal_pick_from_set_locked(task, wait_set) != 0;
    spin_unlock(&task->signal->sighand->siglock);
    return pending;
}

bool task_signal_take_signalfd(task_t *task, sigset_t user_mask,
                               struct signalfd_siginfo *sinfo) {
    if (!task || !task->signal || !task->signal->sighand || !sinfo) {
        return false;
    }

    sigset_t wait_set = sigset_user_to_kernel(user_mask);
    siginfo_t info;
    int sig;

    spin_lock(&task->signal->sighand->siglock);
    sig = signal_pick_from_set_locked(task, wait_set);
    if (sig) {
        signal_take_pending_locked(task, sig, &info);
    }
    spin_unlock(&task->signal->sighand->siglock);

    if (!sig)
        return false;

    signal_fill_signalfd_siginfo(sinfo, sig, &info);
    return true;
}

uint64_t sys_ssetmask(int how, const sigset_t *nset, sigset_t *oset,
                      size_t sigsetsize) {
    if (!signal_sigset_size_valid(sigsetsize)) {
        return (uint64_t)-EINVAL;
    }

    spin_lock(&current_task->signal->sighand->siglock);

    if (oset) {
        *oset = sigset_kernel_to_user(current_task->signal->blocked);
    }

    if (nset) {
        sigset_t safe = sigset_user_to_kernel(*nset);
        switch (how) {
        case SIG_BLOCK:
            current_task->signal->blocked |= safe;
            break;
        case SIG_UNBLOCK:
            current_task->signal->blocked &= ~safe;
            break;
        case SIG_SETMASK:
            current_task->signal->blocked = safe;
            break;
        default:
            spin_unlock(&current_task->signal->sighand->siglock);
            return (uint64_t)-EINVAL;
        }
    }

    spin_unlock(&current_task->signal->sighand->siglock);
    return 0;
}

uint64_t sys_sigprocmask(int how, const sigset_t *nset_u, sigset_t *oset_u,
                         size_t sigsetsize) {
    if (!signal_sigset_size_valid(sigsetsize)) {
        return (uint64_t)-EINVAL;
    }

    sigset_t nset = 0;
    sigset_t oset = 0;
    if (nset_u && copy_from_user(&nset, nset_u, sigsetsize)) {
        return (uint64_t)-EFAULT;
    }

    uint64_t ret = sys_ssetmask(how, nset_u ? &nset : NULL,
                                oset_u ? &oset : NULL, sigsetsize);
    if ((int64_t)ret < 0) {
        return ret;
    }

    if (oset_u && copy_to_user(oset_u, &oset, sigsetsize)) {
        return (uint64_t)-EFAULT;
    }

    return ret;
}

uint64_t sys_sigaction(int sig, const void *action, void *oldaction,
                       size_t sigsetsize) {
    if (!signal_sig_in_range(sig) || sig == SIGKILL || sig == SIGSTOP) {
        return (uint64_t)-EINVAL;
    }
    if (!signal_sigset_size_valid(sigsetsize)) {
        return (uint64_t)-EINVAL;
    }

    sigaction_t new_action;
    bool has_new = action != NULL;
    if (has_new) {
        signal_kernel_sigaction_t user_action;
        if (copy_from_user(&user_action, action, sizeof(user_action))) {
            return (uint64_t)-EFAULT;
        }
        memset(&new_action, 0, sizeof(new_action));
        new_action.sa_handler = user_action.handler;
        new_action.sa_flags = (int)user_action.flags;
        new_action.sa_restorer = NULL;
#if SIGNAL_ARCH_HAS_RESTORER_FIELD
        new_action.sa_restorer = user_action.restorer;
#endif
        new_action.sa_mask = sigset_user_to_kernel(user_action.mask);
#if SIGNAL_ARCH_VALIDATE_RESTORER
        if (new_action.sa_handler != SIG_DFL &&
            new_action.sa_handler != SIG_IGN &&
            (new_action.sa_flags & SA_RESTORER) != 0 &&
            new_action.sa_restorer == NULL) {
            return (uint64_t)-EINVAL;
        }
#endif
    }

    sigaction_t old_local;
    bool has_old = oldaction != NULL;

    spin_lock(&current_task->signal->sighand->siglock);
    sigaction_t *slot = &current_task->signal->sighand->actions[sig];
    if (has_old) {
        old_local = *slot;
    }
    if (has_new) {
        *slot = new_action;
    }
    spin_unlock(&current_task->signal->sighand->siglock);

    if (has_old) {
        signal_kernel_sigaction_t user_old = {
            .handler = old_local.sa_handler,
            .flags = (unsigned long)old_local.sa_flags,
            .mask = sigset_kernel_to_user(old_local.sa_mask),
#if SIGNAL_ARCH_HAS_RESTORER_FIELD
            .restorer = old_local.sa_restorer,
#endif
        };

        if (copy_to_user(oldaction, &user_old, sizeof(user_old))) {
            return (uint64_t)-EFAULT;
        }
    }

    return 0;
}

uint64_t sys_sigreturn(struct pt_regs *regs) {
    return signal_arch_sigreturn(regs);
}

uint64_t sys_sigaltstack(const stack_t *uss, stack_t *uoss) {
    task_t *self = current_task;
    if (!self || !self->signal)
        return (uint64_t)-EINVAL;

    stack_t new_stack;
    bool has_new = uss != NULL;
    if (has_new && copy_from_user(&new_stack, uss, sizeof(new_stack)))
        return (uint64_t)-EFAULT;

    if (has_new && (new_stack.ss_flags & ~(SS_DISABLE | SS_AUTODISARM)) != 0)
        return (uint64_t)-EINVAL;

    uint64_t user_sp = signal_current_user_sp(self);
    stack_t old_stack;

    spin_lock(&self->signal->sighand->siglock);
    signal_altstack_format_old(&old_stack, &self->signal->altstack, user_sp);

    if (has_new) {
        int ret = signal_altstack_validate_new(&new_stack);
        if (ret < 0) {
            spin_unlock(&self->signal->sighand->siglock);
            return (uint64_t)ret;
        }
        if (old_stack.ss_flags & SS_ONSTACK) {
            spin_unlock(&self->signal->sighand->siglock);
            return (uint64_t)-EPERM;
        }
        signal_altstack_store(&self->signal->altstack, &new_stack);
    }

    spin_unlock(&self->signal->sighand->siglock);

    if (uoss && copy_to_user(uoss, &old_stack, sizeof(old_stack)))
        return (uint64_t)-EFAULT;

    return 0;
}

uint64_t sys_rt_sigpending(sigset_t *set, size_t sigsetsize) {
    sigset_t pending;

    if (!signal_sigset_size_valid(sigsetsize))
        return (uint64_t)-EINVAL;
    if (!set)
        return (uint64_t)-EFAULT;

    spin_lock(&current_task->signal->sighand->siglock);
    pending = signal_pending_mask_locked(current_task);
    pending &= current_task->signal->blocked;
    spin_unlock(&current_task->signal->sighand->siglock);

    pending = sigset_kernel_to_user(pending);
    if (copy_to_user(set, &pending, sigsetsize))
        return (uint64_t)-EFAULT;

    return 0;
}

uint64_t sys_rt_sigtimedwait(const sigset_t *uthese, siginfo_t *uinfo,
                             const struct timespec *uts, size_t sigsetsize) {
    if (!signal_sigset_size_valid(sigsetsize)) {
        return (uint64_t)-EINVAL;
    }
    if (!uthese) {
        return (uint64_t)-EFAULT;
    }

    sigset_t user_wait_set = 0;
    if (copy_from_user(&user_wait_set, uthese, sigsetsize)) {
        return (uint64_t)-EFAULT;
    }
    sigset_t wait_set = sigset_user_to_kernel(user_wait_set);

    uint64_t deadline = UINT64_MAX;
    if (uts) {
        struct timespec ts;
        if (copy_from_user(&ts, uts, sizeof(ts))) {
            return (uint64_t)-EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
            return (uint64_t)-EINVAL;
        }
        uint64_t wait_ns =
            (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        deadline = nano_time() + wait_ns;
    }

    bool irq_state = arch_interrupt_enabled();
    arch_enable_interrupt();

    while (true) {
        siginfo_t info;
        int sig = 0;
        bool interrupted = false;

        spin_lock(&current_task->signal->sighand->siglock);
        sig = signal_pick_from_set_locked(current_task, wait_set);
        if (sig) {
            signal_take_pending_locked(current_task, sig, &info);
        } else {
            interrupted = signal_has_deliverable_outside_set_locked(
                current_task, wait_set);
        }
        spin_unlock(&current_task->signal->sighand->siglock);

        if (sig) {
            if (uinfo && copy_to_user(uinfo, &info, sizeof(info))) {
                return (uint64_t)-EFAULT;
            }
            return (uint64_t)sig;
        }

        if (interrupted) {
            return (uint64_t)-EINTR;
        }

        if (deadline != UINT64_MAX && nano_time() >= deadline) {
            return (uint64_t)-EAGAIN;
        }

        arch_wait_for_interrupt();
    }

    if (!irq_state)
        arch_disable_interrupt();
}

uint64_t sys_rt_sigqueueinfo(uint64_t tgid, uint64_t sig, siginfo_t *info) {
    if (!signal_sig_in_range((int)sig)) {
        return (uint64_t)-EINVAL;
    }
    if (tgid == 0) {
        return (uint64_t)-ESRCH;
    }
    if (!info) {
        return (uint64_t)-EFAULT;
    }

    task_t *task = task_find_by_pid(tgid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }

    siginfo_t kinfo;
    if (copy_from_user(&kinfo, info, sizeof(kinfo))) {
        return (uint64_t)-EFAULT;
    }

    kinfo.si_signo = (int)sig;
    task_commit_signal(task, (int)sig, &kinfo);

    return 0;
}

uint64_t sys_sigsuspend(const sigset_t *mask, size_t sigsetsize) {
    if (!signal_sigset_size_valid(sigsetsize)) {
        return (uint64_t)-EINVAL;
    }
    if (!mask) {
        return (uint64_t)-EFAULT;
    }

    sigset_t user_mask = 0;
    if (copy_from_user(&user_mask, mask, sigsetsize)) {
        return (uint64_t)-EFAULT;
    }

    sigset_t old_mask;
    spin_lock(&current_task->signal->sighand->siglock);
    old_mask = current_task->signal->blocked;
    current_task->signal->blocked = sigset_user_to_kernel(user_mask);
    current_task->signal->sigsuspend_old_mask = old_mask;
    current_task->signal->sigsuspend_active = 1;
    spin_unlock(&current_task->signal->sighand->siglock);

    while (true) {
        bool should_return = false;
        spin_lock(&current_task->signal->sighand->siglock);
        should_return =
            signal_has_deliverable_outside_set_locked(current_task, 0);
        spin_unlock(&current_task->signal->sighand->siglock);

        if (should_return) {
            return (uint64_t)-EINTR;
        }

        int ret = task_block(current_task, TASK_BLOCKING, -1, "sigsuspend");
        if (ret < 0)
            return (uint64_t)ret;
        if (ret == EOK || ret == ETIMEDOUT || ret >= 128) {
            continue;
        }
    }
}

void task_fill_siginfo(siginfo_t *info, int sig, int code) {
    signal_fill_kernel_siginfo(info, sig, code);
    info->_sifields._kill._pid =
        current_task ? (int)task_effective_tgid(current_task) : 0;
    info->_sifields._kill._uid = current_task ? current_task->uid : 0;
}

void task_send_signal(task_t *task, int sig, int code) {
    if (!task || !signal_sig_in_range(sig)) {
        return;
    }

    siginfo_t info;
    task_fill_siginfo(&info, sig, code);
    task_commit_signal(task, sig, &info);
}

uint64_t sys_kill(int pid, int sig) {
    if (sig < 0 || sig >= MAXSIG) {
        return (uint64_t)-EINVAL;
    }

    if (pid > 0) {
        task_t *target = task_find_by_pid((uint64_t)pid);
        if (!target) {
            return (uint64_t)-ESRCH;
        }
        if (sig == 0) {
            return 0;
        }
        return task_kill_thread_group(task_effective_tgid(target), sig) > 0
                   ? 0
                   : (uint64_t)-ESRCH;
    }

    int sent = 0;
    if (pid == 0 || pid < -1) {
        int pgid = (pid == 0) ? current_task->pgid : -pid;
        sent = task_kill_process_group(pgid, sig);
        return sent ? 0 : (uint64_t)-ESRCH;
    }

    if (pid == -1) {
        sent = task_kill_all(sig);
        return sent ? 0 : (uint64_t)-ESRCH;
    }

    return (uint64_t)-EINVAL;
}

uint64_t sys_tkill(int pid, int sig) {
    if (pid <= 0 || sig < 0 || sig >= MAXSIG) {
        return (uint64_t)-EINVAL;
    }

    task_t *task = task_find_by_pid((uint64_t)pid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }
    if (sig == 0) {
        return 0;
    }

    task_send_signal(task, sig, SI_TKILL);

    return 0;
}

uint64_t sys_tgkill(int tgid, int pid, int sig) {
    if (tgid <= 0 || pid <= 0 || sig < 0 || sig >= MAXSIG) {
        return (uint64_t)-EINVAL;
    }

    task_t *task = task_find_by_pid((uint64_t)pid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }
    if (task_effective_tgid(task) != tgid) {
        return (uint64_t)-ESRCH;
    }
    if (sig == 0) {
        return 0;
    }

    task_send_signal(task, sig, SI_TKILL);

    return 0;
}

__attribute__((used)) void task_signal(struct pt_regs *regs) {
    task_t *self = current_task;
    int64_t group_exit_code;

    if (task_group_exit_code(self, &group_exit_code)) {
        task_exit_thread(group_exit_code);
    }

    if (!self || !self->signal || self->is_kernel || self->state == TASK_DIED ||
        self->state == TASK_UNINTERRUPTABLE) {
        return;
    }
    if (!signal_arch_user_context(regs)) {
        return;
    }
    if (self->signal->signal == 0) {
        return;
    }

    spin_lock(&self->signal->sighand->siglock);

    while (true) {
        int sig = signal_pick_deliverable_locked(self);
        if (!sig) {
            spin_unlock(&self->signal->sighand->siglock);
            return;
        }

        siginfo_t info;
        signal_take_pending_locked(self, sig, &info);

        sigaction_t action = self->signal->sighand->actions[sig];
        bool from_sigsuspend = self->signal->sigsuspend_active != 0;
        sigset_t restore_mask = from_sigsuspend
                                    ? self->signal->sigsuspend_old_mask
                                    : self->signal->blocked;
        self->signal->sigsuspend_active = 0;

        if (ptrace_is_traced(self) && ptrace_signal_should_stop(sig)) {
            spin_unlock(&self->signal->sighand->siglock);
            ptrace_stop_for_signal(self, sig, &info);
            ptrace_resume_from_signal(self);
            spin_lock(&self->signal->sighand->siglock);
            continue;
        }

        if (signal_action_ignored(sig, &action)) {
            continue;
        }

        if (action.sa_handler == SIG_DFL) {
            if (info.si_code == SI_DETHREAD) {
                spin_unlock(&self->signal->sighand->siglock);
                task_exit_thread(128 + sig);
                return;
            }

            switch (signal_internal_decisions[sig]) {
            case SIGNAL_INTERNAL_TERM:
            case SIGNAL_INTERNAL_CORE:
                spin_unlock(&self->signal->sighand->siglock);
                task_exit(128 + sig);
                return;
            case SIGNAL_INTERNAL_STOP:
                // self->state = TASK_BLOCKING;
                self->status = 128 + sig;
                spin_unlock(&self->signal->sighand->siglock);
                return;
            case SIGNAL_INTERNAL_CONT:
                self->state = TASK_READY;
                spin_unlock(&self->signal->sighand->siglock);
                return;
            case SIGNAL_INTERNAL_IGN:
            default:
                continue;
            }
        }

        if (!signal_arch_setup_frame(self, regs, sig, &action, &info,
                                     restore_mask)) {
            spin_unlock(&self->signal->sighand->siglock);
            task_exit(128 + SIGSEGV);
            return;
        }

        if (action.sa_flags & SA_RESETHAND) {
            self->signal->sighand->actions[sig].sa_handler = SIG_DFL;
        }

        self->signal->blocked |= action.sa_mask;
        if (!(action.sa_flags & SA_NODEFER) && signal_sig_maskable(sig)) {
            self->signal->blocked |= signal_sigbit(sig);
        }

        spin_unlock(&self->signal->sighand->siglock);
        return;
    }
}
