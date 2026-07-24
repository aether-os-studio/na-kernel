#include <arch/arch.h>
#include <boot/boot.h>
#include <task/task.h>
#include <task/signal.h>
#include <mm/mm_syscall.h>
#include <task/futex.h>
#include <task/keyring.h>
#include <task/ptrace.h>
#include <task/task_syscall.h>
#include <drivers/rtc.h>
#include <net/net_syscall.h>

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

static uint64_t copy_timeval_to_user(uint64_t user_addr, uint64_t sec,
                                     uint64_t usec) {
    if (!user_addr)
        return 0;

    struct timeval tv = {
        .tv_sec = (long)sec,
        .tv_usec = (long)usec,
    };
    return copy_to_user((void *)user_addr, &tv, sizeof(tv)) ? (uint64_t)-EFAULT
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

static inline uint64_t getrandom_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

uint64_t sys_getrandom(uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint8_t *buffer = (uint8_t *)arg1;
    size_t get_len = (size_t)arg2;
    uint32_t flags = (uint32_t)arg3;
    uint64_t state;
    size_t copied = 0;
    uint8_t chunk[256];

    (void)flags;

    if (get_len == 0) {
        return 0;
    }
    if (get_len > 1024 * 1024) {
        return (uint64_t)-EINVAL;
    }

    if (!buffer || check_user_overflow((uint64_t)buffer, get_len)) {
        return (uint64_t)-EFAULT;
    }

    state = nano_time();
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

        if (copy_to_user(buffer + copied, chunk, todo)) {
            return (uint64_t)-EFAULT;
        }
        copied += todo;
    }

    return get_len;
}

uint64_t sys_clock_gettime(uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg3;
    clockid_t clock_id = (clockid_t)arg1;

    switch (clock_id) {
    case 1: // CLOCK_MONOTONIC
    case 6: // CLOCK_MONOTONIC_COARSE
    case 4: // CLOCK_MONOTONIC_RAW
    {
        uint64_t nano = nano_time();
        return copy_timespec_to_user(arg2, nano / 1000000000ULL,
                                     nano % 1000000000ULL);
    }
    case 7: // CLOCK_BOOTTIME
    {
        uint64_t nano = nano_time();
        return copy_timespec_to_user(arg2, nano / 1000000000ULL,
                                     nano % 1000000000ULL);
    }
    case 0: // CLOCK_REALTIME
    case 5: // CLOCK_REALTIME_COARSE
    {
        rtc_realtime_t now;
        rtc_read_realtime(&now);
        return copy_timespec_to_user(arg2, now.sec, now.nsec);
    }
    case 2: // CLOCK_PROCESS_CPUTIME_ID
    case 3: // CLOCK_THREAD_CPUTIME_ID
        return linux_clock_gettime_cpu(clock_id, arg2);
    default:
        if (linux_clockid_is_cpu(clock_id))
            return linux_clock_gettime_cpu(clock_id, arg2);
        return (uint64_t)-EINVAL;
    }
}

uint64_t sys_clock_getres(uint64_t arg1, uint64_t arg2) {
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

uint64_t sys_time(uint64_t arg1) {
    rtc_realtime_t now;
    rtc_read_realtime(&now);
    uint64_t timestamp = now.sec;
    if (arg1) {
        if (copy_to_user((void *)arg1, &timestamp, sizeof(timestamp))) {
            return (uint64_t)-EFAULT;
        }
    }
    return timestamp;
}

uint64_t sys_accept_normal(uint64_t arg1, struct sockaddr_un *arg2,
                           socklen_t *arg3) {
    return sys_accept(arg1, arg2, arg3, 0);
}

uint64_t sys_pipe_normal(uint64_t arg1) { return sys_pipe((int *)arg1, 0); }

uint64_t sys_gettimeofday(uint64_t arg1) {
    rtc_realtime_t now;
    rtc_read_realtime(&now);
    return copy_timeval_to_user(arg1, now.sec, now.nsec / 1000);
}

uint64_t sys_uname(uint64_t arg1) {
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
    const char *machine = uts_ns ? uts_ns->machine : "x86_64";
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

static uint64_t aarch64_sys_clone(struct pt_regs *frame, uint64_t flags,
                                  uint64_t newsp, uint64_t parent_tid,
                                  uint64_t tls, uint64_t child_tid,
                                  uint64_t unused) {
    (void)unused;
    return sys_clone(frame, flags, newsp, (int *)parent_tid, (int *)child_tid,
                     tls);
}

void regist_syscall_handler(int nr, syscall_handle_t handler) {
    syscall_handlers[nr] = handler;
}

/*
 * Syscall table maintenance notes:
 * - Every registered entry should target Linux userspace-visible semantics,
 *   even when the in-kernel implementation is still incomplete.
 * - Entries routed to dummy_syscall_handler intentionally surface -ENOSYS and
 *   should be treated as ABI placeholders.
 * - Commented-out registrations document syscall numbers that are still
 *   entirely unimplemented on this architecture.
 */
void syscall_handler_init() {
    regist_syscall_handler(SYS_READ, (syscall_handle_t)sys_read);
    regist_syscall_handler(SYS_WRITE, (syscall_handle_t)sys_write);
    regist_syscall_handler(SYS_CLOSE, (syscall_handle_t)sys_close);
    regist_syscall_handler(SYS_FSTAT, (syscall_handle_t)sys_fstat);
    regist_syscall_handler(SYS_FSTATAT, (syscall_handle_t)sys_newfstatat);
    regist_syscall_handler(SYS_LSEEK, (syscall_handle_t)sys_lseek);
    regist_syscall_handler(SYS_MMAP, (syscall_handle_t)sys_mmap);
    regist_syscall_handler(SYS_MPROTECT, (syscall_handle_t)sys_mprotect);
    regist_syscall_handler(SYS_MUNMAP, (syscall_handle_t)sys_munmap);
    regist_syscall_handler(SYS_BRK, (syscall_handle_t)sys_brk);
    regist_syscall_handler(SYS_RT_SIGACTION, (syscall_handle_t)sys_sigaction);
    regist_syscall_handler(SYS_RT_SIGPROCMASK,
                           (syscall_handle_t)sys_sigprocmask);
    regist_syscall_handler(SYS_RT_SIGRETURN, (syscall_handle_t)sys_sigreturn);
    regist_syscall_handler(SYS_IOCTL, (syscall_handle_t)sys_ioctl);
    regist_syscall_handler(SYS_PREAD64, (syscall_handle_t)sys_pread64);
    regist_syscall_handler(SYS_PWRITE64, (syscall_handle_t)sys_pwrite64);
    regist_syscall_handler(SYS_READV, (syscall_handle_t)sys_readv);
    regist_syscall_handler(SYS_WRITEV, (syscall_handle_t)sys_writev);
    regist_syscall_handler(SYS_SCHED_YIELD, (syscall_handle_t)sys_yield);
    regist_syscall_handler(SYS_MREMAP, (syscall_handle_t)sys_mremap);
    regist_syscall_handler(SYS_MSYNC, (syscall_handle_t)sys_msync);
    regist_syscall_handler(SYS_MINCORE, (syscall_handle_t)sys_mincore);
    regist_syscall_handler(SYS_MADVISE, (syscall_handle_t)sys_madvise);
    regist_syscall_handler(SYS_SHMGET, (syscall_handle_t)sys_shmget);
    regist_syscall_handler(SYS_SHMAT, (syscall_handle_t)sys_shmat);
    regist_syscall_handler(SYS_SHMCTL, (syscall_handle_t)sys_shmctl);
    regist_syscall_handler(SYS_DUP, (syscall_handle_t)sys_dup);
    regist_syscall_handler(SYS_NANOSLEEP, (syscall_handle_t)sys_nanosleep);
    // regist_syscall_handler(SYS_GETITIMER, (syscall_handle_t)sys_getitimer);
    regist_syscall_handler(SYS_SETITIMER, (syscall_handle_t)sys_setitimer);
    regist_syscall_handler(SYS_GETPID, (syscall_handle_t)sys_getpid);
    regist_syscall_handler(SYS_SENDFILE, (syscall_handle_t)sys_sendfile);
    regist_syscall_handler(SYS_SOCKET, (syscall_handle_t)sys_socket);
    regist_syscall_handler(SYS_CONNECT, (syscall_handle_t)sys_connect);
    regist_syscall_handler(SYS_ACCEPT, (syscall_handle_t)sys_accept_normal);
    regist_syscall_handler(SYS_SENDTO, (syscall_handle_t)sys_send);
    regist_syscall_handler(SYS_RECVFROM, (syscall_handle_t)sys_recv);
    regist_syscall_handler(SYS_SENDMSG, (syscall_handle_t)sys_sendmsg);
    regist_syscall_handler(SYS_RECVMSG, (syscall_handle_t)sys_recvmsg);
    regist_syscall_handler(SYS_SHUTDOWN, (syscall_handle_t)sys_shutdown);
    regist_syscall_handler(SYS_BIND, (syscall_handle_t)sys_bind);
    regist_syscall_handler(SYS_LISTEN, (syscall_handle_t)sys_listen);
    regist_syscall_handler(SYS_GETSOCKNAME, (syscall_handle_t)sys_getsockname);
    regist_syscall_handler(SYS_GETPEERNAME, (syscall_handle_t)sys_getpeername);
    regist_syscall_handler(SYS_SOCKETPAIR, (syscall_handle_t)sys_socketpair);
    regist_syscall_handler(SYS_SETSOCKOPT, (syscall_handle_t)sys_setsockopt);
    regist_syscall_handler(SYS_GETSOCKOPT, (syscall_handle_t)sys_getsockopt);
    regist_syscall_handler(SYS_CLONE, (syscall_handle_t)sys_clone);
    regist_syscall_handler(SYS_EXECVE, (syscall_handle_t)task_execve);
    regist_syscall_handler(SYS_EXIT, (syscall_handle_t)task_exit_thread);
    regist_syscall_handler(SYS_WAIT4, (syscall_handle_t)sys_wait4);
    regist_syscall_handler(SYS_KILL, (syscall_handle_t)sys_kill);
    regist_syscall_handler(SYS_UNAME, (syscall_handle_t)sys_uname);
    // regist_syscall_handler(SYS_SEMGET, (syscall_handle_t)sys_semget);
    // regist_syscall_handler(SYS_SEMOP, (syscall_handle_t)sys_semop);
    // regist_syscall_handler(SYS_SEMCTL, (syscall_handle_t)sys_semctl);
    regist_syscall_handler(SYS_SHMDT, (syscall_handle_t)sys_shmdt);
    // regist_syscall_handler(SYS_MSGGET, (syscall_handle_t)sys_msgget);
    // regist_syscall_handler(SYS_MSGSND, (syscall_handle_t)sys_msgsnd);
    // regist_syscall_handler(SYS_MSGRCV, (syscall_handle_t)sys_msgrcv);
    // regist_syscall_handler(SYS_MSGCTL, (syscall_handle_t)sys_msgctl);
    regist_syscall_handler(SYS_FCNTL, (syscall_handle_t)sys_fcntl);
    regist_syscall_handler(SYS_FLOCK, (syscall_handle_t)sys_flock);
    regist_syscall_handler(SYS_FSYNC, (syscall_handle_t)sys_fsync);
    regist_syscall_handler(SYS_FDATASYNC, (syscall_handle_t)sys_fdatasync);
    regist_syscall_handler(SYS_TRUNCATE, (syscall_handle_t)sys_truncate);
    regist_syscall_handler(SYS_FTRUNCATE, (syscall_handle_t)sys_ftruncate);
    regist_syscall_handler(SYS_GETCWD, (syscall_handle_t)sys_getcwd);
    regist_syscall_handler(SYS_CHDIR, (syscall_handle_t)sys_chdir);
    regist_syscall_handler(SYS_FCHDIR, (syscall_handle_t)sys_fchdir);
    // regist_syscall_handler(SYS_CREAT, (syscall_handle_t)sys_creat);
    regist_syscall_handler(SYS_FCHMOD, (syscall_handle_t)sys_fchmod);
    regist_syscall_handler(SYS_FCHOWN, (syscall_handle_t)sys_fchown);
    regist_syscall_handler(SYS_UMASK, (syscall_handle_t)sys_umask);
    regist_syscall_handler(SYS_GETTIMEOFDAY,
                           (syscall_handle_t)sys_gettimeofday);
    regist_syscall_handler(SYS_GETRLIMIT, (syscall_handle_t)sys_get_rlimit);
    regist_syscall_handler(SYS_GETRUSAGE, (syscall_handle_t)sys_getrusage);
    regist_syscall_handler(SYS_SYSINFO, (syscall_handle_t)sys_sysinfo);
    regist_syscall_handler(SYS_TIMES, (syscall_handle_t)sys_times);
    regist_syscall_handler(SYS_PTRACE, (syscall_handle_t)sys_ptrace);
    regist_syscall_handler(SYS_GETUID, (syscall_handle_t)sys_getuid);
    regist_syscall_handler(SYS_SYSLOG, (syscall_handle_t)sys_syslog);
    regist_syscall_handler(SYS_GETGID, (syscall_handle_t)sys_getgid);
    regist_syscall_handler(SYS_SETUID, (syscall_handle_t)sys_setuid);
    regist_syscall_handler(SYS_SETGID, (syscall_handle_t)sys_setgid);
    regist_syscall_handler(SYS_GETEUID, (syscall_handle_t)sys_geteuid);
    regist_syscall_handler(SYS_GETEGID, (syscall_handle_t)sys_getegid);
    regist_syscall_handler(SYS_SETPGID, (syscall_handle_t)sys_setpgid);
    regist_syscall_handler(SYS_GETPPID, (syscall_handle_t)sys_getppid);
    regist_syscall_handler(SYS_SETSID, (syscall_handle_t)sys_setsid);
    regist_syscall_handler(SYS_SETREUID, (syscall_handle_t)sys_setreuid);
    regist_syscall_handler(SYS_SETREGID, (syscall_handle_t)sys_setregid);
    regist_syscall_handler(SYS_GETGROUPS, (syscall_handle_t)sys_getgroups);
    regist_syscall_handler(SYS_SETGROUPS, (syscall_handle_t)sys_setgroups);
    regist_syscall_handler(SYS_SETRESUID, (syscall_handle_t)sys_setresuid);
    regist_syscall_handler(SYS_GETRESUID, (syscall_handle_t)sys_getresuid);
    regist_syscall_handler(SYS_SETRESGID, (syscall_handle_t)sys_setresgid);
    regist_syscall_handler(SYS_GETRESGID, (syscall_handle_t)sys_getresgid);
    regist_syscall_handler(SYS_GETPGID, (syscall_handle_t)sys_getpgid);
    regist_syscall_handler(SYS_SETFSUID, (syscall_handle_t)sys_setfsuid);
    regist_syscall_handler(SYS_SETFSGID, (syscall_handle_t)sys_setfsgid);
    regist_syscall_handler(SYS_GETSID, (syscall_handle_t)sys_getsid);
    regist_syscall_handler(SYS_CAPGET, (syscall_handle_t)sys_capget);
    regist_syscall_handler(SYS_CAPSET, (syscall_handle_t)sys_capset);
    // regist_syscall_handler(SYS_RT_SIGPENDING,
    // (syscall_handle_t)sys_rt_sigpending);
    regist_syscall_handler(SYS_RT_SIGTIMEDWAIT,
                           (syscall_handle_t)sys_rt_sigtimedwait);
    regist_syscall_handler(SYS_RT_SIGQUEUEINFO,
                           (syscall_handle_t)sys_rt_sigqueueinfo);
    regist_syscall_handler(SYS_RT_SIGSUSPEND, (syscall_handle_t)sys_sigsuspend);
    regist_syscall_handler(SYS_SIGALTSTACK, (syscall_handle_t)sys_sigaltstack);
    // regist_syscall_handler(SYS_UTIME, (syscall_handle_t)sys_utime);
    // regist_syscall_handler(SYS_USELIB, (syscall_handle_t)sys_uselib);
    regist_syscall_handler(SYS_PERSONALITY, (syscall_handle_t)sys_personality);
    // regist_syscall_handler(SYS_USTAT, (syscall_handle_t)sys_ustat);
    regist_syscall_handler(SYS_STATFS, (syscall_handle_t)sys_statfs);
    regist_syscall_handler(SYS_FSTATFS, (syscall_handle_t)sys_fstatfs);
    // regist_syscall_handler(SYS_SYSFS, (syscall_handle_t)sys_sysfs);
    regist_syscall_handler(SYS_GETPRIORITY, (syscall_handle_t)sys_getpriority);
    regist_syscall_handler(SYS_SETPRIORITY, (syscall_handle_t)sys_setpriority);
    regist_syscall_handler(SYS_SCHED_SETPARAM,
                           (syscall_handle_t)sys_sched_setparam);
    regist_syscall_handler(SYS_SCHED_GETPARAM,
                           (syscall_handle_t)sys_sched_getparam);
    regist_syscall_handler(SYS_SCHED_SETSCHEDULER,
                           (syscall_handle_t)sys_sched_setscheduler);
    regist_syscall_handler(SYS_SCHED_GETSCHEDULER,
                           (syscall_handle_t)sys_sched_getscheduler);
    regist_syscall_handler(SYS_SCHED_GET_PRIORITY_MAX,
                           (syscall_handle_t)sys_sched_get_priority_max);
    regist_syscall_handler(SYS_SCHED_GET_PRIORITY_MIN,
                           (syscall_handle_t)sys_sched_get_priority_min);
    regist_syscall_handler(SYS_SCHED_RR_GET_INTERVAL,
                           (syscall_handle_t)sys_sched_rr_get_interval);
    regist_syscall_handler(SYS_MLOCK, (syscall_handle_t)sys_mlock);
    regist_syscall_handler(SYS_MUNLOCK, (syscall_handle_t)sys_munlock);
    regist_syscall_handler(SYS_MLOCKALL, (syscall_handle_t)sys_mlockall);
    regist_syscall_handler(SYS_MUNLOCKALL, (syscall_handle_t)sys_munlockall);
    regist_syscall_handler(SYS_VHANGUP, (syscall_handle_t)sys_vhangup);
    // regist_syscall_handler(SYS_MODIFY_LDT,
    // (syscall_handle_t)sys_modify_ldt);
    regist_syscall_handler(SYS_PIVOT_ROOT, (syscall_handle_t)sys_pivot_root);
    // regist_syscall_handler(SYS__SYSCTL,
    // (syscall_handle_t)sys__sysctl);
    regist_syscall_handler(SYS_PRCTL, (syscall_handle_t)sys_prctl);
    // regist_syscall_handler(SYS_ADJTIMEX, (syscall_handle_t)sys_adjtimex);
    regist_syscall_handler(SYS_SETRLIMIT, (syscall_handle_t)sys_set_rlimit);
    regist_syscall_handler(SYS_CHROOT, (syscall_handle_t)sys_chroot);
    regist_syscall_handler(SYS_SYNC, (syscall_handle_t)dummy_syscall_handler);
    // regist_syscall_handler(SYS_ACCT, (syscall_handle_t)sys_acct);
    regist_syscall_handler(SYS_SETTIMEOFDAY,
                           (syscall_handle_t)dummy_syscall_handler);
    regist_syscall_handler(SYS_MOUNT, (syscall_handle_t)sys_mount);
    regist_syscall_handler(SYS_UMOUNT2, (syscall_handle_t)sys_umount2);
    // regist_syscall_handler(SYS_SWAPON, (syscall_handle_t)sys_swapon);
    // regist_syscall_handler(SYS_SWAPOFF, (syscall_handle_t)sys_swapoff);
    regist_syscall_handler(SYS_REBOOT, (syscall_handle_t)sys_reboot);
    regist_syscall_handler(SYS_SETHOSTNAME,
                           (syscall_handle_t)dummy_syscall_handler);
    // regist_syscall_handler(SYS_SETDOMAINNAME,
    // (syscall_handle_t)sys_setdomainname); regist_syscall_handler(SYS_IOPL,
    // (syscall_handle_t)sys_iopl); regist_syscall_handler(SYS_IOPERM,
    // (syscall_handle_t)sys_ioperm); regist_syscall_handler(SYS_CREATE_MODULE,
    // (syscall_handle_t)sys_create_module);
    // regist_syscall_handler(SYS_INIT_MODULE,
    // (syscall_handle_t)sys_init_module);
    // regist_syscall_handler(SYS_DELETE_MODULE,
    // (syscall_handle_t)sys_delete_module);
    // regist_syscall_handler(SYS_GET_KERNEL_SYMS,
    // (syscall_handle_t)sys_get_kernel_syms);
    // regist_syscall_handler(SYS_QUERY_MODULE,
    // (syscall_handle_t)sys_query_module);
    regist_syscall_handler(SYS_QUOTACTL, (syscall_handle_t)sys_quotactl);
    // regist_syscall_handler(SYS_NFSSERVCTL,
    // (syscall_handle_t)sys_nfsservctl); regist_syscall_handler(SYS_GETPMSG,
    // (syscall_handle_t)sys_getpmsg); regist_syscall_handler(SYS_PUTPMSG,
    // (syscall_handle_t)sys_putpmsg); regist_syscall_handler(SYS_AFS_SYSCALL,
    // (syscall_handle_t)sys_afs_syscall); regist_syscall_handler(SYS_TUXCALL,
    // (syscall_handle_t)sys_tuxcall); regist_syscall_handler(SYS_SECURITY,
    // (syscall_handle_t)sys_security);
    regist_syscall_handler(SYS_GETTID, (syscall_handle_t)sys_gettid);
    regist_syscall_handler(SYS_READAHEAD, (syscall_handle_t)sys_readahead);
    regist_syscall_handler(SYS_SETXATTR, (syscall_handle_t)sys_setxattr);
    regist_syscall_handler(SYS_LSETXATTR, (syscall_handle_t)sys_lsetxattr);
    regist_syscall_handler(SYS_FSETXATTR, (syscall_handle_t)sys_fsetxattr);
    regist_syscall_handler(SYS_GETXATTR, (syscall_handle_t)sys_getxattr);
    regist_syscall_handler(SYS_LGETXATTR, (syscall_handle_t)sys_lgetxattr);
    regist_syscall_handler(SYS_FGETXATTR, (syscall_handle_t)sys_fgetxattr);
    regist_syscall_handler(SYS_LISTXATTR, (syscall_handle_t)sys_listxattr);
    regist_syscall_handler(SYS_LLISTXATTR, (syscall_handle_t)sys_llistxattr);
    regist_syscall_handler(SYS_FLISTXATTR, (syscall_handle_t)sys_flistxattr);
    regist_syscall_handler(SYS_REMOVEXATTR, (syscall_handle_t)sys_removexattr);
    regist_syscall_handler(SYS_LREMOVEXATTR,
                           (syscall_handle_t)sys_lremovexattr);
    regist_syscall_handler(SYS_FREMOVEXATTR,
                           (syscall_handle_t)sys_fremovexattr);
    regist_syscall_handler(SYS_TKILL, (syscall_handle_t)sys_tkill);
    regist_syscall_handler(SYS_FUTEX, (syscall_handle_t)sys_futex);
    regist_syscall_handler(SYS_SCHED_SETAFFINITY,
                           (syscall_handle_t)sys_sched_setaffinity);
    regist_syscall_handler(SYS_SCHED_GETAFFINITY,
                           (syscall_handle_t)sys_sched_getaffinity);
    // regist_syscall_handler(SYS_SET_THREAD_AREA,
    // (syscall_handle_t)sys_set_thread_area);
    // regist_syscall_handler(SYS_IO_SETUP, (syscall_handle_t)sys_io_setup);
    // regist_syscall_handler(SYS_IO_DESTROY,
    // (syscall_handle_t)sys_io_destroy);
    // regist_syscall_handler(SYS_IO_GETEVENTS,
    // (syscall_handle_t)sys_io_getevents);
    // regist_syscall_handler(SYS_IO_SUBMIT, (syscall_handle_t)sys_io_submit);
    // regist_syscall_handler(SYS_IO_CANCEL, (syscall_handle_t)sys_io_cancel);
    // regist_syscall_handler(SYS_GET_THREAD_AREA,
    // (syscall_handle_t)sys_get_thread_area);
    // regist_syscall_handler(SYS_LOOKUP_DCOOKIE,
    // (syscall_handle_t)sys_lookup_dcookie);
    // regist_syscall_handler(SYS_EPOLL_CTL_OLD,
    // (syscall_handle_t)sys_epoll_ctl_old);
    // regist_syscall_handler(SYS_EPOLL_WAIT_OLD,
    // (syscall_handle_t)sys_epoll_wait_old);
    // regist_syscall_handler(SYS_REMAP_FILE_PAGES,
    // (syscall_handle_t)sys_remap_file_pages);
    regist_syscall_handler(SYS_GETDENTS64, (syscall_handle_t)sys_getdents64);
    regist_syscall_handler(SYS_SET_TID_ADDRESS,
                           (syscall_handle_t)sys_set_tid_address);
    // regist_syscall_handler(SYS_RESTART_SYSCALL,
    // (syscall_handle_t)sys_restart_syscall);
    // regist_syscall_handler(SYS_SEMTIMEDOP,
    // (syscall_handle_t)sys_semtimedop);
    regist_syscall_handler(SYS_FADVISE64,
                           (syscall_handle_t)dummy_syscall_handler);
    regist_syscall_handler(SYS_TIMER_CREATE,
                           (syscall_handle_t)sys_timer_create);
    regist_syscall_handler(SYS_TIMER_SETTIME,
                           (syscall_handle_t)sys_timer_settime);
    regist_syscall_handler(SYS_TIMER_GETTIME,
                           (syscall_handle_t)sys_timer_gettime);
    regist_syscall_handler(SYS_TIMER_GETOVERRUN,
                           (syscall_handle_t)sys_timer_getoverrun);
    regist_syscall_handler(SYS_TIMER_DELETE,
                           (syscall_handle_t)sys_timer_delete);
    // regist_syscall_handler(SYS_CLOCK_SETTIME,
    // (syscall_handle_t)sys_clock_settime);
    regist_syscall_handler(SYS_CLOCK_GETTIME,
                           (syscall_handle_t)sys_clock_gettime);
    regist_syscall_handler(SYS_CLOCK_GETRES,
                           (syscall_handle_t)sys_clock_getres);
    regist_syscall_handler(SYS_CLOCK_NANOSLEEP,
                           (syscall_handle_t)sys_clock_nanosleep);
    regist_syscall_handler(SYS_EXIT_GROUP, (syscall_handle_t)task_exit);
    regist_syscall_handler(SYS_EPOLL_CTL, (syscall_handle_t)sys_epoll_ctl);
    regist_syscall_handler(SYS_TGKILL, (syscall_handle_t)sys_tgkill);
    // regist_syscall_handler(SYS_UTIMES, (syscall_handle_t)sys_utimes);
    // regist_syscall_handler(SYS_VSERVER, (syscall_handle_t)sys_vserver);
    regist_syscall_handler(SYS_MBIND, (syscall_handle_t)sys_mbind);
    regist_syscall_handler(SYS_SET_MEMPOLICY,
                           (syscall_handle_t)sys_set_mempolicy);
    regist_syscall_handler(SYS_GET_MEMPOLICY,
                           (syscall_handle_t)sys_get_mempolicy);
    // regist_syscall_handler(SYS_MQ_OPEN, (syscall_handle_t)sys_mq_open);
    // regist_syscall_handler(SYS_MQ_UNLINK, (syscall_handle_t)sys_mq_unlink);
    // regist_syscall_handler(SYS_MQ_TIMEDSEND,
    // (syscall_handle_t)sys_mq_timedsend);
    // regist_syscall_handler(SYS_MQ_TIMEDRECEIVE,
    // (syscall_handle_t)sys_mq_timedreceive);
    // regist_syscall_handler(SYS_MQ_NOTIFY, (syscall_handle_t)sys_mq_notify);
    // regist_syscall_handler(SYS_MQ_GETSETATTR,
    // (syscall_handle_t)sys_mq_getsetattr);
    // regist_syscall_handler(SYS_KEXEC_LOAD,
    // (syscall_handle_t)sys_kexec_load);
    regist_syscall_handler(SYS_WAITID, (syscall_handle_t)sys_waitid);
    // regist_syscall_handler(SYS_ADD_KEY, (syscall_handle_t)sys_add_key);
    // regist_syscall_handler(SYS_REQUEST_KEY,
    // (syscall_handle_t)sys_request_key); regist_syscall_handler(SYS_KEYCTL,
    // (syscall_handle_t)sys_keyctl); regist_syscall_handler(SYS_IOPRIO_SET,
    // (syscall_handle_t)sys_ioprio_set);
    // regist_syscall_handler(SYS_IOPRIO_GET,
    // (syscall_handle_t)sys_ioprio_get);
    regist_syscall_handler(SYS_INOTIFY_ADD_WATCH,
                           (syscall_handle_t)sys_inotify_add_watch);
    regist_syscall_handler(SYS_INOTIFY_RM_WATCH,
                           (syscall_handle_t)sys_inotify_rm_watch);
    // regist_syscall_handler(SYS_MIGRATE_PAGES,
    // (syscall_handle_t)sys_migrate_pages);
    regist_syscall_handler(SYS_OPENAT, (syscall_handle_t)sys_openat);
    regist_syscall_handler(SYS_MKDIRAT, (syscall_handle_t)sys_mkdirat);
    regist_syscall_handler(SYS_MKNODAT, (syscall_handle_t)sys_mknodat);
    regist_syscall_handler(SYS_FCHOWNAT, (syscall_handle_t)sys_fchownat);
    regist_syscall_handler(SYS_UNLINKAT, (syscall_handle_t)sys_unlinkat);
    regist_syscall_handler(SYS_RENAMEAT, (syscall_handle_t)sys_renameat);
    regist_syscall_handler(SYS_LINKAT, (syscall_handle_t)sys_linkat);
    regist_syscall_handler(SYS_SYMLINKAT, (syscall_handle_t)sys_symlinkat);
    regist_syscall_handler(SYS_READLINKAT, (syscall_handle_t)sys_readlinkat);
    regist_syscall_handler(SYS_FCHMODAT, (syscall_handle_t)sys_fchmodat);
    regist_syscall_handler(SYS_FACCESSAT, (syscall_handle_t)sys_faccessat);
    regist_syscall_handler(SYS_PSELECT6, (syscall_handle_t)sys_pselect6);
    regist_syscall_handler(SYS_PPOLL, (syscall_handle_t)sys_ppoll);
    regist_syscall_handler(SYS_UNSHARE, (syscall_handle_t)sys_unshare);
    regist_syscall_handler(SYS_SET_ROBUST_LIST,
                           (syscall_handle_t)sys_set_robust_list);
    regist_syscall_handler(SYS_GET_ROBUST_LIST,
                           (syscall_handle_t)sys_get_robust_list);
    regist_syscall_handler(SYS_SPLICE, (syscall_handle_t)sys_splice);
    // regist_syscall_handler(SYS_TEE, (syscall_handle_t)sys_tee);
    // regist_syscall_handler(SYS_SYNC_FILE_RANGE,
    // (syscall_handle_t)sys_sync_file_range);
    regist_syscall_handler(SYS_VMSPLICE, (syscall_handle_t)sys_vmsplice);
    // regist_syscall_handler(SYS_MOVE_PAGES,
    // (syscall_handle_t)sys_move_pages);
    regist_syscall_handler(SYS_UTIMENSAT, (syscall_handle_t)sys_utimensat);
    regist_syscall_handler(SYS_EPOLL_PWAIT, (syscall_handle_t)sys_epoll_pwait);
    regist_syscall_handler(SYS_TIMERFD_CREATE,
                           (syscall_handle_t)sys_timerfd_create);
    regist_syscall_handler(SYS_FALLOCATE, (syscall_handle_t)sys_fallocate);
    regist_syscall_handler(SYS_TIMERFD_SETTIME,
                           (syscall_handle_t)sys_timerfd_settime);
    // regist_syscall_handler(SYS_TIMERFD_GETTIME,
    // (syscall_handle_t)sys_timerfd_gettime);
    regist_syscall_handler(SYS_ACCEPT4, (syscall_handle_t)sys_accept);
    regist_syscall_handler(SYS_SIGNALFD4, (syscall_handle_t)sys_signalfd4);
    regist_syscall_handler(SYS_EVENTFD2, (syscall_handle_t)sys_eventfd2);
    regist_syscall_handler(SYS_EPOLL_CREATE1,
                           (syscall_handle_t)sys_epoll_create1);
    regist_syscall_handler(SYS_DUP3, (syscall_handle_t)sys_dup3);
    regist_syscall_handler(SYS_PIPE2, (syscall_handle_t)sys_pipe);
    regist_syscall_handler(SYS_INOTIFY_INIT1,
                           (syscall_handle_t)sys_inotify_init1);
    regist_syscall_handler(SYS_PREADV, (syscall_handle_t)sys_preadv);
    regist_syscall_handler(SYS_PWRITEV, (syscall_handle_t)sys_pwritev);
    // regist_syscall_handler(SYS_RT_TGSIGQUEUEINFO,
    // (syscall_handle_t)sys_rt_tgsigqueueinfo);
    // regist_syscall_handler(SYS_PERF_EVENT_OPEN,
    // (syscall_handle_t)sys_perf_event_open);
#ifdef SYS_RECVMMSG
    regist_syscall_handler(SYS_RECVMMSG, (syscall_handle_t)sys_recvmmsg);
#endif
#ifdef SYS_RECVMMSG_TIME64
    regist_syscall_handler(SYS_RECVMMSG_TIME64, (syscall_handle_t)sys_recvmmsg);
#endif
    regist_syscall_handler(SYS_FANOTIFY_INIT,
                           (syscall_handle_t)sys_fanotify_init);
    regist_syscall_handler(SYS_FANOTIFY_MARK,
                           (syscall_handle_t)sys_fanotify_mark);
    regist_syscall_handler(SYS_PRLIMIT64, (syscall_handle_t)sys_prlimit64);
    regist_syscall_handler(SYS_NAME_TO_HANDLE_AT,
                           (syscall_handle_t)sys_name_to_handle_at);
    regist_syscall_handler(SYS_OPEN_BY_HANDLE_AT,
                           (syscall_handle_t)sys_open_by_handle_at);
    // regist_syscall_handler(SYS_CLOCK_ADJTIME,
    // (syscall_handle_t)sys_clock_adjtime);
    regist_syscall_handler(SYS_SYNCFS, (syscall_handle_t)sys_syncfs);
#ifdef SYS_SENDMMSG
    regist_syscall_handler(SYS_SENDMMSG, (syscall_handle_t)sys_sendmmsg);
#endif
    regist_syscall_handler(SYS_SETNS, (syscall_handle_t)sys_setns);
    regist_syscall_handler(SYS_GETCPU, (syscall_handle_t)sys_getcpu);
    regist_syscall_handler(SYS_PROCESS_VM_READV,
                           (syscall_handle_t)sys_process_vm_readv);
    regist_syscall_handler(SYS_PROCESS_VM_WRITEV,
                           (syscall_handle_t)sys_process_vm_writev);
    regist_syscall_handler(SYS_KCMP, (syscall_handle_t)sys_kcmp);
    // regist_syscall_handler(SYS_FINIT_MODULE,
    // (syscall_handle_t)sys_finit_module);
    // regist_syscall_handler(SYS_SCHED_SETATTR,
    // (syscall_handle_t)sys_sched_setattr);
    // regist_syscall_handler(SYS_SCHED_GETATTR,
    // (syscall_handle_t)sys_sched_getattr);
    regist_syscall_handler(SYS_RENAMEAT2, (syscall_handle_t)sys_renameat2);
    // regist_syscall_handler(SYS_SECCOMP, (syscall_handle_t)sys_seccomp);
    regist_syscall_handler(SYS_GETRANDOM, (syscall_handle_t)sys_getrandom);
    regist_syscall_handler(SYS_ADD_KEY, (syscall_handle_t)sys_add_key);
    regist_syscall_handler(SYS_REQUEST_KEY, (syscall_handle_t)sys_request_key);
    regist_syscall_handler(SYS_KEYCTL, (syscall_handle_t)sys_keyctl);
    regist_syscall_handler(SYS_MEMFD_CREATE,
                           (syscall_handle_t)sys_memfd_create);
    // regist_syscall_handler(SYS_KEXEC_FILE_LOAD,
    // (syscall_handle_t)sys_kexec_file_load); regist_syscall_handler(SYS_BPF,
    // (syscall_handle_t)sys_bpf);
    regist_syscall_handler(SYS_EXECVEAT, (syscall_handle_t)sys_execveat);
    // regist_syscall_handler(SYS_USERFAULTFD,
    // (syscall_handle_t)sys_userfaultfd);
    regist_syscall_handler(SYS_MEMBARRIER, (syscall_handle_t)sys_membarrier);
    // regist_syscall_handler(SYS_MLOCK2, (syscall_handle_t)sys_mlock2);
    regist_syscall_handler(SYS_COPY_FILE_RANGE,
                           (syscall_handle_t)sys_copy_file_range);
    regist_syscall_handler(SYS_PREADV2, (syscall_handle_t)sys_preadv2);
    regist_syscall_handler(SYS_PWRITEV2, (syscall_handle_t)sys_pwritev2);
    // regist_syscall_handler(SYS_PKEY_MPROTECT,
    // (syscall_handle_t)sys_pkey_mprotect);
    // regist_syscall_handler(SYS_PKEY_ALLOC,
    // (syscall_handle_t)sys_pkey_alloc); regist_syscall_handler(SYS_PKEY_FREE,
    // (syscall_handle_t)sys_pkey_free);
    regist_syscall_handler(SYS_STATX, (syscall_handle_t)sys_statx);
    // regist_syscall_handler(SYS_IO_PGETEVENTS,
    // (syscall_handle_t)sys_io_pgetevents);
    regist_syscall_handler(SYS_RSEQ, (syscall_handle_t)sys_rseq);
    regist_syscall_handler(SYS_PIDFD_SEND_SIGNAL,
                           (syscall_handle_t)sys_pidfd_send_signal);
    // regist_syscall_handler(SYS_IO_URING_SETUP,
    // (syscall_handle_t)sys_io_uring_setup);
    // regist_syscall_handler(SYS_IO_URING_ENTER,
    // (syscall_handle_t)sys_io_uring_enter);
    // regist_syscall_handler(SYS_IO_URING_REGISTER,
    // (syscall_handle_t)sys_io_uring_register);
    regist_syscall_handler(SYS_OPEN_TREE, (syscall_handle_t)sys_open_tree);
    regist_syscall_handler(SYS_MOVE_MOUNT, (syscall_handle_t)sys_move_mount);
    regist_syscall_handler(SYS_FSOPEN, (syscall_handle_t)sys_fsopen);
    regist_syscall_handler(SYS_FSCONFIG, (syscall_handle_t)sys_fsconfig);
    regist_syscall_handler(SYS_FSMOUNT, (syscall_handle_t)sys_fsmount);
    regist_syscall_handler(SYS_FSPICK, (syscall_handle_t)sys_fspick);
    regist_syscall_handler(SYS_PIDFD_OPEN, (syscall_handle_t)sys_pidfd_open);
    regist_syscall_handler(SYS_CLONE3, (syscall_handle_t)sys_clone3);
    regist_syscall_handler(SYS_CLOSE_RANGE, (syscall_handle_t)sys_close_range);
    regist_syscall_handler(SYS_OPENAT2, (syscall_handle_t)sys_openat2);
    // regist_syscall_handler(SYS_PIDFD_GETFD,
    // (syscall_handle_t)sys_pidfd_getfd);
    regist_syscall_handler(SYS_FACCESSAT2, (syscall_handle_t)sys_faccessat2);
    // regist_syscall_handler(SYS_PROCESS_MADVISE,
    // (syscall_handle_t)sys_process_madvise);
    regist_syscall_handler(SYS_EPOLL_PWAIT2,
                           (syscall_handle_t)sys_epoll_pwait2);
    // regist_syscall_handler(SYS_MOUNT_SETATTR,
    // (syscall_handle_t)sys_mount_setattr);
    // regist_syscall_handler(SYS_LANDLOCK_CREATE_RULESET,
    // (syscall_handle_t)sys_landlock_create_ruleset);
    // regist_syscall_handler(SYS_LANDLOCK_ADD_RULE,
    // (syscall_handle_t)sys_landlock_add_rule);
    // regist_syscall_handler(SYS_LANDLOCK_RESTRICT_SELF,
    // (syscall_handle_t)sys_landlock_restrict_self);
    // regist_syscall_handler(SYS_MEMFD_SECRET,
    // (syscall_handle_t)sys_memfd_secret);
    // regist_syscall_handler(SYS_PROCESS_MRELEASE,
    // (syscall_handle_t)sys_process_mrelease);
    // regist_syscall_handler(SYS_FUTEX_WAITV,
    // (syscall_handle_t)sys_futex_waitv);
    // regist_syscall_handler(SYS_SET_MEMPOLICY_HOME_NODE,
    // (syscall_handle_t)sys_set_mempolicy_home_node);
    // regist_syscall_handler(SYS_CACHESTAT, (syscall_handle_t)sys_cachestat);
    regist_syscall_handler(SYS_FCHMODAT2, (syscall_handle_t)sys_fchmodat2);
}

void aarch64_do_syscall(struct pt_regs *frame) {
    uint64_t ret = 0;

    uint64_t idx = frame->x8;
    uint64_t arg1 = frame->x0;
    uint64_t arg2 = frame->x1;
    uint64_t arg3 = frame->x2;
    uint64_t arg4 = frame->x3;
    uint64_t arg5 = frame->x4;
    uint64_t arg6 = frame->x5;

    task_t *self = current_task;
    if (!self) {
        frame->x0 = (uint64_t)-ENOSYS;
        goto done;
    }

    frame->x0 = self->last_syscall_ret;
    frame->syscallno = idx;
    ptrace_on_syscall_enter(frame);

    if (idx > MAX_SYSCALL_NUM) {
        frame->x0 = (uint64_t)-ENOSYS;
        goto done;
    }

    syscall_handle_t handler = syscall_handlers[idx];
    if (!handler) {
        frame->x0 = (uint64_t)-ENOSYS;
        goto done;
    }

    if (idx == SYS_CLONE) {
        frame->x0 =
            aarch64_sys_clone(frame, arg1, arg2, arg3, arg4, arg5, arg6);
    } else if (idx == SYS_CLONE3 || idx == SYS_RT_SIGRETURN) {
        special_syscall_handle_t h = (special_syscall_handle_t)handler;
        frame->x0 = h(frame, arg1, arg2, arg3, arg4, arg5, arg6);
    } else {
        frame->x0 = handler(arg1, arg2, arg3, arg4, arg5, arg6);
    }

    if ((idx != SYS_BRK) && (idx != SYS_MMAP) && (idx != SYS_MREMAP) &&
        (idx != SYS_SHMAT) && (idx != SYS_FCNTL) && (idx != SYS_RT_SIGRETURN) &&
        (int)frame->x0 < 0 && ((frame->x0 & 0x8000000000000000) == 0))
        frame->x0 |= 0xffffffff00000000;
    else if ((int64_t)frame->x0 < 0 && ((frame->x0 & 0xffffffff) == 0))
        frame->x0 = 0;

done:
    self->last_syscall_ret = frame->x0;

    ptrace_on_syscall_exit(frame);

    if (idx != SYS_BRK && idx != SYS_RSEQ && frame->x0 == (uint64_t)-ENOSYS) {
        serial_fprintk("syscall %d not implemented\n", idx);
    }

    task_signal(frame);
    frame->syscallno = NO_SYSCALL;
}
