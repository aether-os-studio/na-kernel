#pragma once

#include <libs/klibc.h>
#include <libs/llist.h>
#include <libs/rbtree.h>
#include <libs/termios.h>
#include <mm/shm.h>
#include <task/ns.h>
#include <task/wait.h>
#include <arch/task_abi.h>

typedef enum task_state {
    TASK_CREATING = 1,
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKING,
    TASK_UNINTERRUPTABLE,
    TASK_DIED,
} task_state_t;

#define TASK_FLAG_CPU_PINNED (1ULL << 0)

struct arch_context;
typedef struct arch_context arch_context_t;
typedef struct task_mm_info task_mm_info_t;

struct vfs_file;
struct vfs_path;

struct rlimit {
    size_t rlim_cur;
    size_t rlim_max;
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

typedef struct int_timer_internal {
    uint64_t at;
    uint64_t reset;
} int_timer_internal_t;

typedef union sigval {
    int sival_int;
    void *sival_ptr;
} sigval_t;

typedef struct sigaltstack {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

#define SS_ONSTACK 1
#define SS_DISABLE 2
#define SS_AUTODISARM (1U << 31)

#define MINSIGSTKSZ ARCH_MINSIGSTKSZ
#define SIGSTKSZ ARCH_SIGSTKSZ

#define SIGEV_SIGNAL 0    /* notify via signal */
#define SIGEV_NONE 1      /* other notification: meaningless */
#define SIGEV_THREAD 2    /* deliver via thread creation */
#define SIGEV_THREAD_ID 4 /* deliver to thread */

typedef struct kernel_timer {
    clockid_t clock_type;
    int sigev_signo;
    union sigval sigev_value;
    int sigev_notify;
    int sigev_notify_thread_id;
    int overrun;
    uint64_t expires;
    uint64_t interval;
} kernel_timer_t;

#define MAX_TIMERS_NUM 8

#define FD_TABLE_INITIAL_SIZE 64
#define DEFAULT_NOFILE_LIMIT 1024
#define MAX_FD_NUM 1048576
#define MAX_SHM_NUM 32

typedef struct fd_entry {
    struct vfs_file *file;
    unsigned int flags;
} fd_entry_t;

typedef struct signalfd_ref {
    struct llist_header node;
    struct vfs_file *file;
    int fd;
} signalfd_ref_t;

typedef struct fd_info {
    fd_entry_t *fds;
    size_t max_fds;
    size_t next_fd;
    struct llist_header signalfd_refs;
    spinlock_t fdt_lock;
    volatile int ref_count;
} fd_info_t;

#define with_fd_info_lock(fd_info, op)                                         \
    do {                                                                       \
        if (!fd_info)                                                          \
            break;                                                             \
        spin_lock(&fd_info->fdt_lock);                                         \
        do {                                                                   \
            op;                                                                \
        } while (0);                                                           \
        spin_unlock(&fd_info->fdt_lock);                                       \
    } while (0)

static inline void task_fd_info_ref_init(fd_info_t *fd_info, int refs) {
    if (!fd_info)
        return;
    __atomic_store_n(&fd_info->ref_count, refs, __ATOMIC_RELEASE);
}

static inline int task_fd_info_ref_get(fd_info_t *fd_info) {
    if (!fd_info)
        return 0;
    return __atomic_add_fetch(&fd_info->ref_count, 1, __ATOMIC_ACQ_REL);
}

static inline int task_fd_info_ref_put(fd_info_t *fd_info) {
    if (!fd_info)
        return 0;
    return __atomic_sub_fetch(&fd_info->ref_count, 1, __ATOMIC_ACQ_REL);
}

static inline int task_fd_info_ref_read(fd_info_t *fd_info) {
    if (!fd_info)
        return 0;
    return __atomic_load_n(&fd_info->ref_count, __ATOMIC_ACQUIRE);
}

#define TASK_NAME_MAX 16

typedef uint64_t sigset_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0) // 默认的信号处理程序（信号句柄）
#define SIG_IGN ((sighandler_t)1) // 忽略信号的处理程序

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER 0x04000000

typedef struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, void *, void *);
    } __sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
} sigaction_t;

#define sa_handler __sa_handler.sa_handler
#define sa_sigaction __sa_handler.sa_sigaction

#define MINSIG 1
#define MAXSIG 65

#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

union __sifields {
    /* kill() */
    struct {
        int _pid;      /* sender's pid */
        uint32_t _uid; /* sender's uid */
    } _kill;

    /* POSIX.1b timers */
    struct {
        int _tid;         /* timer id */
        int _overrun;     /* overrun count */
        sigval_t _sigval; /* same as below */
        int _sys_private; /* Not used by the kernel. Historic leftover. Always
                             0. */
    } _timer;

    /* POSIX.1b signals */
    struct {
        int _pid;      /* sender's pid */
        uint32_t _uid; /* sender's uid */
        sigval_t _sigval;
    } _rt;

    /* SIGCHLD */
    struct {
        int _pid;      /* which child */
        uint32_t _uid; /* sender's uid */
        int _status;   /* exit code */
        long _utime;
        long _stime;
    } _sigchld;

    /* SIGILL, SIGFPE, SIGSEGV, SIGBUS, SIGTRAP, SIGEMT */
    struct {
        void *_addr; /* faulting insn/memory ref. */

#define __ADDR_BND_PKEY_PAD                                                    \
    (__alignof__(void *) < sizeof(short) ? sizeof(short) : __alignof__(void *))
        union {
            /* used on alpha and sparc */
            int _trapno; /* TRAP # which caused the signal */
            /*
             * used when si_code=BUS_MCEERR_AR or
             * used when si_code=BUS_MCEERR_AO
             */
            short _addr_lsb; /* LSB of the reported address */
            /* used when si_code=SEGV_BNDERR */
            struct {
                char _dummy_bnd[__ADDR_BND_PKEY_PAD];
                void *_lower;
                void *_upper;
            } _addr_bnd;
            /* used when si_code=SEGV_PKUERR */
            struct {
                char _dummy_pkey[__ADDR_BND_PKEY_PAD];
                uint32_t _pkey;
            } _addr_pkey;
            /* used when si_code=TRAP_PERF */
            struct {
                unsigned long _data;
                uint32_t _type;
                uint32_t _flags;
            } _perf;
        };
    } _sigfault;

    /* SIGPOLL */
    struct {
        long _band; /* POLL_IN, POLL_OUT, POLL_MSG */
        int _fd;
    } _sigpoll;

    /* SIGSYS */
    struct {
        void *_call_addr;   /* calling user insn */
        int _syscall;       /* triggering system call number */
        unsigned int _arch; /* AUDIT_ARCH_* of syscall */
    } _sigsys;
};

#define __SIGINFO                                                              \
    struct {                                                                   \
        int si_signo;                                                          \
        int si_errno;                                                          \
        int si_code;                                                           \
        union __sifields _sifields;                                            \
    }

typedef struct siginfo {
    union {
        __SIGINFO;
        int _si_pad[128 / sizeof(int)];
    };
} siginfo_t;

typedef struct pending_signal {
    sigset_t info_mask;
    siginfo_t info[MAXSIG];
} pending_signal_t;

typedef struct task_cpu_account {
    volatile uint64_t runtime_ns;
    volatile int ref_count;
} task_cpu_account_t;

typedef struct task_sighand {
    spinlock_t siglock;
    int ref_count;
    bool group_exit;
    int64_t group_exit_code;
    sigaction_t actions[MAXSIG];
} task_sighand_t;

typedef struct task_signal_info {
    sigset_t signal;
    pending_signal_t pending_signal;
    sigset_t blocked;
    sigset_t sigsuspend_old_mask;
    uint8_t sigsuspend_active;
    stack_t altstack;
    task_sighand_t *sighand;
    task_cpu_account_t *cpu_account;
} task_signal_info_t;

typedef struct task_keyring task_keyring_t;

typedef struct task {
    uint64_t syscall_stack;
    uint64_t kernel_stack;
    void *syscall_stack_base;
    void *kernel_stack_base;
    struct llist_header free_node;
    struct llist_header parent_node;
    struct llist_header pgid_node;
    struct llist_header tgid_node;
    struct llist_header tick_work_node;
    uint64_t pid;
    struct task *parent;
    uint64_t parent_pid;
    int64_t uid;
    int64_t gid;
    int64_t euid;
    int64_t egid;
    int64_t suid;
    int64_t sgid;
    int64_t fsuid;
    int64_t fsgid;
    spinlock_t groups_lock;
    uint32_t *supplementary_groups;
    uint32_t supplementary_group_count;
    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_inheritable;
    uint64_t cap_bounding;
    uint64_t cap_ambient;
    uint32_t securebits;
    bool keep_caps;
    int64_t pgid;
    int64_t tgid;
    int64_t sid;
    uint64_t waitpid;
    uint64_t status;
    rb_node_t timeout_node;
    rb_node_t signal_timer_node;
    uint64_t last_sched_in_ns;
    uint64_t start_time_ns;
    uint64_t user_time_ns;
    uint64_t system_time_ns;
    uint64_t child_user_time_ns;
    uint64_t child_system_time_ns;
    uint64_t preempt_count;
    void *preempt_caller;
    uint32_t cpu_id;
    char name[TASK_NAME_MAX];
    struct vfs_file *exec_file;
    int nice;
    int ioprio;
    int sched_policy;
    int sched_priority;
    void *sched_info;
    volatile task_state_t state;
    volatile task_state_t current_state;
    const char *blocking_reason;
    uint64_t force_wakeup_ns;
    uint64_t signal_timer_deadline_ns;
    uint64_t load_start;
    uint64_t load_end;
    task_mm_info_t *mm;
    arch_context_t *arch_context;
    task_signal_info_t *signal;
    task_keyring_t *session_keyring;
    task_fs_t *fs;
    spinlock_t fd_info_lock;
    fd_info_t *fd_info;
    shm_mapping_t *shm_ids;
    struct vfs_path *procfs_path;
    struct vfs_path *procfs_thread_path;
    uint64_t arg_start;
    uint64_t arg_end;
    uint64_t env_start;
    uint64_t env_end;
    int_timer_internal_t itimer_real;
    spinlock_t timers_lock;
    kernel_timer_t *timers[MAX_TIMERS_NUM];
    struct rlimit rlim[16];
    uint64_t parent_death_sig;
    wait_queue_head_t child_wait;
    int *tidptr;
    int *set_tidptr;
    void *robust_list_head;
    size_t robust_list_len;
    uint32_t ptrace_opts;
    uint32_t ptrace_wait_status;
    uint64_t ptrace_tracer_pid;
    uint64_t ptrace_message;
    siginfo_t ptrace_siginfo;
    struct pt_regs ptrace_regs;
    uint64_t last_syscall_ret;
    uint8_t ptrace_resume_action;
    uint8_t ptrace_last_stop;
    uint8_t ptrace_resume_sig;
    bool ptrace_stopped;
    bool ptrace_wait_pending;
    bool ptrace_syscall_exit_pending;
    bool ptrace_exec_event_pending;
    uint32_t personality;
    uint64_t clone_flags;
    uint64_t flags;
    task_ns_proxy_t *nsproxy;
    bool no_new_privs;
    bool child_subreaper;
    bool is_kernel;
    bool is_clone;
    bool child_vfork_done;
    uint64_t vfork_parent_pid;
    bool orphaned_to_init;
    bool exit_reaped;
    bool on_cpu;
    bool need_resched;
    sigset_t sigwait_mask;
    bool wake_pending;
    bool block_preparing;
    spinlock_t block_lock;
    uint64_t membarrier_seen_seq;
    bool tick_work_active;
    bool tick_work_queued;
    uint32_t tick_work_queue_id;
    bool timeout_queued;
    bool signal_timer_queued;
    bool exited_by_signal;
} task_t;
