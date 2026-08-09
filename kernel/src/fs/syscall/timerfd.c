#include <fs/fs_syscall.h>
#include <fs/proc.h>
#include <fs/vfs/vfs.h>
#include <arch/arch.h>
#include <boot/boot.h>
#include <drivers/deadline.h>
#include <irq/softirq.h>
#include <libs/klibc.h>
#include <libs/rbtree.h>
#include <task/signal.h>
#include <task/task_syscall.h>
#include <init/callbacks.h>

#define TIMERFDFS_MAGIC 0x74666473ULL

static struct vfs_file_system_type timerfdfs_fs_type;
static const struct vfs_super_operations timerfdfs_super_ops;
static const struct vfs_file_operations timerfdfs_dir_file_ops;
static const struct vfs_file_operations timerfdfs_file_ops;
static spinlock_t timerfdfs_mount_lock;
static struct vfs_mount *timerfdfs_internal_mnt;

int timerfdfs_id = 0;
static rb_root_t timerfd_mono_root = RB_ROOT_INIT;
static rb_root_t timerfd_real_root = RB_ROOT_INIT;
static spinlock_t timerfd_mono_lock = SPIN_INIT;
static spinlock_t timerfd_real_lock = SPIN_INIT;
static uint64_t timerfd_mono_next_ns = UINT64_MAX;
static uint64_t timerfd_real_next_ns = UINT64_MAX;
static deadline_source_t timerfd_mono_deadline_source;
static deadline_source_t timerfd_real_deadline_source;

typedef struct timerfdfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} timerfdfs_info_t;

typedef struct timerfdfs_inode_info {
    struct vfs_inode vfs_inode;
} timerfdfs_inode_info_t;

static uint64_t get_current_time_ns(int clock_type);

static inline bool timerfd_clock_supported(int clock_type) {
    return clock_type == CLOCK_REALTIME || clock_type == CLOCK_MONOTONIC ||
           clock_type == CLOCK_BOOTTIME;
}

static inline bool timerfd_clock_is_realtime(int clock_type) {
    return clock_type == CLOCK_REALTIME;
}

static inline timerfdfs_info_t *timerfdfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (timerfdfs_info_t *)sb->s_fs_info : NULL;
}

static inline timerfd_t *timerfd_file_handle(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (timerfd_t *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (timerfd_t *)file->f_inode->i_private;
}

static inline spinlock_t *timerfd_tree_lock_for_clock(int clock_type) {
    return timerfd_clock_is_realtime(clock_type) ? &timerfd_real_lock
                                                 : &timerfd_mono_lock;
}

static inline rb_root_t *timerfd_root_for_clock(int clock_type) {
    return timerfd_clock_is_realtime(clock_type) ? &timerfd_real_root
                                                 : &timerfd_mono_root;
}

static inline uint64_t *timerfd_next_deadline_for_clock(int clock_type) {
    return timerfd_clock_is_realtime(clock_type) ? &timerfd_real_next_ns
                                                 : &timerfd_mono_next_ns;
}

static inline int timerfd_cmp(timerfd_t *left, timerfd_t *right) {
    if (left->timer.expires < right->timer.expires)
        return -1;
    if (left->timer.expires > right->timer.expires)
        return 1;
    if ((uint64_t)(uintptr_t)left->node < (uint64_t)(uintptr_t)right->node)
        return -1;
    if ((uint64_t)(uintptr_t)left->node > (uint64_t)(uintptr_t)right->node)
        return 1;
    return 0;
}

static inline timerfd_t *timerfd_first_locked(rb_root_t *root) {
    rb_node_t *first = rb_first(root);
    return first ? rb_entry(first, timerfd_t, timeout_node) : NULL;
}

static inline void timerfd_refresh_next_locked(int clock_type) {
    timerfd_t *first = timerfd_first_locked(timerfd_root_for_clock(clock_type));
    uint64_t next = first ? first->timer.expires : UINT64_MAX;

    __atomic_store_n(timerfd_next_deadline_for_clock(clock_type), next,
                     __ATOMIC_RELEASE);
    deadline_source_update(timerfd_clock_is_realtime(clock_type)
                               ? &timerfd_real_deadline_source
                               : &timerfd_mono_deadline_source,
                           next);
}

static void timerfd_timeout_remove_locked(timerfd_t *tfd) {
    if (!tfd || !tfd->timeout_queued)
        return;

    int clock_type = tfd->timer.clock_type;

    rb_erase(&tfd->timeout_node, timerfd_root_for_clock(clock_type));
    memset(&tfd->timeout_node, 0, sizeof(tfd->timeout_node));
    tfd->timeout_queued = false;
    timerfd_refresh_next_locked(clock_type);
}

static void timerfd_timeout_add_locked(timerfd_t *tfd) {
    if (!tfd || !tfd->node || !tfd->timer.expires || tfd->timeout_queued)
        return;

    rb_root_t *root = timerfd_root_for_clock(tfd->timer.clock_type);
    rb_node_t **slot = &root->rb_node;
    rb_node_t *parent = NULL;

    while (*slot) {
        timerfd_t *curr = rb_entry(*slot, timerfd_t, timeout_node);
        int cmp = timerfd_cmp(tfd, curr);
        parent = *slot;
        if (cmp < 0)
            slot = &(*slot)->rb_left;
        else
            slot = &(*slot)->rb_right;
    }

    tfd->timeout_node.rb_left = NULL;
    tfd->timeout_node.rb_right = NULL;
    rb_set_parent(&tfd->timeout_node, parent);
    rb_set_color(&tfd->timeout_node, KRB_RED);
    *slot = &tfd->timeout_node;
    rb_insert_color(&tfd->timeout_node, root);
    tfd->timeout_queued = true;
    timerfd_refresh_next_locked(tfd->timer.clock_type);
}

static bool timerfd_update_due_locked(timerfd_t *tfd, uint64_t now) {
    if (!tfd || !tfd->timer.expires || now < tfd->timer.expires)
        return false;

    /*
     * timerfd readiness is a counter, not a boolean latch. Periodic timers may
     * accumulate multiple expirations before userspace consumes them, and the
     * read side is supposed to observe that count.
     */
    if (tfd->timer.interval) {
        uint64_t delta = now - tfd->timer.expires;
        uint64_t periods = delta / tfd->timer.interval + 1;
        tfd->count += periods;
        tfd->timer.expires += periods * tfd->timer.interval;
    } else {
        tfd->count++;
        tfd->timer.expires = 0;
    }

    return true;
}

static bool timerfd_update_due_requeue_locked(timerfd_t *tfd, uint64_t now) {
    if (!tfd || !tfd->timer.expires || now < tfd->timer.expires)
        return false;

    bool was_queued = tfd->timeout_queued;
    if (was_queued)
        timerfd_timeout_remove_locked(tfd);

    bool changed = timerfd_update_due_locked(tfd, now);
    timerfd_timeout_add_locked(tfd);
    return changed;
}

static inline void timerfd_snapshot_state(timerfd_t *tfd, int *clock_type,
                                          uint64_t *expires, uint64_t *count) {
    spin_lock(&tfd->lock);
    if (clock_type)
        *clock_type = tfd->timer.clock_type;
    if (expires)
        *expires = tfd->timer.expires;
    if (count)
        *count = tfd->count;
    spin_unlock(&tfd->lock);
}

static bool timerfd_refresh_due(timerfd_t *tfd, int clock_type, uint64_t now,
                                uint64_t *count, uint64_t *expires) {
    if (!tfd)
        return false;

    spinlock_t *tree_lock = timerfd_tree_lock_for_clock(clock_type);
    bool changed = false;

    spin_lock(tree_lock);
    spin_lock(&tfd->lock);
    if (tfd->timer.clock_type == clock_type && tfd->timer.expires &&
        now >= tfd->timer.expires) {
        changed = timerfd_update_due_requeue_locked(tfd, now);
    }
    if (count)
        *count = tfd->count;
    if (expires)
        *expires = tfd->timer.expires;
    spin_unlock(&tfd->lock);
    spin_unlock(tree_lock);

    return changed;
}

void timerfd_check_wakeup(void) {
    bool raise = false;
    uint64_t mono_next =
        __atomic_load_n(&timerfd_mono_next_ns, __ATOMIC_ACQUIRE);

    if (mono_next != UINT64_MAX && mono_next <= nano_time())
        raise = true;

    if (!raise) {
        uint64_t real_next =
            __atomic_load_n(&timerfd_real_next_ns, __ATOMIC_ACQUIRE);
        if (real_next != UINT64_MAX &&
            real_next <= get_current_time_ns(CLOCK_REALTIME))
            raise = true;
    }

    if (raise) {
        if (softirq_raise(SOFTIRQ_TIMERFD))
            sched_wake_softirqd(current_cpu_id);
    }
}

void timerfd_softirq(void) {
    while (true) {
        bool progressed = false;
        int clocks[2] = {CLOCK_MONOTONIC, CLOCK_REALTIME};

        for (int i = 0; i < 2; i++) {
            int clock_type = clocks[i];
            uint64_t now = get_current_time_ns(clock_type);
            spinlock_t *tree_lock = timerfd_tree_lock_for_clock(clock_type);

            while (true) {
                vfs_node_t *notify_node = NULL;

                spin_lock(tree_lock);
                timerfd_t *tfd =
                    timerfd_first_locked(timerfd_root_for_clock(clock_type));
                if (!tfd || tfd->timer.expires > now) {
                    spin_unlock(tree_lock);
                    break;
                }

                spin_lock(&tfd->lock);
                if (tfd->timer.expires && tfd->timer.expires <= now) {
                    timerfd_timeout_remove_locked(tfd);
                    if (timerfd_update_due_locked(tfd, now)) {
                        notify_node = tfd->node;
                        if (notify_node)
                            vfs_igrab(notify_node);
                    }
                    timerfd_timeout_add_locked(tfd);
                    progressed = true;
                }
                spin_unlock(&tfd->lock);
                spin_unlock(tree_lock);

                if (!notify_node)
                    continue;

                vfs_poll_notify_inode(notify_node, EPOLLIN | EPOLLRDNORM);
                vfs_iput(notify_node);
            }
        }

        if (!progressed)
            return;
    }
}

static struct vfs_inode *timerfdfs_alloc_inode(struct vfs_super_block *sb) {
    timerfdfs_inode_info_t *info = calloc(1, sizeof(*info));
    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void timerfdfs_destroy_inode(struct vfs_inode *inode) {
    timerfd_t *tfd;

    if (!inode)
        return;
    tfd = (timerfd_t *)inode->i_private;
    if (tfd) {
        spin_lock(timerfd_tree_lock_for_clock(tfd->timer.clock_type));
        spin_lock(&tfd->lock);
        timerfd_timeout_remove_locked(tfd);
        spin_unlock(&tfd->lock);
        spin_unlock(timerfd_tree_lock_for_clock(tfd->timer.clock_type));
        free(tfd);
        inode->i_private = NULL;
    }
    free(container_of(inode, timerfdfs_inode_info_t, vfs_inode));
}

static int timerfdfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int timerfdfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    timerfdfs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *root;

    sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!sb)
        return -ENOMEM;

    fsi = calloc(1, sizeof(*fsi));
    if (!fsi) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    spin_init(&fsi->lock);
    fsi->next_ino = 1;
    sb->s_magic = TIMERFDFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &timerfdfs_super_ops;
    sb->s_type = &timerfdfs_fs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &timerfdfs_dir_file_ops;

    root = vfs_d_alloc(sb, NULL, NULL);
    if (!root) {
        vfs_iput(inode);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root, inode);
    sb->s_root = root;
    fc->sb = sb;
    vfs_iput(inode);
    return 0;
}

static void timerfdfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static const struct vfs_super_operations timerfdfs_super_ops = {
    .alloc_inode = timerfdfs_alloc_inode,
    .destroy_inode = timerfdfs_destroy_inode,
    .put_super = timerfdfs_put_super,
};

static struct vfs_file_system_type timerfdfs_fs_type = {
    .name = "timefdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = timerfdfs_init_fs_context,
    .get_tree = timerfdfs_get_tree,
};

static struct vfs_mount *timerfdfs_get_internal_mount(void) {
    int ret;

    spin_lock(&timerfdfs_mount_lock);
    if (!timerfdfs_internal_mnt) {
        ret =
            vfs_kern_mount("timefdfs", 0, NULL, NULL, &timerfdfs_internal_mnt);
        if (ret < 0)
            timerfdfs_internal_mnt = NULL;
    }
    if (timerfdfs_internal_mnt)
        vfs_mntget(timerfdfs_internal_mnt);
    spin_unlock(&timerfdfs_mount_lock);
    return timerfdfs_internal_mnt;
}

static ino64_t timerfdfs_next_ino(struct vfs_super_block *sb) {
    timerfdfs_info_t *fsi = timerfdfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static loff_t timerfdfs_llseek(struct vfs_file *file, loff_t offset,
                               int whence) {
    loff_t pos;

    if (!file || !file->f_inode)
        return -EBADF;
    vfs_file_pos_lock(file);
    switch (whence) {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        pos = file->f_pos + offset;
        break;
    case SEEK_END:
        pos = (loff_t)file->f_inode->i_size + offset;
        break;
    default:
        vfs_file_pos_unlock(file);
        return -EINVAL;
    }
    if (pos < 0) {
        vfs_file_pos_unlock(file);
        return -EINVAL;
    }
    file->f_pos = pos;
    vfs_file_pos_unlock(file);
    return pos;
}

static int timerfdfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    file->private_data = inode->i_private;
    return 0;
}

static int timerfdfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    timerfd_t *tfd = timerfd_file_handle(file);
    vfs_node_t *node = NULL;

    if (file)
        file->private_data = NULL;
    if (!tfd)
        return 0;

    spin_lock(timerfd_tree_lock_for_clock(tfd->timer.clock_type));
    spin_lock(&tfd->lock);
    timerfd_timeout_remove_locked(tfd);
    node = tfd->node;
    tfd->node = NULL;
    spin_unlock(&tfd->lock);
    spin_unlock(timerfd_tree_lock_for_clock(tfd->timer.clock_type));

    if (node) {
        vfs_poll_notify_inode(node, EPOLLHUP | EPOLLERR);
        vfs_iput(node);
    }
    (void)inode;
    return 0;
}

static uint64_t get_current_time_ns(int clock_type) {
    if (clock_type == CLOCK_MONOTONIC || clock_type == CLOCK_BOOTTIME)
        return nano_time();
    if (clock_type == CLOCK_REALTIME)
        return boot_get_boottime() * 1000000000ULL + nano_time();
    return 0;
}

static int timerfdfs_create_file(struct vfs_file **out_file, int clockid,
                                 unsigned int flags, timerfd_t **out_tfd) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    timerfd_t *tfd;
    char label[32];

    if (!out_file)
        return -EINVAL;

    mnt = timerfdfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    tfd = calloc(1, sizeof(*tfd));
    if (!tfd) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }
    spin_init(&tfd->lock);
    tfd->timer.clock_type = clockid;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(tfd);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = timerfdfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFCHR | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &timerfdfs_file_ops;
    inode->i_private = tfd;

    snprintf(label, sizeof(label), "timerfd-%llu",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, label);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        inode->i_private = NULL;
        vfs_iput(inode);
        free(tfd);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDONLY | (flags & TFD_NONBLOCK));
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        free(tfd);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = tfd;
    file->f_mode |= VFS_FMODE_NO_POS_LOCK;
    tfd->node = vfs_igrab(inode);
    *out_file = file;
    if (out_tfd)
        *out_tfd = tfd;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

uint64_t sys_timerfd_create(int clockid, int flags) {
    struct vfs_file *file = NULL;
    int ret;

    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC))
        return -EINVAL;
    if (!timerfd_clock_supported(clockid))
        return -EINVAL;

    ret = timerfdfs_create_file(&file, clockid, (unsigned int)flags, NULL);
    if (ret < 0)
        return ret;

    ret = task_install_file(current_task, file,
                            (flags & TFD_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_timerfd_settime(int fd, int flags,
                             const struct itimerspec *new_value,
                             struct itimerspec *old_value) {
    struct vfs_file *file;
    timerfd_t *tfd;

    if (!new_value)
        return -EINVAL;
    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
        return -EINVAL;
    if (new_value->it_value.tv_sec < 0 || new_value->it_value.tv_nsec < 0 ||
        new_value->it_interval.tv_sec < 0 ||
        new_value->it_interval.tv_nsec < 0 ||
        new_value->it_value.tv_nsec >= 1000000000L ||
        new_value->it_interval.tv_nsec >= 1000000000L)
        return -EINVAL;

    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;
    tfd = timerfd_file_handle(file);
    if (!tfd) {
        vfs_file_put(file);
        return -EBADF;
    }

    int clock_type = tfd->timer.clock_type;
    bool raise_softirq = false;
    bool have_now = false;
    uint64_t now = 0;
    uint64_t interval = new_value->it_interval.tv_sec * 1000000000ULL +
                        (uint64_t)new_value->it_interval.tv_nsec;
    uint64_t value = new_value->it_value.tv_sec * 1000000000ULL +
                     (uint64_t)new_value->it_value.tv_nsec;

    if (old_value || (!(flags & TFD_TIMER_ABSTIME) && value != 0)) {
        now = get_current_time_ns(clock_type);
        have_now = true;
    }

    spin_lock(timerfd_tree_lock_for_clock(clock_type));
    spin_lock(&tfd->lock);
    timerfd_timeout_remove_locked(tfd);

    if (old_value) {
        uint64_t remaining =
            tfd->timer.expires > now ? tfd->timer.expires - now : 0;

        old_value->it_interval.tv_sec = tfd->timer.interval / 1000000000ULL;
        old_value->it_interval.tv_nsec = tfd->timer.interval % 1000000000ULL;
        old_value->it_value.tv_sec = remaining / 1000000000ULL;
        old_value->it_value.tv_nsec = remaining % 1000000000ULL;
    }

    uint64_t expires =
        (flags & TFD_TIMER_ABSTIME) ? value : (value ? now + value : 0);
    tfd->timer.expires = expires;
    tfd->timer.interval = interval;
    if (value == 0)
        tfd->count = 0;
    timerfd_timeout_add_locked(tfd);
    if (tfd->timer.expires && have_now && tfd->timer.expires <= now)
        raise_softirq = true;
    spin_unlock(&tfd->lock);
    spin_unlock(timerfd_tree_lock_for_clock(clock_type));

    if (raise_softirq) {
        if (softirq_raise(SOFTIRQ_TIMERFD))
            sched_wake_softirqd(current_cpu_id);
        vfs_poll_notify_inode(tfd->node, EPOLLIN | EPOLLRDNORM);
    }

    vfs_file_put(file);
    return 0;
}

static __poll_t timerfdfs_poll(struct vfs_file *file,
                               struct vfs_poll_table *pt) {
    timerfd_t *tfd = timerfd_file_handle(file);
    int clock_type = CLOCK_MONOTONIC;
    uint64_t expires = 0;
    uint64_t count = 0;

    (void)pt;
    if (!tfd)
        return EPOLLNVAL;

    timerfd_snapshot_state(tfd, &clock_type, &expires, &count);
    if (count == 0 && expires) {
        uint64_t now = get_current_time_ns(clock_type);
        if (now >= expires)
            timerfd_refresh_due(tfd, clock_type, now, &count, &expires);
    }

    return ((count > 0) ? (EPOLLIN | EPOLLRDNORM) : 0);
}

static ssize_t timerfdfs_read(struct vfs_file *file, void *addr, size_t size,
                              loff_t *ppos) {
    timerfd_t *tfd = timerfd_file_handle(file);

    (void)ppos;
    if (!tfd)
        return -EBADF;

    for (;;) {
        uint64_t count = 0;
        uint64_t expires = 0;
        int clock_type = CLOCK_MONOTONIC;
        uint64_t now = 0;

        timerfd_snapshot_state(tfd, &clock_type, &expires, &count);
        if (count == 0 && expires) {
            now = get_current_time_ns(clock_type);
            if (now >= expires)
                timerfd_refresh_due(tfd, clock_type, now, &count, &expires);
        }

        if (count != 0)
            break;

        if (expires == 0) {
            if (file->f_flags & O_NONBLOCK) {
                return -EAGAIN;
            } else {
                int reason = vfs_poll_wait_interruptible(
                    file, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLNVAL);
                if (reason < 0)
                    return reason;
                continue;
            }
        }

        if (now < expires && !(file->f_flags & O_NONBLOCK)) {
            if (task_signal_has_deliverable(current_task)) {
                return -EINTR;
            }

            bool irq = arch_interrupt_enabled();
            arch_enable_interrupt();
            arch_wait_for_interrupt();
            if (!irq)
                arch_disable_interrupt();

            continue;
        }

        if (now < expires && (file->f_flags & O_NONBLOCK))
            return -EAGAIN;
    }

    if (size < sizeof(uint64_t))
        return -EINVAL;

    spin_lock(&tfd->lock);
    uint64_t count = tfd->count;
    tfd->count = 0;
    spin_unlock(&tfd->lock);

    /*
     * The read side is where the counter is consumed. That division of labor is
     * important: settime()/poll paths may inspect or update timing state, but
     * they should not steal already-delivered expirations from readers.
     */
    *(uint64_t *)addr = count;
    return sizeof(uint64_t);
}

#define TFD_IOC_SET_TICKS _IOW('T', 0, uint64_t)

static long timerfdfs_ioctl(struct vfs_file *file, unsigned long cmd,
                            unsigned long arg) {
    timerfd_t *tfd = timerfd_file_handle(file);

    if (!tfd)
        return -EBADF;
    switch (cmd) {
    case TFD_IOC_SET_TICKS:
        spin_lock(&tfd->lock);
        tfd->count = arg;
        spin_unlock(&tfd->lock);
        if (arg)
            vfs_poll_notify_file(file, EPOLLIN | EPOLLRDNORM);
        return 0;
    default:
        printk("timerfd_ioctl: Unsupported cmd %#018lx\n", cmd);
        return -ENOSYS;
    }
}

static const struct vfs_file_operations timerfdfs_dir_file_ops = {
    .llseek = timerfdfs_llseek,
    .open = timerfdfs_open,
    .release = timerfdfs_release,
};

static const struct vfs_file_operations timerfdfs_file_ops = {
    .llseek = timerfdfs_llseek,
    .read = timerfdfs_read,
    .unlocked_ioctl = timerfdfs_ioctl,
    .poll = timerfdfs_poll,
    .open = timerfdfs_open,
    .release = timerfdfs_release,
};

void timerfd_init() {
    spin_init(&timerfdfs_mount_lock);
    spin_init(&timerfd_mono_lock);
    spin_init(&timerfd_real_lock);
    timerfd_mono_root = RB_ROOT_INIT;
    timerfd_real_root = RB_ROOT_INIT;
    __atomic_store_n(&timerfd_mono_next_ns, UINT64_MAX, __ATOMIC_RELEASE);
    __atomic_store_n(&timerfd_real_next_ns, UINT64_MAX, __ATOMIC_RELEASE);
    deadline_source_init(&timerfd_mono_deadline_source,
                         DEADLINE_SOURCE_TIMERFD_MONO, 0);
    deadline_source_init(&timerfd_real_deadline_source,
                         DEADLINE_SOURCE_TIMERFD_REAL, 0);
    softirq_register(SOFTIRQ_TIMERFD, timerfd_softirq);
    vfs_register_filesystem(&timerfdfs_fs_type);
    timerfdfs_id = 1;
}
