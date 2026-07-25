#include <arch/arch.h>
#include <boot/boot.h>
#include <drivers/rtc.h>
#include <mm/mm_syscall.h>
#include <net/net_syscall.h>
#include <task/futex.h>
#include <task/keyring.h>
#include <task/ptrace.h>
#include <task/signal.h>
#include <task/task.h>
#include <task/task_syscall.h>

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

uint64_t sys_clock_gettime(uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg3;
    clockid_t clock_id = (clockid_t)arg1;

    switch (clock_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE: {
        rtc_realtime_t now;
        rtc_read_realtime(&now);
        return copy_timespec_to_user(arg2, now.sec, now.nsec);
    }
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME: {
        uint64_t nano = nano_time();
        return copy_timespec_to_user(arg2, nano / 1000000000ULL,
                                     nano % 1000000000ULL);
    }
    default:
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

uint64_t sys_gettimeofday(uint64_t arg1) {
    rtc_realtime_t now;

    rtc_read_realtime(&now);
    return copy_timeval_to_user(arg1, now.sec, now.nsec / 1000);
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
    const char *machine = uts_ns ? uts_ns->machine : "loongarch64";
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
    if (copy_to_user((void *)arg1, &utsname, sizeof(utsname)))
        return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t sys_ni_syscall(void) { return (uint64_t)-ENOSYS; }

syscall_handle_t syscall_handlers[MAX_SYSCALL_NUM] = {NULL};

static void regist_syscall_handler(int nr, syscall_handle_t handler) {
    if (nr >= 0 && nr < MAX_SYSCALL_NUM)
        syscall_handlers[nr] = handler;
}

void syscall_handler_init() {
    memset(syscall_handlers, 0, sizeof(syscall_handlers));

    regist_syscall_handler(SYS_READ, (syscall_handle_t)sys_read);
    regist_syscall_handler(SYS_WRITE, (syscall_handle_t)sys_write);
    regist_syscall_handler(SYS_CLOSE, (syscall_handle_t)sys_close);
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
    regist_syscall_handler(SYS_SETITIMER, (syscall_handle_t)sys_setitimer);
    regist_syscall_handler(SYS_GETPID, (syscall_handle_t)sys_getpid);
    regist_syscall_handler(SYS_SENDFILE, (syscall_handle_t)sys_sendfile);
    regist_syscall_handler(SYS_SOCKET, (syscall_handle_t)sys_socket);
    regist_syscall_handler(SYS_CONNECT, (syscall_handle_t)sys_connect);
    regist_syscall_handler(SYS_ACCEPT, (syscall_handle_t)sys_accept);
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
    regist_syscall_handler(SYS_UNAME, (syscall_handle_t)sys_newuname);
    regist_syscall_handler(SYS_SHMDT, (syscall_handle_t)sys_shmdt);
    regist_syscall_handler(SYS_FCNTL, (syscall_handle_t)sys_fcntl);
    regist_syscall_handler(SYS_FLOCK, (syscall_handle_t)sys_flock);
    regist_syscall_handler(SYS_FSYNC, (syscall_handle_t)sys_fsync);
    regist_syscall_handler(SYS_FDATASYNC, (syscall_handle_t)sys_fdatasync);
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
    regist_syscall_handler(SYS_TRUNCATE, (syscall_handle_t)sys_truncate);
    regist_syscall_handler(SYS_FTRUNCATE, (syscall_handle_t)sys_ftruncate);
    regist_syscall_handler(SYS_GETDENTS64, (syscall_handle_t)sys_getdents64);
    regist_syscall_handler(SYS_GETCWD, (syscall_handle_t)sys_getcwd);
    regist_syscall_handler(SYS_CHDIR, (syscall_handle_t)sys_chdir);
    regist_syscall_handler(SYS_FCHDIR, (syscall_handle_t)sys_fchdir);
    regist_syscall_handler(SYS_FCHMOD, (syscall_handle_t)sys_fchmod);
    regist_syscall_handler(SYS_FCHOWN, (syscall_handle_t)sys_fchown);
    regist_syscall_handler(SYS_UMASK, (syscall_handle_t)sys_umask);
    regist_syscall_handler(SYS_GETTIMEOFDAY,
                           (syscall_handle_t)sys_gettimeofday);
    regist_syscall_handler(SYS_GETRUSAGE, (syscall_handle_t)sys_getrusage);
    regist_syscall_handler(SYS_SYSINFO, (syscall_handle_t)sys_sysinfo);
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
    regist_syscall_handler(SYS_GETGROUPS, (syscall_handle_t)sys_getgroups);
    regist_syscall_handler(SYS_TIMES, (syscall_handle_t)sys_times);
    regist_syscall_handler(SYS_SETGROUPS, (syscall_handle_t)sys_setgroups);
    regist_syscall_handler(SYS_SETREUID, (syscall_handle_t)sys_setreuid);
    regist_syscall_handler(SYS_SETREGID, (syscall_handle_t)sys_setregid);
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
    regist_syscall_handler(SYS_RT_SIGTIMEDWAIT,
                           (syscall_handle_t)sys_rt_sigtimedwait);
    regist_syscall_handler(SYS_RT_SIGPENDING,
                           (syscall_handle_t)sys_rt_sigpending);
    regist_syscall_handler(SYS_RT_SIGQUEUEINFO,
                           (syscall_handle_t)sys_rt_sigqueueinfo);
    regist_syscall_handler(SYS_RT_SIGSUSPEND, (syscall_handle_t)sys_sigsuspend);
    regist_syscall_handler(SYS_SIGALTSTACK, (syscall_handle_t)sys_sigaltstack);
    regist_syscall_handler(SYS_PERSONALITY, (syscall_handle_t)sys_personality);
    regist_syscall_handler(SYS_STATFS, (syscall_handle_t)sys_statfs);
    regist_syscall_handler(SYS_FSTATFS, (syscall_handle_t)sys_fstatfs);
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
    regist_syscall_handler(SYS_PRCTL, (syscall_handle_t)sys_prctl);
    regist_syscall_handler(SYS_CHROOT, (syscall_handle_t)sys_chroot);
    regist_syscall_handler(SYS_SYNC, (syscall_handle_t)dummy_syscall_handler);
    regist_syscall_handler(SYS_MOUNT, (syscall_handle_t)sys_mount);
    regist_syscall_handler(SYS_UMOUNT2, (syscall_handle_t)sys_umount2);
    regist_syscall_handler(SYS_REBOOT, (syscall_handle_t)sys_reboot);
    regist_syscall_handler(SYS_GETTID, (syscall_handle_t)sys_gettid);
    regist_syscall_handler(SYS_TKILL, (syscall_handle_t)sys_tkill);
    regist_syscall_handler(SYS_FUTEX, (syscall_handle_t)sys_futex);
    regist_syscall_handler(SYS_SCHED_SETAFFINITY,
                           (syscall_handle_t)sys_sched_setaffinity);
    regist_syscall_handler(SYS_SCHED_GETAFFINITY,
                           (syscall_handle_t)sys_sched_getaffinity);
    regist_syscall_handler(SYS_EPOLL_CREATE1,
                           (syscall_handle_t)sys_epoll_create1);
    regist_syscall_handler(SYS_SET_TID_ADDRESS,
                           (syscall_handle_t)sys_set_tid_address);
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
    regist_syscall_handler(SYS_CLOCK_GETTIME,
                           (syscall_handle_t)sys_clock_gettime);
    regist_syscall_handler(SYS_CLOCK_GETRES,
                           (syscall_handle_t)sys_clock_getres);
    regist_syscall_handler(SYS_CLOCK_NANOSLEEP,
                           (syscall_handle_t)sys_clock_nanosleep);
    regist_syscall_handler(SYS_EXIT_GROUP, (syscall_handle_t)task_exit);
    regist_syscall_handler(SYS_EPOLL_CTL, (syscall_handle_t)sys_epoll_ctl);
    regist_syscall_handler(SYS_TGKILL, (syscall_handle_t)sys_tgkill);
    regist_syscall_handler(SYS_SET_MEMPOLICY,
                           (syscall_handle_t)sys_set_mempolicy);
    regist_syscall_handler(SYS_GET_MEMPOLICY,
                           (syscall_handle_t)sys_get_mempolicy);
    regist_syscall_handler(SYS_WAITID, (syscall_handle_t)sys_waitid);
    regist_syscall_handler(SYS_IOPRIO_SET, (syscall_handle_t)sys_ioprio_set);
    regist_syscall_handler(SYS_IOPRIO_GET, (syscall_handle_t)sys_ioprio_get);
    regist_syscall_handler(SYS_INOTIFY_INIT1,
                           (syscall_handle_t)sys_inotify_init1);
    regist_syscall_handler(SYS_INOTIFY_ADD_WATCH,
                           (syscall_handle_t)sys_inotify_add_watch);
    regist_syscall_handler(SYS_INOTIFY_RM_WATCH,
                           (syscall_handle_t)sys_inotify_rm_watch);
    regist_syscall_handler(SYS_FANOTIFY_INIT,
                           (syscall_handle_t)sys_fanotify_init);
    regist_syscall_handler(SYS_FANOTIFY_MARK,
                           (syscall_handle_t)sys_fanotify_mark);
    regist_syscall_handler(SYS_OPENAT, (syscall_handle_t)sys_openat);
    regist_syscall_handler(SYS_MKDIRAT, (syscall_handle_t)sys_mkdirat);
    regist_syscall_handler(SYS_MKNODAT, (syscall_handle_t)sys_mknodat);
    regist_syscall_handler(SYS_FCHOWNAT, (syscall_handle_t)sys_fchownat);
    regist_syscall_handler(SYS_NEWFSTATAT, (syscall_handle_t)sys_newfstatat);
    regist_syscall_handler(SYS_FSTAT, (syscall_handle_t)sys_fstat);
    regist_syscall_handler(SYS_UNLINKAT, (syscall_handle_t)sys_unlinkat);
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
    regist_syscall_handler(SYS_UTIMENSAT, (syscall_handle_t)sys_utimensat);
    regist_syscall_handler(SYS_EPOLL_PWAIT, (syscall_handle_t)sys_epoll_pwait);
    regist_syscall_handler(SYS_TIMERFD_CREATE,
                           (syscall_handle_t)sys_timerfd_create);
    regist_syscall_handler(SYS_FALLOCATE, (syscall_handle_t)sys_fallocate);
    regist_syscall_handler(SYS_TIMERFD_SETTIME,
                           (syscall_handle_t)sys_timerfd_settime);
    regist_syscall_handler(SYS_ACCEPT4, (syscall_handle_t)sys_accept);
    regist_syscall_handler(SYS_SIGNALFD4, (syscall_handle_t)sys_signalfd4);
    regist_syscall_handler(SYS_EVENTFD2, (syscall_handle_t)sys_eventfd2);
    regist_syscall_handler(SYS_DUP3, (syscall_handle_t)sys_dup3);
    regist_syscall_handler(SYS_PIPE2, (syscall_handle_t)sys_pipe);
    regist_syscall_handler(SYS_PREADV, (syscall_handle_t)sys_preadv);
    regist_syscall_handler(SYS_PWRITEV, (syscall_handle_t)sys_pwritev);
    regist_syscall_handler(SYS_VMSPLICE, (syscall_handle_t)sys_vmsplice);
    regist_syscall_handler(SYS_SPLICE, (syscall_handle_t)sys_splice);
    regist_syscall_handler(SYS_RECVMMSG, (syscall_handle_t)sys_recvmmsg);
    regist_syscall_handler(SYS_PRLIMIT64, (syscall_handle_t)sys_prlimit64);
    regist_syscall_handler(SYS_NAME_TO_HANDLE_AT,
                           (syscall_handle_t)sys_name_to_handle_at);
    regist_syscall_handler(SYS_OPEN_BY_HANDLE_AT,
                           (syscall_handle_t)sys_open_by_handle_at);
    regist_syscall_handler(SYS_SENDMMSG, (syscall_handle_t)sys_sendmmsg);
    regist_syscall_handler(SYS_SETNS, (syscall_handle_t)sys_setns);
    regist_syscall_handler(SYS_GETCPU, (syscall_handle_t)sys_getcpu);
    regist_syscall_handler(SYS_PROCESS_VM_READV,
                           (syscall_handle_t)sys_process_vm_readv);
    regist_syscall_handler(SYS_PROCESS_VM_WRITEV,
                           (syscall_handle_t)sys_process_vm_writev);
    regist_syscall_handler(SYS_KCMP, (syscall_handle_t)sys_kcmp);
    regist_syscall_handler(SYS_RENAMEAT2, (syscall_handle_t)sys_renameat2);
    regist_syscall_handler(SYS_GETRANDOM, (syscall_handle_t)sys_getrandom);
    regist_syscall_handler(SYS_ADD_KEY, (syscall_handle_t)sys_add_key);
    regist_syscall_handler(SYS_REQUEST_KEY, (syscall_handle_t)sys_request_key);
    regist_syscall_handler(SYS_KEYCTL, (syscall_handle_t)sys_keyctl);
    regist_syscall_handler(SYS_MEMFD_CREATE,
                           (syscall_handle_t)sys_memfd_create);
    regist_syscall_handler(SYS_SYNCFS, (syscall_handle_t)sys_syncfs);
    regist_syscall_handler(SYS_EXECVEAT, (syscall_handle_t)sys_execveat);
    regist_syscall_handler(SYS_MEMBARRIER, (syscall_handle_t)sys_membarrier);
    regist_syscall_handler(SYS_COPY_FILE_RANGE,
                           (syscall_handle_t)sys_copy_file_range);
    regist_syscall_handler(SYS_PREADV2, (syscall_handle_t)sys_preadv2);
    regist_syscall_handler(SYS_PWRITEV2, (syscall_handle_t)sys_pwritev2);
    regist_syscall_handler(SYS_STATX, (syscall_handle_t)sys_statx);
    regist_syscall_handler(SYS_RSEQ, (syscall_handle_t)sys_rseq);
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
    regist_syscall_handler(SYS_FACCESSAT2, (syscall_handle_t)sys_faccessat2);
    regist_syscall_handler(SYS_EPOLL_PWAIT2,
                           (syscall_handle_t)sys_epoll_pwait2);
    regist_syscall_handler(SYS_FCHMODAT2, (syscall_handle_t)sys_fchmodat2);
    regist_syscall_handler(SYS_RSEQ_SLICE_YIELD,
                           (syscall_handle_t)sys_ni_syscall);
}

void loongarch64_do_syscall(struct pt_regs *frame) {
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
        frame->pc += 4;
        return;
    }

    frame->a0 = self->last_syscall_ret;
    frame->syscallno = idx;
    ptrace_on_syscall_enter(frame);

    if (idx >= MAX_SYSCALL_NUM || !syscall_handlers[idx]) {
        frame->a0 = (uint64_t)-ENOSYS;
        goto done;
    }

    arch_enable_user_access();

    if (idx == SYS_CLONE || idx == SYS_CLONE3 || idx == SYS_RT_SIGRETURN) {
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
    if (idx != SYS_BRK && idx != SYS_RSEQ && frame->a0 == (uint64_t)-ENOSYS) {
        serial_fprintk("syscall %d not implemented\n", idx);
    }
    uint64_t next_pc = frame->pc + 4;
    bool restored_context = idx == SYS_RT_SIGRETURN;
    task_signal(frame);
    frame->syscallno = NO_SYSCALL;
    if (!restored_context && frame->pc == next_pc - 4) {
        frame->pc = next_pc;
    }
}
