#include <arch/arch.h>
#include <boot/boot.h>
#include <arch/riscv64/time/time.h>
#include <task/task.h>
#include <task/signal.h>
#include <mm/mm_syscall.h>
#include <task/futex.h>
#include <task/keyring.h>
#include <task/ptrace.h>
#include <task/task_syscall.h>
#include <drivers/rtc.h>
#include <net/net_syscall.h>

static uint64_t sys_riscv_hwprobe(void) { return (uint64_t)-ENOSYS; }

static uint64_t sys_riscv_flush_icache(uint64_t start, uint64_t end,
                                       uint64_t flags) {
    (void)start;
    (void)end;
    (void)flags;
    /*
     * Userland uses this as a cache coherency barrier after JIT/code writes.
     * The current kernel does not model per-range icache maintenance, but a
     * successful no-op is still far more compatible than ENOSYS.
     */
    return 0;
}

static inline uint64_t getrandom_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static uint64_t riscv64_sys_clone(struct pt_regs *frame, uint64_t flags,
                                  uint64_t newsp, uint64_t parent_tid,
                                  uint64_t tls, uint64_t child_tid) {
    return sys_clone(frame, flags, newsp, (int *)parent_tid, (int *)child_tid,
                     tls);
}

static uint64_t copy_timespec_to_user(uint64_t user_addr, uint64_t sec,
                                      uint64_t nsec) {
    if (!user_addr)
        return 0;

    struct timespec ts = {
        .tv_sec = (long long)sec,
        .tv_nsec = (long)nsec,
    };
    return copy_to_user((void *)user_addr, &ts, sizeof(ts)) ? (uint64_t)-EFAULT
                                                            : 0;
}
#define LINUX_CPUCLOCK_PERTHREAD_MASK 4
#define LINUX_CPUCLOCK_CLOCK_MASK 3
#define LINUX_CPUCLOCK_PROF 0
#define LINUX_CPUCLOCK_VIRT 1
#define LINUX_CPUCLOCK_SCHED 2
#define LINUX_CLOCKFD 3

static inline bool linux_clockid_is_cpu(clockid_t clock_id) {
    return clock_id < 0 &&
           (clock_id & LINUX_CPUCLOCK_CLOCK_MASK) != LINUX_CLOCKFD;
}

static inline int linux_clockid_pid(clockid_t clock_id) {
    return ((uint32_t)(~clock_id)) >> 3;
}

static inline bool linux_clockid_perthread(clockid_t clock_id) {
    return (clock_id & LINUX_CPUCLOCK_PERTHREAD_MASK) != 0;
}

static inline int linux_clockid_which(clockid_t clock_id) {
    return clock_id & LINUX_CPUCLOCK_CLOCK_MASK;
}

static inline uint64_t linux_task_cpu_clock_sample(task_t *task, int which) {
    if (!task)
        return 0;

    switch (which) {
    case LINUX_CPUCLOCK_PROF:
    case LINUX_CPUCLOCK_SCHED:
        return task->user_time_ns;
    case LINUX_CPUCLOCK_VIRT:
        return task_self_user_ns(task);
    default:
        return (uint64_t)-EINVAL;
    }
}

static bool linux_resolve_cpu_clock(clockid_t clock_id, uint64_t *target_id,
                                    bool *perthread, int *which) {
    if (!current_task)
        return false;

    if (clock_id == CLOCK_PROCESS_CPUTIME_ID) {
        *target_id = task_effective_tgid(current_task);
        *perthread = false;
        *which = LINUX_CPUCLOCK_SCHED;
        return true;
    }

    if (clock_id == CLOCK_THREAD_CPUTIME_ID) {
        *target_id = current_task->pid;
        *perthread = true;
        *which = LINUX_CPUCLOCK_SCHED;
        return true;
    }

    if (!linux_clockid_is_cpu(clock_id))
        return false;

    *perthread = linux_clockid_perthread(clock_id);
    *which = linux_clockid_which(clock_id);
    *target_id = linux_clockid_pid(clock_id);

    if (*target_id == 0) {
        *target_id =
            *perthread ? current_task->pid : task_effective_tgid(current_task);
    }

    return *which <= LINUX_CPUCLOCK_SCHED;
}

static uint64_t linux_sample_cpu_clock(clockid_t clock_id,
                                       uint64_t *sample_ns) {
    uint64_t target_id = 0;
    bool perthread = false;
    int which = 0;

    if (!linux_resolve_cpu_clock(clock_id, &target_id, &perthread, &which))
        return (uint64_t)-EINVAL;

    uint64_t sample = 0;
    bool found = false;

    spin_lock(&task_queue_lock);

    if (perthread) {
        task_t *task = task_lookup_by_pid_nolock(target_id);
        if (task) {
            sample = linux_task_cpu_clock_sample(task, which);
            found = true;
        }
    } else if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            if (entry->state != HASHMAP_ENTRY_OCCUPIED)
                continue;

            task_t *task = (task_t *)entry->value;
            if (!task || task_effective_tgid(task) != target_id)
                continue;

            sample += linux_task_cpu_clock_sample(task, which);
            found = true;
        }
    }

    spin_unlock(&task_queue_lock);

    if (!found)
        return (uint64_t)-EINVAL;

    *sample_ns = sample;
    return 0;
}

static uint64_t linux_clock_gettime_cpu(clockid_t clock_id,
                                        uint64_t user_addr) {
    uint64_t sample_ns = 0;
    uint64_t ret = linux_sample_cpu_clock(clock_id, &sample_ns);
    if (ret != 0)
        return ret;

    return copy_timespec_to_user(user_addr, sample_ns / 1000000000ULL,
                                 sample_ns % 1000000000ULL);
}

static uint64_t linux_clock_getres_cpu(clockid_t clock_id, uint64_t user_addr) {
    uint64_t sample_ns = 0;
    uint64_t ret = linux_sample_cpu_clock(clock_id, &sample_ns);
    if (ret != 0)
        return ret;

    return copy_timespec_to_user(user_addr, 0, 1);
}

static uint64_t sys_clock_gettime(uint64_t clockid, uint64_t tp, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;

    if ((int64_t)clockid != CLOCK_REALTIME &&
        (int64_t)clockid != CLOCK_MONOTONIC)
        return (uint64_t)-EINVAL;

    if ((int64_t)clockid == CLOCK_REALTIME) {
        rtc_realtime_t now;
        rtc_read_realtime(&now);
        return copy_timespec_to_user(tp, now.sec, now.nsec);
    }

    uint64_t nano_now = nano_time();
    return copy_timespec_to_user(tp, nano_now / 1000000000ULL,
                                 nano_now % 1000000000ULL);
}

static uint64_t sys_clock_getres(uint64_t arg1, uint64_t arg2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
    clockid_t clock_id = (clockid_t)arg1;

    switch (clock_id) {
    case 2: // CLOCK_PROCESS_CPUTIME_ID
    case 3: // CLOCK_THREAD_CPUTIME_ID
        return linux_clock_getres_cpu(clock_id, arg2);
    default:
        if (linux_clockid_is_cpu(clock_id))
            return linux_clock_getres_cpu(clock_id, arg2);
        break;
    }

    if (arg2) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1};
        if (copy_to_user((void *)arg2, &ts, sizeof(ts))) {
            return (uint64_t)-EFAULT;
        }
    }
    return 0;
}

uint64_t sys_getrandom(uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint8_t *buffer = (uint8_t *)arg1;
    size_t get_len = (size_t)arg2;
    uint32_t flags = (uint32_t)arg3;
    uint64_t state;
    size_t copied = 0;
    uint8_t chunk[256];

    (void)flags;

    if (get_len == 0)
        return 0;
    if (get_len > 1024 * 1024)
        return (uint64_t)-EINVAL;
    if (!buffer || check_user_overflow((uint64_t)buffer, get_len))
        return (uint64_t)-EFAULT;

    state = nano_time() ^ get_counter();
    if (current_task)
        state ^= current_task->pid ^ ((uint64_t)current_task->cpu_id << 32);
    if (state == 0)
        state = 0x9e3779b97f4a7c15ULL;

    while (copied < get_len) {
        size_t todo = MIN(sizeof(chunk), get_len - copied);
        size_t offset = 0;

        while (offset < todo) {
            uint64_t value = getrandom_next(&state);
            size_t take = MIN(sizeof(value), todo - offset);
            memcpy(chunk + offset, &value, take);
            offset += take;
        }

        if (copy_to_user(buffer + copied, chunk, todo))
            return (uint64_t)-EFAULT;
        copied += todo;
    }

    return get_len;
}

uint64_t sys_uname(uint64_t arg1) {
    struct utsname utsname;
    task_t *task = current_task;
    task_uts_namespace_t *uts_ns =
        (task && task->nsproxy) ? task->nsproxy->uts_ns : NULL;
    const char *nodename = uts_ns ? uts_ns->nodename : "naos";
    const char *machine = uts_ns ? uts_ns->machine : "riscv64";
    const char *sysname = uts_ns ? uts_ns->sysname : "naos";
    const char *release = uts_ns ? uts_ns->release : BUILD_VERSION;
    const char *version = uts_ns ? uts_ns->version : BUILD_VERSION;

    memset(&utsname, 0, sizeof(utsname));
    strcpy(utsname.nodename, nodename);
    strcpy(utsname.machine, machine);
    strcpy(utsname.sysname, sysname);
    strcpy(utsname.release, release);
    strcpy(utsname.version, version);
    strcpy(utsname.domainname, "localdomain");

    return copy_to_user((void *)arg1, &utsname, sizeof(utsname))
               ? (uint64_t)-EFAULT
               : 0;
}

static uint64_t sys_gettimeofday_rv(uint64_t tv, uint64_t tz, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)tz;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;

    if (!tv)
        return 0;

    rtc_realtime_t now;
    rtc_read_realtime(&now);
    struct timeval value = {
        .tv_sec = (long)now.sec,
        .tv_usec = (long)(now.nsec / 1000),
    };
    return copy_to_user((void *)tv, &value, sizeof(value)) ? (uint64_t)-EFAULT
                                                           : 0;
}

uint64_t sys_newuname(uint64_t arg1) {
    bool fake_linux = true;
    task_t *task = current_task;
    if (task &&
        (strstr(task->name, "uname") || strstr(task->name, "fastfetch") ||
         strstr(task->name, "neofetch")))
        fake_linux = false;

    struct utsname utsname;
    memset(&utsname, 0, sizeof(utsname));
    task_uts_namespace_t *uts_ns =
        (task && task->nsproxy) ? task->nsproxy->uts_ns : NULL;
    const char *nodename = uts_ns ? uts_ns->nodename : "aether";
    const char *machine = uts_ns ? uts_ns->machine : "riscv64";
    const char *sysname = uts_ns ? uts_ns->sysname : "na-kernel";
    const char *release = uts_ns ? uts_ns->release : BUILD_VERSION;
    const char *version = uts_ns ? uts_ns->version : BUILD_VERSION;

    strcpy(utsname.nodename, nodename);
    strcpy(utsname.machine, machine);
    if (fake_linux) {
        strcpy(utsname.sysname, "Linux");
        strcpy(utsname.release, "6.0.0-aether");
        strcpy(utsname.version,
               "#1 SMP PREEMPT_DYNAMIC " __DATE__ " " __TIME__);
    } else {
        strcpy(utsname.sysname, sysname);
        strcpy(utsname.release, release);
        strcpy(utsname.version, version);
    }
    if (copy_to_user((void *)arg1, &utsname, sizeof(utsname))) {
        return (uint64_t)-EFAULT;
    }
    return 0;
}

syscall_handle_t syscall_handlers[MAX_SYSCALL_NUM] = {NULL};

static void regist_syscall_handler(int nr, syscall_handle_t handler) {
    syscall_handlers[nr] = handler;
}

void syscall_handler_init() {
    memset(syscall_handlers, 0, MAX_SYSCALL_NUM);

    syscall_handlers[SYS_READ] = (syscall_handle_t)sys_read;
    syscall_handlers[SYS_WRITE] = (syscall_handle_t)sys_write;
    syscall_handlers[SYS_CLOSE] = (syscall_handle_t)sys_close;
    syscall_handlers[SYS_LSEEK] = (syscall_handle_t)sys_lseek;
    syscall_handlers[SYS_MMAP] = (syscall_handle_t)sys_mmap;
    syscall_handlers[SYS_MPROTECT] = (syscall_handle_t)sys_mprotect;
    syscall_handlers[SYS_MUNMAP] = (syscall_handle_t)sys_munmap;
    syscall_handlers[SYS_BRK] = (syscall_handle_t)sys_brk;
    syscall_handlers[SYS_RT_SIGACTION] = (syscall_handle_t)sys_sigaction;
    syscall_handlers[SYS_RT_SIGPROCMASK] = (syscall_handle_t)sys_sigprocmask;
    syscall_handlers[SYS_RT_SIGRETURN] = (syscall_handle_t)sys_sigreturn;
    syscall_handlers[SYS_IOCTL] = (syscall_handle_t)sys_ioctl;
    syscall_handlers[SYS_PREAD64] = (syscall_handle_t)sys_pread64;
    syscall_handlers[SYS_PWRITE64] = (syscall_handle_t)sys_pwrite64;
    syscall_handlers[SYS_READV] = (syscall_handle_t)sys_readv;
    syscall_handlers[SYS_WRITEV] = (syscall_handle_t)sys_writev;
    syscall_handlers[SYS_SCHED_YIELD] = (syscall_handle_t)sys_yield;
    syscall_handlers[SYS_MREMAP] = (syscall_handle_t)sys_mremap;
    syscall_handlers[SYS_MSYNC] = (syscall_handle_t)sys_msync;
    syscall_handlers[SYS_MINCORE] = (syscall_handle_t)sys_mincore;
    syscall_handlers[SYS_MADVISE] = (syscall_handle_t)sys_madvise;
    syscall_handlers[SYS_SHMGET] = (syscall_handle_t)sys_shmget;
    syscall_handlers[SYS_SHMAT] = (syscall_handle_t)sys_shmat;
    syscall_handlers[SYS_SHMCTL] = (syscall_handle_t)sys_shmctl;
    syscall_handlers[SYS_DUP] = (syscall_handle_t)sys_dup;
    syscall_handlers[SYS_NANOSLEEP] = (syscall_handle_t)sys_nanosleep;
    // syscall_handlers[SYS_GETITIMER] = (syscall_handle_t)sys_getitimer;
    syscall_handlers[SYS_SETITIMER] = (syscall_handle_t)sys_setitimer;
    syscall_handlers[SYS_GETPID] = (syscall_handle_t)sys_getpid;
    syscall_handlers[SYS_SENDFILE64] = (syscall_handle_t)sys_sendfile;
    syscall_handlers[SYS_SOCKET] = (syscall_handle_t)sys_socket;
    syscall_handlers[SYS_CONNECT] = (syscall_handle_t)sys_connect;
    syscall_handlers[SYS_ACCEPT] = (syscall_handle_t)sys_accept;
    syscall_handlers[SYS_SENDTO] = (syscall_handle_t)sys_send;
    syscall_handlers[SYS_RECVFROM] = (syscall_handle_t)sys_recv;
    syscall_handlers[SYS_SENDMSG] = (syscall_handle_t)sys_sendmsg;
    syscall_handlers[SYS_RECVMSG] = (syscall_handle_t)sys_recvmsg;
    syscall_handlers[SYS_SHUTDOWN] = (syscall_handle_t)sys_shutdown;
    syscall_handlers[SYS_BIND] = (syscall_handle_t)sys_bind;
    syscall_handlers[SYS_LISTEN] = (syscall_handle_t)sys_listen;
    syscall_handlers[SYS_GETSOCKNAME] = (syscall_handle_t)sys_getsockname;
    syscall_handlers[SYS_GETPEERNAME] = (syscall_handle_t)sys_getpeername;
    syscall_handlers[SYS_SOCKETPAIR] = (syscall_handle_t)sys_socketpair;
    syscall_handlers[SYS_SETSOCKOPT] = (syscall_handle_t)sys_setsockopt;
    syscall_handlers[SYS_GETSOCKOPT] = (syscall_handle_t)sys_getsockopt;
    syscall_handlers[SYS_CLONE] = (syscall_handle_t)sys_clone;
    syscall_handlers[SYS_EXECVE] = (syscall_handle_t)task_execve;
    syscall_handlers[SYS_EXIT] = (syscall_handle_t)task_exit_thread;
    syscall_handlers[SYS_WAIT4] = (syscall_handle_t)sys_wait4;
    syscall_handlers[SYS_KILL] = (syscall_handle_t)sys_kill;
    // syscall_handlers[SYS_SEMGET] = (syscall_handle_t)sys_semget;
    // syscall_handlers[SYS_SEMOP] = (syscall_handle_t)sys_semop;
    // syscall_handlers[SYS_SEMCTL] = (syscall_handle_t)sys_semctl;
    syscall_handlers[SYS_SHMDT] = (syscall_handle_t)sys_shmdt;
    // syscall_handlers[SYS_MSGGET] = (syscall_handle_t)sys_msgget;
    // syscall_handlers[SYS_MSGSND] = (syscall_handle_t)sys_msgsnd;
    // syscall_handlers[SYS_MSGRCV] = (syscall_handle_t)sys_msgrcv;
    // syscall_handlers[SYS_MSGCTL] = (syscall_handle_t)sys_msgctl;
    syscall_handlers[SYS_FCNTL] = (syscall_handle_t)sys_fcntl;
    syscall_handlers[SYS_FLOCK] = (syscall_handle_t)sys_flock;
    syscall_handlers[SYS_FSYNC] = (syscall_handle_t)sys_fsync;
    syscall_handlers[SYS_FDATASYNC] = (syscall_handle_t)sys_fdatasync;
    syscall_handlers[SYS_TRUNCATE] = (syscall_handle_t)sys_truncate;
    syscall_handlers[SYS_FTRUNCATE] = (syscall_handle_t)sys_ftruncate;
    syscall_handlers[SYS_GETDENTS64] = (syscall_handle_t)sys_getdents64;
    syscall_handlers[SYS_GETCWD] = (syscall_handle_t)sys_getcwd;
    syscall_handlers[SYS_CHDIR] = (syscall_handle_t)sys_chdir;
    syscall_handlers[SYS_FCHDIR] = (syscall_handle_t)sys_fchdir;
    syscall_handlers[SYS_FCHOWN] = (syscall_handle_t)sys_fchown;
    syscall_handlers[SYS_UMASK] = (syscall_handle_t)sys_umask;
    syscall_handlers[SYS_GETTIMEOFDAY] = (syscall_handle_t)sys_gettimeofday_rv;
    syscall_handlers[SYS_GETRLIMIT] = (syscall_handle_t)sys_get_rlimit;
    syscall_handlers[SYS_GETRUSAGE] = (syscall_handle_t)sys_getrusage;
    syscall_handlers[SYS_SYSINFO] = (syscall_handle_t)sys_sysinfo;
    syscall_handlers[SYS_TIMES] = (syscall_handle_t)sys_times;
    syscall_handlers[SYS_PTRACE] = (syscall_handle_t)sys_ptrace;
    syscall_handlers[SYS_GETUID] = (syscall_handle_t)sys_getuid;
    syscall_handlers[SYS_SYSLOG] = (syscall_handle_t)sys_syslog;
    syscall_handlers[SYS_GETGID] = (syscall_handle_t)sys_getgid;
    syscall_handlers[SYS_SETUID] = (syscall_handle_t)sys_setuid;
    syscall_handlers[SYS_SETGID] = (syscall_handle_t)sys_setgid;
    syscall_handlers[SYS_GETEUID] = (syscall_handle_t)sys_geteuid;
    syscall_handlers[SYS_GETEGID] = (syscall_handle_t)sys_getegid;
    syscall_handlers[SYS_SETPGID] = (syscall_handle_t)sys_setpgid;
    syscall_handlers[SYS_GETPPID] = (syscall_handle_t)sys_getppid;
    syscall_handlers[SYS_SETSID] = (syscall_handle_t)sys_setsid;
    syscall_handlers[SYS_GETGROUPS] = (syscall_handle_t)sys_getgroups;
    syscall_handlers[SYS_SETGROUPS] = (syscall_handle_t)sys_setgroups;
    syscall_handlers[SYS_SETREUID] = (syscall_handle_t)sys_setreuid;
    syscall_handlers[SYS_SETREGID] = (syscall_handle_t)sys_setregid;
    syscall_handlers[SYS_SETRESUID_] = (syscall_handle_t)sys_setresuid;
    syscall_handlers[SYS_GETRESUID_] = (syscall_handle_t)sys_getresuid;
    syscall_handlers[SYS_SETRESUID] = (syscall_handle_t)sys_setresuid;
    syscall_handlers[SYS_GETRESUID] = (syscall_handle_t)sys_getresuid;
    syscall_handlers[SYS_GETPGID] = (syscall_handle_t)sys_getpgid;
    syscall_handlers[SYS_SETFSUID] = (syscall_handle_t)sys_setfsuid;
    syscall_handlers[SYS_SETFSUID_] = (syscall_handle_t)sys_setfsuid;
    syscall_handlers[SYS_GETSID] = (syscall_handle_t)sys_getsid;
    syscall_handlers[SYS_NEWUNAME] = (syscall_handle_t)sys_newuname;
    syscall_handlers[SYS_CAPGET] = (syscall_handle_t)sys_capget;
    syscall_handlers[SYS_CAPSET] = (syscall_handle_t)sys_capset;
    syscall_handlers[SYS_RT_SIGPENDING] = (syscall_handle_t)sys_rt_sigpending;
    syscall_handlers[SYS_RT_SIGTIMEDWAIT_TIME32] =
        (syscall_handle_t)sys_rt_sigtimedwait;
    syscall_handlers[SYS_RT_SIGQUEUEINFO] =
        (syscall_handle_t)sys_rt_sigqueueinfo;
    syscall_handlers[SYS_RT_SIGSUSPEND] = (syscall_handle_t)sys_sigsuspend;
    syscall_handlers[SYS_SIGALTSTACK] = (syscall_handle_t)sys_sigaltstack;
    // syscall_handlers[SYS_USELIB] = (syscall_handle_t)sys_uselib;
    syscall_handlers[SYS_PERSONALITY] = (syscall_handle_t)sys_personality;
    // syscall_handlers[SYS_USTAT] = (syscall_handle_t)sys_ustat;
    syscall_handlers[SYS_STATFS] = (syscall_handle_t)sys_statfs;
    syscall_handlers[SYS_FSTATFS] = (syscall_handle_t)sys_fstatfs;
    // syscall_handlers[SYS_SYSFS] = (syscall_handle_t)sys_sysfs;
    syscall_handlers[SYS_GETPRIORITY] = (syscall_handle_t)sys_getpriority;
    syscall_handlers[SYS_SETPRIORITY] = (syscall_handle_t)sys_setpriority;
    syscall_handlers[SYS_SCHED_SETPARAM] = (syscall_handle_t)sys_sched_setparam;
    syscall_handlers[SYS_SCHED_GETPARAM] = (syscall_handle_t)sys_sched_getparam;
    syscall_handlers[SYS_SCHED_SETSCHEDULER] =
        (syscall_handle_t)sys_sched_setscheduler;
    syscall_handlers[SYS_SCHED_GETSCHEDULER] =
        (syscall_handle_t)sys_sched_getscheduler;
    // syscall_handlers[SYS_SCHED_GET_PRIORITY_MAX] =
    //     (syscall_handle_t)sys_sched_get_priority_max;
    // syscall_handlers[SYS_SCHED_GET_PRIORITY_MIN] =
    //     (syscall_handle_t)sys_sched_get_priority_min;
    // syscall_handlers[SYS_SCHED_RR_GET_INTERVAL] =
    //     (syscall_handle_t)sys_sched_rr_get_interval;
    syscall_handlers[SYS_MLOCK] = (syscall_handle_t)sys_mlock;
    syscall_handlers[SYS_MUNLOCK] = (syscall_handle_t)sys_munlock;
    syscall_handlers[SYS_MLOCKALL] = (syscall_handle_t)sys_mlockall;
    syscall_handlers[SYS_MUNLOCKALL] = (syscall_handle_t)sys_munlockall;
    // syscall_handlers[SYS_VHANGUP] = (syscall_handle_t)sys_vhangup;
    // syscall_handlers[SYS_MODIFY_LDT] = (syscall_handle_t)sys_modify_ldt;
    syscall_handlers[SYS_PIVOT_ROOT] = (syscall_handle_t)sys_pivot_root;
    // syscall_handlers[SYS__SYSCTL] = (syscall_handle_t)sys__sysctl;
    syscall_handlers[SYS_PRCTL] = (syscall_handle_t)sys_prctl;
    // syscall_handlers[SYS_ADJTIMEX] = (syscall_handle_t)sys_adjtimex;
    // syscall_handlers[SYS_SETRLIMIT] = (syscall_handle_t)sys_setrlimit;
    syscall_handlers[SYS_CHROOT] = (syscall_handle_t)sys_chroot;
    syscall_handlers[SYS_SYNC] = (syscall_handle_t)dummy_syscall_handler;
    // syscall_handlers[SYS_ACCT] = (syscall_handle_t)sys_acct;
    // syscall_handlers[SYS_SETTIMEOFDAY] =
    // (syscall_handle_t)sys_settimeofday_rv;
    syscall_handlers[SYS_MOUNT] = (syscall_handle_t)sys_mount;
    syscall_handlers[SYS_UMOUNT] = (syscall_handle_t)sys_umount2;
    // syscall_handlers[SYS_SWAPON] = (syscall_handle_t)sys_swapon;
    // syscall_handlers[SYS_SWAPOFF] = (syscall_handle_t)sys_swapoff;
    syscall_handlers[SYS_REBOOT] = (syscall_handle_t)sys_reboot;
    // syscall_handlers[SYS_SETHOSTNAME] = (syscall_handle_t)sys_sethostname;
    // syscall_handlers[SYS_SETDOMAINNAME] =
    // (syscall_handle_t)sys_setdomainname; syscall_handlers[SYS_IOPL] =
    // (syscall_handle_t)sys_iopl; syscall_handlers[SYS_IOPERM] =
    // (syscall_handle_t)sys_ioperm; syscall_handlers[SYS_CREATE_MODULE] =
    // (syscall_handle_t)sys_create_module; syscall_handlers[SYS_INIT_MODULE] =
    // (syscall_handle_t)sys_init_module; syscall_handlers[SYS_DELETE_MODULE] =
    // (syscall_handle_t)sys_delete_module;
    // syscall_handlers[SYS_GET_KERNEL_SYMS] =
    //     (syscall_handle_t)sys_get_kernel_syms;
    // syscall_handlers[SYS_QUERY_MODULE] = (syscall_handle_t)sys_query_module;
    // syscall_handlers[SYS_QUOTACTL] = (syscall_handle_t)sys_quotactl;
    // syscall_handlers[SYS_NFSSERVCTL] = (syscall_handle_t)sys_nfsservctl;
    // syscall_handlers[SYS_GETPMSG] = (syscall_handle_t)sys_getpmsg;
    // syscall_handlers[SYS_PUTPMSG] = (syscall_handle_t)sys_putpmsg;
    // syscall_handlers[SYS_AFS_SYSCALL] = (syscall_handle_t)sys_afs_syscall;
    // syscall_handlers[SYS_TUXCALL] = (syscall_handle_t)sys_tuxcall;
    // syscall_handlers[SYS_SECURITY] = (syscall_handle_t)sys_security;
    syscall_handlers[SYS_GETTID] = (syscall_handle_t)sys_gettid;
    syscall_handlers[SYS_READAHEAD] = (syscall_handle_t)sys_readahead;
    syscall_handlers[SYS_SETXATTR] = (syscall_handle_t)sys_setxattr;
    syscall_handlers[SYS_LSETXATTR] = (syscall_handle_t)sys_lsetxattr;
    syscall_handlers[SYS_FSETXATTR] = (syscall_handle_t)sys_fsetxattr;
    syscall_handlers[SYS_GETXATTR] = (syscall_handle_t)sys_getxattr;
    syscall_handlers[SYS_LGETXATTR] = (syscall_handle_t)sys_lgetxattr;
    syscall_handlers[SYS_FGETXATTR] = (syscall_handle_t)sys_fgetxattr;
    syscall_handlers[SYS_LISTXATTR] = (syscall_handle_t)sys_listxattr;
    syscall_handlers[SYS_LLISTXATTR] = (syscall_handle_t)sys_llistxattr;
    syscall_handlers[SYS_FLISTXATTR] = (syscall_handle_t)sys_flistxattr;
    syscall_handlers[SYS_REMOVEXATTR] = (syscall_handle_t)sys_removexattr;
    syscall_handlers[SYS_LREMOVEXATTR] = (syscall_handle_t)sys_lremovexattr;
    syscall_handlers[SYS_FREMOVEXATTR] = (syscall_handle_t)sys_fremovexattr;
    syscall_handlers[SYS_TKILL] = (syscall_handle_t)sys_tkill;
    // syscall_handlers[SYS_TIME] = (syscall_handle_t)sys_time;
    syscall_handlers[SYS_FUTEX_] = (syscall_handle_t)sys_futex;
    syscall_handlers[SYS_FUTEX] = (syscall_handle_t)sys_futex;
    syscall_handlers[SYS_SCHED_SETAFFINITY] =
        (syscall_handle_t)sys_sched_setaffinity;
    syscall_handlers[SYS_SCHED_GETAFFINITY] =
        (syscall_handle_t)sys_sched_getaffinity;
    // syscall_handlers[SYS_SET_THREAD_AREA] =
    //     (syscall_handle_t)sys_set_thread_area;
    // syscall_handlers[SYS_IO_SETUP] = (syscall_handle_t)sys_io_setup;
    // syscall_handlers[SYS_IO_DESTROY] = (syscall_handle_t)sys_io_destroy;
    // syscall_handlers[SYS_IO_GETEVENTS] = (syscall_handle_t)sys_io_getevents;
    // syscall_handlers[SYS_IO_SUBMIT] = (syscall_handle_t)sys_io_submit;
    // syscall_handlers[SYS_IO_CANCEL] = (syscall_handle_t)sys_io_cancel;
    // syscall_handlers[SYS_GET_THREAD_AREA] =
    //     (syscall_handle_t)sys_get_thread_area;
    // syscall_handlers[SYS_LOOKUP_DCOOKIE] =
    // (syscall_handle_t)sys_lookup_dcookie;
    syscall_handlers[SYS_EPOLL_CREATE1] = (syscall_handle_t)sys_epoll_create;
    // syscall_handlers[SYS_EPOLL_CTL_OLD] =
    // (syscall_handle_t)sys_epoll_ctl_old; syscall_handlers[SYS_EPOLL_WAIT_OLD]
    // = (syscall_handle_t)sys_epoll_wait_old;
    // syscall_handlers[SYS_REMAP_FILE_PAGES] =
    //     (syscall_handle_t)sys_remap_file_pages;
    syscall_handlers[SYS_GETDENTS64] = (syscall_handle_t)sys_getdents64;
    syscall_handlers[SYS_SET_TID_ADDRESS] =
        (syscall_handle_t)sys_set_tid_address;
    // syscall_handlers[SYS_RESTART_SYSCALL] =
    //     (syscall_handle_t)sys_restart_syscall;
    // syscall_handlers[SYS_SEMTIMEDOP] = (syscall_handle_t)sys_semtimedop;
    syscall_handlers[SYS_FADVISE64_64] = (syscall_handle_t)sys_fadvise64;
    syscall_handlers[SYS_TIMER_CREATE] = (syscall_handle_t)sys_timer_create;
    syscall_handlers[SYS_CLOCK_GETTIME_TIME32] =
        (syscall_handle_t)sys_clock_gettime;
    syscall_handlers[SYS_TIMER_SETTIME] = (syscall_handle_t)sys_timer_settime;
    syscall_handlers[SYS_TIMER_GETTIME] = (syscall_handle_t)sys_timer_gettime;
    syscall_handlers[SYS_TIMER_GETOVERRUN] =
        (syscall_handle_t)sys_timer_getoverrun;
    syscall_handlers[SYS_TIMER_DELETE] = (syscall_handle_t)sys_timer_delete;
    // syscall_handlers[SYS_CLOCK_SETTIME] =
    // (syscall_handle_t)sys_clock_settime;
    syscall_handlers[SYS_CLOCK_GETTIME_TIME32] =
        (syscall_handle_t)sys_clock_gettime;
    syscall_handlers[SYS_CLOCK_GETTIME] = (syscall_handle_t)sys_clock_gettime;
    syscall_handlers[SYS_CLOCK_GETRES_TIME32] =
        (syscall_handle_t)sys_clock_getres;
    syscall_handlers[SYS_CLOCK_GETRES] = (syscall_handle_t)sys_clock_getres;
    syscall_handlers[SYS_CLOCK_NANOSLEEP_TIME32] =
        (syscall_handle_t)sys_clock_nanosleep;
    syscall_handlers[SYS_CLOCK_NANOSLEEP] =
        (syscall_handle_t)sys_clock_nanosleep;
    syscall_handlers[SYS_EXIT_GROUP] = (syscall_handle_t)task_exit;
    syscall_handlers[SYS_EPOLL_CTL] = (syscall_handle_t)sys_epoll_ctl;
    syscall_handlers[SYS_TGKILL] = (syscall_handle_t)sys_tgkill;
    // syscall_handlers[SYS_VSERVER] = (syscall_handle_t)sys_vserver;
    // syscall_handlers[SYS_MBIND] = (syscall_handle_t)sys_mbind;
    regist_syscall_handler(SYS_SET_MEMPOLICY,
                           (syscall_handle_t)sys_set_mempolicy);
    regist_syscall_handler(SYS_GET_MEMPOLICY,
                           (syscall_handle_t)sys_get_mempolicy);
    // syscall_handlers[SYS_MQ_OPEN] =
    // (syscall_handle_t)sys_mq_open; syscall_handlers[SYS_MQ_UNLINK] =
    // (syscall_handle_t)sys_mq_unlink; syscall_handlers[SYS_MQ_TIMEDSEND] =
    // (syscall_handle_t)sys_mq_timedsend; syscall_handlers[SYS_MQ_TIMEDRECEIVE]
    // =
    //     (syscall_handle_t)sys_mq_timedreceive;
    // syscall_handlers[SYS_MQ_NOTIFY] = (syscall_handle_t)sys_mq_notify;
    // syscall_handlers[SYS_MQ_GETSETATTR] =
    // (syscall_handle_t)sys_mq_getsetattr; syscall_handlers[SYS_KEXEC_LOAD] =
    // (syscall_handle_t)sys_kexec_load;
    syscall_handlers[SYS_WAITID] = (syscall_handle_t)sys_waitid;
    syscall_handlers[SYS_ADD_KEY] = (syscall_handle_t)sys_add_key;
    syscall_handlers[SYS_REQUEST_KEY] = (syscall_handle_t)sys_request_key;
    syscall_handlers[SYS_KEYCTL] = (syscall_handle_t)sys_keyctl;
    syscall_handlers[SYS_IOPRIO_SET] = (syscall_handle_t)sys_ioprio_set;
    syscall_handlers[SYS_IOPRIO_GET] = (syscall_handle_t)sys_ioprio_get;
    syscall_handlers[SYS_INOTIFY_INIT1] = (syscall_handle_t)sys_inotify_init1;
    syscall_handlers[SYS_INOTIFY_ADD_WATCH] =
        (syscall_handle_t)sys_inotify_add_watch;
    syscall_handlers[SYS_INOTIFY_RM_WATCH] =
        (syscall_handle_t)sys_inotify_rm_watch;
    // syscall_handlers[SYS_MIGRATE_PAGES] =
    // (syscall_handle_t)sys_migrate_pages;
    syscall_handlers[SYS_RISCV_HWPROBE] = (syscall_handle_t)sys_riscv_hwprobe;
    syscall_handlers[SYS_RISCV_FLUSH_ICACHE] =
        (syscall_handle_t)sys_riscv_flush_icache;
    syscall_handlers[SYS_OPENAT] = (syscall_handle_t)sys_openat;
    syscall_handlers[SYS_MKDIRAT] = (syscall_handle_t)sys_mkdirat;
    syscall_handlers[SYS_MKNODAT] = (syscall_handle_t)sys_mknodat;
    syscall_handlers[SYS_FCHOWNAT] = (syscall_handle_t)sys_fchownat;
    syscall_handlers[SYS_NEWFSTATAT] = (syscall_handle_t)sys_newfstatat;
    syscall_handlers[SYS_NEWFSTAT] = (syscall_handle_t)sys_fstat;
    syscall_handlers[SYS_UNLINKAT] = (syscall_handle_t)sys_unlinkat;
    syscall_handlers[SYS_RENAMEAT] = (syscall_handle_t)sys_renameat;
    syscall_handlers[SYS_LINKAT] = (syscall_handle_t)sys_linkat;
    syscall_handlers[SYS_SYMLINKAT] = (syscall_handle_t)sys_symlinkat;
    syscall_handlers[SYS_READLINKAT] = (syscall_handle_t)sys_readlinkat;
    syscall_handlers[SYS_FCHMOD] = (syscall_handle_t)sys_fchmod;
    syscall_handlers[SYS_FCHMODAT] = (syscall_handle_t)sys_fchmodat;
    syscall_handlers[SYS_FACCESSAT] = (syscall_handle_t)sys_faccessat;
    syscall_handlers[SYS_UNSHARE] = (syscall_handle_t)sys_unshare;
    syscall_handlers[SYS_SET_ROBUST_LIST] =
        (syscall_handle_t)sys_set_robust_list;
    syscall_handlers[SYS_GET_ROBUST_LIST] =
        (syscall_handle_t)sys_get_robust_list;
    syscall_handlers[SYS_SPLICE] = (syscall_handle_t)sys_splice;
    // syscall_handlers[SYS_TEE] = (syscall_handle_t)sys_tee;
    // syscall_handlers[SYS_SYNC_FILE_RANGE] =
    //     (syscall_handle_t)sys_sync_file_range;
    syscall_handlers[SYS_VMSPLICE] = (syscall_handle_t)sys_vmsplice;
    // syscall_handlers[SYS_MOVE_PAGES] = (syscall_handle_t)sys_move_pages;
    syscall_handlers[SYS_UTIMENSAT_] = (syscall_handle_t)sys_utimensat;
    syscall_handlers[SYS_UTIMENSAT] = (syscall_handle_t)sys_utimensat;
    syscall_handlers[SYS_EPOLL_PWAIT] = (syscall_handle_t)sys_epoll_pwait;
    syscall_handlers[SYS_TIMERFD_CREATE] = (syscall_handle_t)sys_timerfd_create;
    syscall_handlers[SYS_FALLOCATE] = (syscall_handle_t)sys_fallocate;
    syscall_handlers[SYS_TIMERFD_SETTIME_] =
        (syscall_handle_t)sys_timerfd_settime;
    syscall_handlers[SYS_TIMERFD_SETTIME] =
        (syscall_handle_t)sys_timerfd_settime;
    // syscall_handlers[SYS_TIMERFD_GETTIME] =
    //     (syscall_handle_t)sys_timerfd_gettime;
    syscall_handlers[SYS_ACCEPT4] = (syscall_handle_t)sys_accept;
    syscall_handlers[SYS_SIGNALFD4] = (syscall_handle_t)sys_signalfd4;
    syscall_handlers[SYS_EVENTFD2] = (syscall_handle_t)sys_eventfd2;
    syscall_handlers[SYS_EPOLL_CREATE1] = (syscall_handle_t)sys_epoll_create1;
    syscall_handlers[SYS_DUP3] = (syscall_handle_t)sys_dup3;
    syscall_handlers[SYS_PIPE2] = (syscall_handle_t)sys_pipe;
    syscall_handlers[SYS_PREADV] = (syscall_handle_t)sys_preadv;
    syscall_handlers[SYS_PWRITEV] = (syscall_handle_t)sys_pwritev;
    syscall_handlers[SYS_PPOLL_TIME32] = (syscall_handle_t)sys_ppoll;
    syscall_handlers[SYS_PSELECT6_TIME32] = (syscall_handle_t)sys_pselect6;
    // syscall_handlers[SYS_RT_TGSIGQUEUEINFO] =
    //     (syscall_handle_t)sys_rt_tgsigqueueinfo;
    // syscall_handlers[SYS_PERF_EVENT_OPEN] =
    //     (syscall_handle_t)sys_perf_event_open;
    syscall_handlers[SYS_RECVMMSG_TIME32] = (syscall_handle_t)sys_recvmmsg;
    // syscall_handlers[SYS_FANOTIFY_INIT] =
    syscall_handlers[SYS_FANOTIFY_INIT] = (syscall_handle_t)sys_fanotify_init;
    syscall_handlers[SYS_FANOTIFY_MARK] = (syscall_handle_t)sys_fanotify_mark;
    syscall_handlers[SYS_PRLIMIT64] = (syscall_handle_t)sys_prlimit64;
    syscall_handlers[SYS_NAME_TO_HANDLE_AT] =
        (syscall_handle_t)sys_name_to_handle_at;
    syscall_handlers[SYS_OPEN_BY_HANDLE_AT] =
        (syscall_handle_t)sys_open_by_handle_at;
    // syscall_handlers[SYS_CLOCK_ADJTIME] =
    // (syscall_handle_t)sys_clock_adjtime;
    syscall_handlers[SYS_SYNCFS] = (syscall_handle_t)sys_syncfs;
    syscall_handlers[SYS_SENDMMSG] = (syscall_handle_t)sys_sendmmsg;
    syscall_handlers[SYS_SETNS] = (syscall_handle_t)sys_setns;
    syscall_handlers[SYS_GETCPU] = (syscall_handle_t)sys_getcpu;
    syscall_handlers[SYS_PROCESS_VM_READV] =
        (syscall_handle_t)sys_process_vm_readv;
    syscall_handlers[SYS_PROCESS_VM_WRITEV] =
        (syscall_handle_t)sys_process_vm_writev;
    syscall_handlers[SYS_KCMP] = (syscall_handle_t)sys_kcmp;
    // syscall_handlers[SYS_FINIT_MODULE] = (syscall_handle_t)sys_finit_module;
    // syscall_handlers[SYS_SCHED_SETATTR] =
    // (syscall_handle_t)sys_sched_setattr; syscall_handlers[SYS_SCHED_GETATTR]
    // = (syscall_handle_t)sys_sched_getattr;
    syscall_handlers[SYS_RENAMEAT2] = (syscall_handle_t)sys_renameat2;
    // syscall_handlers[SYS_SECCOMP] = (syscall_handle_t)sys_seccomp;
    syscall_handlers[SYS_GETRANDOM] = (syscall_handle_t)sys_getrandom;
    syscall_handlers[SYS_MEMFD_CREATE] = (syscall_handle_t)sys_memfd_create;
    // syscall_handlers[SYS_KEXEC_FILE_LOAD] =
    //     (syscall_handle_t)sys_kexec_file_load;
    // syscall_handlers[SYS_BPF] = (syscall_handle_t)sys_bpf;
    syscall_handlers[SYS_EXECVEAT] = (syscall_handle_t)sys_execveat;
    // syscall_handlers[SYS_USERFAULTFD] = (syscall_handle_t)sys_userfaultfd;
    syscall_handlers[SYS_MEMBARRIER] = (syscall_handle_t)sys_membarrier;
    // syscall_handlers[SYS_MLOCK2] = (syscall_handle_t)sys_mlock2;
    syscall_handlers[SYS_COPY_FILE_RANGE] =
        (syscall_handle_t)sys_copy_file_range;
    syscall_handlers[SYS_PREADV2] = (syscall_handle_t)sys_preadv2;
    syscall_handlers[SYS_PWRITEV2] = (syscall_handle_t)sys_pwritev2;
    // syscall_handlers[SYS_PKEY_MPROTECT] =
    // (syscall_handle_t)sys_pkey_mprotect; syscall_handlers[SYS_PKEY_ALLOC] =
    // (syscall_handle_t)sys_pkey_alloc; syscall_handlers[SYS_PKEY_FREE] =
    // (syscall_handle_t)sys_pkey_free;
    syscall_handlers[SYS_STATX] = (syscall_handle_t)sys_statx;
    // syscall_handlers[SYS_IO_PGETEVENTS] =
    // (syscall_handle_t)sys_io_pgetevents;
    syscall_handlers[SYS_RSEQ] = (syscall_handle_t)sys_rseq;
    // syscall_handlers[SYS_PIDFD_SEND_SIGNAL] =
    //     (syscall_handle_t)sys_pidfd_send_signal;
    // syscall_handlers[SYS_IO_URING_SETUP] =
    // (syscall_handle_t)sys_io_uring_setup;
    // syscall_handlers[SYS_IO_URING_ENTER] =
    // (syscall_handle_t)sys_io_uring_enter;
    // syscall_handlers[SYS_IO_URING_REGISTER] =
    //     (syscall_handle_t)sys_io_uring_register;
    syscall_handlers[SYS_OPEN_TREE] = (syscall_handle_t)sys_open_tree;
    syscall_handlers[SYS_MOVE_MOUNT] = (syscall_handle_t)sys_move_mount;
    syscall_handlers[SYS_FSOPEN] = (syscall_handle_t)sys_fsopen;
    syscall_handlers[SYS_FSCONFIG] = (syscall_handle_t)sys_fsconfig;
    syscall_handlers[SYS_FSMOUNT] = (syscall_handle_t)sys_fsmount;
    syscall_handlers[SYS_FSPICK] = (syscall_handle_t)sys_fspick;
    syscall_handlers[SYS_PIDFD_OPEN] = (syscall_handle_t)sys_pidfd_open;
    syscall_handlers[SYS_CLONE3] = (syscall_handle_t)sys_clone3;
    syscall_handlers[SYS_CLOSE_RANGE] = (syscall_handle_t)sys_close_range;
    syscall_handlers[SYS_OPENAT2] = (syscall_handle_t)sys_openat2;
    // syscall_handlers[SYS_PIDFD_GETFD] = (syscall_handle_t)sys_pidfd_getfd;
    syscall_handlers[SYS_FACCESSAT2] = (syscall_handle_t)sys_faccessat2;
    // syscall_handlers[SYS_PROCESS_MADVISE] =
    //     (syscall_handle_t)sys_process_madvise;
    syscall_handlers[SYS_EPOLL_PWAIT2] = (syscall_handle_t)sys_epoll_pwait2;
    // syscall_handlers[SYS_MOUNT_SETATTR] =
    // (syscall_handle_t)sys_mount_setattr;
    // syscall_handlers[SYS_LANDLOCK_CREATE_RULESET] =
    //     (syscall_handle_t)sys_landlock_create_ruleset;
    // syscall_handlers[SYS_LANDLOCK_ADD_RULE] =
    //     (syscall_handle_t)sys_landlock_add_rule;
    // syscall_handlers[SYS_LANDLOCK_RESTRICT_SELF] =
    //     (syscall_handle_t)sys_landlock_restrict_self;
    // syscall_handlers[SYS_MEMFD_SECRET] = (syscall_handle_t)sys_memfd_secret;
    // syscall_handlers[SYS_PROCESS_MRELEASE] =
    //     (syscall_handle_t)sys_process_mrelease;
    // syscall_handlers[SYS_FUTEX_WAITV] = (syscall_handle_t)sys_futex_waitv;
    // syscall_handlers[SYS_SET_MEMPOLICY_HOME_NODE] =
    //     (syscall_handle_t)sys_set_mempolicy_home_node;
    // syscall_handlers[SYS_CACHESTAT] = (syscall_handle_t)sys_cachestat;
    // syscall_handlers[SYS_FCHMODAT2] = (syscall_handle_t)sys_fchmodat2;
}

void riscv64_do_syscall(struct pt_regs *frame) {
    uint64_t idx = frame->a7;
    uint64_t arg1 = frame->a0;
    uint64_t arg2 = frame->a1;
    uint64_t arg3 = frame->a2;
    uint64_t arg4 = frame->a3;
    uint64_t arg5 = frame->a4;
    uint64_t arg6 = frame->a5;
    task_t *self = current_task;

    if (!self) {
        frame->a0 = (uint64_t)-ENOSYS;
        frame->sepc += 4;
        return;
    }
    self->in_syscall = true;

    frame->a0 = self->last_syscall_ret;
    frame->syscallno = idx;
    ptrace_on_syscall_enter(frame);

    if (idx >= MAX_SYSCALL_NUM || !syscall_handlers[idx]) {
        frame->a0 = (uint64_t)-ENOSYS;
        goto done;
    }

    arch_enable_user_access();
    if (idx == SYS_CLONE) {
        frame->a0 = riscv64_sys_clone(frame, arg1, arg2, arg3, arg4, arg5);
    } else if (idx == SYS_CLONE3 || idx == SYS_RT_SIGRETURN) {
        special_syscall_handle_t handler =
            (special_syscall_handle_t)syscall_handlers[idx];
        frame->a0 = handler(frame, arg1, arg2, arg3, arg4, arg5, arg6);
    } else {
        frame->a0 = syscall_handlers[idx](arg1, arg2, arg3, arg4, arg5, arg6);
    }
    arch_disable_user_access();

done:
    self->last_syscall_ret = frame->a0;
    ptrace_on_syscall_exit(frame);
    if (idx != SYS_BRK && idx != SYS_RSEQ && idx != SYS_RISCV_HWPROBE &&
        frame->a0 == (uint64_t)-ENOSYS) {
        serial_fprintk("syscall %d not implemented\n", idx);
    }
    uint64_t next_sepc = frame->sepc + 4;
    bool restored_context = idx == SYS_RT_SIGRETURN;
    task_signal(frame);
    frame->syscallno = NO_SYSCALL;
    if (!restored_context && frame->sepc == next_sepc - 4) {
        frame->sepc = next_sepc;
    }
    self->in_syscall = false;
}
