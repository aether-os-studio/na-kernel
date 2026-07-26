#include <fs/vfs/notify.h>
#include <task/task.h>

#define NOTIFYFS_MAGIC 0x6e6f7469ULL
#define NOTIFYFS_WATCH_HASH_BITS 6U
#define NOTIFYFS_WATCH_HASH_SIZE (1U << NOTIFYFS_WATCH_HASH_BITS)
#define NOTIFYFS_MAX_QUEUED_EVENTS 16384U

typedef struct vfs_notify_event {
    struct llist_header node;
    char *name;
    uint32_t name_len;
    uint64_t mask;
    uint32_t cookie;
    int wd;
    struct vfs_inode *fid_inode;
    int32_t pid;
} vfs_notify_event_t;

typedef struct notifyfs_watch_bucket_entry {
    struct llist_header node;
    struct llist_header watches;
    struct vfs_inode *inode;
} notifyfs_watch_bucket_entry_t;

typedef struct notifyfs_watch_bucket {
    spinlock_t lock;
    struct llist_header entries;
} notifyfs_watch_bucket_t;

struct notifyfs_watch {
    struct llist_header node;
    struct llist_header inode_node;
    struct notifyfs_handle *handle;
    notifyfs_watch_bucket_entry_t *bucket;
    struct vfs_inode *watch_inode;
    struct vfs_inode *owner_inode;
    uint64_t mask;
    uint64_t wd;
    bool active;
};

struct notifyfs_handle {
    spinlock_t lock;
    struct llist_header watches;
    struct llist_header events;
    spinlock_t events_lock;
    uint64_t next_wd;
    uint32_t event_count;
    bool overflow_queued;
    bool fanotify;
    unsigned int fanotify_flags;
};

typedef struct notifyfs_fs_info {
    spinlock_t lock;
    ino64_t next_ino;
} notifyfs_fs_info_t;

typedef struct notifyfs_inode_info {
    struct vfs_inode vfs_inode;
} notifyfs_inode_info_t;

struct inotify_event {
    int wd;
    unsigned int mask;
    unsigned int cookie;
    unsigned int len;
    char name[];
};

struct fanotify_event_metadata {
    uint32_t event_len;
    uint8_t vers;
    uint8_t reserved;
    uint16_t metadata_len;
    uint64_t mask;
    int32_t fd;
    int32_t pid;
};

struct fanotify_event_info_header {
    uint8_t info_type;
    uint8_t pad;
    uint16_t len;
};

struct fanotify_event_info_fid_prefix {
    struct fanotify_event_info_header hdr;
    int32_t fsid[2];
    uint32_t handle_bytes;
    int32_t handle_type;
    uint64_t ino;
} __attribute__((packed));

_Static_assert(sizeof(struct fanotify_event_info_fid_prefix) == 28,
               "fanotify FID info must match the Linux userspace ABI");

#define FANOTIFY_METADATA_VERSION 3U
#define FAN_EVENT_INFO_TYPE_DFID_NAME 2U
#define FAN_NOFD (-1)

static struct vfs_file_system_type notifyfs_fs_type;
static const struct vfs_super_operations notifyfs_super_ops;
static const struct vfs_inode_operations notifyfs_dir_inode_ops;
static const struct vfs_file_operations notifyfs_dir_file_ops;
static const struct vfs_file_operations notifyfs_file_ops;
static spinlock_t notifyfs_mount_lock;
static struct vfs_mount *notifyfs_internal_mnt;
static notifyfs_watch_bucket_t notifyfs_watch_index[NOTIFYFS_WATCH_HASH_SIZE];
static volatile uint32_t notifyfs_cookie_seq = 1;

static inline notifyfs_fs_info_t *notifyfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (notifyfs_fs_info_t *)sb->s_fs_info : NULL;
}

static inline size_t notifyfs_watch_bucket_id(struct vfs_inode *inode) {
    return (((uintptr_t)inode) >> 6) & (NOTIFYFS_WATCH_HASH_SIZE - 1);
}

static inline notifyfs_watch_bucket_t *
notifyfs_watch_bucket_for(struct vfs_inode *inode) {
    return &notifyfs_watch_index[notifyfs_watch_bucket_id(inode)];
}

notifyfs_handle_t *notifyfs_file_handle(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (notifyfs_handle_t *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (notifyfs_handle_t *)file->f_inode->i_private;
}

int notifyfs_is_file(struct vfs_file *file) {
    notifyfs_handle_t *handle = notifyfs_file_handle(file);

    if (!handle || !file || !file->f_inode || !file->f_inode->i_sb ||
        !file->f_inode->i_sb->s_type) {
        return 0;
    }

    return file->f_inode->i_sb->s_type == &notifyfs_fs_type;
}

static size_t notify_event_name_len(const vfs_notify_event_t *event) {
    size_t len;

    if (!event || !event->name_len)
        return 0;

    len = (size_t)event->name_len + 1;
    return (len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}

static vfs_notify_event_t *notifyfs_event_alloc(int wd, const char *name,
                                                uint64_t mask, uint32_t cookie,
                                                struct vfs_inode *fid_inode) {
    vfs_notify_event_t *event;

    event = calloc(1, sizeof(*event));
    if (!event)
        return NULL;

    llist_init_head(&event->node);
    event->mask = mask;
    event->cookie = cookie;
    event->wd = wd;
    event->fid_inode = fid_inode ? vfs_igrab(fid_inode) : NULL;
    event->pid = current_task ? (int32_t)task_effective_tgid(current_task) : 0;

    if (name && name[0]) {
        size_t name_len = strlen(name);
        if (name_len > UINT32_MAX) {
            free(event);
            return NULL;
        }

        event->name = strdup(name);
        if (!event->name) {
            free(event);
            return NULL;
        }

        event->name_len = (uint32_t)name_len;
    }

    return event;
}

static void notifyfs_event_free(vfs_notify_event_t *event) {
    if (!event)
        return;

    free(event->name);
    if (event->fid_inode)
        vfs_iput(event->fid_inode);
    free(event);
}

static bool notifyfs_handle_queue_event(notifyfs_handle_t *handle, int wd,
                                        const char *name, uint64_t mask,
                                        uint32_t cookie,
                                        struct vfs_inode *fid_inode) {
    vfs_notify_event_t *event;
    vfs_notify_event_t *overflow = NULL;
    bool queued = false;

    if (!handle || !mask)
        return false;

    event = notifyfs_event_alloc(wd, name, mask, cookie, fid_inode);
    if (!event)
        return false;

    if (mask != IN_Q_OVERFLOW)
        overflow = notifyfs_event_alloc(-1, NULL, IN_Q_OVERFLOW, 0, NULL);

    spin_lock(&handle->events_lock);
    if (handle->event_count >= NOTIFYFS_MAX_QUEUED_EVENTS) {
        if (!handle->overflow_queued && overflow) {
            llist_append(&handle->events, &overflow->node);
            handle->event_count++;
            handle->overflow_queued = true;
            overflow = NULL;
            queued = true;
        }
    } else {
        llist_append(&handle->events, &event->node);
        handle->event_count++;
        event = NULL;
        queued = true;
    }
    spin_unlock(&handle->events_lock);

    notifyfs_event_free(event);
    notifyfs_event_free(overflow);

    return queued;
}

static void notifyfs_signal_owner(notifyfs_watch_t *watch) {
    if (!watch || !watch->owner_inode)
        return;

    vfs_poll_notify_inode(watch->owner_inode, EPOLLIN | EPOLLRDNORM);
}

static notifyfs_watch_bucket_entry_t *
notifyfs_find_bucket_entry_locked(notifyfs_watch_bucket_t *bucket,
                                  struct vfs_inode *inode) {
    notifyfs_watch_bucket_entry_t *entry, *tmp;

    if (!bucket || !inode)
        return NULL;

    llist_for_each(entry, tmp, &bucket->entries, node) {
        if (entry->inode == inode)
            return entry;
    }

    return NULL;
}

static void notifyfs_watch_unindex_locked(notifyfs_watch_t *watch,
                                          notifyfs_watch_bucket_t *bucket,
                                          bool drop_empty_entry) {
    notifyfs_watch_bucket_entry_t *entry;

    if (!watch || !bucket)
        return;

    entry = watch->bucket;
    if (!entry && watch->watch_inode)
        entry = notifyfs_find_bucket_entry_locked(bucket, watch->watch_inode);
    if (entry && !llist_empty(&watch->inode_node))
        llist_delete(&watch->inode_node);

    watch->bucket = NULL;
    if (drop_empty_entry && entry && llist_empty(&entry->watches)) {
        if (!llist_empty(&entry->node))
            llist_delete(&entry->node);
        vfs_iput(entry->inode);
        free(entry);
    }
}

static int notifyfs_watch_index_locked(notifyfs_watch_t *watch) {
    notifyfs_watch_bucket_t *bucket;
    notifyfs_watch_bucket_entry_t *entry;

    if (!watch || !watch->watch_inode)
        return -EINVAL;

    bucket = notifyfs_watch_bucket_for(watch->watch_inode);
    spin_lock(&bucket->lock);
    entry = notifyfs_find_bucket_entry_locked(bucket, watch->watch_inode);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            spin_unlock(&bucket->lock);
            return -ENOMEM;
        }

        llist_init_head(&entry->node);
        llist_init_head(&entry->watches);
        entry->inode = vfs_igrab(watch->watch_inode);
        llist_append(&bucket->entries, &entry->node);
    }

    if (llist_empty(&watch->inode_node))
        llist_append(&entry->watches, &watch->inode_node);
    watch->bucket = entry;
    spin_unlock(&bucket->lock);
    return 0;
}

static void notifyfs_watch_unindex(notifyfs_watch_t *watch) {
    notifyfs_watch_bucket_t *bucket;

    if (!watch || !watch->watch_inode)
        return;

    bucket = notifyfs_watch_bucket_for(watch->watch_inode);
    spin_lock(&bucket->lock);
    notifyfs_watch_unindex_locked(watch, bucket, true);
    spin_unlock(&bucket->lock);
}

static void
notifyfs_maybe_collect_stale_watch_locked(notifyfs_watch_t *watch,
                                          struct llist_header *stale) {
    if (!watch || watch->active || llist_empty(&watch->node)) {
        return;
    }

    llist_delete(&watch->node);
    if (stale)
        llist_append(stale, &watch->node);
}

static void notifyfs_watch_deactivate_locked(notifyfs_watch_t *watch,
                                             bool queue_ignored) {
    if (!watch || !watch->active)
        return;

    watch->active = false;
    notifyfs_watch_unindex(watch);

    if (!queue_ignored)
        return;

    if (notifyfs_handle_queue_event(watch->handle, (int)watch->wd, NULL,
                                    IN_IGNORED, 0, watch->watch_inode))
        notifyfs_signal_owner(watch);
}

static void
notifyfs_watch_deactivate_bucket_locked(notifyfs_watch_t *watch,
                                        notifyfs_watch_bucket_t *bucket,
                                        bool queue_ignored) {
    if (!watch || !watch->active)
        return;

    watch->active = false;
    notifyfs_watch_unindex_locked(watch, bucket, false);

    if (!queue_ignored)
        return;

    if (notifyfs_handle_queue_event(watch->handle, (int)watch->wd, NULL,
                                    IN_IGNORED, 0, watch->watch_inode))
        notifyfs_signal_owner(watch);
}

static void notifyfs_free_watch(notifyfs_watch_t *watch) {
    if (!watch)
        return;

    notifyfs_watch_unindex(watch);

    if (watch->watch_inode)
        vfs_iput(watch->watch_inode);
    if (watch->owner_inode)
        vfs_iput(watch->owner_inode);
    free(watch);
}

static void notifyfs_free_watch_list(struct llist_header *stale_watches) {
    notifyfs_watch_t *watch, *tmp;

    if (!stale_watches)
        return;

    llist_for_each(watch, tmp, stale_watches, node) {
        llist_delete(&watch->node);
        notifyfs_free_watch(watch);
    }
}

static bool notifyfs_handle_has_events(notifyfs_handle_t *handle) {
    bool has_events;

    if (!handle)
        return false;

    spin_lock(&handle->events_lock);
    has_events = !llist_empty(&handle->events);
    spin_unlock(&handle->events_lock);
    return has_events;
}

static ssize_t
notifyfs_drain_events_locked(notifyfs_handle_t *handle, void *addr, size_t size,
                             struct llist_header *stale_watches) {
    uint8_t *write_pos;
    uint64_t total_write;

    if (!handle || !addr)
        return -EINVAL;

    write_pos = (uint8_t *)addr;
    total_write = 0;

    spin_lock(&handle->events_lock);
    vfs_notify_event_t *event, *event_tmp;
    llist_for_each(event, event_tmp, &handle->events, node) {
        struct inotify_event *raw;
        size_t name_len;
        size_t total_len;

        name_len = notify_event_name_len(event);
        total_len = sizeof(*raw) + name_len;
        if (total_write + total_len > size) {
            spin_unlock(&handle->events_lock);
            if (total_write == 0)
                return -EINVAL;
            return (ssize_t)total_write;
        }

        llist_delete(&event->node);
        if (handle->event_count)
            handle->event_count--;
        if (event->mask & IN_Q_OVERFLOW)
            handle->overflow_queued = false;

        raw = (struct inotify_event *)write_pos;
        memset(raw, 0, total_len);
        raw->wd = event->wd;
        raw->mask = (unsigned int)event->mask;
        raw->cookie = event->cookie;
        raw->len = (unsigned int)name_len;
        if (name_len && event->name)
            memcpy(raw->name, event->name, event->name_len);
        write_pos += total_len;
        total_write += total_len;
        notifyfs_event_free(event);
    }
    spin_unlock(&handle->events_lock);

    spin_lock(&handle->lock);
    notifyfs_watch_t *watch, *watch_tmp;
    llist_for_each(watch, watch_tmp, &handle->watches, node)
        notifyfs_maybe_collect_stale_watch_locked(watch, stale_watches);
    spin_unlock(&handle->lock);
    return (ssize_t)total_write;
}

static size_t notifyfs_fanotify_event_size(const vfs_notify_event_t *event) {
    size_t name_len;
    size_t info_len;

    if (!event || (event->mask & IN_Q_OVERFLOW))
        return sizeof(struct fanotify_event_metadata);

    name_len = event->name ? (size_t)event->name_len + 1 : 1;
    info_len = sizeof(struct fanotify_event_info_fid_prefix) + name_len;
    info_len = (info_len + 7U) & ~7U;
    return sizeof(struct fanotify_event_metadata) + info_len;
}

static ssize_t notifyfs_drain_fanotify_events(notifyfs_handle_t *handle,
                                              void *addr, size_t size) {
    uint8_t *write_pos = addr;
    size_t total_write = 0;

    if (!handle || !addr)
        return -EINVAL;

    spin_lock(&handle->events_lock);
    vfs_notify_event_t *event, *event_tmp;
    llist_for_each(event, event_tmp, &handle->events, node) {
        size_t event_len = notifyfs_fanotify_event_size(event);
        if (total_write + event_len > size) {
            spin_unlock(&handle->events_lock);
            return total_write ? (ssize_t)total_write : -EINVAL;
        }

        llist_delete(&event->node);
        if (handle->event_count)
            handle->event_count--;
        if (event->mask & IN_Q_OVERFLOW)
            handle->overflow_queued = false;

        memset(write_pos, 0, event_len);
        struct fanotify_event_metadata *metadata =
            (struct fanotify_event_metadata *)write_pos;
        metadata->event_len = (uint32_t)event_len;
        metadata->vers = FANOTIFY_METADATA_VERSION;
        metadata->metadata_len = sizeof(*metadata);
        metadata->mask = event->mask;
        metadata->fd = FAN_NOFD;
        metadata->pid = event->pid;

        if (!(event->mask & IN_Q_OVERFLOW)) {
            struct fanotify_event_info_fid_prefix *info =
                (struct fanotify_event_info_fid_prefix *)(write_pos +
                                                          sizeof(*metadata));
            size_t info_len = event_len - sizeof(*metadata);
            dev64_t dev = event->fid_inode && event->fid_inode->i_sb
                              ? event->fid_inode->i_sb->s_dev
                              : 0;

            info->hdr.info_type = FAN_EVENT_INFO_TYPE_DFID_NAME;
            info->hdr.len = (uint16_t)info_len;
            info->fsid[0] = (int32_t)(uint32_t)dev;
            info->fsid[1] = (int32_t)(uint32_t)(dev >> 32);
            info->handle_bytes = sizeof(uint64_t);
            info->handle_type = 1;
            info->ino = event->fid_inode ? event->fid_inode->i_ino : 0;

            char *name = (char *)(info + 1);
            if (event->name && event->name_len)
                memcpy(name, event->name, event->name_len);
            name[event->name_len] = '\0';
        }

        write_pos += event_len;
        total_write += event_len;
        notifyfs_event_free(event);
    }
    spin_unlock(&handle->events_lock);
    return (ssize_t)total_write;
}

static void notifyfs_destroy_handle(notifyfs_handle_t *handle) {
    struct llist_header stale_watches;
    vfs_notify_event_t *event, *event_tmp;

    if (!handle)
        return;

    llist_init_head(&stale_watches);
    spin_lock(&handle->lock);
    while (!llist_empty(&handle->watches)) {
        notifyfs_watch_t *watch =
            list_entry(handle->watches.next, notifyfs_watch_t, node);
        notifyfs_watch_deactivate_locked(watch, false);
        llist_delete(&watch->node);
        llist_append(&stale_watches, &watch->node);
    }
    spin_unlock(&handle->lock);

    notifyfs_free_watch_list(&stale_watches);

    spin_lock(&handle->events_lock);
    llist_for_each(event, event_tmp, &handle->events, node) {
        llist_delete(&event->node);
        notifyfs_event_free(event);
    }
    spin_unlock(&handle->events_lock);

    free(handle);
}

int notifyfs_handle_add_watch(notifyfs_handle_t *handle,
                              struct vfs_inode *owner_inode,
                              struct vfs_inode *watch_inode, uint64_t mask,
                              uint64_t *wd_out) {
    notifyfs_watch_t *watch, *tmp;
    uint64_t new_mask;
    int ret;

    if (!handle || !owner_inode || !watch_inode)
        return -EINVAL;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE))
        return -EINVAL;
    if ((mask & IN_ONLYDIR) && !S_ISDIR(watch_inode->i_mode))
        return -ENOTDIR;

    new_mask = mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);
    if (!new_mask)
        return -EINVAL;

    spin_lock(&handle->lock);
    llist_for_each(watch, tmp, &handle->watches, node) {
        if (watch->watch_inode != watch_inode)
            continue;
        if (!watch->active)
            continue;
        if (mask & IN_MASK_CREATE) {
            spin_unlock(&handle->lock);
            return -EEXIST;
        }

        if (mask & IN_MASK_ADD)
            watch->mask |= new_mask;
        else
            watch->mask = new_mask;
        if (wd_out)
            *wd_out = watch->wd;
        spin_unlock(&handle->lock);
        return 0;
    }

    watch = calloc(1, sizeof(*watch));
    if (!watch) {
        spin_unlock(&handle->lock);
        return -ENOMEM;
    }

    llist_init_head(&watch->node);
    llist_init_head(&watch->inode_node);
    watch->handle = handle;
    watch->watch_inode = vfs_igrab(watch_inode);
    watch->owner_inode = vfs_igrab(owner_inode);
    watch->mask = new_mask;
    watch->wd = handle->next_wd++;
    if (!watch->wd)
        watch->wd = handle->next_wd++;
    watch->active = true;
    llist_append(&handle->watches, &watch->node);
    if (wd_out)
        *wd_out = watch->wd;
    spin_unlock(&handle->lock);

    ret = notifyfs_watch_index_locked(watch);
    if (ret < 0) {
        spin_lock(&handle->lock);
        if (!llist_empty(&watch->node))
            llist_delete(&watch->node);
        spin_unlock(&handle->lock);
        notifyfs_free_watch(watch);
        return ret;
    }

    return 0;
}

int notifyfs_handle_remove_watch(notifyfs_handle_t *handle, uint64_t wd) {
    notifyfs_watch_t *watch, *tmp;
    notifyfs_watch_t *removed = NULL;

    if (!handle || !wd)
        return -EINVAL;

    spin_lock(&handle->lock);
    llist_for_each(watch, tmp, &handle->watches, node) {
        if (watch->wd != wd || !watch->active)
            continue;
        notifyfs_watch_deactivate_locked(watch, true);
        llist_delete(&watch->node);
        removed = watch;
        spin_unlock(&handle->lock);
        notifyfs_free_watch(removed);
        return 0;
    }
    spin_unlock(&handle->lock);
    return -EINVAL;
}

int notifyfs_handle_remove_watch_inode(notifyfs_handle_t *handle,
                                       struct vfs_inode *watch_inode) {
    notifyfs_watch_t *watch, *tmp;

    if (!handle || !watch_inode)
        return -EINVAL;

    spin_lock(&handle->lock);
    llist_for_each(watch, tmp, &handle->watches, node) {
        if (watch->watch_inode != watch_inode || !watch->active)
            continue;
        notifyfs_watch_deactivate_locked(watch, false);
        llist_delete(&watch->node);
        spin_unlock(&handle->lock);
        notifyfs_free_watch(watch);
        return 0;
    }
    spin_unlock(&handle->lock);
    return -ENOENT;
}

int notifyfs_handle_set_fanotify(notifyfs_handle_t *handle,
                                 unsigned int init_flags) {
    if (!handle)
        return -EINVAL;
    if (!llist_empty(&handle->watches) || !llist_empty(&handle->events))
        return -EBUSY;
    handle->fanotify = true;
    handle->fanotify_flags = init_flags;
    return 0;
}

bool notifyfs_handle_is_fanotify(notifyfs_handle_t *handle) {
    return handle && handle->fanotify;
}

uint32_t notifyfs_next_cookie(void) {
    uint32_t cookie =
        __atomic_add_fetch(&notifyfs_cookie_seq, 1, __ATOMIC_ACQ_REL);

    if (!cookie)
        cookie = __atomic_add_fetch(&notifyfs_cookie_seq, 1, __ATOMIC_ACQ_REL);
    return cookie;
}

bool notifyfs_queue_inode_event(struct vfs_inode *watch_inode,
                                struct vfs_inode *changed_inode,
                                const char *name, uint64_t mask,
                                uint32_t cookie) {
    notifyfs_watch_bucket_t *bucket;
    notifyfs_watch_bucket_entry_t *entry;
    notifyfs_watch_t *watch, *tmp;
    bool queued = false;
    uint64_t event_mask;

    if (!watch_inode || !mask)
        return false;

    event_mask = mask;
    if (changed_inode && S_ISDIR(changed_inode->i_mode))
        event_mask |= IN_ISDIR;

    bucket = notifyfs_watch_bucket_for(watch_inode);
    spin_lock(&bucket->lock);
    entry = notifyfs_find_bucket_entry_locked(bucket, watch_inode);
    if (!entry) {
        spin_unlock(&bucket->lock);
        return false;
    }

    llist_for_each(watch, tmp, &entry->watches, inode_node) {
        uint64_t match_mask;

        if (!watch->active)
            continue;

        match_mask = event_mask & ~(uint64_t)IN_ISDIR;
        if (!(watch->mask & match_mask))
            continue;

        if (!notifyfs_handle_queue_event(watch->handle, (int)watch->wd, name,
                                         event_mask, cookie, watch_inode))
            continue;

        notifyfs_signal_owner(watch);
        queued = true;

        if (watch->mask & IN_ONESHOT)
            notifyfs_watch_deactivate_bucket_locked(watch, bucket, true);
    }

    if (llist_empty(&entry->watches)) {
        if (!llist_empty(&entry->node))
            llist_delete(&entry->node);
        vfs_iput(entry->inode);
        free(entry);
    }

    spin_unlock(&bucket->lock);
    return queued;
}

static ssize_t notifyfs_read(struct vfs_file *file, void *addr, size_t size,
                             loff_t *ppos) {
    notifyfs_handle_t *handle;

    (void)ppos;
    handle = notifyfs_file_handle(file);

    for (;;) {
        struct llist_header stale_watches;
        ssize_t ret;

        llist_init_head(&stale_watches);
        ret = handle && handle->fanotify
                  ? notifyfs_drain_fanotify_events(handle, addr, size)
                  : notifyfs_drain_events_locked(handle, addr, size,
                                                 &stale_watches);
        notifyfs_free_watch_list(&stale_watches);
        if (ret != 0)
            return ret;

        if (fd_get_flags(file) & O_NONBLOCK)
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(
            file, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLNVAL);
        if (reason < 0)
            return reason;
    }
}

static long notifyfs_ioctl(struct vfs_file *file, unsigned long cmd,
                           unsigned long arg) {
    notifyfs_handle_t *handle;

    handle = notifyfs_file_handle(file);
    if (!handle)
        return -EINVAL;

    switch (cmd) {
    case FIONREAD: {
        int total = 0;

        spin_lock(&handle->events_lock);
        vfs_notify_event_t *event, *event_tmp;
        llist_for_each(event, event_tmp, &handle->events, node) {
            total += handle->fanotify ? notifyfs_fanotify_event_size(event)
                                      : sizeof(struct inotify_event) +
                                            notify_event_name_len(event);
        }
        spin_unlock(&handle->events_lock);

        if (copy_to_user((void *)arg, &total, sizeof(total)))
            return -EFAULT;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static __poll_t notifyfs_poll(struct vfs_file *file,
                              struct vfs_poll_table *pt) {
    notifyfs_handle_t *handle;
    __poll_t revents = 0;

    (void)pt;
    handle = notifyfs_file_handle(file);
    if (!handle)
        return EPOLLNVAL;
    if (notifyfs_handle_has_events(handle))
        revents |= EPOLLIN | EPOLLRDNORM;
    return revents;
}

static int notifyfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;

    file->f_op = inode->i_fop;
    file->private_data = inode->i_private;
    return 0;
}

static int notifyfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    notifyfs_handle_t *handle = notifyfs_file_handle(file);

    if (!handle && inode)
        handle = (notifyfs_handle_t *)inode->i_private;
    if (!handle)
        return 0;

    if (file)
        file->private_data = NULL;
    if (inode)
        inode->i_private = NULL;
    notifyfs_destroy_handle(handle);
    return 0;
}

static struct vfs_dentry *notifyfs_lookup(struct vfs_inode *dir,
                                          struct vfs_dentry *dentry,
                                          unsigned int flags) {
    (void)dir;
    (void)flags;
    vfs_d_instantiate(dentry, NULL);
    return dentry;
}

static int notifyfs_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                            uint32_t request_mask, unsigned int flags) {
    (void)request_mask;
    (void)flags;
    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static struct vfs_inode *notifyfs_alloc_inode(struct vfs_super_block *sb) {
    notifyfs_inode_info_t *info = calloc(1, sizeof(*info));

    if (!info)
        return NULL;
    return &info->vfs_inode;
}

static void notifyfs_destroy_inode(struct vfs_inode *inode) {
    notifyfs_inode_info_t *info;

    if (!inode)
        return;
    if (inode->i_private) {
        notifyfs_destroy_handle((notifyfs_handle_t *)inode->i_private);
        inode->i_private = NULL;
    }

    info = container_of(inode, notifyfs_inode_info_t, vfs_inode);
    free(info);
}

static void notifyfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static int notifyfs_statfs(struct vfs_path *path, void *buf) {
    (void)path;
    (void)buf;
    return 0;
}

static int notifyfs_init_fs_context(struct vfs_fs_context *fc) {
    fc->sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!fc->sb)
        return -ENOMEM;
    return 0;
}

static int notifyfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb = fc->sb;
    notifyfs_fs_info_t *fsi;
    struct vfs_inode *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};

    if (!sb)
        return -EINVAL;

    fsi = calloc(1, sizeof(*fsi));
    if (!fsi)
        return -ENOMEM;

    spin_init(&fsi->lock);
    fsi->next_ino = 1;

    sb->s_fs_info = fsi;
    sb->s_op = &notifyfs_super_ops;
    sb->s_type = &notifyfs_fs_type;
    sb->s_magic = NOTIFYFS_MAGIC;

    root_inode = vfs_alloc_inode(sb);
    if (!root_inode)
        return -ENOMEM;

    root_inode->i_ino = fsi->next_ino++;
    root_inode->inode = root_inode->i_ino;
    root_inode->i_mode = S_IFDIR | 0700;
    root_inode->i_nlink = 2;
    root_inode->i_op = &notifyfs_dir_inode_ops;
    root_inode->i_fop = &notifyfs_dir_file_ops;

    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    vfs_iput(root_inode);
    return 0;
}

static const struct vfs_super_operations notifyfs_super_ops = {
    .alloc_inode = notifyfs_alloc_inode,
    .destroy_inode = notifyfs_destroy_inode,
    .put_super = notifyfs_put_super,
    .statfs = notifyfs_statfs,
};

static const struct vfs_inode_operations notifyfs_dir_inode_ops = {
    .lookup = notifyfs_lookup,
    .getattr = notifyfs_getattr,
};

static const struct vfs_file_operations notifyfs_dir_file_ops = {
    .open = notifyfs_open,
    .release = notifyfs_release,
};

static const struct vfs_file_operations notifyfs_file_ops = {
    .read = notifyfs_read,
    .unlocked_ioctl = notifyfs_ioctl,
    .poll = notifyfs_poll,
    .open = notifyfs_open,
    .release = notifyfs_release,
};

static struct vfs_file_system_type notifyfs_fs_type = {
    .name = "notifyfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = notifyfs_init_fs_context,
    .get_tree = notifyfs_get_tree,
};

static struct vfs_mount *notifyfs_get_internal_mount(void) {
    int ret;

    spin_lock(&notifyfs_mount_lock);
    if (!notifyfs_internal_mnt) {
        ret = vfs_kern_mount("notifyfs", 0, NULL, NULL, &notifyfs_internal_mnt);
        if (ret < 0)
            notifyfs_internal_mnt = NULL;
    }
    if (notifyfs_internal_mnt)
        vfs_mntget(notifyfs_internal_mnt);
    spin_unlock(&notifyfs_mount_lock);
    return notifyfs_internal_mnt;
}

int notifyfs_create_handle_file(struct vfs_file **out_file,
                                unsigned int open_flags,
                                notifyfs_handle_t **out_handle) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    notifyfs_fs_info_t *fsi;
    notifyfs_handle_t *handle;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    char namebuf[32];

    if (!out_file)
        return -EINVAL;

    mnt = notifyfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;

    sb = mnt->mnt_sb;
    fsi = notifyfs_sb_info(sb);
    handle = calloc(1, sizeof(*handle));
    if (!handle) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    spin_init(&handle->lock);
    llist_init_head(&handle->watches);
    llist_init_head(&handle->events);
    spin_init(&handle->events_lock);
    handle->next_wd = 1;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(handle);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    spin_lock(&fsi->lock);
    inode->i_ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFREG | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &notifyfs_file_ops;
    inode->i_private = handle;

    snprintf(namebuf, sizeof(namebuf), "anon-%llu",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDONLY | (open_flags & O_NONBLOCK));
    if (!file) {
        vfs_dput(dentry);
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = handle;
    file->f_mode |= VFS_FMODE_NO_POS_LOCK;
    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);

    *out_file = file;
    if (out_handle)
        *out_handle = handle;
    return 0;
}

void notifyfs_init(void) {
    for (size_t i = 0; i < NOTIFYFS_WATCH_HASH_SIZE; ++i) {
        spin_init(&notifyfs_watch_index[i].lock);
        llist_init_head(&notifyfs_watch_index[i].entries);
    }

    spin_init(&notifyfs_mount_lock);
    vfs_register_filesystem(&notifyfs_fs_type);
}
