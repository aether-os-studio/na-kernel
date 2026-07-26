#include "fs/vfs/vfs_internal.h"
#include "fs/vfs/notify.h"
#include "drivers/logger.h"
#include "task/task.h"
#include <arch/arch.h>
#include <fs/fs_syscall.h>
#include <mm/mm.h>
#include <task/signal.h>

static inline uint32_t vfs_poll_expand_events(uint32_t events) {
    if (events & EPOLLIN)
        events |= EPOLLRDNORM;
    if (events & EPOLLRDNORM)
        events |= EPOLLIN;
    if (events & EPOLLOUT)
        events |= EPOLLWRNORM;
    if (events & EPOLLWRNORM)
        events |= EPOLLOUT;
    return events;
}

static void vfs_poll_table_queue_proc(struct vfs_file *file,
                                      struct vfs_poll_table *pt) {
    vfs_poll_wait_table_t *table;
    vfs_poll_wait_entry_t *entry;

    if (!file || !file->f_inode || !pt || !pt->private_data)
        return;

    table = (vfs_poll_wait_table_t *)pt->private_data;
    if (table->error)
        return;

    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        table->error = -ENOMEM;
        return;
    }

    entry->inode = vfs_igrab(file->f_inode);
    if (!entry->inode) {
        free(entry);
        table->error = -EBADF;
        return;
    }
    entry->seq = __atomic_load_n(&entry->inode->poll_seq, __ATOMIC_ACQUIRE);

    llist_init_head(&entry->node);
    wait_queue_entry_init(&entry->wait, table->task, pt->events, NULL, table);
    wait_queue_add(&entry->inode->poll_wait, &entry->wait);
    llist_append(&table->entries, &entry->node);
}

void vfs_poll_wait_table_init(vfs_poll_wait_table_t *table, task_t *task) {
    if (!table)
        return;

    memset(table, 0, sizeof(*table));
    table->pt.queue_proc = vfs_poll_table_queue_proc;
    table->pt.private_data = table;
    table->task = task;
    llist_init_head(&table->entries);
    task_prepare_block(task);
}

void vfs_poll_wait_table_cleanup(vfs_poll_wait_table_t *table) {
    if (!table)
        return;

    while (!llist_empty(&table->entries)) {
        vfs_poll_wait_entry_t *entry =
            list_entry(table->entries.next, vfs_poll_wait_entry_t, node);

        wait_queue_remove(&entry->inode->poll_wait, &entry->wait);
        llist_delete(&entry->node);
        vfs_iput(entry->inode);
        free(entry);
    }
    task_cancel_block_prepare(table->task);
}

int vfs_poll_wait_table_error(vfs_poll_wait_table_t *table) {
    return table ? table->error : -EINVAL;
}

bool vfs_poll_wait_table_seq_changed(vfs_poll_wait_table_t *table) {
    if (!table)
        return false;

    vfs_poll_wait_entry_t *entry, *tmp;
    llist_for_each(entry, tmp, &table->entries, node) {
        uint64_t seq =
            __atomic_load_n(&entry->inode->poll_seq, __ATOMIC_ACQUIRE);
        if (seq != entry->seq)
            return true;
    }

    return false;
}

void vfs_poll_wait(struct vfs_file *file, struct vfs_poll_table *pt) {
    if (!pt || !pt->queue_proc)
        return;
    pt->queue_proc(file, pt);
}

void vfs_poll_notify_inode(struct vfs_inode *inode, uint32_t events) {
    if (!inode || !events)
        return;
    __atomic_add_fetch(&inode->poll_seq, 1, __ATOMIC_ACQ_REL);
    wait_queue_wake_all(&inode->poll_wait, vfs_poll_expand_events(events), EOK);
}

void vfs_poll_notify_file(struct vfs_file *file, uint32_t events) {
    if (!file)
        return;
    vfs_poll_notify_inode(file->f_inode, events);
}

static unsigned int vfs_open_lookup_flags(const struct vfs_open_how *how) {
    unsigned int flags = 0;

    if (!how)
        return 0;

    if (how->resolve & RESOLVE_NO_SYMLINKS)
        flags |= LOOKUP_NO_SYMLINKS;
    if (how->resolve & RESOLVE_BENEATH)
        flags |= LOOKUP_BENEATH;
    if (how->resolve & RESOLVE_IN_ROOT)
        flags |= LOOKUP_IN_ROOT;
    if (how->resolve & RESOLVE_CACHED)
        flags |= LOOKUP_CACHED;
    if (how->resolve & RESOLVE_NO_XDEV)
        flags |= LOOKUP_NO_XDEV;

    return flags;
}

static bool vfs_open_flags_want_write(unsigned int flags) {
    unsigned int accmode = flags & O_ACCMODE_FLAGS;

    if (flags & O_PATH)
        return false;
    return accmode == O_WRONLY || accmode == O_RDWR;
}

static int vfs_open_from_root(struct vfs_path *start, struct vfs_path *root,
                              const char *name, const struct vfs_open_how *how,
                              struct vfs_file **out, bool kernel);

struct vfs_file *vfs_alloc_file(const struct vfs_path *path,
                                unsigned int open_flags) {
    struct vfs_file *file;

    if (!path || !path->dentry || !path->dentry->d_inode)
        return NULL;

    file = calloc(1, sizeof(*file));
    if (!file)
        return NULL;

    if (!vfs_path_get((struct vfs_path *)path)) {
        free(file);
        return NULL;
    }
    file->f_path = *path;
    file->f_inode = vfs_igrab(path->dentry->d_inode);
    if (!file->f_inode) {
        vfs_path_put(&file->f_path);
        free(file);
        return NULL;
    }
    file->node = file->f_inode;
    file->f_op = file->f_inode->i_fop;
    /* O_CLOEXEC is a descriptor-creation flag. It is stored in the fd table
     * as FD_CLOEXEC and must never leak through F_GETFL or SCM_RIGHTS into the
     * shared open-file-description status flags. */
    file->f_flags = open_flags & ~O_CLOEXEC;
    /* Only regular files and directories use the VFS-managed shared file
     * position. Stream-like files can block indefinitely in read while a
     * peer must write through the same open file description; serializing
     * both directions on f_pos_lock would deadlock them even though their
     * backends ignore ppos. */
    if (!S_ISREG(file->f_inode->i_mode) && !S_ISDIR(file->f_inode->i_mode))
        file->f_mode |= VFS_FMODE_NO_POS_LOCK;
    wait_mutex_init(&file->f_pos_lock);
    spin_init(&file->f_lock);
    file->f_fd_refs = 0;
    spin_init(&file->epoll_watches_lock);
    llist_init_head(&file->epoll_watches);
    vfs_ref_init(&file->f_ref, 1);
    return file;
}

struct vfs_file *vfs_file_get(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (!vfs_ref_try_get(&file->f_ref))
        return NULL;
    return file;
}

static void vfs_file_release_access_modes(struct vfs_file *file) {
    struct vfs_inode *inode;

    if (!file)
        return;

    inode = file->f_inode;
    if ((file->f_mode & VFS_FMODE_WRITE_ACCESS) && inode) {
        vfs_inode_put_write_access(inode);
        file->f_mode &= ~VFS_FMODE_WRITE_ACCESS;
    }
    if ((file->f_mode & VFS_FMODE_EXEC_ACCESS) && inode) {
        if (S_ISREG(inode->i_mode)) {
            spin_lock(&inode->i_lock);
            if (__atomic_load_n(&inode->i_exec_count, __ATOMIC_ACQUIRE) > 0) {
                __atomic_sub_fetch(&inode->i_exec_count, 1, __ATOMIC_ACQ_REL);
            }
            spin_unlock(&inode->i_lock);
        }
        file->f_mode &= ~VFS_FMODE_EXEC_ACCESS;
    }
}

void vfs_file_put(struct vfs_file *file) {
    if (!file)
        return;
    if (!vfs_ref_put(&file->f_ref))
        return;

    vfs_file_release_access_modes(file);

    if (file->f_inode) {
        vfs_bsd_lock_t *flock = &file->f_inode->flock_lock;
        spin_lock(&flock->spin);
        if (flock->owner == (uintptr_t)file) {
            flock->l_type = F_UNLCK;
            flock->owner = 0;
        }
        spin_unlock(&flock->spin);

        spin_lock(&file->f_inode->file_locks_lock);
        struct llist_header *node = file->f_inode->file_locks.next;
        while (node != &file->f_inode->file_locks) {
            vfs_file_lock_t *lock = list_entry(node, vfs_file_lock_t, node);

            node = node->next;
            if (!lock->ofd || lock->owner != (uintptr_t)file)
                continue;
            llist_delete(&lock->node);
            free(lock);
        }
        spin_unlock(&file->f_inode->file_locks_lock);
    }

    if (file->f_op && file->f_op->release)
        file->f_op->release(file->f_inode, file);
    if (file->f_inode) {
        vfs_iput(file->f_inode);
        file->f_inode = NULL;
    }
    vfs_path_put(&file->f_path);
    free(file);
}

int vfs_openat(int dfd, const char *name, const struct vfs_open_how *how,
               struct vfs_file **out, bool kernel) {
    struct vfs_path start = {0};
    struct vfs_path root = {0};
    int ret;

    if (!name || !out)
        return -EINVAL;

    ret =
        vfs_get_fs_start(dfd, name, vfs_open_lookup_flags(how), &start, &root);
    if (ret < 0)
        return ret;

    ret = vfs_open_from_root(&start, &root, name, how, out, kernel);
    vfs_path_put(&start);
    vfs_path_put(&root);
    return ret;
}

int vfs_open_from(struct vfs_path *start, const char *name,
                  const struct vfs_open_how *how, struct vfs_file **out,
                  bool kernel) {
    struct vfs_path root = {0};
    int ret;

    if (!start)
        return -EINVAL;

    if (!vfs_path_copy(&root, start))
        return -ENOENT;
    ret = vfs_open_from_root(start, &root, name, how, out, kernel);
    vfs_path_put(&root);
    return ret;
}

static int vfs_open_from_root(struct vfs_path *start, struct vfs_path *root,
                              const char *name, const struct vfs_open_how *how,
                              struct vfs_file **out, bool kernel) {
    struct vfs_open_how local_how;
    struct vfs_path parent = {0};
    struct vfs_path target = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *dentry = NULL;
    struct vfs_file *file;
    struct vfs_inode *dir;
    struct vfs_path *open_path;
    bool dentry_owned_by_target = false;
    bool created = false;
    int ret;
    unsigned int lookup_flags;
    unsigned int component_flags;

    if (!start || !root || !name || !out)
        return -EINVAL;

    memset(&local_how, 0, sizeof(local_how));
    if (how)
        local_how = *how;
    if ((local_how.flags & (O_CREAT | O_TMPFILE)) && current_task &&
        current_task->fs)
        local_how.mode &= ~current_task->fs->umask;

    if ((local_how.flags & O_TMPFILE) == O_TMPFILE) {
        if ((local_how.flags & O_ACCMODE_FLAGS) == O_RDONLY) {
            ret = -EINVAL;
            goto out;
        }
        local_how.flags &= ~(O_TMPFILE | O_CREAT | O_EXCL | O_TRUNC);
        local_how.flags |= O_DIRECTORY;
    }

    lookup_flags = LOOKUP_PARENT | vfs_open_lookup_flags(&local_how);
    component_flags = LOOKUP_CREATE | vfs_open_lookup_flags(&local_how);
    ret = vfs_path_parent_lookup_from(start, root, name, lookup_flags, &parent,
                                      &last, NULL);
    if (ret < 0)
        goto out;

    dir = parent.dentry->d_inode;
    if (!parent.dentry || !dir) {
        ret = -ENOENT;
        goto out;
    }
    if (!S_ISDIR(dir->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    dentry = vfs_lookup_component(&parent, last.name, component_flags);
    if (IS_ERR(dentry)) {
        ret = PTR_ERR(dentry);
        dentry = NULL;
        if (!(ret == -ENOENT && (local_how.flags & O_CREAT)))
            goto out;
    }
    if (!dentry || !dentry->d_inode) {
        if (!(local_how.flags & O_CREAT)) {
            ret = -ENOENT;
            goto out;
        }
        if (!dentry) {
            dentry = vfs_d_alloc(parent.dentry->d_sb, parent.dentry, &last);
            if (!dentry) {
                ret = -ENOMEM;
                goto out;
            }
        }
        if (!kernel) {
            ret = vfs_inode_permission(dir, VFS_MAY_WRITE | VFS_MAY_EXEC);
            if (ret < 0)
                goto out;
        }

        if (local_how.flags & O_DIRECTORY) {
            if (!dir->i_op || !dir->i_op->mkdir) {
                ret = -EOPNOTSUPP;
                goto out;
            }
            ret = dir->i_op->mkdir(dir, dentry, (umode_t)local_how.mode);
        } else {
            if (!dir->i_op || !dir->i_op->create) {
                ret = -EOPNOTSUPP;
                goto out;
            }
            ret = dir->i_op->create(dir, dentry, (umode_t)local_how.mode,
                                    !!(local_how.flags & O_EXCL));
        }
        if (ret < 0)
            goto out;
        created = true;
        if (!(dentry->d_flags & VFS_DENTRY_HASHED))
            vfs_d_add(parent.dentry, dentry);
    } else if ((local_how.flags & O_CREAT) && (local_how.flags & O_EXCL)) {
        ret = -EEXIST;
        goto out;
    }

    if (!dentry->d_inode) {
        ret = -ENOENT;
        goto out;
    }

    open_path = &(struct vfs_path){.mnt = parent.mnt, .dentry = dentry};
    if (!created) {
        struct vfs_path mounted = {0};

        if (!vfs_path_copy(&mounted, open_path)) {
            ret = -ENOENT;
            goto out;
        }
        vfs_follow_mount(&mounted);
        if (mounted.mnt != parent.mnt || mounted.dentry != dentry) {
            target = mounted;
            open_path = &target;
            dentry = target.dentry;
            dentry_owned_by_target = true;
        } else {
            vfs_path_put(&mounted);
        }
    }
    if (!created && S_ISLNK(dentry->d_inode->i_mode)) {
        if (local_how.resolve & RESOLVE_NO_SYMLINKS) {
            if (!((local_how.flags & O_NOFOLLOW) &&
                  (local_how.flags & O_PATH))) {
                ret = -ELOOP;
                goto out;
            }
        } else if (local_how.flags & O_NOFOLLOW) {
            if (!(local_how.flags & O_PATH)) {
                ret = -ELOOP;
                goto out;
            }
        } else {
            ret = vfs_filename_lookup_from(
                start, root, name,
                LOOKUP_FOLLOW | vfs_open_lookup_flags(&local_how), &target);
            if (ret < 0)
                goto out;
            if (!target.dentry || !target.dentry->d_inode) {
                ret = -ENOENT;
                goto out;
            }
            open_path = &target;
            dentry = target.dentry;
            dentry_owned_by_target = true;
        }
    }

    if ((local_how.flags & O_DIRECTORY) && !S_ISDIR(dentry->d_inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }
    if (vfs_open_flags_want_write((unsigned int)local_how.flags)) {
        ret = vfs_inode_get_write_access(dentry->d_inode);
        if (ret < 0)
            goto out;
    }

    if (!(local_how.flags & O_PATH) && (local_how.flags & O_TRUNC) &&
        ((local_how.flags & O_ACCMODE_FLAGS) == O_WRONLY ||
         (local_how.flags & O_ACCMODE_FLAGS) == O_RDWR) &&
        !S_ISDIR(dentry->d_inode->i_mode)) {
        ret = vfs_truncate_path(open_path, 0);
        if (ret < 0) {
            if (vfs_open_flags_want_write((unsigned int)local_how.flags))
                vfs_inode_put_write_access(dentry->d_inode);
            goto out;
        }
    }

    file = vfs_alloc_file(open_path, (unsigned int)local_how.flags);
    if (!file) {
        if (vfs_open_flags_want_write((unsigned int)local_how.flags))
            vfs_inode_put_write_access(dentry->d_inode);
        ret = -ENOMEM;
        goto out;
    }
    if (vfs_open_flags_want_write((unsigned int)local_how.flags))
        file->f_mode |= VFS_FMODE_WRITE_ACCESS;
    if (kernel)
        file->f_mode |= VFS_FMODE_KERNEL_IO;

    if (((how ? how->flags : 0) & O_TMPFILE) == O_TMPFILE) {
        if (!dentry->d_inode->i_op || !dentry->d_inode->i_op->tmpfile) {
            ret = -EOPNOTSUPP;
            vfs_file_put(file);
            goto out;
        }
        if (!kernel) {
            ret = vfs_inode_permission(dentry->d_inode,
                                       VFS_MAY_WRITE | VFS_MAY_EXEC);
            if (ret < 0) {
                vfs_file_put(file);
                goto out;
            }
        }
        ret = dentry->d_inode->i_op->tmpfile(dentry->d_inode, file,
                                             (umode_t)(how ? how->mode : 0600));
        if (ret < 0) {
            vfs_file_put(file);
            goto out;
        }
        file->f_flags &= ~O_DIRECTORY;
    } else if (!(local_how.flags & O_PATH) && dentry->d_inode->i_op &&
               dentry->d_inode->i_op->atomic_open) {
        ret = dentry->d_inode->i_op->atomic_open(dir, dentry, file,
                                                 (unsigned int)local_how.flags,
                                                 (umode_t)local_how.mode);
        if (ret < 0) {
            vfs_file_put(file);
            goto out;
        }
    } else if (!(local_how.flags & O_PATH) && file->f_op && file->f_op->open) {
        ret = file->f_op->open(dentry->d_inode, file);
        if (ret < 0) {
            vfs_file_put(file);
            goto out;
        }
    }

    if (created)
        notifyfs_queue_inode_event(dir, dentry->d_inode, last.name, IN_CREATE,
                                   0);

    if (!(local_how.flags & O_PATH))
        notifyfs_queue_inode_event(dentry->d_inode, dentry->d_inode, NULL,
                                   IN_OPEN, 0);
    *out = file;
    ret = 0;

out:
    vfs_path_put(&target);
    if (dentry && !dentry_owned_by_target)
        vfs_dput(dentry);
    vfs_path_put(&parent);
    vfs_qstr_destroy(&last);
    return ret;
}

int vfs_close_file(struct vfs_file *file) {
    return vfs_close_file_for_task(file, NULL);
}

int vfs_close_file_for_task(struct vfs_file *file, struct task *task) {
    uint64_t close_mask;

    if (!file)
        return -EBADF;
    if (task && file->f_inode) {
        int32_t owner_pid = (int32_t)task_effective_tgid(task);
        bool changed = false;

        spin_lock(&file->f_inode->file_locks_lock);
        struct llist_header *node = file->f_inode->file_locks.next;
        while (node != &file->f_inode->file_locks) {
            vfs_file_lock_t *lock = list_entry(node, vfs_file_lock_t, node);

            node = node->next;
            if (lock->ofd || lock->pid != owner_pid)
                continue;
            llist_delete(&lock->node);
            free(lock);
            changed = true;
        }
        if (changed) {
            node = file->f_inode->file_lock_waiters.next;
            while (node != &file->f_inode->file_lock_waiters) {
                vfs_file_lock_waiter_t *waiter =
                    list_entry(node, vfs_file_lock_waiter_t, node);

                node = node->next;
                if (waiter->task)
                    task_unblock(waiter->task, EOK);
            }
        }
        spin_unlock(&file->f_inode->file_locks_lock);
    }
    if (file->f_op && file->f_op->flush)
        file->f_op->flush(file);
    close_mask = ((file->f_flags & O_ACCMODE_FLAGS) == O_RDONLY)
                     ? IN_CLOSE_NOWRITE
                     : IN_CLOSE_WRITE;
    notifyfs_queue_inode_event(file->f_inode, file->f_inode, NULL, close_mask,
                               0);
    vfs_file_put(file);
    return 0;
}

ssize_t vfs_read_file(struct vfs_file *file, void *buf, size_t count,
                      loff_t *ppos) {
    ssize_t ret;
    loff_t pos;
    loff_t new_pos;

    /*
     * This is one of the final read forwarding points. Keep count == 0 on the
     * normal path and let file->read decide what the call means.
     */
    if (!file || !file->f_op || !file->f_op->read)
        return -EINVAL;

    bool lock_pos = !ppos && !(file->f_mode & VFS_FMODE_NO_POS_LOCK);

    if (lock_pos)
        vfs_file_pos_lock(file);
    pos = ppos ? *ppos : file->f_pos;

    new_pos = pos;
    ret = file->f_op->read(file, buf, count, &new_pos);

    if (ret >= 0) {
        if (ppos)
            *ppos = new_pos;
        else
            file->f_pos = new_pos;
    }

    if (lock_pos)
        vfs_file_pos_unlock(file);
    return ret;
}

ssize_t vfs_write_file(struct vfs_file *file, const void *buf, size_t count,
                       loff_t *ppos) {
    ssize_t ret;
    loff_t pos;
    loff_t new_pos;

    /*
     * Same policy for writes. A 0-byte request usually does not modify data,
     * but the backend still owns the exact observable behavior.
     */
    if (!file || !file->f_op || !file->f_op->write)
        return -EINVAL;

    bool lock_pos = !ppos && !(file->f_mode & VFS_FMODE_NO_POS_LOCK);

    if (lock_pos)
        vfs_file_pos_lock(file);
    pos = ppos ? *ppos : file->f_pos;
    if (!ppos && (file->f_flags & O_APPEND))
        pos = (loff_t)file->f_inode->i_size;

    new_pos = pos;
    ret = file->f_op->write(file, buf, count, &new_pos);

    if (ret >= 0) {
        if (ppos)
            *ppos = new_pos;
        else
            file->f_pos = new_pos;
        if (ret > 0)
            notifyfs_queue_inode_event(file->f_inode, file->f_inode, NULL,
                                       IN_MODIFY, 0);
    }

    if (lock_pos)
        vfs_file_pos_unlock(file);
    return ret;
}

ssize_t vfs_read_kernel_file(struct vfs_file *file, void *buf, size_t count,
                             loff_t *ppos) {
    ssize_t ret;
    loff_t pos;
    loff_t new_pos;

    if (!file)
        return -EINVAL;
    if (!file->f_op || !file->f_op->read)
        return -EINVAL;

    struct vfs_file kernel_file = *file;
    kernel_file.f_mode |= VFS_FMODE_KERNEL_IO;

    bool lock_pos = !ppos && !(file->f_mode & VFS_FMODE_NO_POS_LOCK);
    if (lock_pos)
        vfs_file_pos_lock(file);
    pos = ppos ? *ppos : file->f_pos;

    new_pos = pos;
    ret = kernel_file.f_op->read(&kernel_file, buf, count, &new_pos);
    if (ret >= 0) {
        if (ppos)
            *ppos = new_pos;
        else
            file->f_pos = new_pos;
    }

    if (lock_pos)
        vfs_file_pos_unlock(file);
    return ret;
}

ssize_t vfs_write_kernel_file(struct vfs_file *file, const void *buf,
                              size_t count, loff_t *ppos) {
    ssize_t ret;
    loff_t pos;
    loff_t new_pos;

    if (!file)
        return -EINVAL;
    if (!file->f_op || !file->f_op->write)
        return -EINVAL;

    struct vfs_file kernel_file = *file;
    kernel_file.f_mode |= VFS_FMODE_KERNEL_IO;

    bool lock_pos = !ppos && !(file->f_mode & VFS_FMODE_NO_POS_LOCK);
    if (lock_pos)
        vfs_file_pos_lock(file);
    pos = ppos ? *ppos : file->f_pos;
    if (!ppos && (file->f_flags & O_APPEND))
        pos = (loff_t)file->f_inode->i_size;

    new_pos = pos;
    ret = kernel_file.f_op->write(&kernel_file, buf, count, &new_pos);
    if (ret >= 0) {
        if (ppos)
            *ppos = new_pos;
        else
            file->f_pos = new_pos;
        if (ret > 0)
            notifyfs_queue_inode_event(file->f_inode, file->f_inode, NULL,
                                       IN_MODIFY, 0);
    }

    if (lock_pos)
        vfs_file_pos_unlock(file);
    return ret;
}

void vfs_file_pos_lock(struct vfs_file *file) {
    if (file)
        wait_mutex_lock(&file->f_pos_lock);
}

void vfs_file_pos_unlock(struct vfs_file *file) {
    if (file)
        wait_mutex_unlock(&file->f_pos_lock);
}

loff_t vfs_llseek_file(struct vfs_file *file, loff_t offset, int whence) {
    loff_t new_pos;

    if (!file)
        return -EBADF;
    if (file->f_op && file->f_op->llseek)
        return file->f_op->llseek(file, offset, whence);

    vfs_file_pos_lock(file);
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = file->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = (loff_t)file->f_inode->i_size + offset;
        break;
    default:
        vfs_file_pos_unlock(file);
        return -EINVAL;
    }

    if (new_pos < 0) {
        vfs_file_pos_unlock(file);
        return -EINVAL;
    }

    file->f_pos = new_pos;
    vfs_file_pos_unlock(file);
    return new_pos;
}

int vfs_iterate_dir(struct vfs_file *file, struct vfs_dir_context *ctx) {
    if (!file || !ctx)
        return -EINVAL;
    if (!file->f_op || !file->f_op->iterate_shared)
        return -ENOTDIR;
    return file->f_op->iterate_shared(file, ctx);
}

long vfs_ioctl_file(struct vfs_file *file, unsigned long cmd,
                    unsigned long arg) {
    if (!file)
        return -EBADF;
    if (!file->f_op || !file->f_op->unlocked_ioctl)
        return -ENOTTY;
    return file->f_op->unlocked_ioctl(file, cmd, arg);
}

int vfs_fsync_file(struct vfs_file *file) {
    if (!file || !file->f_inode)
        return -EBADF;
    return vfs_fsync_file_range(file, 0, (loff_t)file->f_inode->i_size, 0);
}

int vfs_fsync_file_range(struct vfs_file *file, loff_t start, loff_t end,
                         int datasync) {
    if (!file)
        return -EBADF;
    if (!file->f_op || !file->f_op->fsync)
        return 0;
    return file->f_op->fsync(file, start, end, datasync);
}

int vfs_poll(struct vfs_file *file, uint32_t events) {
    return vfs_poll_with_table(file, events, NULL);
}

int vfs_poll_with_table(struct vfs_file *file, uint32_t events,
                        struct vfs_poll_table *pt) {
    if (!file || !file->f_op || !file->f_op->poll)
        return -ENOTSUP;

    uint32_t request_events = events | EPOLLERR | EPOLLHUP | EPOLLNVAL;
    uint32_t wait_events = vfs_poll_expand_events(request_events);
    if (pt) {
        pt->events = wait_events;
        vfs_poll_wait(file, pt);
    }

    return (int)(vfs_poll_expand_events(file->f_op->poll(file, pt)) &
                 request_events);
}

int vfs_poll_wait_interruptible(struct vfs_file *file, uint32_t events) {
    while (true) {
        vfs_poll_wait_table_t table;
        vfs_poll_wait_table_init(&table, current_task);

        int polled = vfs_poll_with_table(file, events, &table.pt);
        int table_error = vfs_poll_wait_table_error(&table);
        if (table_error) {
            vfs_poll_wait_table_cleanup(&table);
            return table_error;
        }
        if (polled < 0) {
            vfs_poll_wait_table_cleanup(&table);
            return polled;
        }
        if (vfs_poll_expand_events((uint32_t)polled) &
            vfs_poll_expand_events(events)) {
            vfs_poll_wait_table_cleanup(&table);
            return EOK;
        }

        if (task_signal_has_deliverable(current_task)) {
            vfs_poll_wait_table_cleanup(&table);
            return -EINTR;
        }
        if (vfs_poll_wait_table_seq_changed(&table)) {
            vfs_poll_wait_table_cleanup(&table);
            continue;
        }

        int reason =
            task_block(current_task, TASK_BLOCKING, -1, "vfs_poll_wait");
        vfs_poll_wait_table_cleanup(&table);

        if (reason == ETIMEDOUT)
            return -ETIMEDOUT;
        if (reason < 0)
            return reason;
        if (reason != EOK && task_signal_has_deliverable(current_task))
            return -EINTR;
    }
}

int vfs_truncate_path(const struct vfs_path *path, uint64_t size) {
    struct vfs_kstat stat;
    struct vfs_inode *inode;
    int ret;

    if (!path || !path->dentry || !path->dentry->d_inode)
        return -ENOENT;

    inode = path->dentry->d_inode;
    if (!inode->i_op || !inode->i_op->setattr)
        return -EOPNOTSUPP;

    vfs_fill_generic_kstat(path, &stat);
    stat.size = size;

    ret = inode->i_op->setattr(path->dentry, &stat);
    if (ret == 0) {
        notifyfs_queue_inode_event(inode, inode, NULL, IN_MODIFY, 0);
    }
    return ret;
}
