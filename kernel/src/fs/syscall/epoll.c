#include <init/callbacks.h>
#include <fs/fs_syscall.h>
#include <task/signal.h>
#include <task/task.h>

#define EPOLL_ALWAYS_EVENTS (EPOLLERR | EPOLLHUP | EPOLLNVAL)
#define EPOLL_CONTROL_FLAGS                                                    \
    (EPOLLET | EPOLLONESHOT | EPOLLWAKEUP | EPOLLEXCLUSIVE)
#define EPOLL_IN_EVENTS (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLRDHUP)
#define EPOLL_OUT_EVENTS (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)
#define EPOLL_PRI_EVENTS (EPOLLPRI | EPOLLMSG)
#define EPOLLFS_MAGIC 0x65706f6cULL
#define EPOLL_STACK_EVENTS 64

typedef struct epollfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} epollfs_info_t;

static struct vfs_file_system_type epollfs_fs_type;
static const struct vfs_super_operations epollfs_super_ops;
static const struct vfs_file_operations epollfs_dir_file_ops;
static const struct vfs_file_operations epollfs_file_ops;
static spinlock_t epollfs_mount_lock;
static spinlock_t epoll_watch_lifecycle_lock;
static struct vfs_mount *epollfs_internal_mnt;

static inline epollfs_info_t *epollfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (epollfs_info_t *)sb->s_fs_info : NULL;
}

static inline uint32_t epoll_filter_events(uint32_t events) {
    return events & ~EPOLL_CONTROL_FLAGS;
}

static inline uint32_t epoll_reportable_events(uint32_t requested,
                                               uint32_t ready) {
    uint32_t requested_events = requested | EPOLL_ALWAYS_EVENTS;

    if (requested_events & EPOLLIN)
        requested_events |= EPOLLRDNORM;
    if (requested_events & EPOLLRDNORM)
        requested_events |= EPOLLIN;
    if (requested_events & EPOLLOUT)
        requested_events |= EPOLLWRNORM;
    if (requested_events & EPOLLWRNORM)
        requested_events |= EPOLLOUT;

    return ready & requested_events;
}

static epoll_t *epoll_file_handle(struct vfs_file *file) {
    if (!file || !file->f_inode || !file->f_inode->i_sb ||
        !file->f_inode->i_sb->s_type ||
        file->f_inode->i_sb->s_type != &epollfs_fs_type) {
        return NULL;
    }

    if (file->private_data)
        return (epoll_t *)file->private_data;
    return (epoll_t *)file->f_inode->i_private;
}

static bool epoll_file_is_epoll(struct vfs_file *file) {
    return epoll_file_handle(file) != NULL;
}

static bool epoll_watch_mark_ready(epoll_watch_t *watch) {
    epoll_t *epoll = watch ? watch->owner : NULL;
    bool newly_ready = false;

    if (!epoll || !watch)
        return false;

    spin_lock(&epoll->ready_lock);
    if (!watch->ready_queued) {
        llist_append(&epoll->ready_watches, &watch->ready_node);
        watch->ready_queued = true;
        newly_ready = true;
    }
    spin_unlock(&epoll->ready_lock);
    return newly_ready;
}

static void epoll_watch_remove_ready_locked(epoll_t *epoll,
                                            epoll_watch_t *watch) {
    if (!epoll || !watch)
        return;

    spin_lock(&epoll->ready_lock);
    if (watch->ready_queued) {
        llist_delete(&watch->ready_node);
        watch->ready_queued = false;
    }
    spin_unlock(&epoll->ready_lock);
}

static int epoll_watch_wake(wait_queue_entry_t *entry, uint32_t events,
                            int reason) {
    epoll_watch_t *watch;

    (void)reason;
    watch = entry ? (epoll_watch_t *)entry->private_data : NULL;
    if (!watch || !watch->owner || !watch->owner->inode)
        return 0;

    watch->last_ready_events = 0;
    if (!epoll_watch_mark_ready(watch))
        return 1;

    /*
     * The watched file changed state; the epoll file itself becomes readable
     * if a subsequent ready scan finds an event.
     */
    vfs_poll_notify_inode(watch->owner->inode, EPOLLIN | EPOLLRDNORM);
    return 1;
}

static void epoll_watch_arm(epoll_t *epoll, epoll_watch_t *watch) {
    uint32_t events;

    if (!epoll || !watch || !watch->file || !watch->file->f_inode ||
        watch->wait_armed)
        return;

    events = watch->events | EPOLL_ALWAYS_EVENTS;
    wait_queue_entry_init(&watch->wait, NULL, events, epoll_watch_wake, watch);
    watch->owner = epoll;
    wait_queue_add(&watch->file->f_inode->poll_wait, &watch->wait);
    watch->wait_armed = true;
}

static void epoll_watch_disarm(epoll_watch_t *watch) {
    if (!watch || !watch->wait_armed || !watch->file || !watch->file->f_inode)
        return;

    wait_queue_remove(&watch->file->f_inode->poll_wait, &watch->wait);
    watch->wait_armed = false;
}

static void epoll_watch_update_events(epoll_t *epoll, epoll_watch_t *watch,
                                      uint32_t events) {
    bool was_armed;

    if (!watch)
        return;

    was_armed = watch->wait_armed;
    if (was_armed)
        epoll_watch_disarm(watch);
    watch->events = events;
    if (was_armed)
        epoll_watch_arm(epoll, watch);
}

static void epoll_watch_link_file(epoll_watch_t *watch) {
    struct vfs_file *file = watch ? watch->file : NULL;

    if (!file)
        return;

    spin_lock(&file->epoll_watches_lock);
    if (llist_empty(&watch->file_node))
        llist_append(&file->epoll_watches, &watch->file_node);
    spin_unlock(&file->epoll_watches_lock);
}

static void epoll_watch_unlink_file(epoll_watch_t *watch) {
    struct vfs_file *file = watch ? watch->file : NULL;

    if (!file)
        return;

    spin_lock(&file->epoll_watches_lock);
    if (!llist_empty(&watch->file_node))
        llist_delete(&watch->file_node);
    spin_unlock(&file->epoll_watches_lock);
}

static void epoll_watch_detach_locked(epoll_watch_t *watch, bool unlink_file) {
    if (!watch)
        return;

    epoll_watch_disarm(watch);
    epoll_watch_remove_ready_locked(watch->owner, watch);
    if (unlink_file)
        epoll_watch_unlink_file(watch);
    if (!llist_empty(&watch->node))
        llist_delete(&watch->node);

    watch->file = NULL;
    watch->owner = NULL;
}

static void epoll_watch_destroy(epoll_watch_t *watch) {
    if (!watch)
        return;

    epoll_watch_detach_locked(watch, true);
    free(watch);
}

int epoll_on_close_file(task_t *task, int fd, struct vfs_file *file) {
    struct llist_header drop_watches;
    struct llist_header *node;
    int watch_count = 0;

    (void)task;
    (void)fd;
    if (!file)
        return 0;

    if (vfs_file_fd_ref_read(file) > 0)
        return 0;

    llist_init_head(&drop_watches);
    spin_lock(&epoll_watch_lifecycle_lock);

    spin_lock(&file->epoll_watches_lock);
    node = file->epoll_watches.next;
    while (node != &file->epoll_watches) {
        watch_count++;
        node = node->next;
    }

    if (watch_count <= 0 || vfs_file_fd_ref_read(file) > 0) {
        spin_unlock(&file->epoll_watches_lock);
        spin_unlock(&epoll_watch_lifecycle_lock);
        return 0;
    }

    while (!llist_empty(&file->epoll_watches)) {
        node = file->epoll_watches.next;
        llist_delete(node);
        llist_append(&drop_watches, node);
    }
    spin_unlock(&file->epoll_watches_lock);

    while (!llist_empty(&drop_watches)) {
        epoll_watch_t *drop =
            list_entry(drop_watches.next, epoll_watch_t, file_node);
        epoll_t *owner = drop->owner;

        llist_delete(&drop->file_node);

        if (owner)
            spin_lock(&owner->lock);
        epoll_watch_detach_locked(drop, false);
        if (owner)
            spin_unlock(&owner->lock);

        free(drop);
    }
    spin_unlock(&epoll_watch_lifecycle_lock);

    return 0;
}

static bool epoll_contains_file(epoll_t *epoll, struct vfs_file *needle,
                                unsigned int depth) {
    struct llist_header *node;

    if (!epoll || !needle)
        return false;
    if (depth > 32)
        return true;

    node = epoll->watches.next;
    while (node != &epoll->watches) {
        epoll_watch_t *watch = list_entry(node, epoll_watch_t, node);
        epoll_t *nested;

        node = node->next;
        if (!watch->file)
            continue;
        if (watch->file == needle)
            return true;

        nested = epoll_file_handle(watch->file);
        if (nested && epoll_contains_file(nested, needle, depth + 1))
            return true;
    }

    return false;
}

static int epoll_collect_ready_locked(epoll_t *epoll,
                                      struct epoll_event *events, int maxevents,
                                      struct vfs_poll_table *pt, bool consume) {
    int ready = 0;
    struct llist_header *node;

    if (consume) {
        struct llist_header requeue;
        uint64_t collect_seq = ++epoll->collect_seq;

        if (!collect_seq)
            collect_seq = ++epoll->collect_seq;
        llist_init_head(&requeue);
        /* Notifications enqueue the affected watch. Revalidate only those
         * watches; scanning the complete epoll set on every network event is
         * prohibitively expensive for browser-sized descriptor sets. */
        while (ready < maxevents) {
            epoll_watch_t *browse;

            spin_lock(&epoll->ready_lock);
            if (llist_empty(&epoll->ready_watches)) {
                spin_unlock(&epoll->ready_lock);
                break;
            }
            node = epoll->ready_watches.next;
            browse = list_entry(node, epoll_watch_t, ready_node);
            if (browse->collect_seq == collect_seq) {
                llist_delete(node);
                llist_append(&requeue, node);
                spin_unlock(&epoll->ready_lock);
                continue;
            }
            llist_delete(node);
            browse->ready_queued = false;
            browse->collect_seq = collect_seq;
            spin_unlock(&epoll->ready_lock);

            if (!browse->file || browse->disabled)
                continue;

            uint32_t request_events = browse->events | EPOLL_ALWAYS_EVENTS;
            int poll_ret =
                vfs_poll_with_table(browse->file, request_events, pt);
            uint32_t ready_events =
                poll_ret < 0 ? EPOLLNVAL
                             : epoll_reportable_events(browse->events,
                                                       (uint32_t)poll_ret);

            if (browse->edge_triggered) {
                uint32_t changed_events =
                    ready_events & ~browse->last_ready_events;
                if (!changed_events) {
                    browse->last_ready_events = ready_events;
                    continue;
                }
            }

            if (ready_events) {
                events[ready].events = ready_events;
                events[ready].data.u64 = browse->data;
                browse->last_ready_events = ready_events;
                ready++;
                if (browse->one_shot)
                    browse->disabled = true;
                else if (!browse->edge_triggered)
                    epoll_watch_mark_ready(browse);
            } else {
                browse->last_ready_events = 0;
            }
        }

        spin_lock(&epoll->ready_lock);
        while (!llist_empty(&requeue)) {
            node = requeue.next;
            llist_delete(node);
            llist_append(&epoll->ready_watches, node);
        }
        spin_unlock(&epoll->ready_lock);
        return ready;
    }

    spin_lock(&epoll->ready_lock);
    bool has_ready = !llist_empty(&epoll->ready_watches);
    spin_unlock(&epoll->ready_lock);
    if (has_ready)
        return 1;

    /*
     * epoll readiness is collected from the watched file objects, not from fd
     * numbers in the abstract. That detail matters because dup()/close() can
     * move integer descriptors around while the underlying open file
     * description being watched stays the same.
     */
    node = epoll->watches.next;
    while (node != &epoll->watches) {
        epoll_watch_t *browse = list_entry(node, epoll_watch_t, node);

        node = node->next;
        if (browse->disabled)
            continue;
        if (ready >= maxevents)
            break;
        if (!browse->file)
            continue;

        uint32_t request_events = browse->events | EPOLL_ALWAYS_EVENTS;
        int poll_ret = vfs_poll_with_table(browse->file, request_events, pt);
        uint32_t ready_events =
            poll_ret < 0
                ? EPOLLNVAL
                : epoll_reportable_events(browse->events, (uint32_t)poll_ret);

        if (browse->edge_triggered) {
            uint32_t changed_events = ready_events & ~browse->last_ready_events;

            if (!changed_events) {
                if (consume)
                    browse->last_ready_events = ready_events;
                continue;
            }
        }

        if (ready_events) {
            events[ready].events = ready_events;
            events[ready].data.u64 = browse->data;
            if (consume)
                browse->last_ready_events = ready_events;
            if (consume && browse->one_shot)
                browse->disabled = true;
            ready++;
        } else if (consume) {
            browse->last_ready_events = 0;
        }
    }

    return ready;
}

static int epollfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    epoll_t *epoll = epoll_file_handle(file);

    (void)inode;
    if (!epoll)
        return 0;

    spin_lock(&epoll_watch_lifecycle_lock);
    spin_lock(&epoll->lock);
    while (!llist_empty(&epoll->watches))
        epoll_watch_destroy(
            list_entry(epoll->watches.next, epoll_watch_t, node));
    spin_unlock(&epoll->lock);
    spin_unlock(&epoll_watch_lifecycle_lock);

    free(epoll);
    file->private_data = NULL;
    if (file->f_inode)
        file->f_inode->i_private = NULL;
    return 0;
}

static __poll_t epollfs_poll(struct vfs_file *file, struct vfs_poll_table *pt) {
    struct epoll_event event;
    epoll_t *epoll = epoll_file_handle(file);

    (void)pt;
    if (!epoll)
        return EPOLLNVAL;

    if (!spin_trylock(&epoll->lock))
        return 0;
    int ready = epoll_collect_ready_locked(epoll, &event, 1, pt, false);
    spin_unlock(&epoll->lock);
    return ready > 0 ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static loff_t epollfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int epollfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!file)
        return -EINVAL;
    file->private_data = inode ? inode->i_private : NULL;
    return 0;
}

static const struct vfs_file_operations epollfs_dir_file_ops = {
    .llseek = epollfs_llseek,
};

static const struct vfs_file_operations epollfs_file_ops = {
    .llseek = epollfs_llseek,
    .poll = epollfs_poll,
    .open = epollfs_open,
    .release = epollfs_release,
};

static struct vfs_mount *epollfs_get_internal_mount(void) {
    int ret;

    spin_lock(&epollfs_mount_lock);
    if (!epollfs_internal_mnt) {
        ret = vfs_kern_mount("epollfs", 0, NULL, NULL, &epollfs_internal_mnt);
        if (ret < 0)
            epollfs_internal_mnt = NULL;
    }
    if (epollfs_internal_mnt)
        vfs_mntget(epollfs_internal_mnt);
    spin_unlock(&epollfs_mount_lock);
    return epollfs_internal_mnt;
}

static int epollfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int epollfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    epollfs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *root;

    if (!fc)
        return -EINVAL;

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
    sb->s_magic = EPOLLFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &epollfs_super_ops;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(fsi);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &epollfs_dir_file_ops;

    root = vfs_d_alloc(sb, NULL, NULL);
    if (!root) {
        vfs_iput(inode);
        free(fsi);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root, inode);
    sb->s_root = root;
    fc->sb = sb;
    return 0;
}

static void epollfs_kill_sb(struct vfs_super_block *sb) {
    if (!sb)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations epollfs_super_ops = {
    .put_super = epollfs_kill_sb,
};

static struct vfs_file_system_type epollfs_fs_type = {
    .name = "epollfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = epollfs_init_fs_context,
    .get_tree = epollfs_get_tree,
};

static int epoll_create_handle_file(struct vfs_file **out_file,
                                    unsigned int open_flags,
                                    epoll_t **out_epoll) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    epollfs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    epoll_t *epoll;
    char namebuf[32];

    if (!out_file)
        return -EINVAL;

    mnt = epollfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;

    epoll = calloc(1, sizeof(*epoll));
    if (!epoll) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }
    spin_init(&epoll->lock);
    spin_init(&epoll->ready_lock);
    llist_init_head(&epoll->watches);
    llist_init_head(&epoll->ready_watches);

    sb = mnt->mnt_sb;
    fsi = epollfs_sb_info(sb);
    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(epoll);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    spin_lock(&fsi->lock);
    inode->i_ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFREG | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &epollfs_file_ops;
    inode->i_private = epoll;
    epoll->inode = inode;

    snprintf(namebuf, sizeof(namebuf), "anon-%llu",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        vfs_iput(inode);
        free(epoll);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDONLY | (open_flags & O_NONBLOCK));
    if (!file) {
        vfs_dput(dentry);
        vfs_iput(inode);
        free(epoll);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = epoll;
    *out_file = file;
    if (out_epoll)
        *out_epoll = epoll;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

size_t epoll_create1(int flags) {
    struct vfs_file *file = NULL;
    int ret;

    ret = epoll_create_handle_file(&file, (unsigned int)flags, NULL);
    if (ret < 0)
        return (uint64_t)ret;

    ret = task_install_file(current_task, file,
                            (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_epoll_create(int size) {
    if (size <= 0)
        return (uint64_t)-EINVAL;
    return epoll_create1(0);
}

static uint64_t do_epoll_wait(struct vfs_file *epoll_file,
                              struct epoll_event *events, int maxevents,
                              int64_t timeout_ns) {
    epoll_t *epoll;
    uint64_t deadline_ns;
    bool infinite_timeout;

    if (!epoll_file || maxevents < 1)
        return (uint64_t)-EINVAL;

    epoll = epoll_file_handle(epoll_file);
    if (!epoll)
        return (uint64_t)-EBADF;

    infinite_timeout = timeout_ns < 0;
    deadline_ns =
        infinite_timeout ? UINT64_MAX : nano_time() + (uint64_t)timeout_ns;

    while (true) {
        vfs_poll_wait_table_t table;
        vfs_poll_wait_table_init(&table, current_task);
        /*
         * Every watched file already owns a persistent epoll_watch entry
         * which forwards state changes to the epoll inode.  Wait on that one
         * inode instead of allocating and attaching a temporary wait entry to
         * every watched file on every epoll_wait().  Firefox keeps large epoll
         * sets and calls this path very frequently, so the old behaviour was
         * both O(n) allocation churn and O(n) wait-queue locking per call.
         */
        table.pt.events = EPOLLIN | EPOLLRDNORM | EPOLLERR | EPOLLHUP;
        vfs_poll_wait(epoll_file, &table.pt);

        spin_lock(&epoll->lock);
        int ready =
            epoll_collect_ready_locked(epoll, events, maxevents, NULL, true);
        if (ready > 0) {
            spin_unlock(&epoll->lock);
            vfs_poll_wait_table_cleanup(&table);
            return (uint64_t)ready;
        }
        int table_error = vfs_poll_wait_table_error(&table);
        spin_unlock(&epoll->lock);
        if (table_error) {
            vfs_poll_wait_table_cleanup(&table);
            return (uint64_t)table_error;
        }

        if (task_signal_has_deliverable(current_task)) {
            vfs_poll_wait_table_cleanup(&table);
            return (uint64_t)-EINTR;
        }
        if (vfs_poll_wait_table_seq_changed(&table)) {
            vfs_poll_wait_table_cleanup(&table);
            continue;
        }
        if (timeout_ns == 0) {
            vfs_poll_wait_table_cleanup(&table);
            return 0;
        }

        int64_t sleep_ns = -1;
        if (!infinite_timeout) {
            uint64_t now = nano_time();
            if (now >= deadline_ns) {
                vfs_poll_wait_table_cleanup(&table);
                return 0;
            }
            sleep_ns = (int64_t)(deadline_ns - now);
        }

        int reason =
            task_block(current_task, TASK_BLOCKING, sleep_ns, "epoll_wait");
        vfs_poll_wait_table_cleanup(&table);

        if (reason == ETIMEDOUT)
            continue;
        if (reason < 0)
            return (uint64_t)reason;
        if (reason != EOK && task_signal_has_deliverable(current_task))
            return (uint64_t)-EINTR;
    }

    return 0;
}

static size_t epoll_ctl_file(struct vfs_file *epoll_file, int op, int fd,
                             const struct epoll_event *event) {
    struct vfs_file *target;
    epoll_t *epoll;
    epoll_watch_t *existing = NULL;
    struct llist_header *node;
    int ret = 0;

    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_DEL && op != EPOLL_CTL_MOD)
        return (uint64_t)-EINVAL;
    if ((op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && !event)
        return (uint64_t)-EFAULT;

    epoll = epoll_file_handle(epoll_file);
    if (!epoll)
        return (uint64_t)-EBADF;

    target = task_get_file(current_task, fd);
    if (!target)
        return (uint64_t)-EBADF;

    // Linux cannot watch an epoll fd from itself or another epoll fd, so we
    // enforce that here by checking the target before taking the lock. This
    // also prevents deadlock if the caller tries to add an epoll fd that's
    // already in the same watch list.
    if (target == epoll_file) {
        vfs_file_put(target);
        return (uint64_t)-EINVAL;
    }
    if (!target->f_op || !target->f_op->poll) {
        vfs_file_put(target);
        return (uint64_t)-EPERM;
    }

    spin_lock(&epoll_watch_lifecycle_lock);
    spin_lock(&epoll->lock);
    if (vfs_file_fd_ref_read(target) <= 0) {
        ret = -EBADF;
        goto out_unlock;
    }
    if (op == EPOLL_CTL_ADD && epoll_file_is_epoll(target)) {
        epoll_t *target_epoll = epoll_file_handle(target);

        if (epoll_contains_file(target_epoll, epoll_file, 0)) {
            ret = -ELOOP;
            goto out_unlock;
        }
    }

    node = epoll->watches.next;
    while (node != &epoll->watches) {
        epoll_watch_t *b = list_entry(node, epoll_watch_t, node);

        node = node->next;
        if (b->file == target) {
            existing = b;
            break;
        }
    }

    switch (op) {
    case EPOLL_CTL_ADD: {
        epoll_watch_t *new_watch;

        if (existing) {
            ret = -EEXIST;
            break;
        }
        /*
         * One watch per underlying file object is the important rule here.
         * Comparing integer fd values would be wrong because duplicated fds can
         * refer to the same open file description and Linux epoll semantics key
         * off that lower-level object.
         */
        new_watch = calloc(1, sizeof(*new_watch));
        if (!new_watch) {
            ret = -ENOMEM;
            break;
        }
        /*
         * Keep this as a non-owning pointer. The fd close callback removes
         * watches before the watched file's final put, so epoll must not keep
         * sockets alive after userspace has closed the last descriptor.
         */
        new_watch->file = target;
        new_watch->owner = epoll;
        new_watch->events = epoll_filter_events(event->events);
        new_watch->data = event->data.u64;
        new_watch->edge_triggered = (event->events & EPOLLET) != 0;
        new_watch->one_shot = (event->events & EPOLLONESHOT) != 0;
        llist_init_head(&new_watch->node);
        llist_init_head(&new_watch->file_node);
        llist_init_head(&new_watch->ready_node);
        llist_init_head(&new_watch->wait.node);
        llist_append(&epoll->watches, &new_watch->node);
        epoll_watch_link_file(new_watch);
        epoll_watch_arm(epoll, new_watch);
        if (vfs_poll(new_watch->file, new_watch->events | EPOLL_ALWAYS_EVENTS) >
            0) {
            epoll_watch_mark_ready(new_watch);
            vfs_poll_notify_inode(epoll->inode, EPOLLIN | EPOLLRDNORM);
        }
        break;
    }
    case EPOLL_CTL_DEL:
        if (!existing) {
            ret = -ENOENT;
            break;
        }
        epoll_watch_destroy(existing);
        break;
    case EPOLL_CTL_MOD:
        if (!existing) {
            ret = -ENOENT;
            break;
        }
        epoll_watch_update_events(epoll, existing,
                                  epoll_filter_events(event->events));
        existing->data = event->data.u64;
        existing->edge_triggered = (event->events & EPOLLET) != 0;
        existing->one_shot = (event->events & EPOLLONESHOT) != 0;
        existing->disabled = false;
        existing->last_ready_events = 0;
        if (vfs_poll(existing->file, existing->events | EPOLL_ALWAYS_EVENTS) >
            0) {
            epoll_watch_mark_ready(existing);
            vfs_poll_notify_inode(epoll->inode, EPOLLIN | EPOLLRDNORM);
        }
        break;
    default:
        ret = -EINVAL;
        break;
    }

out_unlock:
    spin_unlock(&epoll->lock);
    spin_unlock(&epoll_watch_lifecycle_lock);
    vfs_file_put(target);
    return (uint64_t)ret;
}

static size_t do_epoll_pwait(struct vfs_file *epoll_file,
                             struct epoll_event *events, int maxevents,
                             int64_t timeout_ns, const sigset_t *sigmask,
                             size_t sigsetsize) {
    sigset_t origmask;

    if (sigmask)
        sys_ssetmask(SIG_SETMASK, sigmask, &origmask, sizeof(sigset_t));
    uint64_t ret = do_epoll_wait(epoll_file, events, maxevents, timeout_ns);
    if (sigmask)
        sys_ssetmask(SIG_SETMASK, &origmask, 0, sizeof(sigset_t));
    return ret;
}

uint64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                        int timeout) {
    struct vfs_file *epoll_file;
    struct epoll_event stack_events[EPOLL_STACK_EVENTS];
    struct epoll_event *kevents = stack_events;
    uint64_t ret;
    size_t events_bytes;

    if (maxevents < 1)
        return (uint64_t)-EINVAL;
    if (!events ||
        check_user_array_overflow((uint64_t)events, (size_t)maxevents,
                                  sizeof(*events), &events_bytes)) {
        return (uint64_t)-EFAULT;
    }

    epoll_file = task_get_file(current_task, epfd);
    if (!epoll_file)
        return (uint64_t)-EBADF;

    if (maxevents > EPOLL_STACK_EVENTS) {
        kevents = calloc((size_t)maxevents, sizeof(*kevents));
        if (!kevents) {
            vfs_file_put(epoll_file);
            return (uint64_t)-ENOMEM;
        }
    }

    ret = do_epoll_wait(epoll_file, kevents, maxevents,
                        timeout < 0 ? -1 : (int64_t)timeout * 1000000LL);
    if ((int64_t)ret > 0) {
        size_t ready_bytes = (size_t)ret * sizeof(*kevents);
        if (copy_to_user(events, kevents, ready_bytes))
            ret = (uint64_t)-EFAULT;
    }

    if (kevents != stack_events)
        free(kevents);
    vfs_file_put(epoll_file);
    return ret;
}

uint64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    struct vfs_file *epoll_file;
    struct epoll_event k_event;
    const struct epoll_event *event_ptr = NULL;
    uint64_t ret;

    if ((op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) &&
        check_user_overflow((uint64_t)event, sizeof(*event))) {
        return (uint64_t)-EFAULT;
    }

    epoll_file = task_get_file(current_task, epfd);
    if (!epoll_file)
        return (uint64_t)-EBADF;

    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (!event || copy_from_user(&k_event, event, sizeof(k_event))) {
            vfs_file_put(epoll_file);
            return (uint64_t)-EFAULT;
        }
        event_ptr = &k_event;
    }

    ret = epoll_ctl_file(epoll_file, op, fd, event_ptr);
    vfs_file_put(epoll_file);
    return ret;
}

uint64_t sys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                         int timeout, sigset_t *sigmask, size_t sigsetsize) {
    struct vfs_file *epoll_file;
    struct epoll_event stack_events[EPOLL_STACK_EVENTS];
    struct epoll_event *kevents = stack_events;
    sigset_t newmask = 0;
    const sigset_t *sigmask_ptr = NULL;
    uint64_t ret;
    size_t events_bytes;

    if (maxevents < 1)
        return (uint64_t)-EINVAL;
    if (!events ||
        check_user_array_overflow((uint64_t)events, (size_t)maxevents,
                                  sizeof(*events), &events_bytes)) {
        return (uint64_t)-EFAULT;
    }
    if (sigmask && sigsetsize < sizeof(sigset_t))
        return (uint64_t)-EINVAL;

    epoll_file = task_get_file(current_task, epfd);
    if (!epoll_file)
        return (uint64_t)-EBADF;

    if (sigmask) {
        if (copy_from_user(&newmask, sigmask, sizeof(newmask))) {
            vfs_file_put(epoll_file);
            return (uint64_t)-EFAULT;
        }
        sigmask_ptr = &newmask;
    }

    if (maxevents > EPOLL_STACK_EVENTS) {
        kevents = calloc((size_t)maxevents, sizeof(*kevents));
        if (!kevents) {
            vfs_file_put(epoll_file);
            return (uint64_t)-ENOMEM;
        }
    }

    ret = do_epoll_pwait(epoll_file, kevents, maxevents,
                         timeout < 0 ? -1 : (int64_t)timeout * 1000000LL,
                         sigmask_ptr, sigsetsize);
    if ((int64_t)ret > 0) {
        size_t ready_bytes = (size_t)ret * sizeof(*kevents);
        if (copy_to_user(events, kevents, ready_bytes))
            ret = (uint64_t)-EFAULT;
    }

    if (kevents != stack_events)
        free(kevents);
    vfs_file_put(epoll_file);
    return ret;
}

uint64_t sys_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                          struct timespec *timeout, sigset_t *sigmask,
                          size_t sigsetsize) {
    struct vfs_file *epoll_file;
    struct epoll_event stack_events[EPOLL_STACK_EVENTS];
    struct epoll_event *kevents = stack_events;
    sigset_t newmask = 0;
    const sigset_t *sigmask_ptr = NULL;
    int64_t timeout_ns = -1;
    uint64_t ret;
    size_t events_bytes;

    if (maxevents < 1)
        return (uint64_t)-EINVAL;
    if (!events ||
        check_user_array_overflow((uint64_t)events, (size_t)maxevents,
                                  sizeof(*events), &events_bytes)) {
        return (uint64_t)-EFAULT;
    }
    if (sigmask && sigsetsize < sizeof(sigset_t))
        return (uint64_t)-EINVAL;

    epoll_file = task_get_file(current_task, epfd);
    if (!epoll_file)
        return (uint64_t)-EBADF;

    if (timeout) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout, sizeof(ts))) {
            vfs_file_put(epoll_file);
            return (uint64_t)-EFAULT;
        }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) {
            vfs_file_put(epoll_file);
            return (uint64_t)-EINVAL;
        }
        timeout_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }

    if (sigmask) {
        if (copy_from_user(&newmask, sigmask, sizeof(newmask))) {
            vfs_file_put(epoll_file);
            return (uint64_t)-EFAULT;
        }
        sigmask_ptr = &newmask;
    }

    if (maxevents > EPOLL_STACK_EVENTS) {
        kevents = calloc((size_t)maxevents, sizeof(*kevents));
        if (!kevents) {
            vfs_file_put(epoll_file);
            return (uint64_t)-ENOMEM;
        }
    }

    ret = do_epoll_pwait(epoll_file, kevents, maxevents, timeout_ns,
                         sigmask_ptr, sigsetsize);
    if ((int64_t)ret > 0) {
        size_t ready_bytes = (size_t)ret * sizeof(*kevents);
        if (copy_to_user(events, kevents, ready_bytes))
            ret = (uint64_t)-EFAULT;
    }

    if (kevents != stack_events)
        free(kevents);
    vfs_file_put(epoll_file);
    return ret;
}

uint64_t sys_epoll_create1(int flags) { return epoll_create1(flags); }

void epoll_init(void) {
    spin_init(&epollfs_mount_lock);
    spin_init(&epoll_watch_lifecycle_lock);
    vfs_register_filesystem(&epollfs_fs_type);
    regist_on_close_file_callback(epoll_on_close_file);
}
