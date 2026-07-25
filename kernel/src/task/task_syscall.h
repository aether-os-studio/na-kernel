#pragma once

#include <fs/vfs/vfs.h>
#include <fs/proc.h>
#include <task/task.h>
#include <task/sched.h>
#include <fs/fs_syscall.h>

uint64_t task_fork(struct pt_regs *regs, bool vfork);

#define WNOHANG 0x00000001
#define WUNTRACED 0x00000002
#define WSTOPPED WUNTRACED
#define WEXITED 0x00000004
#define WCONTINUED 0x00000008
#define WNOWAIT 0x01000000

/* idtype for waitid */
#define P_ALL 0
#define P_PID 1
#define P_PGID 2
#define P_PIDFD 3

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD 1

/**
 * Linux contract: wait for child exit/stop/continue events selected by pid and
 * options.
 * Current kernel: supports normal child reaping and ptrace stop notifications.
 * Gaps: the implementation is not yet a complete reproduction of every Linux
 * wait status corner case.
 */
uint64_t sys_wait4(int pid, int *status, uint64_t options,
                   struct rusage *rusage);
uint64_t sys_times(struct tms *buf);
/**
 * Linux contract: waitid(2) with siginfo reporting and optional WNOWAIT.
 * Current kernel: supports P_ALL, P_PID, P_PGID, and P_PIDFD selection plus
 * ptrace wait events.
 * Gaps: Linux-accurate WCONTINUED and some less common waitid state transitions
 * are not fully modeled yet, and the implementation is still centered on exit
 * and ptrace-stop reporting.
 */
uint64_t sys_waitid(int idtype, uint64_t id, siginfo_t *infop, int options,
                    struct rusage *rusage);
/**
 * Linux contract: implement ptrace request dispatch for traced tasks.
 * Current kernel: supports the subset wired by task/ptrace.c.
 */
uint64_t sys_ptrace(uint64_t request, uint64_t pid, void *addr, void *data);
/**
 * Linux contract: report resource usage for the selected task set.
 * Current kernel: supports SELF, THREAD, and CHILDREN accounting.
 * Gaps: only the counters currently maintained by the scheduler/task runtime
 * code are reported.
 */
uint64_t sys_getrusage(int who, struct rusage *ru);
uint64_t sys_capget(void *header_user, void *data_user);
uint64_t sys_capset(void *header_user, const void *data_user);
/**
 * Linux contract: create a task or thread according to clone(2) flags.
 * Current kernel: supports the common CLONE_VM/FILES/FS/SIGHAND/THREAD/VFORK
 * family plus namespace and pidfd integration used elsewhere in the kernel.
 * Gaps: only the flag combinations explicitly validated in sys_clone_internal()
 * are supported; unsupported Linux clone features are rejected or ignored if no
 * kernel support exists behind them.
 */
uint64_t sys_clone(struct pt_regs *regs, uint64_t flags, uint64_t newsp,
                   int *parent_tid, int *child_tid, uint64_t tls);
/**
 * Linux contract: unshare task state such as file tables and namespaces.
 * Current kernel: supports CLONE_FS, CLONE_FILES, CLONE_SYSVSEM, and the
 * namespace flags listed in task_syscall.c.
 * Gaps: only state with an implemented clone/put path can actually be detached,
 * and unsupported Linux unshare bits are rejected up front.
 */
uint64_t sys_unshare(uint64_t unshare_flags);
/**
 * Linux contract: join an existing namespace referenced by a namespace fd.
 * Current kernel: supports mount and user namespaces only.
 * Gaps: other Linux namespace types still fail even if the fd is valid, and
 * setns support is limited to the namespace fd formats recognized by procfs.
 */
uint64_t sys_setns(int fd, uint64_t nstype);

typedef struct clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
} clone_args_t;

/**
 * Linux contract: clone3(2) with a structured argument block.
 * Current kernel: supports the fields consumed by sys_clone_internal(),
 * including pidfd creation and cgroupfd-based placement.
 * Gaps: fields such as set_tid/set_tid_size are not acted on, and support is
 * limited to the clone flags implemented by sys_clone_internal().
 */
uint64_t sys_clone3(struct pt_regs *regs, clone_args_t *args,
                    uint64_t args_size);
int read_task_user_memory(task_t *task, uint64_t uaddr, void *dst, size_t size);
int write_task_user_memory(task_t *task, uint64_t uaddr, const void *src,
                           size_t size);
/**
 * Linux contract: copy memory from another task with process_vm_readv(2).
 * Current kernel: supports vector copies through the target task's page tables.
 * Gaps: there are no ptrace-like permission checks yet; only basic pid lookup,
 * userspace pointer validation, and mapped-page access are enforced.
 */
uint64_t sys_process_vm_readv(uint64_t pid, const struct iovec *lvec,
                              uint64_t liovcnt, const struct iovec *rvec,
                              uint64_t riovcnt, uint64_t flags);
/**
 * Linux contract: copy memory into another task with process_vm_writev(2).
 * Current kernel: supports vector copies through the target task's page tables.
 * Gaps: there are no ptrace-like permission checks yet; only basic pid lookup,
 * userspace pointer validation, and mapped-page access are enforced.
 */
uint64_t sys_process_vm_writev(uint64_t pid, const struct iovec *lvec,
                               uint64_t liovcnt, const struct iovec *rvec,
                               uint64_t riovcnt, uint64_t flags);

struct timespec;
/**
 * Linux contract: sleep until the requested relative interval expires or a
 * signal interrupts the sleep.
 * Current kernel: performs a relative sleep using force_wakeup_ns scheduling.
 * Gaps: interruption handling and rem reporting are not Linux-complete yet, so
 * the call currently behaves more like an uninterruptible relative delay.
 */
uint64_t sys_nanosleep(struct timespec *req, struct timespec *rem);
/**
 * Linux contract: clock_nanosleep(2) with selectable clocks and TIMER_ABSTIME.
 * Current kernel: supports CLOCK_REALTIME and CLOCK_MONOTONIC validation.
 * Gaps: the current implementation sleeps relative to "now" even when
 * TIMER_ABSTIME is supplied, so absolute-sleep semantics are incomplete.
 */
uint64_t sys_clock_nanosleep(int clock_id, int flags,
                             const struct timespec *request,
                             struct timespec *remain);

#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6
#define SCHED_RESET_ON_FORK 0x40000000

struct sched_param {
    int sched_priority;
};

/**
 * Linux contract: set the scheduling parameters for a target task.
 * Current kernel: validates the target and priority range.
 * Gaps: it does not yet reconfigure the scheduler's runtime state.
 */
uint64_t sys_sched_setparam(int pid, const struct sched_param *param);
/**
 * Linux contract: return the active scheduling parameters for a target task.
 * Current kernel: always reports priority 0 after validating the target.
 */
uint64_t sys_sched_getparam(int pid, struct sched_param *param);
/**
 * Linux contract: change the scheduling policy and parameters of a target task.
 * Current kernel: validates policy/priority combinations and returns success.
 * Gaps: runtime policy changes are not applied to the scheduler yet.
 */
uint64_t sys_sched_setscheduler(int pid, int policy,
                                const struct sched_param *param);
/**
 * Linux contract: report the active scheduling policy of a target task.
 * Current kernel: always reports SCHED_OTHER for existing tasks.
 */
uint64_t sys_sched_getscheduler(int pid);
/**
 * Linux contract: report the highest priority allowed for a policy.
 * Current kernel: returns bounds from the local scheduler policy table.
 */
uint64_t sys_sched_get_priority_max(int policy);
/**
 * Linux contract: report the lowest priority allowed for a policy.
 * Current kernel: returns bounds from the local scheduler policy table.
 */
uint64_t sys_sched_get_priority_min(int policy);
/**
 * Linux contract: report the round-robin quantum for a target task.
 * Current kernel: returns a fixed 100 ms interval after validating the target.
 * Gaps: the value is synthetic and not derived from a real per-policy runtime
 * scheduler configuration.
 */
uint64_t sys_sched_rr_get_interval(int pid, struct timespec *interval);
/**
 * Linux contract: restrict a task to the CPUs in the supplied affinity mask.
 * Current kernel: validates the mask and target existence.
 * Gaps: the mask is not yet enforced by the scheduler, so this is currently a
 * validation-only compatibility interface.
 */
uint64_t sys_sched_setaffinity(int pid, size_t len,
                               const unsigned long *user_mask_ptr);
/**
 * Linux contract: return the current CPU affinity mask for a target task.
 * Current kernel: reports a synthetic mask that only exposes CPU 0.
 * Gaps: the returned mask does not reflect a real per-task affinity state.
 */
uint64_t sys_sched_getaffinity(int pid, size_t len,
                               unsigned long *user_mask_ptr);

/**
 * Linux contract: configure ITIMER_REAL/VTALRM/PROF timers.
 * Current kernel: implements ITIMER_REAL only.
 * Gaps: other timer classes return -ENOSYS.
 */
size_t sys_setitimer(int which, struct itimerval *value, struct itimerval *old);

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_GET_UNALIGN 5
#define PR_SET_UNALIGN 6
#define PR_UNALIGN_NOPRINT 1
#define PR_UNALIGN_SIGBUS 2
#define PR_GET_KEEPCAPS 7
#define PR_SET_KEEPCAPS 8
#define PR_GET_FPEMU 9
#define PR_SET_FPEMU 10
#define PR_FPEMU_NOPRINT 1
#define PR_FPEMU_SIGFPE 2
#define PR_GET_FPEXC 11
#define PR_SET_FPEXC 12
#define PR_FP_EXC_SW_ENABLE 0x80
#define PR_FP_EXC_DIV 0x010000
#define PR_FP_EXC_OVF 0x020000
#define PR_FP_EXC_UND 0x040000
#define PR_FP_EXC_RES 0x080000
#define PR_FP_EXC_INV 0x100000
#define PR_FP_EXC_DISABLED 0
#define PR_FP_EXC_NONRECOV 1
#define PR_FP_EXC_ASYNC 2
#define PR_FP_EXC_PRECISE 3
#define PR_GET_TIMING 13
#define PR_SET_TIMING 14
#define PR_TIMING_STATISTICAL 0
#define PR_TIMING_TIMESTAMP 1
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_GET_ENDIAN 19
#define PR_SET_ENDIAN 20
#define PR_ENDIAN_BIG 0
#define PR_ENDIAN_LITTLE 1
#define PR_ENDIAN_PPC_LITTLE 2
#define PR_GET_SECCOMP 21
#define PR_SET_SECCOMP 22
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24
#define PR_GET_TSC 25
#define PR_SET_TSC 26
#define PR_TSC_ENABLE 1
#define PR_TSC_SIGSEGV 2
#define PR_GET_SECUREBITS 27
#define PR_SET_SECUREBITS 28
#define PR_SET_TIMERSLACK 29
#define PR_GET_TIMERSLACK 30

#define PR_TASK_PERF_EVENTS_DISABLE 31
#define PR_TASK_PERF_EVENTS_ENABLE 32

#define PR_MCE_KILL 33
#define PR_MCE_KILL_CLEAR 0
#define PR_MCE_KILL_SET 1
#define PR_MCE_KILL_LATE 0
#define PR_MCE_KILL_EARLY 1
#define PR_MCE_KILL_DEFAULT 2
#define PR_MCE_KILL_GET 34

#define PR_SET_MM 35
#define PR_SET_MM_START_CODE 1
#define PR_SET_MM_END_CODE 2
#define PR_SET_MM_START_DATA 3
#define PR_SET_MM_END_DATA 4
#define PR_SET_MM_START_STACK 5
#define PR_SET_MM_START_BRK 6
#define PR_SET_MM_BRK 7
#define PR_SET_MM_ARG_START 8
#define PR_SET_MM_ARG_END 9
#define PR_SET_MM_ENV_START 10
#define PR_SET_MM_ENV_END 11
#define PR_SET_MM_AUXV 12
#define PR_SET_MM_EXE_FILE 13
#define PR_SET_MM_MAP 14
#define PR_SET_MM_MAP_SIZE 15

#define PR_SET_PTRACER 0x59616d61
#define PR_SET_PTRACER_ANY (-1UL)

#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37

#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

#define PR_GET_TID_ADDRESS 40

#define PR_SET_THP_DISABLE 41
#define PR_GET_THP_DISABLE 42

#define PR_MPX_ENABLE_MANAGEMENT 43
#define PR_MPX_DISABLE_MANAGEMENT 44

#define PR_SET_FP_MODE 45
#define PR_GET_FP_MODE 46
#define PR_FP_MODE_FR (1 << 0)
#define PR_FP_MODE_FRE (1 << 1)

#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_LOWER 3
#define PR_CAP_AMBIENT_CLEAR_ALL 4

#define SECURE_NOROOT 0
#define SECURE_NOROOT_LOCKED 1
#define SECURE_NO_SETUID_FIXUP 2
#define SECURE_NO_SETUID_FIXUP_LOCKED 3
#define SECURE_KEEP_CAPS 4
#define SECURE_KEEP_CAPS_LOCKED 5
#define SECURE_NO_CAP_AMBIENT_RAISE 6
#define SECURE_NO_CAP_AMBIENT_RAISE_LOCKED 7
#define SECURE_EXEC_RESTRICT_FILE 8
#define SECURE_EXEC_RESTRICT_FILE_LOCKED 9
#define SECURE_EXEC_DENY_INTERACTIVE 10
#define SECURE_EXEC_DENY_INTERACTIVE_LOCKED 11

#define SECBIT(bit) (1U << (bit))
#define SECBIT_NOROOT SECBIT(SECURE_NOROOT)
#define SECBIT_NOROOT_LOCKED SECBIT(SECURE_NOROOT_LOCKED)
#define SECBIT_NO_SETUID_FIXUP SECBIT(SECURE_NO_SETUID_FIXUP)
#define SECBIT_NO_SETUID_FIXUP_LOCKED SECBIT(SECURE_NO_SETUID_FIXUP_LOCKED)
#define SECBIT_KEEP_CAPS SECBIT(SECURE_KEEP_CAPS)
#define SECBIT_KEEP_CAPS_LOCKED SECBIT(SECURE_KEEP_CAPS_LOCKED)
#define SECBIT_NO_CAP_AMBIENT_RAISE SECBIT(SECURE_NO_CAP_AMBIENT_RAISE)
#define SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED                                     \
    SECBIT(SECURE_NO_CAP_AMBIENT_RAISE_LOCKED)

static inline bool task_keep_caps_enabled(const task_t *task) {
    return task && (task->keep_caps || (task->securebits & SECBIT_KEEP_CAPS));
}

#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT 1
#define SECCOMP_MODE_FILTER 2

/**
 * Linux contract: multiplex process control features such as PR_SET_NAME,
 * PR_SET_NO_NEW_PRIVS, seccomp state, and parent-death signals.
 * Current kernel: implements only a small subset of prctl options.
 * Gaps: unsupported options often return -EINVAL, and features such as real
 * seccomp state, dumpability, or ambient capabilities are not implemented;
 * several accepted options currently just return success without changing any
 * deeper kernel policy.
 */
uint64_t sys_prctl(uint64_t options, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5);
/**
 * Linux contract: install or query seccomp filters.
 * Current kernel: declared for ABI completeness only.
 * Gaps: there is no implementation and the syscall number is not wired into the
 * architecture syscall tables yet.
 */
uint64_t sys_seccomp(uint64_t operation, uint64_t flags, void *uargs);
/**
 * Linux contract: register the robust futex list for the current thread.
 * Current kernel: validates the ABI size and stores the user pointer verbatim.
 * Gaps: only one Linux ABI layout size is accepted.
 */
uint64_t sys_set_robust_list(void *head, size_t len);
/**
 * Linux contract: query the robust futex list of a thread.
 * Current kernel: returns the stored list pointer and length.
 * Gaps: there are no ptrace-like permission checks beyond pid lookup.
 */
uint64_t sys_get_robust_list(int pid, void **head_ptr, size_t *len_ptr);
/**
 * Linux contract: register restartable sequences for per-CPU critical sections.
 * Current kernel: not implemented and always returns -ENOSYS.
 */
uint64_t sys_rseq(void *rseq, uint32_t rseq_len, int flags, uint32_t sig);
/**
 * Linux contract: configure the task's SIGALRM delivery via alarm(2).
 * Current kernel: wraps sys_setitimer(ITIMER_REAL).
 * Gaps: inherits the ITIMER_REAL-only limitation of sys_setitimer().
 */
uint64_t sys_alarm(uint64_t seconds);
/**
 * Linux contract: create a POSIX timer.
 * Supports CLOCK_REALTIME and CLOCK_MONOTONIC with SIGEV_NONE, SIGEV_SIGNAL,
 * and Linux's SIGEV_THREAD_ID notification mode.
 */
uint64_t sys_timer_create(clockid_t clockid, struct sigevent *sevp,
                          int *timerid);
/**
 * Linux contract: arm or disarm a POSIX timer.
 * Current kernel: supports relative and absolute expiry for the timer objects
 * created by sys_timer_create().
 */
uint64_t sys_timer_settime(int timerid, int flags,
                           const struct itimerspec *new_value,
                           struct itimerspec *old_value);
uint64_t sys_timer_gettime(int timerid, struct itimerspec *value);
uint64_t sys_timer_getoverrun(int timerid);
uint64_t sys_timer_delete(int timerid);

/**
 * Linux contract: reboot, halt, or power off the machine after validating the
 * Linux magic values and caller permissions.
 * Current kernel: toggles CAD state and accepts restart/poweroff commands.
 * Gaps: restart and poweroff currently return success without actually driving
 * hardware reset or shutdown, and Linux privilege checks are not enforced here.
 */
uint64_t sys_reboot(int magic1, int magic2, uint32_t cmd, void *arg);

/**
 * Linux contract: return the process group ID of the target process.
 * Current kernel: looks up the task by pid and returns its cached pgid.
 */
uint64_t sys_getpgid(uint64_t pid);
/**
 * Linux contract: move a process into a process group subject to session rules.
 * Current kernel: directly rewrites the target thread group's pgid.
 * Gaps: Linux session/exec/leadership permission checks are not fully enforced.
 */
uint64_t sys_setpgid(uint64_t pid, uint64_t pgid);
/**
 * Linux contract: execveat(2) with dirfd-relative path resolution and flags.
 * Current kernel: routes through the local exec loader and VFS path lookup.
 * Gaps: support is limited to the flags and interpreter/ELF handling
 * implemented by task_do_execve().
 */
uint64_t sys_execveat(uint64_t dirfd, const char *path, const char **argv,
                      const char **envp, uint64_t flags);
/**
 * Linux contract: return the session ID of the selected process.
 * Current kernel: returns the cached sid from the target task struct.
 */
uint64_t sys_getsid(uint64_t pid);
/**
 * Linux contract: create a new session and detach from the controlling group.
 * Current kernel: only permits thread-group leaders and rewrites sid/pgid for
 * the whole thread group.
 */
uint64_t sys_setsid(void);

/**
 * Linux contract: return the real user ID of the calling task.
 */
static inline uint64_t sys_getuid() { return current_task->uid; }

static inline int64_t cred_keep_or_set(int value, int64_t current) {
    return value == -1 ? current : value;
}

static inline void task_cred_update_capabilities_after_uid_change(
    task_t *task, int64_t old_uid, int64_t old_euid, int64_t old_suid) {
    bool old_had_root;
    bool new_has_root;

    if (!task)
        return;

    if (task->securebits & SECBIT_NO_SETUID_FIXUP) {
        if (old_euid == 0 && task->euid != 0)
            task->cap_ambient = 0;
        return;
    }

    old_had_root = old_uid == 0 || old_euid == 0 || old_suid == 0;
    new_has_root = task->uid == 0 || task->euid == 0 || task->suid == 0;

    if (old_had_root && !new_has_root) {
        task->cap_effective = 0;
        if (!task_keep_caps_enabled(task))
            task->cap_permitted = 0;
        task->cap_ambient = 0;
    } else if (old_euid == 0 && task->euid != 0) {
        /* A temporary seteuid() drop keeps the permitted set while a real or
         * saved root UID remains, so returning to euid 0 can regain it. */
        task->cap_effective = 0;
        task->cap_ambient = 0;
    } else if (old_euid != 0 && task->euid == 0) {
        task->cap_effective = task->cap_permitted;
    }
}

static inline void
task_cred_update_capabilities_after_gid_change(task_t *task) {
    (void)task;
}

/**
 * Linux contract: virtually hang up the caller's controlling terminal.
 * Current kernel: accepts the request as a compatibility no-op. This is enough
 * for login/getty paths that call vhangup() to reset inherited terminal state.
 */
static inline uint64_t sys_vhangup(void) { return 0; }

/**
 * Linux contract: set real/effective/saved user IDs according to setuid(2).
 * Current kernel: updates all three IDs unconditionally for the caller.
 * Gaps: Linux capability and permission checks are not implemented.
 */
static inline uint64_t sys_setuid(uint64_t uid) {
    int64_t old_uid;
    int64_t old_euid;
    int64_t old_suid;

    if (uid == (uint64_t)-1)
        return (uint64_t)-EINVAL;

    old_uid = current_task->uid;
    old_euid = current_task->euid;
    old_suid = current_task->suid;
    current_task->uid = uid;
    current_task->euid = uid;
    current_task->suid = uid;
    current_task->fsuid = uid;
    task_cred_update_capabilities_after_uid_change(current_task, old_uid,
                                                   old_euid, old_suid);
    return 0;
}

/**
 * Linux contract: return the real group ID of the calling task.
 */
static inline uint64_t sys_getgid() { return current_task->gid; }

/**
 * Linux contract: return the effective user ID of the calling task.
 */
static inline uint64_t sys_geteuid() { return current_task->euid; }

/**
 * Linux contract: return the effective group ID of the calling task.
 */
static inline uint64_t sys_getegid() { return current_task->egid; }

/**
 * Linux contract: return real/effective/saved user IDs.
 */
static inline uint64_t sys_getresuid(int *ruid, int *euid, int *suid) {
    int value;

    if (ruid) {
        value = current_task->uid;
        if (copy_to_user(ruid, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }
    if (euid) {
        value = current_task->euid;
        if (copy_to_user(euid, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }
    if (suid) {
        value = current_task->suid;
        if (copy_to_user(suid, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }

    return 0;
}

/**
 * Linux contract: set any subset of real/effective/saved user IDs.
 * Current kernel: directly updates the stored IDs.
 * Gaps: Linux privilege checks are not enforced.
 */
static inline uint64_t sys_setresuid(int ruid, int euid, int suid) {
    int64_t old_uid = current_task->uid;
    int64_t old_euid = current_task->euid;
    int64_t old_suid = current_task->suid;

    current_task->uid = cred_keep_or_set(ruid, current_task->uid);
    current_task->euid = cred_keep_or_set(euid, current_task->euid);
    current_task->suid = cred_keep_or_set(suid, current_task->suid);
    if (euid != -1)
        current_task->fsuid = current_task->euid;
    task_cred_update_capabilities_after_uid_change(current_task, old_uid,
                                                   old_euid, old_suid);

    return 0;
}

/**
 * Linux contract: change real/effective user IDs with Linux saved-ID rules.
 * Current kernel: applies the requested values and updates suid using a reduced
 * version of the Linux rules.
 */
static inline uint64_t sys_setreuid(int ruid, int euid) {
    int64_t old_ruid = current_task->uid;
    int64_t old_euid = current_task->euid;
    int64_t old_suid = current_task->suid;

    current_task->uid = cred_keep_or_set(ruid, current_task->uid);
    current_task->euid = cred_keep_or_set(euid, current_task->euid);
    if (euid != -1)
        current_task->fsuid = current_task->euid;
    if (ruid != -1 || (euid != -1 && current_task->euid != old_ruid))
        current_task->suid = current_task->euid;
    task_cred_update_capabilities_after_uid_change(current_task, old_ruid,
                                                   old_euid, old_suid);

    return 0;
}

/**
 * Linux contract: return real/effective/saved group IDs.
 */
static inline uint64_t sys_getresgid(int *rgid, int *egid, int *sgid) {
    int value;

    if (rgid) {
        value = current_task->gid;
        if (copy_to_user(rgid, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }
    if (egid) {
        value = current_task->egid;
        if (copy_to_user(egid, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }
    if (sgid) {
        value = current_task->sgid;
        if (copy_to_user(sgid, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }

    return 0;
}

/**
 * Linux contract: set any subset of real/effective/saved group IDs.
 * Current kernel: directly updates the stored IDs.
 * Gaps: Linux privilege checks are not enforced.
 */
static inline uint64_t sys_setresgid(int rgid, int egid, int sgid) {
    current_task->gid = cred_keep_or_set(rgid, current_task->gid);
    current_task->egid = cred_keep_or_set(egid, current_task->egid);
    current_task->sgid = cred_keep_or_set(sgid, current_task->sgid);
    if (egid != -1)
        current_task->fsgid = current_task->egid;
    task_cred_update_capabilities_after_gid_change(current_task);

    return 0;
}

/**
 * Linux contract: change real/effective group IDs with Linux saved-ID rules.
 */
static inline uint64_t sys_setregid(int rgid, int egid) {
    int64_t old_rgid = current_task->gid;

    current_task->gid = cred_keep_or_set(rgid, current_task->gid);
    current_task->egid = cred_keep_or_set(egid, current_task->egid);
    if (egid != -1)
        current_task->fsgid = current_task->egid;
    if (rgid != -1 || (egid != -1 && current_task->egid != old_rgid))
        current_task->sgid = current_task->egid;
    task_cred_update_capabilities_after_gid_change(current_task);

    return 0;
}

/**
 * Linux contract: set real/effective/saved group IDs according to setgid(2).
 * Current kernel: updates all three IDs unconditionally for the caller.
 * Gaps: Linux capability and permission checks are not implemented.
 */
static inline uint64_t sys_setgid(uint64_t gid) {
    if (gid == (uint64_t)-1)
        return (uint64_t)-EINVAL;

    current_task->gid = gid;
    current_task->egid = gid;
    current_task->sgid = gid;
    current_task->fsgid = gid;
    task_cred_update_capabilities_after_gid_change(current_task);
    return 0;
}

/**
 * Linux contract: set the caller's filesystem uid and return the old fsuid.
 * The change is accepted for root or for one of the caller's existing saved
 * user IDs; otherwise Linux keeps the old fsuid and still returns it.
 */
static inline uint64_t sys_setfsuid(uint64_t uid) {
    int64_t old = current_task->fsuid;

    if (uid <= 0xffffffffULL &&
        (current_task->euid == 0 || (int64_t)uid == current_task->uid ||
         (int64_t)uid == current_task->euid ||
         (int64_t)uid == current_task->suid ||
         (int64_t)uid == current_task->fsuid)) {
        current_task->fsuid = (int64_t)uid;
    }

    return (uint64_t)old;
}

/**
 * Linux contract: set the caller's filesystem gid and return the old fsgid.
 */
static inline uint64_t sys_setfsgid(uint64_t gid) {
    int64_t old = current_task->fsgid;

    if (gid <= 0xffffffffULL &&
        (current_task->euid == 0 || (int64_t)gid == current_task->gid ||
         (int64_t)gid == current_task->egid ||
         (int64_t)gid == current_task->sgid ||
         (int64_t)gid == current_task->fsgid)) {
        current_task->fsgid = (int64_t)gid;
    }

    return (uint64_t)old;
}

/**
 * Linux contract: fork the current task with a private address space.
 */
static inline uint64_t sys_fork(struct pt_regs *regs) {
    return task_fork(regs, false);
}

/**
 * Linux contract: vfork the current task and block the parent until exec/exit.
 */
static inline uint64_t sys_vfork(struct pt_regs *regs) {
    return task_fork(regs, true);
}

/**
 * Linux contract: return the caller's thread-group ID.
 */
static inline uint64_t sys_getpid() {
    return task_effective_tgid(current_task);
}

/**
 * Linux contract: return the caller's thread ID.
 */
static inline uint64_t sys_gettid() { return current_task->pid; }

/**
 * Linux contract: return the parent process ID visible to the caller.
 */
static inline uint64_t sys_getppid() { return task_parent_pid(current_task); }

/**
 * Linux contract: return the caller's process group ID.
 */
static inline uint64_t sys_getpgrp() { return current_task->pgid; }

/** Linux contract: report the supplementary group list. */
static inline uint64_t sys_getgroups(int gidsetsize, int *gids) {
    uint32_t *groups = NULL;
    int count;

    if (gidsetsize < 0)
        return (uint64_t)-EINVAL;

    count = task_groups_copy(current_task, NULL, 0);
    if (count < 0)
        return (uint64_t)count;

    if (gidsetsize == 0)
        return (uint64_t)count;
    if (gidsetsize < count)
        return (uint64_t)-EINVAL;
    if (count && !gids)
        return (uint64_t)-EFAULT;

    if (count) {
        groups = malloc((size_t)count * sizeof(*groups));
        if (!groups)
            return (uint64_t)-ENOMEM;
        int copied = task_groups_copy(current_task, groups, (size_t)count);
        if (copied < 0) {
            free(groups);
            return (uint64_t)copied;
        }
        count = copied;
    }

    if (count && copy_to_user(gids, groups, (size_t)count * sizeof(*groups))) {
        free(groups);
        return (uint64_t)-EFAULT;
    }

    free(groups);
    return (uint64_t)count;
}

/** Linux contract: replace the supplementary group list. */
static inline uint64_t sys_setgroups(int gidsetsize, const uint32_t *gids) {
    uint32_t *groups = NULL;
    int ret;

    if (gidsetsize < 0 || (uint32_t)gidsetsize > TASK_NGROUPS_MAX)
        return (uint64_t)-EINVAL;
    if (gidsetsize && !gids)
        return (uint64_t)-EFAULT;
    if (current_task->euid != 0 &&
        !(current_task->cap_effective & (UINT64_C(1) << 6)))
        return (uint64_t)-EPERM;
    if (current_task->nsproxy && current_task->nsproxy->user_ns &&
        current_task->nsproxy->user_ns->setgroups_state ==
            TASK_USERNS_SETGROUPS_DENY)
        return (uint64_t)-EPERM;

    if (gidsetsize) {
        groups = malloc((size_t)gidsetsize * sizeof(*groups));
        if (!groups)
            return (uint64_t)-ENOMEM;
        if (copy_from_user(groups, gids,
                           (size_t)gidsetsize * sizeof(*groups))) {
            free(groups);
            return (uint64_t)-EFAULT;
        }
        for (int i = 0; i < gidsetsize; i++) {
            if (groups[i] == UINT32_MAX) {
                free(groups);
                return (uint64_t)-EINVAL;
            }
        }
    }

    ret = task_groups_set(current_task, groups, (size_t)gidsetsize);
    free(groups);
    return (uint64_t)ret;
}

/**
 * Linux contract: report the current CPU and NUMA node.
 * Current kernel: returns the scheduler CPU ID and always reports NUMA node 0.
 */
static inline uint64_t sys_getcpu(unsigned *cpup, unsigned *nodep,
                                  void *unused) {
    (void)unused;

    if (cpup) {
        unsigned cpu = current_cpu_id;
        if (copy_to_user(cpup, &cpu, sizeof(cpu)))
            return (uint64_t)-EFAULT;
    }
    if (nodep) {
        unsigned node = 0;
        if (copy_to_user(nodep, &node, sizeof(node)))
            return (uint64_t)-EFAULT;
    }

    return 0;
}

/**
 * Linux contract: register the clear_child_tid pointer and return the caller's
 * thread ID.
 */
static inline uint64_t sys_set_tid_address(int *ptr) {
    current_task->tidptr = ptr;
    return current_task->pid;
}

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

/**
 * Linux contract: adjust a task's nice value.
 * Current kernel: only validates PRIO_PROCESS lookups and does not yet change
 * scheduler behavior.
 * Gaps: PRIO_PGRP and PRIO_USER are not implemented.
 */
uint64_t sys_getpriority(int which, int who);
uint64_t sys_setpriority(int which, int who, int niceval);
uint64_t sys_ioprio_set(int which, int who, int ioprio);
uint64_t sys_ioprio_get(int which, int who);

/**
 * Linux contract: compare shared kernel objects between two tasks via kcmp(2).
 * Current kernel: supports the subset implemented in task_syscall.c.
 */
uint64_t sys_kcmp(uint64_t pid1, uint64_t pid2, int type, uint64_t idx1,
                  uint64_t idx2);

/**
 * Linux contract: get or set the execution-domain personality flags.
 * Current kernel: exposes the local compatibility personality state.
 */
uint32_t sys_personality(uint32_t personality);

int sys_yield();
