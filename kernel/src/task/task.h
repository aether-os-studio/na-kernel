#pragma once

#include <libs/hashmap.h>
#include <arch/arch.h>
#include <task/task_struct.h>
#include <task/signal.h>
#include <libs/termios.h>
#include <mm/bitmap.h>

#define preempt_enable(task)                                                   \
    do {                                                                       \
        task->preempt_caller = __builtin_return_address(0);                    \
        __atomic_fetch_add(&(task)->preempt_count, 1, __ATOMIC_RELAXED);       \
    } while (0);

#define preempt_disable(task)                                                  \
    do {                                                                       \
        __atomic_fetch_sub(&(task)->preempt_count, 1, __ATOMIC_RELAXED);       \
    } while (0);

typedef struct task_index_bucket {
    uint64_t key;
    size_t count;
    struct llist_header tasks;
} task_index_bucket_t;

#define IDLE_PRIORITY 20
#define NORMAL_PRIORITY 0
#define KTHREAD_PRIORITY (-5)
#define TASK_NGROUPS_MAX 65536U

#define AT_NULL 0
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_NOTELF 10
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_CLKTCK 17
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_FPUCW 18
#define AT_DCACHEBSIZE 19
#define AT_ICACHEBSIZE 20
#define AT_UCACHEBSIZE 21
#define AT_IGNOREPPC 22
#define AT_SECURE 23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31
#define AT_SYSINFO 32
#define AT_SYSINFO_EHDR 33

#define USER_MMAP_START 0x10000
#define USER_MMAP_END 0x00007f0000000000

#define USER_STACK_START USER_MMAP_END
#define USER_STACK_END 0x00007ff000000000

#define USER_SIGNAL_TRAMPOLINE_START USER_STACK_END
#define USER_SIGNAL_TRAMPOLINE_END (USER_SIGNAL_TRAMPOLINE_START + PAGE_SIZE)

#define INTERPRETER_BASE_ADDR USER_SIGNAL_TRAMPOLINE_END

#define USER_BRK_START 0x00007ffff0000000
#define USER_BRK_END 0x00007fffff000000

#define PIE_BASE_ADDR USER_BRK_END

#define CLONE_VM 0x00000100 /* set if VM shared between processes */
#define CLONE_FS 0x00000200 /* set if fs info shared between processes */
#define CLONE_FILES                                                            \
    0x00000400 /* set if open files shared between processes                   \
                */
#define CLONE_SIGHAND                                                          \
    0x00000800 /* set if signal handlers and blocked signals shared */
#define CLONE_PIDFD 0x00001000 /* set if a pidfd should be placed in parent */
#define CLONE_PTRACE                                                           \
    0x00002000 /* set if we want to let tracing continue on the child too */
#define CLONE_VFORK                                                            \
    0x00004000 /* set if the parent wants the child to wake it up on           \
                  mm_release */
#define CLONE_PARENT                                                           \
    0x00008000 /* set if we want to have the same parent as the cloner */
#define CLONE_THREAD 0x00010000         /* Same thread group? */
#define CLONE_NEWNS 0x00020000          /* New mount namespace group */
#define CLONE_SYSVSEM 0x00040000        /* share system V SEM_UNDO semantics */
#define CLONE_SETTLS 0x00080000         /* create a new TLS for the child */
#define CLONE_PARENT_SETTID 0x00100000  /* set the TID in the parent */
#define CLONE_CHILD_CLEARTID 0x00200000 /* clear the TID in the child */
#define CLONE_DETACHED 0x00400000       /* Unused, ignored */
#define CLONE_UNTRACED                                                         \
    0x00800000 /* set if the tracing process can't force CLONE_PTRACE on this  \
                  clone */
#define CLONE_CHILD_SETTID 0x01000000 /* set the TID in the child */
#define CLONE_CLEAR_SIGHAND                                                    \
    0x100000000ULL /* reset signal handlers to SIG_DFL in the child */
#define CLONE_INTO_CGROUP                                                      \
    0x200000000ULL                 /* place child into specified cgroup        \
                                    */
#define CLONE_NEWCGROUP 0x02000000 /* New cgroup namespace */
#define CLONE_NEWUTS 0x04000000    /* New utsname namespace */
#define CLONE_NEWIPC 0x08000000    /* New ipc namespace */
#define CLONE_NEWUSER 0x10000000   /* New user namespace */
#define CLONE_NEWPID 0x20000000    /* New pid namespace */
#define CLONE_NEWNET 0x40000000    /* New network namespace */
#define CLONE_IO 0x80000000        /* Clone io context */

extern task_t *arch_get_current();

#define current_task arch_get_current()

static inline bool task_is_on_cpu(task_t *task) {
    return task ? __atomic_load_n(&task->on_cpu, __ATOMIC_ACQUIRE) : false;
}

static inline void task_mark_on_cpu(task_t *task, bool on_cpu) {
    if (!task)
        return;

    __atomic_store_n(&task->on_cpu, on_cpu, __ATOMIC_RELEASE);
}

static inline void task_set_need_resched(task_t *task) {
    if (!task)
        return;

    __atomic_store_n(&task->need_resched, true, __ATOMIC_RELEASE);
}

static inline bool task_set_need_resched_once(task_t *task) {
    if (!task)
        return false;

    return !__atomic_exchange_n(&task->need_resched, true, __ATOMIC_ACQ_REL);
}

static inline void task_clear_need_resched(task_t *task) {
    if (!task)
        return;

    __atomic_store_n(&task->need_resched, false, __ATOMIC_RELEASE);
}

static inline bool task_need_resched(task_t *task) {
    return task ? __atomic_load_n(&task->need_resched, __ATOMIC_ACQUIRE)
                : false;
}

static inline bool task_has_flag(task_t *task, uint64_t flag) {
    return task &&
           (__atomic_load_n(&task->flags, __ATOMIC_ACQUIRE) & flag) != 0;
}

static inline void task_set_flag(task_t *task, uint64_t flag) {
    if (!task)
        return;

    __atomic_fetch_or(&task->flags, flag, __ATOMIC_ACQ_REL);
}

static inline void task_clear_flag(task_t *task, uint64_t flag) {
    if (!task)
        return;

    __atomic_fetch_and(&task->flags, ~flag, __ATOMIC_ACQ_REL);
}

static inline bool task_is_reaped(task_t *task) {
    return task ? __atomic_load_n(&task->exit_reaped, __ATOMIC_ACQUIRE) : true;
}

static inline bool task_try_mark_reaped(task_t *task) {
    bool expected = false;

    return task && __atomic_compare_exchange_n(&task->exit_reaped, &expected,
                                               true, false, __ATOMIC_ACQ_REL,
                                               __ATOMIC_ACQUIRE);
}

static inline task_index_bucket_t *task_index_bucket_lookup(hashmap_t *map,
                                                            uint64_t key) {
    return (task_index_bucket_t *)hashmap_get(map, key);
}

uint32_t alloc_cpu_id();
task_t *get_free_task();

static inline uint64_t task_self_user_ns(task_t *task) {
    if (!task)
        return 0;
    if (task->user_time_ns <= task->system_time_ns)
        return 0;
    return task->user_time_ns - task->system_time_ns;
}

static inline uint64_t task_total_user_ns(task_t *task) {
    if (!task)
        return 0;
    return task_self_user_ns(task) + task->child_user_time_ns;
}

static inline uint64_t task_total_system_ns(task_t *task) {
    if (!task)
        return 0;
    return task->system_time_ns + task->child_system_time_ns;
}

static inline uint64_t task_account_runtime_ns(task_t *task, uint64_t now_ns) {
    if (!task || !task->last_sched_in_ns || now_ns <= task->last_sched_in_ns)
        return 0;

    uint64_t delta = now_ns - task->last_sched_in_ns;
    task->last_sched_in_ns = now_ns;

    task->user_time_ns += delta;
    if (task->signal && task->signal->cpu_account)
        __atomic_add_fetch(&task->signal->cpu_account->runtime_ns, delta,
                           __ATOMIC_RELAXED);
    return delta;
}

static inline void task_aggregate_child_usage(task_t *parent, task_t *child) {
    if (!parent || !child)
        return;
    parent->child_user_time_ns += task_total_user_ns(child);
    parent->child_system_time_ns += task_total_system_ns(child);
}

static inline bool task_should_index_pgid(task_t *task, int64_t pgid) {
    return task && task->pid && pgid != 0;
}

extern hashmap_t task_pid_map;
extern hashmap_t task_tgid_map;

void task_tgid_index_attach_locked(task_t *task);
void task_tgid_index_detach_locked(task_t *task);

static inline task_t *task_lookup_by_pid_nolock(uint64_t pid) {
    if (pid == 0)
        return NULL;

    return (task_t *)hashmap_get(&task_pid_map, pid);
}

static inline uint64_t task_effective_wait_parent_pid(task_t *task) {
    if (!task) {
        return 0;
    }

    return task->tgid > 0 ? (uint64_t)task->tgid : task->pid;
}

static inline uint64_t task_effective_tgid(task_t *task) {
    if (!task) {
        return 0;
    }

    return task->tgid > 0 ? (uint64_t)task->tgid : task->pid;
}

static inline bool task_has_parent(task_t *task) {
    return task && task->parent_pid != 0;
}

static inline uint64_t task_parent_pid(task_t *task) {
    return task_has_parent(task) ? task->parent_pid : 0;
}

static inline uint64_t task_parent_wait_key(task_t *task) {
    return task_parent_pid(task);
}

static inline struct vfs_process_fs *task_vfs_fs(task_t *task) {
    return (task && task->fs) ? &task->fs->vfs : NULL;
}

static inline const struct vfs_path *task_fs_root_path(task_t *task) {
    return (task && task->fs) ? &task->fs->vfs.root : &vfs_root_path;
}

static inline const struct vfs_path *task_fs_pwd_path(task_t *task) {
    return (task && task->fs) ? &task->fs->vfs.pwd : task_fs_root_path(task);
}

static inline bool task_should_index_parent(task_t *task) {
    return task && task->pid && task_parent_wait_key(task) != 0 &&
           !task->is_clone && !(task->clone_flags & CLONE_THREAD);
}

void sched_wake_softirqd(uint32_t cpu_id);
void sched_check_wakeup();
uint64_t sched_next_wakeup_ns(void);
void sched_request_resched(task_t *task);
void sched_resched_if_needed(void);
void task_wait_poll_loop(void);
void sched_refresh_preempt_deadline(uint32_t cpu_id, task_t *task,
                                    uint64_t now_ns);

struct vfs_process_fs *task_current_vfs_fs(void);
fd_info_t *task_fd_info_alloc(size_t max_fds);
fd_info_t *task_fd_info_clone(fd_info_t *old);
int task_fd_info_expand(fd_info_t *fd_info, size_t min_fds);
void task_fd_info_free(fd_info_t *fd_info);
fd_info_t *task_fd_info_get(task_t *task);
fd_info_t *task_fd_info_replace(task_t *task, fd_info_t *new_info);
fd_info_t *task_fd_info_detach(task_t *task);
int task_fd_slot_install(fd_info_t *fd_info, int fd, struct vfs_file *file,
                         unsigned int flags);
struct vfs_file *task_fd_slot_take(fd_info_t *fd_info, int fd,
                                   unsigned int *flags);
struct vfs_file *task_get_file(task_t *task, int fd);
int task_get_fd_flags_for_file(task_t *task, int fd, struct vfs_file *file,
                               unsigned int *flags);
int task_set_fd_flags_mask_for_file(task_t *task, int fd, struct vfs_file *file,
                                    unsigned int set, unsigned int clear);
int task_install_file(task_t *task, struct vfs_file *file,
                      unsigned int fd_flags, int min_fd);
int task_replace_file(task_t *task, int fd, struct vfs_file *file,
                      unsigned int fd_flags);
int task_close_file_descriptor(task_t *task, int fd);
void task_fd_info_put(fd_info_t *fd_info, task_t *task);
void task_refresh_tick_work_state(task_t *task);
void task_schedule_reap(void);
void task_enqueue_mm_free(task_mm_info_t *mm);
void task_detach_children_from_parent_locked(task_t *owner);
void task_detach_children_from_parent(task_t *owner);
int task_groups_set(task_t *task, const uint32_t *groups, size_t count);
int task_groups_copy(task_t *task, uint32_t *groups, size_t capacity);
int task_groups_inherit(task_t *child, task_t *parent);
void task_groups_release(task_t *task);
bool task_in_supplementary_group(task_t *task, uint32_t gid);

task_t *task_create(const char *name, void (*entry)(uint64_t), uint64_t arg,
                    int priority);
void task_cleanup_partial(task_t *task, bool kernel_mm);
void task_init();
task_signal_info_t *task_signal_create_empty(void);
task_signal_info_t *task_signal_clone(task_t *parent, uint64_t flags);
task_signal_info_t *task_signal_reset_after_exec(task_t *task);
void task_signal_free(task_signal_info_t *signal);
size_t task_thread_group_count(uint64_t tgid);
bool task_group_exit_code(task_t *task, int64_t *code_out);

struct pt_regs;

uint64_t task_execve(const char *path, const char **argv, const char **envp);
void task_exit_inner(task_t *task, int64_t code);
uint64_t task_exit_thread(int64_t code);
uint64_t task_exit(int64_t code);

int task_block(task_t *task, task_state_t state, int64_t timeout_ns,
               const char *blocking_reason);

typedef struct task_unblock_token {
    task_t *task;
    bool accepted;
    bool prepared;
    bool cancel_timeout;
} task_unblock_token_t;

bool task_unblock_prepare(task_t *task, int reason,
                          task_unblock_token_t *token);
void task_unblock_finish(task_unblock_token_t *token);
void task_unblock(task_t *task, int reason);
void task_prepare_block(task_t *task);
void task_cancel_block_prepare(task_t *task);
void task_membarrier_checkpoint(task_t *task);

void futex_init();
task_t *task_find_by_pid(uint64_t pid);
void task_complete_vfork(task_t *task);
size_t task_count(void);
int task_kill_all(int sig);
int task_kill_thread_group(uint64_t tgid, int sig);
int task_kill_process_group(int pgid, int sig);
size_t task_reap_deferred(size_t budget);
extern spinlock_t task_queue_lock;

extern task_t *idle_tasks[MAX_CPU_NUM];

#define SCHED_FLAG_YIELD (1UL << 0)
void schedule(uint64_t sched_flags);
