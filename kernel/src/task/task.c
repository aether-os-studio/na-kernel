#include <boot/boot.h>
#include <libs/rbtree.h>
#include <arch/arch.h>
#include <task/task.h>
#include <task/futex.h>
#include <task/ptrace.h>
#include <task/sched.h>
#include <drivers/logger.h>
#include <drivers/clockevent.h>
#include <drivers/deadline.h>
#include <fs/vfs/vfs.h>
#include <fs/fs_syscall.h>
#include <arch/arch.h>
#include <mm/mm.h>
#include <mm/shm.h>
#include <net/socket.h>
#include <irq/irq_manager.h>
#include <irq/softirq.h>
#include <init/callbacks.h>
#include <task/keyring.h>
#include <task/ns.h>

volatile unsigned long jiffies;

sched_rq_t schedulers[MAX_CPU_NUM];

spinlock_t task_queue_lock = SPIN_INIT;
task_t *idle_tasks[MAX_CPU_NUM];
uint64_t next_task_pid = 1;
hashmap_t task_pid_map = HASHMAP_INIT;
hashmap_t task_parent_map = HASHMAP_INIT;
hashmap_t task_pgid_map = HASHMAP_INIT;
hashmap_t task_tgid_map = HASHMAP_INIT;
rb_root_t task_timeout_roots[MAX_CPU_NUM];
rb_root_t task_signal_timer_roots[MAX_CPU_NUM];
spinlock_t task_timeout_locks[MAX_CPU_NUM];
spinlock_t task_signal_timer_locks[MAX_CPU_NUM];
static uint64_t task_timeout_next_ns[MAX_CPU_NUM];
static uint64_t task_signal_timer_next_ns[MAX_CPU_NUM];
static deadline_source_t sched_tick_deadline_sources[MAX_CPU_NUM];
static deadline_source_t task_timeout_deadline_sources[MAX_CPU_NUM];
static deadline_source_t task_signal_timer_deadline_sources[MAX_CPU_NUM];
#define TASK_TIMER_PENDING_WORDS ((MAX_CPU_NUM + 63U) / 64U)
static uint64_t task_timer_pending_cpus[TASK_TIMER_PENDING_WORDS];
spinlock_t should_free_lock = SPIN_INIT;
DEFINE_LLIST(should_free_tasks);
static spinlock_t should_free_mm_lock = SPIN_INIT;
static DEFINE_LLIST(should_free_mms);

int task_groups_set(task_t *task, const uint32_t *groups, size_t count) {
    uint32_t *replacement = NULL;
    uint32_t *old_groups;

    if (!task || count > TASK_NGROUPS_MAX || (count && !groups))
        return -EINVAL;

    if (count) {
        replacement = malloc(count * sizeof(*replacement));
        if (!replacement)
            return -ENOMEM;
        memcpy(replacement, groups, count * sizeof(*replacement));
    }

    spin_lock(&task->groups_lock);
    old_groups = task->supplementary_groups;
    task->supplementary_groups = replacement;
    task->supplementary_group_count = (uint32_t)count;
    spin_unlock(&task->groups_lock);

    free(old_groups);
    return 0;
}

int task_groups_copy(task_t *task, uint32_t *groups, size_t capacity) {
    uint32_t count;

    if (!task)
        return -EINVAL;

    spin_lock(&task->groups_lock);
    count = task->supplementary_group_count;
    if (!groups) {
        spin_unlock(&task->groups_lock);
        return (int)count;
    }
    if (capacity < count) {
        spin_unlock(&task->groups_lock);
        return -EINVAL;
    }
    if (count)
        memcpy(groups, task->supplementary_groups, count * sizeof(*groups));
    spin_unlock(&task->groups_lock);
    return (int)count;
}

int task_groups_inherit(task_t *child, task_t *parent) {
    uint32_t *groups = NULL;
    uint32_t count;

    if (!child || !parent)
        return -EINVAL;

    spin_lock(&parent->groups_lock);
    count = parent->supplementary_group_count;
    if (count) {
        groups = malloc((size_t)count * sizeof(*groups));
        if (!groups) {
            spin_unlock(&parent->groups_lock);
            return -ENOMEM;
        }
        memcpy(groups, parent->supplementary_groups,
               (size_t)count * sizeof(*groups));
    }
    spin_unlock(&parent->groups_lock);

    child->supplementary_groups = groups;
    child->supplementary_group_count = count;
    return 0;
}

void task_groups_release(task_t *task) {
    uint32_t *groups;

    if (!task)
        return;

    spin_lock(&task->groups_lock);
    groups = task->supplementary_groups;
    task->supplementary_groups = NULL;
    task->supplementary_group_count = 0;
    spin_unlock(&task->groups_lock);
    free(groups);
}

bool task_in_supplementary_group(task_t *task, uint32_t gid) {
    bool found = false;

    if (!task)
        return false;

    spin_lock(&task->groups_lock);
    for (uint32_t i = 0; i < task->supplementary_group_count; i++) {
        if (task->supplementary_groups[i] == gid) {
            found = true;
            break;
        }
    }
    spin_unlock(&task->groups_lock);
    return found;
}

typedef struct deferred_mm_free {
    struct llist_header node;
    task_mm_info_t *mm;
} deferred_mm_free_t;

task_t *init_task = NULL;
task_t *softirqd_task = NULL;

static inline void sched_update_preempt_deadline(uint32_t cpu_id, task_t *task,
                                                 uint64_t now_ns) {
    uint64_t deadline =
        sched_next_preempt_deadline(&schedulers[cpu_id], task, now_ns);

    uint64_t tick_deadline = now_ns + (1000000000ULL / SCHED_HZ);

    if (deadline == UINT64_MAX || deadline > tick_deadline)
        deadline = tick_deadline;

    deadline_source_update(&sched_tick_deadline_sources[cpu_id], deadline);
}

void sched_refresh_preempt_deadline(uint32_t cpu_id, task_t *task,
                                    uint64_t now_ns) {
    if (cpu_id >= MAX_CPU_NUM)
        return;

    sched_update_preempt_deadline(cpu_id, task, now_ns);
}

struct vfs_process_fs *task_current_vfs_fs(void) {
    return current_task ? task_vfs_fs(current_task) : NULL;
}

fd_info_t *task_fd_info_alloc(size_t max_fds) {
    if (max_fds == 0)
        max_fds = 1;
    if (max_fds > SIZE_MAX / sizeof(fd_entry_t))
        return NULL;

    fd_info_t *fd_info = calloc(1, sizeof(*fd_info));
    if (!fd_info)
        return NULL;

    fd_info->fds = calloc(max_fds, sizeof(fd_entry_t));
    if (!fd_info->fds) {
        free(fd_info);
        return NULL;
    }

    fd_info->max_fds = max_fds;
    fd_info->next_fd = 0;
    llist_init_head(&fd_info->signalfd_refs);
    spin_init(&fd_info->fdt_lock);
    task_fd_info_ref_init(fd_info, 1);
    return fd_info;
}

fd_info_t *task_fd_info_clone(fd_info_t *old) {
    if (!old)
        return NULL;

    fd_info_t *new_info = task_fd_info_alloc(old->max_fds);
    if (!new_info)
        return NULL;

    bool failed = false;
    with_fd_info_lock(old, {
        for (size_t i = 0; i < old->max_fds; i++) {
            if (!old->fds[i].file)
                continue;
            if (task_fd_slot_install(new_info, (int)i, old->fds[i].file,
                                     old->fds[i].flags) < 0) {
                failed = true;
                break;
            }
        }
    });

    if (failed) {
        task_fd_info_put(new_info, NULL);
        return NULL;
    }
    return new_info;
}

int task_fd_info_expand(fd_info_t *fd_info, size_t min_fds) {
    if (!fd_info)
        return -EINVAL;
    if (min_fds <= fd_info->max_fds)
        return 0;
    if (min_fds > SIZE_MAX / sizeof(fd_entry_t))
        return -ENOMEM;

    fd_entry_t *new_fds = calloc(min_fds, sizeof(*new_fds));
    if (!new_fds)
        return -ENOMEM;

    memcpy(new_fds, fd_info->fds, fd_info->max_fds * sizeof(*new_fds));
    free(fd_info->fds);
    fd_info->fds = new_fds;
    fd_info->max_fds = min_fds;
    return 0;
}

void task_fd_info_free(fd_info_t *fd_info) {
    if (!fd_info)
        return;

    while (!llist_empty(&fd_info->signalfd_refs)) {
        signalfd_ref_t *pos =
            list_entry(fd_info->signalfd_refs.next, signalfd_ref_t, node);

        llist_delete(&pos->node);
        free(pos);
    }

    free(fd_info->fds);
    free(fd_info);
}

static signalfd_ref_t *task_fd_signalfd_ref_find_locked(fd_info_t *fd_info,
                                                        int fd) {
    struct llist_header *node;

    if (!fd_info)
        return NULL;

    node = fd_info->signalfd_refs.next;
    while (node != &fd_info->signalfd_refs) {
        signalfd_ref_t *pos = list_entry(node, signalfd_ref_t, node);

        node = node->next;
        if (pos->fd == fd)
            return pos;
    }

    return NULL;
}

static int task_fd_signalfd_ref_add_locked(fd_info_t *fd_info, int fd,
                                           struct vfs_file *file) {
    signalfd_ref_t *ref;

    if (!fd_info || fd < 0 || !file || !signalfd_is_file(file))
        return 0;

    ref = calloc(1, sizeof(*ref));
    if (!ref)
        return -ENOMEM;

    llist_init_head(&ref->node);
    ref->file = file;
    ref->fd = fd;
    llist_append(&fd_info->signalfd_refs, &ref->node);
    return 0;
}

static void task_fd_signalfd_ref_remove_locked(fd_info_t *fd_info, int fd) {
    signalfd_ref_t *ref = task_fd_signalfd_ref_find_locked(fd_info, fd);

    if (!ref)
        return;

    llist_delete(&ref->node);
    free(ref);
}

int task_fd_slot_install(fd_info_t *fd_info, int fd, struct vfs_file *file,
                         unsigned int flags) {
    int ret;

    if (!fd_info || fd < 0 || (size_t)fd >= fd_info->max_fds || !file)
        return -EINVAL;
    if (fd_info->fds[fd].file)
        return -EEXIST;

    fd_info->fds[fd].file = vfs_file_get(file);
    if (!fd_info->fds[fd].file)
        return -ENOMEM;
    vfs_file_fd_ref_get(file);
    fd_info->fds[fd].flags = flags;

    ret = task_fd_signalfd_ref_add_locked(fd_info, fd, file);
    if (ret < 0) {
        vfs_file_fd_ref_put(fd_info->fds[fd].file);
        vfs_file_put(fd_info->fds[fd].file);
        fd_info->fds[fd].file = NULL;
        fd_info->fds[fd].flags = 0;
        return ret;
    }

    if ((size_t)fd == fd_info->next_fd) {
        while (fd_info->next_fd < fd_info->max_fds &&
               fd_info->fds[fd_info->next_fd].file)
            fd_info->next_fd++;
    }

    return 0;
}

struct vfs_file *task_fd_slot_take(fd_info_t *fd_info, int fd,
                                   unsigned int *flags) {
    struct vfs_file *file;

    if (!fd_info || fd < 0 || (size_t)fd >= fd_info->max_fds)
        return NULL;

    file = fd_info->fds[fd].file;
    if (!file)
        return NULL;

    if (flags)
        *flags = fd_info->fds[fd].flags;

    task_fd_signalfd_ref_remove_locked(fd_info, fd);
    fd_info->fds[fd].file = NULL;
    fd_info->fds[fd].flags = 0;
    vfs_file_fd_ref_put(file);
    if ((size_t)fd < fd_info->next_fd)
        fd_info->next_fd = (size_t)fd;
    return file;
}

fd_info_t *task_fd_info_get(task_t *task) {
    fd_info_t *fd_info = NULL;

    if (!task)
        return NULL;

    spin_lock(&task->fd_info_lock);
    fd_info = task->fd_info;
    if (fd_info)
        task_fd_info_ref_get(fd_info);
    spin_unlock(&task->fd_info_lock);

    return fd_info;
}

fd_info_t *task_fd_info_replace(task_t *task, fd_info_t *new_info) {
    fd_info_t *old = NULL;

    if (!task)
        return NULL;

    spin_lock(&task->fd_info_lock);
    old = task->fd_info;
    task->fd_info = new_info;
    spin_unlock(&task->fd_info_lock);

    return old;
}

fd_info_t *task_fd_info_detach(task_t *task) {
    return task_fd_info_replace(task, NULL);
}

struct vfs_file *task_get_file(task_t *task, int fd) {
    struct vfs_file *file = NULL;
    fd_info_t *fd_info;

    if (!task || fd < 0)
        return NULL;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return NULL;

    with_fd_info_lock(fd_info, {
        if ((size_t)fd >= fd_info->max_fds)
            break;
        file = vfs_file_get(fd_info->fds[fd].file);
    });

    task_fd_info_put(fd_info, task);

    return file;
}

int task_get_fd_flags_for_file(task_t *task, int fd, struct vfs_file *file,
                               unsigned int *flags) {
    fd_info_t *fd_info;
    int ret = -EBADF;

    if (!task || fd < 0 || !file || !flags)
        return -EBADF;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return -EBADF;

    with_fd_info_lock(fd_info, {
        if ((size_t)fd >= fd_info->max_fds)
            break;
        if (fd_info->fds[fd].file != file)
            break;
        *flags = fd_info->fds[fd].flags;
        ret = 0;
    });

    task_fd_info_put(fd_info, task);
    return ret;
}

int task_set_fd_flags_mask_for_file(task_t *task, int fd, struct vfs_file *file,
                                    unsigned int set, unsigned int clear) {
    fd_info_t *fd_info;
    int ret = -EBADF;

    if (!task || fd < 0 || !file)
        return -EBADF;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return -EBADF;

    with_fd_info_lock(fd_info, {
        if ((size_t)fd >= fd_info->max_fds)
            break;
        if (fd_info->fds[fd].file != file)
            break;
        fd_info->fds[fd].flags &= ~clear;
        fd_info->fds[fd].flags |= set;
        ret = 0;
    });

    task_fd_info_put(fd_info, task);
    return ret;
}

int task_install_file(task_t *task, struct vfs_file *file,
                      unsigned int fd_flags, int min_fd) {
    fd_info_t *fd_info;
    int newfd = -EMFILE;

    if (!task || !file)
        return -EINVAL;
    if (min_fd < 0)
        min_fd = 0;
    if ((size_t)min_fd >= task->rlim[RLIMIT_NOFILE].rlim_cur)
        return -EMFILE;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return -EINVAL;

    with_fd_info_lock(fd_info, {
        size_t limit =
            MIN(fd_info->max_fds, task->rlim[RLIMIT_NOFILE].rlim_cur);
        size_t scan_start = MAX((size_t)min_fd, fd_info->next_fd);
        for (size_t i = scan_start; i < limit; ++i) {
            if (fd_info->fds[i].file)
                continue;
            if (task_fd_slot_install(fd_info, (int)i, file, fd_flags) < 0)
                break;
            newfd = (int)i;
            break;
        }

        if (newfd < 0 &&
            task->rlim[RLIMIT_NOFILE].rlim_cur > fd_info->max_fds) {
            size_t old_max = fd_info->max_fds;
            size_t new_max = MIN(task->rlim[RLIMIT_NOFILE].rlim_cur,
                                 MAX(old_max * 2, old_max + 1));
            if ((size_t)min_fd >= new_max)
                new_max = (size_t)min_fd + 1;
            if (task_fd_info_expand(fd_info, new_max) < 0)
                break;

            limit = MIN(fd_info->max_fds, task->rlim[RLIMIT_NOFILE].rlim_cur);
            for (size_t i = MAX((size_t)min_fd, old_max); i < limit; ++i) {
                if (fd_info->fds[i].file)
                    continue;
                if (task_fd_slot_install(fd_info, (int)i, file, fd_flags) < 0)
                    break;
                newfd = (int)i;
                break;
            }
        }
    });

    task_fd_info_put(fd_info, task);

    if (newfd >= 0)
        on_open_file_call(task, newfd);
    return newfd;
}

int task_replace_file(task_t *task, int fd, struct vfs_file *file,
                      unsigned int fd_flags) {
    fd_info_t *fd_info;
    struct vfs_file *old = NULL;
    int ret = fd;

    if (!task || !file || fd < 0 ||
        (size_t)fd >= task->rlim[RLIMIT_NOFILE].rlim_cur)
        return -EINVAL;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return -EINVAL;

    with_fd_info_lock(fd_info, {
        if ((size_t)fd >= fd_info->max_fds &&
            (size_t)fd < task->rlim[RLIMIT_NOFILE].rlim_cur) {
            if (task_fd_info_expand(fd_info, (size_t)fd + 1) < 0) {
                ret = -ENOMEM;
                break;
            }
        }
        if ((size_t)fd >= fd_info->max_fds) {
            ret = -EINVAL;
            break;
        }
        old = task_fd_slot_take(fd_info, fd, NULL);
        if (task_fd_slot_install(fd_info, fd, file, fd_flags) < 0) {
            if (old)
                task_fd_slot_install(fd_info, fd, old, fd_flags);
            ret = -ENOMEM;
            break;
        }
    });

    task_fd_info_put(fd_info, task);

    if (ret < 0)
        return ret;

    if (old) {
        on_close_file_call(task, fd, old);
        vfs_close_file(old);
    } else {
        on_open_file_call(task, fd);
    }

    return ret;
}

int task_close_file_descriptor(task_t *task, int fd) {
    fd_info_t *fd_info;
    struct vfs_file *file = NULL;

    if (!task || fd < 0)
        return -EBADF;

    fd_info = task_fd_info_get(task);
    if (!fd_info)
        return -EBADF;

    with_fd_info_lock(fd_info, {
        if ((size_t)fd >= fd_info->max_fds)
            break;
        file = task_fd_slot_take(fd_info, fd, NULL);
    });

    task_fd_info_put(fd_info, task);

    if (!file)
        return -EBADF;

    on_close_file_call(task, fd, file);
    return vfs_close_file(file);
}

static void sched_update_itimer_task(task_t *task, uint64_t now_ms);
static void task_reap_softirq(void);
static void task_signal_timer_update(task_t *task);

static inline uint64_t realtime_now_ns(void) {
    return boot_get_boottime() * 1000000000ULL + nano_time();
}

static inline uint64_t task_timer_current_time_ns(clockid_t clock_type) {
    if (clock_type == CLOCK_REALTIME)
        return realtime_now_ns();

    return nano_time();
}
void task_refresh_tick_work_state(task_t *task);

static task_sighand_t *task_sighand_alloc(void) {
    task_sighand_t *sighand = calloc(1, sizeof(task_sighand_t));
    if (!sighand)
        return NULL;

    spin_init(&sighand->siglock);
    sighand->ref_count = 1;
    return sighand;
}

static void task_sighand_get(task_sighand_t *sighand) {
    if (!sighand)
        return;

    __atomic_add_fetch(&sighand->ref_count, 1, __ATOMIC_RELAXED);
}

static void task_sighand_put(task_sighand_t *sighand) {
    if (!sighand)
        return;

    if (__atomic_sub_fetch(&sighand->ref_count, 1, __ATOMIC_ACQ_REL) == 0) {
        free(sighand);
    }
}

static task_cpu_account_t *task_cpu_account_alloc(void) {
    task_cpu_account_t *account = calloc(1, sizeof(*account));
    if (account)
        account->ref_count = 1;
    return account;
}

static void task_cpu_account_get(task_cpu_account_t *account) {
    if (account)
        __atomic_add_fetch(&account->ref_count, 1, __ATOMIC_RELAXED);
}

static void task_cpu_account_put(task_cpu_account_t *account) {
    if (!account)
        return;
    if (__atomic_sub_fetch(&account->ref_count, 1, __ATOMIC_ACQ_REL) == 0)
        free(account);
}

task_signal_info_t *task_signal_create_empty(void) {
    task_signal_info_t *signal = calloc(1, sizeof(task_signal_info_t));
    if (!signal)
        return NULL;

    signal->altstack.ss_flags = SS_DISABLE;
    signal->sighand = task_sighand_alloc();
    if (!signal->sighand) {
        free(signal);
        return NULL;
    }
    signal->cpu_account = task_cpu_account_alloc();
    if (!signal->cpu_account) {
        task_sighand_put(signal->sighand);
        free(signal);
        return NULL;
    }

    return signal;
}

task_signal_info_t *task_signal_clone(task_t *parent, uint64_t flags) {
    if (!parent || !parent->signal || !parent->signal->sighand)
        return NULL;

    task_signal_info_t *signal = task_signal_create_empty();
    if (!signal)
        return NULL;

    spin_lock(&parent->signal->sighand->siglock);
    if (flags & CLONE_SIGHAND) {
        task_sighand_put(signal->sighand);
        signal->sighand = parent->signal->sighand;
        task_sighand_get(signal->sighand);
    } else if (!(flags & CLONE_CLEAR_SIGHAND)) {
        memcpy(signal->sighand->actions, parent->signal->sighand->actions,
               sizeof(signal->sighand->actions));
    }

    if (flags & CLONE_THREAD) {
        task_cpu_account_put(signal->cpu_account);
        signal->cpu_account = parent->signal->cpu_account;
        task_cpu_account_get(signal->cpu_account);
    }

    signal->blocked = parent->signal->blocked;
    if ((flags & CLONE_VM) && !(flags & CLONE_VFORK)) {
        signal->altstack.ss_sp = NULL;
        signal->altstack.ss_size = 0;
        signal->altstack.ss_flags = SS_DISABLE;
    } else {
        signal->altstack = parent->signal->altstack;
        signal->altstack.ss_flags &= SS_AUTODISARM;
    }
    spin_unlock(&parent->signal->sighand->siglock);

    return signal;
}

task_signal_info_t *task_signal_reset_after_exec(task_t *task) {
    if (!task || !task->signal || !task->signal->sighand)
        return NULL;

    task_signal_info_t *signal = task_signal_create_empty();
    if (!signal)
        return NULL;

    spin_lock(&task->signal->sighand->siglock);
    // exec resets caught signals to SIG_DFL, but ignored dispositions remain
    // ignored.  Programs use this inherited state as part of their exec
    // protocol, so preserving only SIGCHLD is not sufficient.
    for (int sig = MINSIG; sig < MAXSIG; sig++) {
        if (task->signal->sighand->actions[sig].sa_handler == SIG_IGN)
            signal->sighand->actions[sig].sa_handler = SIG_IGN;
    }
    signal->blocked = task->signal->blocked;
    signal->signal = task->signal->signal;
    signal->pending_signal = task->signal->pending_signal;
    task_cpu_account_put(signal->cpu_account);
    signal->cpu_account = task->signal->cpu_account;
    task_cpu_account_get(signal->cpu_account);
    spin_unlock(&task->signal->sighand->siglock);

    return signal;
}

void task_signal_free(task_signal_info_t *signal) {
    if (!signal)
        return;

    task_sighand_put(signal->sighand);
    task_cpu_account_put(signal->cpu_account);
    free(signal);
}

void task_refresh_tick_work_state(task_t *task) {
    if (!task)
        return;

    task->tick_work_active = false;
    task_signal_timer_update(task);
}

void task_membarrier_checkpoint(task_t *task) {
    if (!task || !task->mm)
        return;

    task_mm_info_t *mm = task->mm;
    uint64_t seq = __atomic_load_n(&mm->membarrier_private_expedited_seq,
                                   __ATOMIC_ACQUIRE);
    if (seq == 0)
        return;

    uint64_t task_seen =
        __atomic_load_n(&task->membarrier_seen_seq, __ATOMIC_RELAXED);
    uint32_t cpu_id = current_cpu_id;
    uint64_t cpu_seen =
        cpu_id < MAX_CPU_NUM
            ? __atomic_load_n(&mm->membarrier_cpu_seen_seq[cpu_id],
                              __ATOMIC_RELAXED)
            : seq;
    if (task_seen >= seq && cpu_seen >= seq)
        return;

    memory_barrier();
    if (task_seen < seq)
        __atomic_store_n(&task->membarrier_seen_seq, seq, __ATOMIC_RELEASE);
    if (cpu_id < MAX_CPU_NUM && cpu_seen < seq)
        __atomic_store_n(&mm->membarrier_cpu_seen_seq[cpu_id], seq,
                         __ATOMIC_RELEASE);
}

static void task_init_default_rlimits(task_t *task) {
    size_t infinity = (size_t)-1;

    for (size_t index = 0; index < sizeof(task->rlim) / sizeof(task->rlim[0]);
         index++) {
        task->rlim[index] = (struct rlimit){infinity, infinity};
    }

    task->rlim[RLIMIT_STACK] = (struct rlimit){
        USER_STACK_END - USER_STACK_START, USER_STACK_END - USER_STACK_START};
    task->rlim[RLIMIT_NPROC] = (struct rlimit){MAX_TASK_NUM, MAX_TASK_NUM};
    task->rlim[RLIMIT_NOFILE] =
        (struct rlimit){DEFAULT_NOFILE_LIMIT, MAX_FD_NUM};
    task->rlim[RLIMIT_CORE] = (struct rlimit){0, 0};
}

static inline int task_timeout_cmp_values(uint64_t left_deadline,
                                          uint64_t left_pid,
                                          uint64_t right_deadline,
                                          uint64_t right_pid) {
    if (left_deadline < right_deadline) {
        return -1;
    }
    if (left_deadline > right_deadline) {
        return 1;
    }
    if (left_pid < right_pid) {
        return -1;
    }
    if (left_pid > right_pid) {
        return 1;
    }
    return 0;
}

static inline uint64_t task_signal_timer_to_mono_deadline(clockid_t clock_type,
                                                          uint64_t expires_ns) {
    if (!expires_ns)
        return UINT64_MAX;

    if (clock_type == CLOCK_REALTIME) {
        uint64_t now_real = realtime_now_ns();
        uint64_t now_mono = nano_time();

        if (expires_ns <= now_real)
            return now_mono;

        return now_mono + (expires_ns - now_real);
    }

    return expires_ns;
}

static inline uint32_t task_timer_cpu_id(task_t *task) {
    return (task && task->cpu_id < MAX_CPU_NUM) ? task->cpu_id : 0;
}

static inline task_t *task_timeout_first_locked(uint32_t cpu_id) {
    rb_node_t *first = rb_first(&task_timeout_roots[cpu_id]);
    return first ? rb_entry(first, task_t, timeout_node) : NULL;
}

static inline void task_timeout_refresh_next_locked(uint32_t cpu_id) {
    task_t *first = task_timeout_first_locked(cpu_id);
    uint64_t next = first ? first->force_wakeup_ns : UINT64_MAX;
    __atomic_store_n(&task_timeout_next_ns[cpu_id], next, __ATOMIC_RELEASE);
}

static inline void task_timeout_publish_next(uint32_t cpu_id) {
    uint64_t next =
        __atomic_load_n(&task_timeout_next_ns[cpu_id], __ATOMIC_ACQUIRE);

    deadline_source_update(&task_timeout_deadline_sources[cpu_id], next);
}

static inline uint64_t task_signal_timer_compute_deadline_locked(task_t *task) {
    uint64_t next = UINT64_MAX;

    if (!task || task->state == TASK_DIED)
        return UINT64_MAX;

    if (task->itimer_real.at) {
        next = task->itimer_real.at * 1000000ULL;
    }

    spin_lock(&task->timers_lock);
    for (int i = 0; i < MAX_TIMERS_NUM; i++) {
        kernel_timer_t *kt = task->timers[i];

        if (!kt || !kt->expires)
            continue;

        uint64_t expires =
            task_signal_timer_to_mono_deadline(kt->clock_type, kt->expires);

        if (expires < next)
            next = expires;
    }
    spin_unlock(&task->timers_lock);

    return next;
}

static inline int task_signal_timer_cmp_values(uint64_t left_deadline,
                                               uint64_t left_pid,
                                               uint64_t right_deadline,
                                               uint64_t right_pid) {
    if (left_deadline < right_deadline)
        return -1;
    if (left_deadline > right_deadline)
        return 1;
    if (left_pid < right_pid)
        return -1;
    if (left_pid > right_pid)
        return 1;
    return 0;
}

static inline task_t *task_signal_timer_first_locked(uint32_t cpu_id) {
    rb_node_t *first = rb_first(&task_signal_timer_roots[cpu_id]);
    return first ? rb_entry(first, task_t, signal_timer_node) : NULL;
}

static inline void task_signal_timer_refresh_next_locked(uint32_t cpu_id) {
    task_t *first = task_signal_timer_first_locked(cpu_id);
    uint64_t next = first ? first->signal_timer_deadline_ns : UINT64_MAX;
    __atomic_store_n(&task_signal_timer_next_ns[cpu_id], next,
                     __ATOMIC_RELEASE);
}

static inline void task_signal_timer_publish_next(uint32_t cpu_id) {
    uint64_t next =
        __atomic_load_n(&task_signal_timer_next_ns[cpu_id], __ATOMIC_ACQUIRE);

    deadline_source_update(&task_signal_timer_deadline_sources[cpu_id], next);
}

static void task_signal_timer_remove_locked(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    if (!task || !task->signal_timer_queued)
        return;

    rb_erase(&task->signal_timer_node, &task_signal_timer_roots[cpu_id]);
    memset(&task->signal_timer_node, 0, sizeof(task->signal_timer_node));
    task->signal_timer_queued = false;
}

static void task_signal_timer_add_locked(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    if (!task || task->signal_timer_deadline_ns == UINT64_MAX ||
        task->signal_timer_queued) {
        return;
    }

    rb_node_t **slot = &task_signal_timer_roots[cpu_id].rb_node;
    rb_node_t *parent = NULL;

    while (*slot) {
        task_t *curr = rb_entry(*slot, task_t, signal_timer_node);
        int cmp = task_signal_timer_cmp_values(
            task->signal_timer_deadline_ns, task->pid,
            curr->signal_timer_deadline_ns, curr->pid);
        parent = *slot;
        if (cmp < 0)
            slot = &(*slot)->rb_left;
        else
            slot = &(*slot)->rb_right;
    }

    task->signal_timer_node.rb_left = NULL;
    task->signal_timer_node.rb_right = NULL;
    rb_set_parent(&task->signal_timer_node, parent);
    rb_set_color(&task->signal_timer_node, KRB_RED);
    *slot = &task->signal_timer_node;
    rb_insert_color(&task->signal_timer_node, &task_signal_timer_roots[cpu_id]);
    task->signal_timer_queued = true;
}

static void task_signal_timer_update(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    spin_lock(&task_signal_timer_locks[cpu_id]);
    task_signal_timer_remove_locked(task);
    task->signal_timer_deadline_ns =
        task_signal_timer_compute_deadline_locked(task);
    task_signal_timer_add_locked(task);
    task_signal_timer_refresh_next_locked(cpu_id);
    spin_unlock(&task_signal_timer_locks[cpu_id]);

    task_signal_timer_publish_next(cpu_id);
}

static void task_signal_timer_cancel(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    spin_lock(&task_signal_timer_locks[cpu_id]);
    task_signal_timer_remove_locked(task);
    if (task)
        task->signal_timer_deadline_ns = UINT64_MAX;
    task_signal_timer_refresh_next_locked(cpu_id);
    spin_unlock(&task_signal_timer_locks[cpu_id]);

    task_signal_timer_publish_next(cpu_id);
}

static inline uint64_t task_signal_timer_next_deadline_ns(uint32_t cpu_id) {
    return __atomic_load_n(&task_signal_timer_next_ns[cpu_id],
                           __ATOMIC_ACQUIRE);
}

static void task_timeout_remove_locked(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    if (!task || !task->timeout_queued) {
        return;
    }

    rb_erase(&task->timeout_node, &task_timeout_roots[cpu_id]);
    memset(&task->timeout_node, 0, sizeof(task->timeout_node));
    task->timeout_queued = false;
}

static void task_timeout_add_locked(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    if (!task || task->force_wakeup_ns == UINT64_MAX || task->timeout_queued) {
        return;
    }

    rb_node_t **slot = &task_timeout_roots[cpu_id].rb_node;
    rb_node_t *parent = NULL;

    while (*slot) {
        task_t *curr = rb_entry(*slot, task_t, timeout_node);
        int cmp = task_timeout_cmp_values(task->force_wakeup_ns, task->pid,
                                          curr->force_wakeup_ns, curr->pid);
        parent = *slot;
        if (cmp < 0) {
            slot = &(*slot)->rb_left;
        } else {
            slot = &(*slot)->rb_right;
        }
    }

    task->timeout_node.rb_left = NULL;
    task->timeout_node.rb_right = NULL;
    rb_set_parent(&task->timeout_node, parent);
    rb_set_color(&task->timeout_node, KRB_RED);
    *slot = &task->timeout_node;
    rb_insert_color(&task->timeout_node, &task_timeout_roots[cpu_id]);
    task->timeout_queued = true;
}

void task_timeout_cancel(task_t *task) {
    if (!task)
        return;

    uint32_t cpu_id = task_timer_cpu_id(task);

    spin_lock(&task_timeout_locks[cpu_id]);
    if (!task->timeout_queued) {
        spin_unlock(&task_timeout_locks[cpu_id]);
        return;
    }
    task_timeout_remove_locked(task);
    task_timeout_refresh_next_locked(cpu_id);
    spin_unlock(&task_timeout_locks[cpu_id]);

    task_timeout_publish_next(cpu_id);
}

static void task_timeout_arm(task_t *task) {
    uint32_t cpu_id = task_timer_cpu_id(task);

    spin_lock(&task_timeout_locks[cpu_id]);
    task_timeout_remove_locked(task);
    task_timeout_add_locked(task);
    task_timeout_refresh_next_locked(cpu_id);
    spin_unlock(&task_timeout_locks[cpu_id]);

    task_timeout_publish_next(cpu_id);
}

static void task_timeout_softirq_cpu(uint32_t cpu_id) {
    task_t *expired[32];

    if (cpu_id >= MAX_CPU_NUM)
        return;

    while (true) {
        size_t expired_count = 0;
        uint64_t now = nano_time();

        bool irq_state = arch_interrupt_enabled();

        arch_disable_interrupt();

        spin_lock(&task_timeout_locks[cpu_id]);
        while (expired_count < (sizeof(expired) / sizeof(expired[0]))) {
            task_t *task = task_timeout_first_locked(cpu_id);
            if (!task || task->force_wakeup_ns > now) {
                break;
            }

            task_timeout_remove_locked(task);
            expired[expired_count++] = task;
        }
        task_timeout_refresh_next_locked(cpu_id);
        spin_unlock(&task_timeout_locks[cpu_id]);

        if (irq_state)
            arch_enable_interrupt();

        task_timeout_publish_next(cpu_id);

        if (!expired_count) {
            return;
        }

        for (size_t i = 0; i < expired_count; i++) {
            task_t *task = expired[i];
            if (!task || task->state == TASK_DIED) {
                continue;
            }
            if (task->force_wakeup_ns != UINT64_MAX &&
                task->force_wakeup_ns <= now) {
                task_unblock(task, ETIMEDOUT);
            }
        }

        if (expired_count < (sizeof(expired) / sizeof(expired[0]))) {
            return;
        }
    }
}

static void task_signal_timer_softirq_cpu(uint32_t cpu_id) {
    task_t *expired[32];

    if (cpu_id >= MAX_CPU_NUM)
        return;

    while (true) {
        size_t expired_count = 0;
        uint64_t now = nano_time();
        uint64_t now_ms = now / 1000000ULL;

        bool irq_state = arch_interrupt_enabled();
        arch_disable_interrupt();

        spin_lock(&task_signal_timer_locks[cpu_id]);
        while (expired_count < (sizeof(expired) / sizeof(expired[0]))) {
            task_t *task = task_signal_timer_first_locked(cpu_id);
            if (!task || task->signal_timer_deadline_ns > now)
                break;

            task_signal_timer_remove_locked(task);
            expired[expired_count++] = task;
        }
        task_signal_timer_refresh_next_locked(cpu_id);
        spin_unlock(&task_signal_timer_locks[cpu_id]);

        if (irq_state)
            arch_enable_interrupt();

        task_signal_timer_publish_next(cpu_id);

        if (!expired_count)
            return;

        for (size_t i = 0; i < expired_count; i++) {
            task_t *task = expired[i];
            if (!task || task->state == TASK_DIED)
                continue;

            /* Re-evaluate all task-owned timers from one monotonic snapshot. */
            sched_update_itimer_task(task, now_ms);
        }

        if (expired_count < (sizeof(expired) / sizeof(expired[0])))
            return;
    }
}

static void task_timer_softirq(void) {
    for (uint32_t word = 0; word < TASK_TIMER_PENDING_WORDS; word++) {
        uint64_t pending = __atomic_exchange_n(&task_timer_pending_cpus[word],
                                               0, __ATOMIC_ACQ_REL);

        while (pending) {
            uint32_t bit = (uint32_t)__builtin_ctzll(pending);
            uint32_t cpu_id = word * 64U + bit;
            pending &= pending - 1;

            if (cpu_id >= cpu_count || cpu_id >= MAX_CPU_NUM)
                continue;
            task_timeout_softirq_cpu(cpu_id);
            task_signal_timer_softirq_cpu(cpu_id);
        }
    }
}

static void task_enqueue_should_free_locked(task_t *task) {
    if (!task || !llist_empty(&task->free_node)) {
        return;
    }

    llist_append(&should_free_tasks, &task->free_node);
}

static bool task_should_free_pending(void) {
    bool pending;

    spin_lock(&should_free_lock);
    pending = !llist_empty(&should_free_tasks);
    spin_unlock(&should_free_lock);

    if (!pending) {
        spin_lock(&should_free_mm_lock);
        pending = !llist_empty(&should_free_mms);
        spin_unlock(&should_free_mm_lock);
    }

    return pending;
}

static void task_schedule_reap_softirq(void) {
    if (softirq_raise(SOFTIRQ_TASK_REAP))
        sched_wake_softirqd(current_cpu_id);
}

void task_schedule_reap(void) { task_schedule_reap_softirq(); }

void task_enqueue_should_free(task_t *task) {
    bool queued = false;

    spin_lock(&should_free_lock);
    if (task && llist_empty(&task->free_node)) {
        task_enqueue_should_free_locked(task);
        queued = true;
    }
    spin_unlock(&should_free_lock);

    if (queued)
        task_schedule_reap_softirq();
}

void task_enqueue_mm_free(task_mm_info_t *mm) {
    if (!mm)
        return;

    deferred_mm_free_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        free_page_table(mm);
        return;
    }

    llist_init_head(&entry->node);
    entry->mm = mm;

    spin_lock(&should_free_mm_lock);
    llist_append(&should_free_mms, &entry->node);
    spin_unlock(&should_free_mm_lock);

    task_schedule_reap_softirq();
}

static task_mm_info_t *task_dequeue_mm_free(void) {
    deferred_mm_free_t *entry = NULL;

    spin_lock(&should_free_mm_lock);
    if (!llist_empty(&should_free_mms)) {
        struct llist_header *node = should_free_mms.next;
        llist_delete(node);
        entry = list_entry(node, deferred_mm_free_t, node);
    }
    spin_unlock(&should_free_mm_lock);

    if (!entry)
        return NULL;

    task_mm_info_t *mm = entry->mm;
    free(entry);
    return mm;
}

task_t *task_dequeue_should_free(void) {
    task_t *task = NULL;

    spin_lock(&should_free_lock);
    if (!llist_empty(&should_free_tasks)) {
        struct llist_header *node = should_free_tasks.next;
        llist_delete(node);
        task = list_entry(node, task_t, free_node);
    }
    spin_unlock(&should_free_lock);

    return task;
}

static void task_reap_softirq(void) {
    const size_t reap_budget = 64;
    const size_t mm_reap_budget = 8;
    size_t reaped = task_reap_deferred(reap_budget);
    size_t mm_reaped = 0;

    unmap_release_deferred_drain();

    for (size_t i = 0; i < mm_reap_budget; i++) {
        task_mm_info_t *mm = task_dequeue_mm_free();
        if (!mm)
            break;

        free_page_table(mm);
        mm_reaped++;
    }

    if ((reaped > 0 || mm_reaped > 0) && task_should_free_pending())
        task_schedule_reap_softirq();
}

size_t task_count(void) {
    size_t count;

    spin_lock(&task_queue_lock);
    count = hashmap_size(&task_pid_map);
    spin_unlock(&task_queue_lock);

    return count;
}

size_t task_thread_group_count(uint64_t tgid) {
    size_t count = 0;

    if (!tgid)
        return 0;

    spin_lock(&task_queue_lock);
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    if (bucket) {
        struct llist_header *node = bucket->tasks.next;
        while (node != &bucket->tasks) {
            task_t *task = list_entry(node, task_t, tgid_node);
            node = node->next;
            if (task && task->state != TASK_DIED && task->arch_context)
                count++;
        }
    }
    spin_unlock(&task_queue_lock);

    return count;
}

/*
 * Linux process-directed signals target a thread group once, not every thread
 * in the group. This kernel does not have a separate shared pending-signal
 * object for thread groups yet, so pick one live representative task per tgid
 * to avoid over-delivering group-wide signals to multi-threaded processes.
 */
static bool task_is_signal_group_representative_locked(task_t *task,
                                                       bool skip_kernel) {
    if (!task || task->state == TASK_DIED || !task->arch_context ||
        (skip_kernel && task->is_kernel)) {
        return false;
    }

    uint64_t tgid = task_effective_tgid(task);
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    if (!tgid || !bucket)
        return true;

    struct llist_header *node = bucket->tasks.next;
    while (node != &bucket->tasks) {
        task_t *peer = list_entry(node, task_t, tgid_node);
        node = node->next;
        if (!peer || peer == task || peer->state == TASK_DIED ||
            !peer->arch_context || (skip_kernel && peer->is_kernel)) {
            continue;
        }

        if (peer->pid < task->pid)
            return false;
    }

    return true;
}

static bool task_signal_forces_group_exit(int sig) { return sig == SIGKILL; }

static bool task_signalable_user_task_locked(task_t *task, uint64_t tgid,
                                             bool skip_kernel) {
    return task && task->state != TASK_DIED && task->arch_context &&
           task_effective_tgid(task) == tgid &&
           !(skip_kernel && task->is_kernel);
}

static task_t *task_pick_thread_group_signal_target_locked(uint64_t tgid,
                                                           bool skip_kernel) {
    task_t *fallback = NULL;

    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    if (!tgid || !bucket)
        return NULL;

    struct llist_header *node = bucket->tasks.next;
    while (node != &bucket->tasks) {
        task_t *task = list_entry(node, task_t, tgid_node);
        node = node->next;
        if (!task_signalable_user_task_locked(task, tgid, skip_kernel))
            continue;

        if (!fallback || task->pid < fallback->pid)
            fallback = task;
        if (task->state != TASK_UNINTERRUPTABLE)
            return task;
    }

    return fallback;
}

static void task_mark_group_exit_locked(task_t *task, int64_t code) {
    if (!task || !task->signal || !task->signal->sighand)
        return;

    spin_lock(&task->signal->sighand->siglock);
    task->signal->sighand->group_exit = true;
    task->signal->sighand->group_exit_code = code;
    spin_unlock(&task->signal->sighand->siglock);
}

static int task_kill_thread_group_locked(uint64_t tgid, int sig, int code,
                                         bool skip_kernel) {
    int sent = 0;

    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    if (!tgid || !bucket)
        return 0;

    if (!task_signal_forces_group_exit(sig)) {
        task_t *target =
            task_pick_thread_group_signal_target_locked(tgid, skip_kernel);
        if (!target)
            return 0;

        task_send_signal(target, sig, code);
        return 1;
    }

    struct llist_header *node = bucket->tasks.next;
    while (node != &bucket->tasks) {
        task_t *task = list_entry(node, task_t, tgid_node);
        node = node->next;
        if (!task_signalable_user_task_locked(task, tgid, skip_kernel))
            continue;

        task_mark_group_exit_locked(task, 128 + sig);
    }

    node = bucket->tasks.next;
    while (node != &bucket->tasks) {
        task_t *task = list_entry(node, task_t, tgid_node);
        node = node->next;
        if (!task_signalable_user_task_locked(task, tgid, skip_kernel))
            continue;

        sent++;
        task_send_signal(task, sig, code);
        if (task->state == TASK_BLOCKING ||
            task->state == TASK_UNINTERRUPTABLE || task->block_preparing) {
            task_unblock(task, EOK);
        }
    }

    return sent;
}

int task_kill_thread_group(uint64_t tgid, int sig) {
    int sent;

    spin_lock(&task_queue_lock);
    sent = task_kill_thread_group_locked(tgid, sig, SI_USER, true);
    spin_unlock(&task_queue_lock);

    return sent;
}

int task_kill_all(int sig) {
    int sent = 0;

    spin_lock(&task_queue_lock);
    if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            if (!hashmap_entry_is_occupied(entry)) {
                continue;
            }

            task_t *task = (task_t *)entry->value;
            if (!task ||
                !task_is_signal_group_representative_locked(task, true)) {
                continue;
            }

            sent++;
            if (sig != 0) {
                if (task_signal_forces_group_exit(sig)) {
                    task_kill_thread_group_locked(task_effective_tgid(task),
                                                  sig, SI_USER, true);
                } else {
                    task_send_signal(task, sig, SI_USER);
                }
            }
        }
    }
    spin_unlock(&task_queue_lock);

    return sent;
}

task_t *task_find_by_pid(uint64_t pid) {
    spin_lock(&task_queue_lock);
    task_t *task = task_lookup_by_pid_nolock(pid);
    spin_unlock(&task_queue_lock);
    return task;
}

void task_complete_vfork(task_t *task) {
    if (!task || !(task->clone_flags & CLONE_VFORK)) {
        return;
    }

    uint64_t parent_pid = task->vfork_parent_pid;
    if (!parent_pid) {
        return;
    }

    spin_lock(&task_queue_lock);
    task_t *parent = task_lookup_by_pid_nolock(parent_pid);
    if (!parent || parent->child_vfork_done) {
        task->vfork_parent_pid = 0;
        spin_unlock(&task_queue_lock);
        return;
    }

    parent->child_vfork_done = true;
    task->vfork_parent_pid = 0;
    if (parent->state == TASK_BLOCKING || parent->block_preparing) {
        task_unblock(parent, EOK);
    }
    spin_unlock(&task_queue_lock);
}

static void task_pid_index_add_locked(task_t *task) {
    if (!task || !task->pid) {
        return;
    }

    ASSERT(hashmap_put(&task_pid_map, task->pid, task) == 0);
}

task_index_bucket_t *task_index_bucket_get_or_create(hashmap_t *map,
                                                     uint64_t key) {
    task_index_bucket_t *bucket = task_index_bucket_lookup(map, key);
    if (bucket) {
        return bucket;
    }

    bucket = calloc(1, sizeof(task_index_bucket_t));
    ASSERT(bucket != NULL);

    bucket->key = key;
    llist_init_head(&bucket->tasks);
    ASSERT(hashmap_put(map, key, bucket) == 0);

    return bucket;
}

void task_index_bucket_destroy_if_empty(hashmap_t *map, uint64_t key) {
    task_index_bucket_t *bucket = task_index_bucket_lookup(map, key);
    if (!bucket || bucket->count || !llist_empty(&bucket->tasks)) {
        return;
    }

    hashmap_remove(map, key);
    free(bucket);
}

void task_tgid_index_attach_locked(task_t *task) {
    uint64_t tgid = task_effective_tgid(task);

    if (!task || !task->pid || !tgid || !llist_empty(&task->tgid_node))
        return;

    task_index_bucket_t *bucket =
        task_index_bucket_get_or_create(&task_tgid_map, tgid);
    if (!bucket)
        return;

    llist_append(&bucket->tasks, &task->tgid_node);
    bucket->count++;
}

void task_tgid_index_detach_locked(task_t *task) {
    uint64_t tgid = task_effective_tgid(task);

    if (!task || !tgid || llist_empty(&task->tgid_node))
        return;

    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    llist_delete(&task->tgid_node);
    if (bucket && bucket->count)
        bucket->count--;
    task_index_bucket_destroy_if_empty(&task_tgid_map, tgid);
}

void task_parent_index_attach_locked(task_t *task) {
    if (!task_should_index_parent(task) || !llist_empty(&task->parent_node)) {
        return;
    }

    uint64_t parent_key = task_parent_wait_key(task);
    task_index_bucket_t *bucket =
        task_index_bucket_get_or_create(&task_parent_map, parent_key);
    if (!bucket) {
        return;
    }

    llist_append(&bucket->tasks, &task->parent_node);
    bucket->count++;
}

void task_parent_index_detach_locked(task_t *task, bool prune_bucket) {
    if (!task_should_index_parent(task) || llist_empty(&task->parent_node)) {
        return;
    }

    uint64_t parent_pid = task_parent_wait_key(task);
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_parent_map, parent_pid);
    llist_delete(&task->parent_node);

    if (bucket && bucket->count) {
        bucket->count--;
    }

    if (prune_bucket) {
        task_index_bucket_destroy_if_empty(&task_parent_map, parent_pid);
    }
}

static void task_set_parent_locked(task_t *task, task_t *parent,
                                   bool prune_old_bucket) {
    uint64_t new_parent_pid = task_effective_tgid(parent);

    if (!task ||
        (task->parent == parent && task->parent_pid == new_parent_pid)) {
        return;
    }

    task_parent_index_detach_locked(task, prune_old_bucket);
    task->parent = parent;
    task->parent_pid = new_parent_pid;
    task->orphaned_to_init = parent && parent->pid == 1 && task->pid != 1;
    task_parent_index_attach_locked(task);
}

static inline bool task_is_live_locked(task_t *task) {
    return task && task->state != TASK_DIED;
}

static task_t *task_find_child_reaper_locked(task_t *task) {
    task_t *parent;

    if (!task) {
        return NULL;
    }

    parent = task_lookup_by_pid_nolock(task_parent_pid(task));
    while (parent) {
        if (task_is_live_locked(parent) && parent->child_subreaper)
            return parent;

        uint64_t next_parent_pid = task_parent_pid(parent);
        if (!next_parent_pid)
            break;
        parent = task_lookup_by_pid_nolock(next_parent_pid);
    }

    return NULL;
}

static task_t *task_pick_reaper_locked(task_t *task) {
    if (!task) {
        return NULL;
    }

    uint64_t current_tgid = task_effective_tgid(task);
    task_t *leader = task_lookup_by_pid_nolock(current_tgid);
    if (task_is_live_locked(leader) && leader != task) {
        return leader;
    }

    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, current_tgid);
    if (bucket) {
        struct llist_header *node = bucket->tasks.next;
        while (node != &bucket->tasks) {
            task_t *candidate = list_entry(node, task_t, tgid_node);
            node = node->next;
            if (!task_is_live_locked(candidate) || candidate == task) {
                continue;
            }

            return candidate;
        }
    }

    task_t *subreaper = task_find_child_reaper_locked(task);
    if (subreaper && subreaper != task)
        return subreaper;

    task_t *init = task_lookup_by_pid_nolock(1);
    return init == task ? NULL : init;
}

static task_t *task_pick_process_reaper_locked(task_t *task) {
    if (!task) {
        return NULL;
    }

    task_t *subreaper = task_find_child_reaper_locked(task);
    if (subreaper && subreaper != task)
        return subreaper;

    task_t *init = task_lookup_by_pid_nolock(1);
    return init == task ? NULL : init;
}

static void task_notify_parent_death_locked(task_t *task) {
    if (!task || task->parent_death_sig == (uint64_t)-1) {
        return;
    }

    task_send_signal(task, task->parent_death_sig,
                     task->parent_death_sig + 128);
    if (task->state == TASK_BLOCKING) {
        task_unblock(task, EOK);
    }
}

static void task_reparent_children_locked(task_t *owner, task_t *new_parent,
                                          bool process_wide) {
    if (!owner) {
        return;
    }

    uint64_t owner_tgid = task_effective_tgid(owner);
    task_index_bucket_t *children =
        task_index_bucket_lookup(&task_parent_map, owner_tgid);
    if (!children) {
        return;
    }

    task_t *task, *tmp;
    llist_for_each(task, tmp, &children->tasks, parent_node) {
        if (!task || task == owner || !task_has_parent(task)) {
            continue;
        }

        if (process_wide) {
            if (task_parent_pid(task) != owner_tgid) {
                continue;
            }
        } else if (task->parent != owner) {
            continue;
        }

        task_set_parent_locked(task, new_parent, false);
        task_notify_parent_death_locked(task);
    }

    task_index_bucket_destroy_if_empty(&task_parent_map, owner_tgid);
}

void task_detach_children_from_parent_locked(task_t *owner) {
    if (!owner) {
        return;
    }

    uint64_t owner_tgid = task_effective_tgid(owner);
    task_index_bucket_t *children =
        task_index_bucket_lookup(&task_parent_map, owner_tgid);
    if (!children) {
        return;
    }

    task_t *task, *tmp;
    llist_for_each(task, tmp, &children->tasks, parent_node) {
        if (!task || task == owner || task->parent != owner) {
            continue;
        }

        task_set_parent_locked(task, NULL, false);
    }

    task_index_bucket_destroy_if_empty(&task_parent_map, owner_tgid);
}

void task_detach_children_from_parent(task_t *owner) {
    spin_lock(&task_queue_lock);
    task_detach_children_from_parent_locked(owner);
    spin_unlock(&task_queue_lock);
}

bool task_initialized = false;
bool can_schedule = false;

#define TASK_CAP_LAST_CAP 40U
#define TASK_CAP_FULL_MASK ((UINT64_C(1) << (TASK_CAP_LAST_CAP + 1U)) - 1U)

extern int unix_socket_fsid;
extern int unix_accept_fsid;

static uint32_t cpu_idx = 0;

uint32_t alloc_cpu_id() {
    if (!cpu_count)
        return 0;

    uint32_t start =
        __atomic_fetch_add(&cpu_idx, 1, __ATOMIC_RELAXED) % cpu_count;
    uint32_t best = start;
    size_t best_running = SIZE_MAX;

    for (uint32_t offset = 0; offset < cpu_count; offset++) {
        uint32_t cpu = (start + offset) % cpu_count;
        size_t nr_running = sched_rq_nr_running_snapshot(&schedulers[cpu]);

        if (nr_running < best_running) {
            best = cpu;
            best_running = nr_running;
            if (nr_running == 0)
                break;
        }
    }

    return best;
}

task_t *get_free_task() {
    for (uint64_t i = 0; i < cpu_count; i++) {
        if (idle_tasks[i] == NULL) {
            task_t *task = (task_t *)malloc(sizeof(task_t));
            memset(task, 0, sizeof(task_t));
            llist_init_head(&task->free_node);
            llist_init_head(&task->parent_node);
            llist_init_head(&task->pgid_node);
            llist_init_head(&task->tgid_node);
            llist_init_head(&task->tick_work_node);
            spin_init(&task->block_lock);
            spin_init(&task->fd_info_lock);
            spin_init(&task->groups_lock);
            wait_queue_init(&task->child_wait);
            task->tick_work_queue_id = UINT32_MAX;
            task->state = TASK_CREATING;
            task->pid = 0;
            task->cpu_id = i;
            idle_tasks[i] = task;
            can_schedule = true;
            return task;
        }
    }

    task_t *task = (task_t *)malloc(sizeof(task_t));
    if (!task)
        return NULL;

    memset(task, 0, sizeof(task_t));
    llist_init_head(&task->free_node);
    llist_init_head(&task->parent_node);
    llist_init_head(&task->pgid_node);
    llist_init_head(&task->tgid_node);
    llist_init_head(&task->tick_work_node);
    spin_init(&task->block_lock);
    spin_init(&task->fd_info_lock);
    spin_init(&task->groups_lock);
    wait_queue_init(&task->child_wait);
    task->tick_work_queue_id = UINT32_MAX;
    task->state = TASK_CREATING;
    task->cpu_id = alloc_cpu_id();
    task->start_time_ns = nano_time();

    spin_lock(&task_queue_lock);
    task->pid = next_task_pid++;
    task_pid_index_add_locked(task);
    spin_unlock(&task_queue_lock);
    return task;
}

task_t *task_create(const char *name, void (*entry)(uint64_t), uint64_t arg,
                    int priority) {
    bool irq_state = arch_interrupt_enabled();

    arch_disable_interrupt();

    can_schedule = false;

    task_t *task = get_free_task();
    if (!task) {
        can_schedule = true;
        if (irq_state)
            arch_enable_interrupt();
        return NULL;
    }
    task->signal = task_signal_create_empty();
    if (!task->signal) {
        goto fail;
    }
    task->parent = NULL;
    task->parent_pid = 0;
    task->uid = 0;
    task->gid = 0;
    task->euid = 0;
    task->egid = 0;
    task->suid = 0;
    task->sgid = 0;
    task->fsuid = 0;
    task->fsgid = 0;
    task->cap_effective = TASK_CAP_FULL_MASK;
    task->cap_permitted = TASK_CAP_FULL_MASK;
    task->cap_inheritable = 0;
    task->cap_bounding = TASK_CAP_FULL_MASK;
    task->cap_ambient = 0;
    task->securebits = 0;
    task->keep_caps = false;
    task->pgid = 0;
    task->tgid = task->pid;
    task->sid = 0;
    task->nice = priority;
    task->sched_policy = 0;
    task->sched_priority = 0;

    void *kernel_stack_base = alloc_frames_bytes(STACK_SIZE);
    if (!kernel_stack_base)
        goto fail;
    task->kernel_stack_base = kernel_stack_base;
    task->kernel_stack = (uint64_t)kernel_stack_base + STACK_SIZE;

    void *syscall_stack_base = alloc_frames_bytes(STACK_SIZE);
    if (!syscall_stack_base)
        goto fail;
    task->syscall_stack_base = syscall_stack_base;
    task->syscall_stack = (uint64_t)syscall_stack_base + STACK_SIZE;

    memset(task->kernel_stack_base, 0, STACK_SIZE);
    memset(task->syscall_stack_base, 0, STACK_SIZE);
    task->mm = calloc(1, sizeof(task_mm_info_t));
    if (!task->mm)
        goto fail;
    task->mm->page_table_addr = (uint64_t)virt_to_phys(get_kernel_page_dir());
    task->mm->ref_count = 1;
    spin_init(&task->mm->lock);
    vma_manager_init(&task->mm->task_vma_mgr, false);
    task_mm_init_aslr(task->mm);
    task->arch_context = calloc(1, sizeof(arch_context_t));
    if (!task->arch_context)
        goto fail;
    arch_context_init(task->arch_context, virt_to_phys(get_kernel_page_dir()),
                      (uint64_t)entry, task->kernel_stack, false, arg);

    task->signal->signal = 0;
    task->status = 0;
    task->fs = task_fs_create(&vfs_root_path, &vfs_root_path);
    if (!task->fs) {
        goto fail;
    }
    task->nsproxy = task_ns_proxy_create_initial();
    if (!task->nsproxy) {
        goto fail;
    }

    task_init_default_rlimits(task);

    task->fd_info = task_fd_info_alloc(FD_TABLE_INITIAL_SIZE);
    if (!task->fd_info)
        goto fail;
    {
        struct vfs_open_how in_how = {.flags = O_RDONLY};
        struct vfs_open_how out_how = {.flags = O_WRONLY};
        struct vfs_file *stdin_file = NULL;
        struct vfs_file *stdout_file = NULL;
        struct vfs_file *stderr_file = NULL;

        if (vfs_openat(AT_FDCWD, "/dev/console", &in_how, &stdin_file, true) ==
            0)
            task_replace_file(task, 0, stdin_file, 0);
        if (vfs_openat(AT_FDCWD, "/dev/console", &out_how, &stdout_file,
                       true) == 0)
            task_replace_file(task, 1, stdout_file, 0);
        if (vfs_openat(AT_FDCWD, "/dev/console", &out_how, &stderr_file,
                       true) == 0)
            task_replace_file(task, 2, stderr_file, 0);
        if (stdin_file)
            vfs_file_put(stdin_file);
        if (stdout_file)
            vfs_file_put(stdout_file);
        if (stderr_file)
            vfs_file_put(stderr_file);
    }
    strncpy(task->name, name, TASK_NAME_MAX);
    task->shm_ids = NULL;

    task->arg_start = 0;
    task->arg_end = 0;
    task->env_start = 0;
    task->env_end = 0;

    spin_init(&task->timers_lock);

    task->clone_flags = 0;

    task->child_vfork_done = false;
    task->is_clone = false;
    task->is_kernel = true;

    task->parent_death_sig = (uint64_t)-1;

    task->state = TASK_READY;
    task->current_state = TASK_READY;

    spin_lock(&task_queue_lock);
    task_tgid_index_attach_locked(task);
    spin_unlock(&task_queue_lock);

    task->sched_info = calloc(1, sizeof(struct sched_entity));
    if (!task->sched_info)
        goto fail;
    add_sched_entity_wakeup(task, &schedulers[task->cpu_id]);

    on_new_task_call(task);

    can_schedule = true;

    if (irq_state)
        arch_enable_interrupt();
    return task;

fail:
    can_schedule = true;
    task_cleanup_partial(task, true);
    if (irq_state)
        arch_enable_interrupt();
    return NULL;
}

extern void init_thread(uint64_t arg);

extern bool system_initialized;

void softirqd_thread(uint64_t arg) {
    uint32_t queue_id = (uint32_t)arg;
    (void)queue_id;

    while (true) {
        arch_enable_interrupt();

        task_prepare_block(current_task);
        if (softirq_has_pending()) {
            task_cancel_block_prepare(current_task);
            softirq_handle_pending();
            continue;
        }

        task_block(current_task, TASK_BLOCKING, -1, "waiting_for_softirq");
        task_cancel_block_prepare(current_task);
    }
}

void task_init() {
    memset(idle_tasks, 0, sizeof(idle_tasks));
    ASSERT(hashmap_init(&task_pid_map, 512) == 0);
    ASSERT(hashmap_init(&task_parent_map, 512) == 0);
    ASSERT(hashmap_init(&task_pgid_map, 512) == 0);
    ASSERT(hashmap_init(&task_tgid_map, 512) == 0);
    for (uint32_t cpu = 0; cpu < MAX_CPU_NUM; cpu++) {
        task_timeout_roots[cpu] = RB_ROOT_INIT;
        task_signal_timer_roots[cpu] = RB_ROOT_INIT;
        __atomic_store_n(&task_timeout_next_ns[cpu], UINT64_MAX,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&task_signal_timer_next_ns[cpu], UINT64_MAX,
                         __ATOMIC_RELEASE);
        deadline_source_init(&task_timeout_deadline_sources[cpu],
                             DEADLINE_SOURCE_TASK_TIMEOUT, cpu);
        deadline_source_init(&task_signal_timer_deadline_sources[cpu],
                             DEADLINE_SOURCE_TASK_SIGNAL_TIMER, cpu);
        deadline_source_init(&sched_tick_deadline_sources[cpu],
                             DEADLINE_SOURCE_SCHED_TICK, cpu);
        spin_init(&task_timeout_locks[cpu]);
        spin_init(&task_signal_timer_locks[cpu]);
    }
    llist_init_head(&should_free_tasks);
    spin_init(&should_free_lock);
    llist_init_head(&should_free_mms);
    spin_init(&should_free_mm_lock);
    softirq_register(SOFTIRQ_TIMER, task_timer_softirq);
    softirq_register(SOFTIRQ_TASK_REAP, task_reap_softirq);
    next_task_pid = 1;

    for (uint64_t cpu = 0; cpu < cpu_count; cpu++) {
        memset(&schedulers[cpu], 0, sizeof(sched_rq_t));
        schedulers[cpu].run_tree = RB_ROOT_INIT;
        schedulers[cpu].min_vruntime = 0;
        schedulers[cpu].load_weight = 0;
        schedulers[cpu].nr_running = 0;
        __atomic_store_n(&schedulers[cpu].nr_running_snapshot, 0,
                         __ATOMIC_RELAXED);
        schedulers[cpu].nr_queued = 0;
        spin_init(&schedulers[cpu].lock);
    }

    for (uint64_t cpu = 0; cpu < cpu_count; cpu++) {
        task_t *idle_task = task_create("idle", NULL, 0, IDLE_PRIORITY);
        idle_task->cpu_id = cpu;
        idle_task->state = TASK_READY;
        idle_task->current_state = TASK_RUNNING;
        idle_task->last_sched_in_ns = nano_time();
        schedulers[cpu].idle = idle_task->sched_info;
        remove_sched_entity(idle_task, &schedulers[cpu]);
        struct sched_entity *idle_entity = idle_task->sched_info;
        idle_entity->rq = &schedulers[cpu];
        idle_entity->on_rq = false;
        idle_entity->exec_start_ns = idle_task->last_sched_in_ns;
        schedulers[cpu].curr = idle_task->sched_info;
    }

    init_task = task_create("init", init_thread, 0, NORMAL_PRIORITY);

    softirqd_task =
        task_create("softirqd", softirqd_thread, 0, KTHREAD_PRIORITY);
    /* There is one global softirq worker, not one worker per CPU.  Keep it on
     * its assigned CPU so timer/reap wakeups from different CPUs do not bounce
     * the same kernel stack and softirq state across the machine. */
    task_set_flag(softirqd_task, TASK_FLAG_CPU_PINNED);
    remove_sched_entity(softirqd_task, &schedulers[softirqd_task->cpu_id]);
    softirqd_task->state = TASK_BLOCKING;
    softirqd_task->blocking_reason = "waiting_for_softirq";

    arch_set_current(idle_tasks[current_cpu_id]);
    task_mark_on_cpu(idle_tasks[current_cpu_id], true);
    task_mm_mark_cpu_active(idle_tasks[current_cpu_id]->mm, current_cpu_id);

    __atomic_store_n(&task_initialized, true, __ATOMIC_RELEASE);

    can_schedule = true;
}

void sched_request_resched(task_t *task) { task_set_need_resched(task); }

void sched_resched_if_needed(void) {
    task_t *self = current_task;

    if (!can_schedule || !self || !task_need_resched(self))
        return;

    schedule(0);
}

void task_wait_poll_loop(void) {
    bool irq_state = arch_interrupt_enabled();

    arch_enable_interrupt();
    arch_wait_for_interrupt();
    if (!irq_state)
        arch_disable_interrupt();
}

void task_prepare_block(task_t *task) {
    if (!task || task->state == TASK_DIED)
        return;

    bool irq_state = arch_interrupt_enabled();
    arch_disable_interrupt();
    spin_lock(&task->block_lock);
    task->wake_pending = false;
    task->status = EOK;
    task->block_preparing = true;
    spin_unlock(&task->block_lock);
    if (irq_state)
        arch_enable_interrupt();
}

void task_cancel_block_prepare(task_t *task) {
    if (!task || task->state == TASK_DIED)
        return;

    bool irq_state = arch_interrupt_enabled();
    arch_disable_interrupt();
    spin_lock(&task->block_lock);
    if (task->block_preparing && task->state != TASK_BLOCKING &&
        task->state != TASK_UNINTERRUPTABLE) {
        task->block_preparing = false;
        task->wake_pending = false;
        task->status = EOK;
    }
    spin_unlock(&task->block_lock);
    if (irq_state)
        arch_enable_interrupt();
}

bool task_group_exit_code(task_t *task, int64_t *code_out) {
    if (!task || !task->signal || !task->signal->sighand)
        return false;

    bool exiting;
    int64_t code;

    spin_lock(&task->signal->sighand->siglock);
    exiting = task->signal->sighand->group_exit;
    code = task->signal->sighand->group_exit_code;
    spin_unlock(&task->signal->sighand->siglock);

    if (!exiting)
        return false;

    if (code_out)
        *code_out = code;
    return true;
}

int task_block(task_t *task, task_state_t state, int64_t timeout_ns,
               const char *blocking_reason) {
    if (!task || task->cpu_id >= cpu_count) {
        return -EINVAL;
    }

    if (task == current_task && task->preempt_count) {
        printk("Task %lu(%s) maybe deadlock: preempt_count=%lu "
               "reason=%s state=%d current_state=%d cpu=%u caller=%p\n",
               task->pid, task->name, task->preempt_count,
               blocking_reason ? blocking_reason : "<unknown>", task->state,
               task->current_state, task->cpu_id, __builtin_return_address(0));
        return -EDEADLK;
    }

    bool should_sleep = false;
    int result = EOK;
    uint32_t target_cpu = task->cpu_id;
    bool should_trigger_sched_ipi = false;
    bool arm_timeout = false;
    bool cancel_timeout = false;

    bool lock_irq_state = arch_interrupt_enabled();

    arch_disable_interrupt();

    spin_lock(&task->block_lock);

    if (task->wake_pending) {
        task->wake_pending = false;
        task->block_preparing = false;
        result = task->status;
        spin_unlock(&task->block_lock);
        goto ret;
    }

    if (timeout_ns == 0) {
        task->block_preparing = false;
        task->status = ETIMEDOUT;
        result = ETIMEDOUT;
        spin_unlock(&task->block_lock);
        goto ret;
    }

    task->status = EOK;
    task->block_preparing = true;

    if (target_cpu != current_cpu_id && task->sched_info) {
        spin_lock(&schedulers[target_cpu].lock);
        should_trigger_sched_ipi = (schedulers[target_cpu].curr ==
                                    (struct sched_entity *)task->sched_info);
        spin_unlock(&schedulers[target_cpu].lock);
    }

    task->force_wakeup_ns =
        (timeout_ns > 0) ? (nano_time() + timeout_ns) : UINT64_MAX;
    task->blocking_reason = blocking_reason;
    arm_timeout = timeout_ns > 0;

    spin_unlock(&task->block_lock);

    if (arm_timeout)
        task_timeout_arm(task);

    spin_lock(&task->block_lock);

    if (task->wake_pending) {
        task->wake_pending = false;
        task->block_preparing = false;
        cancel_timeout = arm_timeout;
        task->state = TASK_READY;
        task->force_wakeup_ns = UINT64_MAX;
        task->blocking_reason = NULL;
        result = task->status;
    } else {
        task->block_preparing = false;
        task->state = state;
        should_sleep = true;
        remove_sched_entity(task, &schedulers[task->cpu_id]);
    }

    spin_unlock(&task->block_lock);

    if (cancel_timeout) {
        task_timeout_cancel(task);
    }

    if (!should_sleep)
        add_sched_entity(task, &schedulers[task->cpu_id]);

    if (!should_sleep)
        goto ret;

    if (should_trigger_sched_ipi) {
        write_barrier();
        irq_trigger_sched_ipi(target_cpu);
    }

    arch_enable_interrupt();

    if (task == current_task) {
        schedule(0);
    }

    if (!lock_irq_state)
        arch_disable_interrupt();

    result = task->status;

ret:
    if (lock_irq_state)
        arch_enable_interrupt();

    if (task != current_task)
        return result;

    return result;
}

bool task_unblock_prepare(task_t *task, int reason,
                          task_unblock_token_t *token) {
    bool irq_state = arch_interrupt_enabled();

    if (token)
        memset(token, 0, sizeof(*token));
    if (!task || !token)
        return false;

    arch_disable_interrupt();

    spin_lock(&task->block_lock);

    if (task->state == TASK_DIED) {
        spin_unlock(&task->block_lock);
        goto ret;
    }

    if (task->state != TASK_BLOCKING && task->state != TASK_UNINTERRUPTABLE) {
        if (!task->block_preparing) {
            spin_unlock(&task->block_lock);
            goto ret;
        }
        task->status = reason;
        task->force_wakeup_ns = UINT64_MAX;
        task->wake_pending = true;
        token->accepted = true;
        spin_unlock(&task->block_lock);
        goto ret;
    }

    task->status = reason;
    task->state = TASK_READY;
    task->block_preparing = false;

    task->blocking_reason = NULL;
    task->wake_pending = false;
    token->task = task;
    token->accepted = true;
    token->prepared = true;
    token->cancel_timeout = task->force_wakeup_ns != UINT64_MAX;

    spin_unlock(&task->block_lock);

ret:
    if (irq_state)
        arch_enable_interrupt();
    return token->prepared;
}

void task_unblock_finish(task_unblock_token_t *token) {
    if (!token || !token->prepared || !token->task)
        return;

    task_t *task = token->task;
    bool cancel_timeout = token->cancel_timeout;

    /* The token may live on the blocked task's syscall stack. Consume it
     * before enqueueing that task, because a remote CPU can run and unwind the
     * stack as soon as add_sched_entity_wakeup() sends its IPI. */
    token->task = NULL;
    token->prepared = false;
    token->cancel_timeout = false;

    if (cancel_timeout) {
        task_timeout_cancel(task);

        spin_lock(&task->block_lock);
        task->force_wakeup_ns = UINT64_MAX;
        spin_unlock(&task->block_lock);
    }

    /* Preserve CPU affinity across short sleeps.  Migrating on every
     * futex/socket/epoll wakeup makes IPC-heavy applications bounce their
     * working set and shared mm between CPUs, multiplying cache coherency,
     * scheduler IPI and TLB shootdown costs.  New tasks are still distributed
     * by alloc_cpu_id(); periodic balancing can be added separately. */
    add_sched_entity_wakeup(task, &schedulers[task->cpu_id]);
}

void task_unblock(task_t *task, int reason) {
    task_unblock_token_t token;

    if (task_unblock_prepare(task, reason, &token))
        task_unblock_finish(&token);
}

void task_cleanup_fd_info(task_t *task) {
    fd_info_t *fd_info = task_fd_info_detach(task);
    if (fd_info) {
        task_fd_info_put(fd_info, task);
    }
}

void task_exit_inner(task_t *task, int64_t code) {
    arch_disable_interrupt();

    if (task->pid == 1)
        arch_shutdown();

    uint64_t before_user_ns = task ? task->user_time_ns : 0;
    task_account_runtime_ns(task, nano_time());
    if (task && task->user_time_ns > before_user_ns)
        task->system_time_ns += task->user_time_ns - before_user_ns;
    task->last_sched_in_ns = 0;

    struct sched_entity *entity = (struct sched_entity *)task->sched_info;
    remove_sched_entity(task, &schedulers[task->cpu_id]);
    if (entity) {
        entity->task = NULL;
    }

    task_timeout_cancel(task);
    task_signal_timer_cancel(task);

    if (task->exec_file) {
        vfs_close_file(task->exec_file);
        task->exec_file = NULL;
    }

    task_cleanup_fd_info(task);
    task_fs_put(task->fs);
    task->fs = NULL;
    task_ns_proxy_put(task->nsproxy);
    task->nsproxy = NULL;

    task->current_state = TASK_DIED;
    task->state = TASK_DIED;
    task->status = (uint64_t)code;
    task->exited_by_signal = code >= 128 && code <= 128 + 64;

    futex_on_exit_task(task);
    pidfd_on_task_exit(task);
    on_exit_task_call(task);

    spin_lock(&task_queue_lock);
    task_t *waiting_task = task_lookup_by_pid_nolock(task->waitpid);
    if (waiting_task && waiting_task->state != TASK_DIED) {
        task_unblock(waiting_task, EOK);
    }
    task_t *wait_parent = task_lookup_by_pid_nolock(task_parent_pid(task));
    if (wait_parent && wait_parent->state != TASK_DIED)
        wait_queue_wake_all(&wait_parent->child_wait, EPOLLIN, EOK);
    if (ptrace_is_traced(task) && task->ptrace_tracer_pid != task->waitpid) {
        task_t *tracer = task_lookup_by_pid_nolock(task->ptrace_tracer_pid);
        if (tracer && tracer->state != TASK_DIED) {
            task_unblock(tracer, EOK);
            wait_queue_wake_all(&tracer->child_wait, EPOLLIN, EOK);
        }
    }
    spin_unlock(&task_queue_lock);

    if (!task->is_clone && task_has_parent(task)) {
        sigaction_t sa = {0};
        bool sigchld_blocked = false;
        spin_lock(&task_queue_lock);
        task_t *parent = task_lookup_by_pid_nolock(task_parent_pid(task));
        if (!parent) {
            task_parent_index_detach_locked(task, true);
            task->parent = NULL;
            task->parent_pid = 0;
        }
        if (parent && parent->signal && parent->signal->sighand) {
            spin_lock(&parent->signal->sighand->siglock);
            sa = parent->signal->sighand->actions[SIGCHLD];
            sigchld_blocked =
                signal_sigset_has(parent->signal->blocked, SIGCHLD);
            spin_unlock(&parent->signal->sighand->siglock);
        }
        bool ignore_sigchld = (sa.sa_handler == SIG_IGN);

        if (parent && !ignore_sigchld) {
            siginfo_t sigchld_info;
            memset(&sigchld_info, 0, sizeof(siginfo_t));
            sigchld_info.si_signo = SIGCHLD;
            sigchld_info.si_errno = 0;
            sigchld_info._sifields._sigchld._pid = task->pid;
            sigchld_info._sifields._sigchld._uid = task->uid;
            sigchld_info._sifields._sigchld._utime = 0;
            sigchld_info._sifields._sigchld._stime = 0;
            if (task->exited_by_signal) {
                sigchld_info.si_code = CLD_KILLED;
                sigchld_info._sifields._sigchld._status = code - 128;
            } else {
                sigchld_info.si_code = CLD_EXITED;
                sigchld_info._sifields._sigchld._status = code;
            }
            task_commit_signal(parent, SIGCHLD, &sigchld_info);
        }

        if (!parent || task->orphaned_to_init || ignore_sigchld ||
            (sa.sa_flags & SA_NOCLDWAIT)) {
            if (task_try_mark_reaped(task))
                task_enqueue_should_free(task);
        }

        if (parent && !ignore_sigchld && !sigchld_blocked)
            task_unblock(parent, 128 + SIGCHLD);
        if (parent)
            wait_queue_wake_all(&parent->child_wait, EPOLLIN, EOK);
        spin_unlock(&task_queue_lock);
    } else if (task->is_clone || !task_has_parent(task)) {
        if (task_try_mark_reaped(task))
            task_enqueue_should_free(task);
    }
}

uint64_t task_exit_thread(int64_t code) {
    arch_disable_interrupt();

    task_t *self = current_task;

    spin_lock(&task_queue_lock);
    task_t *new_parent = task_pick_reaper_locked(self);
    task_reparent_children_locked(self, new_parent, false);
    spin_unlock(&task_queue_lock);

    task_complete_vfork(self);

    task_exit_inner(self, code);

    can_schedule = true;

    while (1) {
        arch_enable_interrupt();
        schedule(SCHED_FLAG_YIELD);
    }
}

uint64_t task_exit(int64_t code) {
    arch_disable_interrupt();

    task_t *self = current_task;
    task_complete_vfork(self);

    uint64_t current_tgid = task_effective_tgid(self);

    spin_lock(&task_queue_lock);
    if (self->signal && self->signal->sighand) {
        spin_lock(&self->signal->sighand->siglock);
        self->signal->sighand->group_exit = true;
        self->signal->sighand->group_exit_code = code;
        spin_unlock(&self->signal->sighand->siglock);
    }

    task_index_bucket_t *group =
        task_index_bucket_lookup(&task_tgid_map, current_tgid);
    if (group) {
        struct llist_header *node = group->tasks.next;
        while (node != &group->tasks) {
            task_t *task = list_entry(node, task_t, tgid_node);
            node = node->next;
            if (!task || task == self || task->state == TASK_DIED ||
                !task->arch_context) {
                continue;
            }

            task->procfs_thread_path = NULL;
            if (task->state == TASK_BLOCKING ||
                task->state == TASK_UNINTERRUPTABLE || task->block_preparing) {
                task_unblock(task, EOK);
            }
        }
    }

    task_t *new_parent = task_pick_process_reaper_locked(self);
    task_reparent_children_locked(self, new_parent, true);
    spin_unlock(&task_queue_lock);

    task_exit_inner(self, code);

    can_schedule = true;

    schedule(0);

    while (1) {
        arch_enable_interrupt();
        schedule(SCHED_FLAG_YIELD);
    }
}

static void sched_update_itimer_task(task_t *task, uint64_t now_ms) {
    if (!task || task->state == TASK_DIED)
        return;

    uint64_t rtAt = task->itimer_real.at;
    uint64_t rtReset = task->itimer_real.reset;

    if (rtAt && rtAt <= now_ms) {
        task_commit_signal(task, SIGALRM, NULL);

        if (rtReset) {
            task->itimer_real.at = now_ms + rtReset;
        } else {
            task->itimer_real.at = 0;
        }
    }

    spin_lock(&task->timers_lock);
    for (int j = 0; j < MAX_TIMERS_NUM; j++) {
        if (task->timers[j] == NULL)
            continue;
        kernel_timer_t *kt = task->timers[j];
        uint64_t now_ns = task_timer_current_time_ns(kt->clock_type);

        if (kt->expires && now_ns >= kt->expires) {
            uint64_t periods = 1;
            if (kt->interval)
                periods = (now_ns - kt->expires) / kt->interval + 1;
            kt->overrun =
                periods > (uint64_t)INT32_MAX ? INT32_MAX : (int)(periods - 1);

            if (kt->sigev_notify == SIGEV_SIGNAL ||
                kt->sigev_notify == SIGEV_THREAD_ID) {
                siginfo_t info;
                memset(&info, 0, sizeof(info));
                info.si_signo = kt->sigev_signo;
                info.si_code = SI_TIMER;
                info._sifields._timer._tid = j;
                info._sifields._timer._overrun = kt->overrun;
                info._sifields._timer._sigval = kt->sigev_value;
                task_t *target = task;
                if (kt->sigev_notify == SIGEV_THREAD_ID)
                    target =
                        task_find_by_pid((uint64_t)kt->sigev_notify_thread_id);
                if (target &&
                    task_effective_tgid(target) == task_effective_tgid(task))
                    task_commit_signal(target, kt->sigev_signo, &info);
            }

            if (kt->interval) {
                kt->expires += periods * kt->interval;
            } else {
                kt->expires = 0;
            }
        }
    }
    spin_unlock(&task->timers_lock);

    task_refresh_tick_work_state(task);
}

void sched_wake_softirqd(uint32_t cpu_id) {
    if (!softirqd_task)
        return;

    (void)cpu_id;
    /* task_unblock() enqueues the worker and sends the remote reschedule IPI
     * when one is required.  Sending another IPI here doubles the interrupt
     * traffic whenever a non-owner CPU raises timer/reap softirq work. */
    task_unblock(softirqd_task, EOK);
}

void sched_check_wakeup() {
    uint32_t cpu_id = current_cpu_id;
    if (cpu_id >= MAX_CPU_NUM)
        return;

    uint64_t now = nano_time();
    uint64_t timeout_next =
        __atomic_load_n(&task_timeout_next_ns[cpu_id], __ATOMIC_ACQUIRE);
    uint64_t signal_next =
        __atomic_load_n(&task_signal_timer_next_ns[cpu_id], __ATOMIC_ACQUIRE);

    /*
     * The deadline queue also contains the scheduler preemption deadline.
     * The IRQ exit path handles that deadline directly; treating every
     * scheduler tick as a task-timer expiry wakes the single softirq worker
     * cross-CPU and makes it scan every CPU once per time slice.
     */
    if (timeout_next > now && signal_next > now)
        return;

    __atomic_fetch_or(&task_timer_pending_cpus[cpu_id / 64U],
                      1ULL << (cpu_id % 64U), __ATOMIC_ACQ_REL);
    if (softirq_raise(SOFTIRQ_TIMER))
        sched_wake_softirqd(current_cpu_id);
}

uint64_t sched_next_wakeup_ns(void) {
    return deadline_next_ns_for_cpu(current_cpu_id);
}

static int task_kill_process_group_internal(int pgid, int sig,
                                            bool skip_kernel) {
    int sent = 0;
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_pgid_map, (uint64_t)pgid);
    if (bucket) {
        struct llist_header *node = bucket->tasks.next;
        while (node != &bucket->tasks) {
            task_t *task = list_entry(node, task_t, pgid_node);
            node = node->next;
            if (!task || !task_is_signal_group_representative_locked(
                             task, skip_kernel)) {
                continue;
            }
            sent++;
            if (sig != 0) {
                if (task_signal_forces_group_exit(sig)) {
                    task_kill_thread_group_locked(task_effective_tgid(task),
                                                  sig, SI_USER, skip_kernel);
                } else {
                    task_send_signal(task, sig, SI_USER);
                }
            }
        }
    }

    return sent;
}

void send_process_group_signal(int pgid, int signal) {
    if (!pgid) {
        return;
    }

    spin_lock(&task_queue_lock);
    task_kill_process_group_internal(pgid, signal, false);
    spin_unlock(&task_queue_lock);
}

int task_kill_process_group(int pgid, int sig) {
    int sent;

    if (!pgid) {
        return 0;
    }

    spin_lock(&task_queue_lock);
    sent = task_kill_process_group_internal(pgid, sig, true);
    spin_unlock(&task_queue_lock);
    return sent;
}

void schedule(uint64_t sched_flags) {
    uint64_t now_ns = nano_time();

    jiffies = (unsigned long)(now_ns / (1000000000ULL / SCHED_HZ));

    bool state = arch_interrupt_enabled();
    task_t *prev = current_task;

    arch_disable_interrupt();

    if (prev->preempt_count) {
        goto ret;
    }

    int cpu_id = prev->cpu_id;

    if (!prev->last_sched_in_ns && prev->current_state == TASK_RUNNING)
        prev->last_sched_in_ns = now_ns;

    task_account_runtime_ns(prev, now_ns);
    sched_account_runtime_until(prev, now_ns);

    bool must_resched =
        task_need_resched(prev) || (sched_flags & SCHED_FLAG_YIELD) ||
        prev->state != TASK_READY || prev->current_state != TASK_RUNNING;
    if (!must_resched &&
        !sched_should_preempt(&schedulers[cpu_id], prev, now_ns)) {
        task_clear_need_resched(prev);
        sched_update_preempt_deadline(cpu_id, prev, now_ns);
        goto ret;
    }

    task_t *next = NULL;
    if (prev->state == TASK_READY && prev->current_state == TASK_RUNNING) {
        next = sched_requeue_current_and_pick_next(
            prev, &schedulers[cpu_id], (sched_flags & SCHED_FLAG_YIELD) != 0);
    } else {
        next = sched_pick_next_task(&schedulers[cpu_id]);
    }

    if (next->state == TASK_DIED) {
        next = idle_tasks[cpu_id];
    }

    if (prev == next) {
        task_clear_need_resched(prev);
        sched_update_preempt_deadline(cpu_id, prev, now_ns);
        goto ret;
    }

    task_clear_need_resched(prev);
    prev->last_sched_in_ns = 0;

    prev->current_state = prev->state;
    next->current_state = TASK_RUNNING;
    next->last_sched_in_ns = now_ns;
    sched_update_preempt_deadline(cpu_id, next, now_ns);

    arch_set_current(next);
    switch_mm(prev, next);
    if (prev->mm != next->mm) {
        task_mm_mark_cpu_inactive(prev->mm, cpu_id);
        task_mm_mark_cpu_active(next->mm, cpu_id);
    }
    task_membarrier_checkpoint(next);
    switch_to(prev, next);

ret:
    if (state) {
        arch_enable_interrupt();
    }
}
