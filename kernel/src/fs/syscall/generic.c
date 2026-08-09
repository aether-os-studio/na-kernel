#include <init/callbacks.h>
#include <fs/fs_syscall.h>
#include <boot/boot.h>
#include <net/socket.h>
#include <net/net_syscall.h>
#include <fs/pipe.h>
#include <fs/vfs/notify.h>
#include <mm/cache.h>
#include <task/ns.h>
#include <task/task_syscall.h>

#define FIONBIO_INTERNAL_DISABLE ((ssize_t) - 1)
#define FIONBIO_INTERNAL_ENABLE ((ssize_t) - 2)

#ifndef UTIME_NOW
#define UTIME_NOW ((1L << 30) - 1L)
#endif

#ifndef UTIME_OMIT
#define UTIME_OMIT ((1L << 30) - 2L)
#endif

#define UIO_MAXIOV 1024
#define MAX_RW_COUNT ((INT64_MAX) & ~(4096 - 1))
#define RWF_HIPRI 0x00000001
#define RWF_DSYNC 0x00000002
#define RWF_SYNC 0x00000004
#define RWF_NOWAIT 0x00000008
#define RWF_APPEND 0x00000010
#define RWF_SUPPORTED                                                          \
    (RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT | RWF_APPEND)
#define SPLICE_F_MOVE 0x01
#define SPLICE_F_NONBLOCK 0x02
#define SPLICE_F_MORE 0x04
#define SPLICE_F_GIFT 0x08
#define SPLICE_F_SUPPORTED                                                     \
    (SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_MORE | SPLICE_F_GIFT)
#define POSIX_FADV_NORMAL 0
#define POSIX_FADV_RANDOM 1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED 3
#define POSIX_FADV_DONTNEED 4
#define POSIX_FADV_NOREUSE 5

static uint64_t do_unlink(const char *name);
static volatile uint64_t tmpfile_seq = 1;

#define GENERIC_MS_TO_VFS_MASK                                                 \
    (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOSYMFOLLOW)
#define GENERIC_MS_REMOUNT_IGNORED_MASK                                        \
    (MS_SILENT | MS_SYNCHRONOUS | MS_MANDLOCK | MS_DIRSYNC | MS_NOATIME |      \
     MS_NODIRATIME | MS_RELATIME | MS_STRICTATIME | MS_I_VERSION |             \
     MS_LAZYTIME)
#define GENERIC_MS_REMOUNT_ALLOWED_MASK                                        \
    (MS_REMOUNT | MS_BIND | GENERIC_MS_TO_VFS_MASK |                           \
     GENERIC_MS_REMOUNT_IGNORED_MASK)

typedef struct linux_file_handle_prefix {
    unsigned int handle_bytes;
    int handle_type;
} linux_file_handle_prefix_t;

static const char *generic_file_type_name(uint32_t type) {
    if (type & file_socket)
        return "socket";
    if (type & file_fifo)
        return "fifo";
    if (type & file_stream)
        return "stream";
    if (type & file_dir)
        return "dir";
    if (type & file_symlink)
        return "symlink";
    if (type & file_block)
        return "block";
    if (type & file_none)
        return "file";
    return "unknown";
}

static int
generic_copy_file_handle_prefix_from_user(const struct file_handle *user_handle,
                                          linux_file_handle_prefix_t *prefix) {
    if (!user_handle || !prefix)
        return -EINVAL;
    if (check_user_overflow((uint64_t)user_handle, sizeof(*prefix)))
        return -EFAULT;
    if (copy_from_user(prefix, user_handle, sizeof(*prefix)))
        return -EFAULT;
    return 0;
}

static int generic_copy_file_handle_prefix_to_user(
    struct file_handle *user_handle, const linux_file_handle_prefix_t *prefix) {
    if (!user_handle || !prefix)
        return -EINVAL;
    if (check_user_overflow((uint64_t)user_handle, sizeof(*prefix)))
        return -EFAULT;
    if (copy_to_user(user_handle, prefix, sizeof(*prefix)))
        return -EFAULT;
    return 0;
}

static int generic_copy_handle_bytes_from_user(const struct file_handle *handle,
                                               void *buf, size_t size) {
    const void *src;

    if (!handle || !buf)
        return -EINVAL;
    src = (const void *)((uint64_t)handle + sizeof(linux_file_handle_prefix_t));
    if (check_user_overflow((uint64_t)src, size))
        return -EFAULT;
    if (copy_from_user(buf, src, size))
        return -EFAULT;
    return 0;
}

static int generic_copy_handle_bytes_to_user(struct file_handle *handle,
                                             const void *buf, size_t size) {
    void *dst;

    if (!handle || !buf)
        return -EINVAL;
    dst = (void *)((uint64_t)handle + sizeof(linux_file_handle_prefix_t));
    if (check_user_overflow((uint64_t)dst, size))
        return -EFAULT;
    if (copy_to_user(dst, buf, size))
        return -EFAULT;
    return 0;
}

static struct vfs_inode *generic_find_inode_by_ino(struct vfs_super_block *sb,
                                                   uint64_t ino) {
    struct vfs_inode *inode, *tmp;
    struct vfs_inode *found = NULL;

    if (!sb)
        return NULL;

    spin_lock(&sb->s_inode_lock);
    if (!llist_empty(&sb->s_inodes)) {
        llist_for_each(inode, tmp, &sb->s_inodes, i_sb_list) {
            if (inode->i_ino != ino)
                continue;
            found = vfs_igrab(inode);
            break;
        }
    }
    spin_unlock(&sb->s_inode_lock);
    return found;
}

static struct vfs_dentry *generic_inode_alias(struct vfs_inode *inode) {
    struct vfs_dentry *dentry;

    if (!inode || llist_empty(&inode->i_dentry_aliases))
        return NULL;

    dentry =
        list_entry(inode->i_dentry_aliases.next, struct vfs_dentry, d_alias);
    return vfs_dget(dentry);
}

static uint64_t fd_open_file_flags(uint64_t open_flags) {
    return (open_flags & O_ACCMODE_FLAGS) | (open_flags & O_STATUS_FLAGS);
}

static unsigned long generic_mount_flags_to_vfs(uint64_t flags) {
    unsigned long mnt_flags = 0;

    if (flags & MS_RDONLY)
        mnt_flags |= VFS_MNT_READONLY;
    if (flags & MS_NOSUID)
        mnt_flags |= VFS_MNT_NOSUID;
    if (flags & MS_NODEV)
        mnt_flags |= VFS_MNT_NODEV;
    if (flags & MS_NOEXEC)
        mnt_flags |= VFS_MNT_NOEXEC;
    if (flags & MS_NOSYMFOLLOW)
        mnt_flags |= VFS_MNT_NOSYMFOLLOW;

    return mnt_flags;
}

static bool file_lock_ranges_overlap(uint64_t start1, uint64_t end1,
                                     uint64_t start2, uint64_t end2) {
    if (end1 != UINT64_MAX && end1 <= start2)
        return false;
    if (end2 != UINT64_MAX && end2 <= start1)
        return false;
    return true;
}

static bool file_lock_ranges_touch_or_overlap(uint64_t start1, uint64_t end1,
                                              uint64_t start2, uint64_t end2) {
    if (file_lock_ranges_overlap(start1, end1, start2, end2))
        return true;
    if (end1 != UINT64_MAX && end1 == start2)
        return true;
    return end2 != UINT64_MAX && end2 == start1;
}

static int lseek_add_offset(int64_t base, int64_t delta, uint64_t *result_out) {
    if (!result_out)
        return -EINVAL;

    if ((delta > 0 && base > INT64_MAX - delta) ||
        (delta < 0 && base < INT64_MIN - delta)) {
        return -EOVERFLOW;
    }

    int64_t result = base + delta;
    if (result < 0)
        return -EINVAL;

    *result_out = (uint64_t)result;
    return 0;
}

static int file_lock_normalize(fd_t *fd, const flock_t *lock,
                               uint64_t *start_out, uint64_t *end_out) {
    if (!fd || !fd->f_inode || !lock || !start_out || !end_out)
        return -EINVAL;
    if (lock->l_len < 0)
        return -EINVAL;

    int64_t base = 0;
    switch (lock->l_whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (int64_t)fd_get_offset(fd);
        break;
    case SEEK_END:
        base = (int64_t)fd->f_inode->i_size;
        break;
    default:
        return -EINVAL;
    }

    if ((lock->l_start > 0 && base > INT64_MAX - lock->l_start) ||
        (lock->l_start < 0 && base < INT64_MIN - lock->l_start)) {
        return -EINVAL;
    }

    int64_t start_signed = base + lock->l_start;
    if (start_signed < 0)
        return -EINVAL;

    uint64_t start = (uint64_t)start_signed;
    uint64_t end = UINT64_MAX;
    if (lock->l_len > 0) {
        if ((uint64_t)lock->l_len > UINT64_MAX - start)
            return -EINVAL;
        end = start + (uint64_t)lock->l_len;
        if (end <= start)
            return -EINVAL;
    }

    *start_out = start;
    *end_out = end;
    return 0;
}

static bool file_lock_same_owner(const vfs_file_lock_t *lock, uintptr_t owner,
                                 bool ofd) {
    return lock && lock->ofd == ofd && lock->owner == owner;
}

static vfs_file_lock_t *file_lock_find_conflict(vfs_node_t *node,
                                                uint64_t start, uint64_t end,
                                                int16_t type, uintptr_t owner,
                                                bool ofd) {
    if (!node)
        return NULL;

    vfs_file_lock_t *lock = NULL, *tmp = NULL;
    llist_for_each(lock, tmp, &node->file_locks, node) {
        if (file_lock_same_owner(lock, owner, ofd))
            continue;
        if (!file_lock_ranges_overlap(lock->start, lock->end, start, end))
            continue;
        if (lock->type == F_WRLCK || type == F_WRLCK)
            return lock;
    }

    return NULL;
}

static bool file_lock_waiter_reaches_owner(vfs_node_t *node, uintptr_t from,
                                           bool ofd, uintptr_t target,
                                           unsigned int depth) {
    if (!node)
        return false;
    if (from == target)
        return true;
    if (depth > 64)
        return false;

    vfs_file_lock_waiter_t *waiter = NULL, *tmp = NULL;
    llist_for_each(waiter, tmp, &node->file_lock_waiters, node) {
        if (waiter->ofd != ofd || waiter->owner != from)
            continue;
        if (waiter->blocking_owner == target)
            return true;
        if (file_lock_waiter_reaches_owner(node, waiter->blocking_owner, ofd,
                                           target, depth + 1))
            return true;
    }

    return false;
}

static bool file_lock_would_deadlock(vfs_node_t *node, uintptr_t owner,
                                     const vfs_file_lock_t *conflict,
                                     bool ofd) {
    if (!node || !conflict || ofd || conflict->ofd)
        return false;
    return file_lock_waiter_reaches_owner(node, conflict->owner, ofd, owner, 0);
}

static void file_lock_waiter_remove(vfs_node_t *node,
                                    vfs_file_lock_waiter_t *waiter) {
    if (!node || !waiter)
        return;

    spin_lock(&node->file_locks_lock);
    if (!llist_empty(&waiter->node))
        llist_delete(&waiter->node);
    spin_unlock(&node->file_locks_lock);
}

static void file_lock_wake_waiters_locked(vfs_node_t *node) {
    vfs_file_lock_waiter_t *waiter = NULL, *tmp = NULL;

    if (!node)
        return;

    llist_for_each(waiter, tmp, &node->file_lock_waiters, node) {
        if (waiter->task)
            task_unblock(waiter->task, EOK);
    }
}

static int file_lock_unlock_owner_range(vfs_node_t *node, uintptr_t owner,
                                        bool ofd, uint64_t start,
                                        uint64_t end) {
    if (!node)
        return -EINVAL;

    bool changed = false;
    vfs_file_lock_t *lock = NULL, *tmp = NULL;
    llist_for_each(lock, tmp, &node->file_locks, node) {
        if (!file_lock_same_owner(lock, owner, ofd))
            continue;
        if (!file_lock_ranges_overlap(lock->start, lock->end, start, end))
            continue;

        if (start <= lock->start && (end == UINT64_MAX || end >= lock->end)) {
            llist_delete(&lock->node);
            free(lock);
            changed = true;
            continue;
        }

        if (start <= lock->start) {
            lock->start = end;
            changed = true;
            continue;
        }

        if (end == UINT64_MAX || end >= lock->end) {
            lock->end = start;
            changed = true;
            continue;
        }

        vfs_file_lock_t *split = calloc(1, sizeof(vfs_file_lock_t));
        if (!split)
            return -ENOMEM;
        llist_init_head(&split->node);
        split->start = end;
        split->end = lock->end;
        split->owner = lock->owner;
        split->pid = lock->pid;
        split->type = lock->type;
        split->ofd = lock->ofd;
        lock->end = start;
        llist_append(&node->file_locks, &split->node);
        changed = true;
    }

    if (changed)
        file_lock_wake_waiters_locked(node);
    return 0;
}

static int file_lock_getlk(fd_t *fd, flock_t *lock, bool ofd) {
    if (lock->l_type != F_RDLCK && lock->l_type != F_WRLCK)
        return -EINVAL;
    if (ofd && lock->l_pid != 0)
        return -EINVAL;

    uint64_t start = 0, end = 0;
    int ret = file_lock_normalize(fd, lock, &start, &end);
    if (ret < 0)
        return ret;

    vfs_node_t *node = fd->f_inode;
    uint64_t tgid = task_effective_tgid(current_task);
    uintptr_t owner = ofd ? (uintptr_t)fd : (uintptr_t)(uint32_t)tgid;
    spin_lock(&node->file_locks_lock);
    vfs_file_lock_t *conflict =
        file_lock_find_conflict(node, start, end, lock->l_type, owner, ofd);
    if (!conflict) {
        lock->l_type = F_UNLCK;
        lock->l_pid = 0;
        spin_unlock(&node->file_locks_lock);
        return 0;
    }

    lock->l_type = conflict->type;
    lock->l_whence = SEEK_SET;
    lock->l_start = (int64_t)conflict->start;
    lock->l_len = conflict->end == UINT64_MAX
                      ? 0
                      : (int64_t)(conflict->end - conflict->start);
    lock->l_pid = conflict->ofd ? -1 : conflict->pid;
    spin_unlock(&node->file_locks_lock);
    return 0;
}

static int file_lock_setlk(fd_t *fd, const flock_t *req, bool wait, bool ofd) {
    uint64_t start = 0, end = 0;
    int ret = file_lock_normalize(fd, req, &start, &end);
    if (ret < 0)
        return ret;
    if (ofd && req->l_pid != 0)
        return -EINVAL;

    vfs_node_t *node = fd->f_inode;
    uint64_t tgid = task_effective_tgid(current_task);
    int32_t pid = (int32_t)tgid;
    uintptr_t owner = ofd ? (uintptr_t)fd : (uintptr_t)(uint32_t)pid;
    vfs_file_lock_waiter_t waiter;

    memset(&waiter, 0, sizeof(waiter));
    llist_init_head(&waiter.node);
    waiter.owner = owner;
    waiter.task = current_task;
    waiter.ofd = ofd;

    for (;;) {
        spin_lock(&node->file_locks_lock);

        if (req->l_type == F_UNLCK) {
            ret = file_lock_unlock_owner_range(node, owner, ofd, start, end);
            spin_unlock(&node->file_locks_lock);
            return ret;
        }

        vfs_file_lock_t *conflict =
            file_lock_find_conflict(node, start, end, req->l_type, owner, ofd);
        if (!conflict) {
            ret = file_lock_unlock_owner_range(node, owner, ofd, start, end);
            if (ret < 0) {
                spin_unlock(&node->file_locks_lock);
                file_lock_waiter_remove(node, &waiter);
                return ret;
            }
            if (!llist_empty(&waiter.node))
                llist_delete(&waiter.node);

            uint64_t merged_start = start;
            uint64_t merged_end = end;
            vfs_file_lock_t *lock = NULL, *tmp = NULL;
            llist_for_each(lock, tmp, &node->file_locks, node) {
                if (!file_lock_same_owner(lock, owner, ofd) ||
                    lock->type != req->l_type)
                    continue;
                if (!file_lock_ranges_touch_or_overlap(
                        lock->start, lock->end, merged_start, merged_end))
                    continue;
                if (lock->start < merged_start)
                    merged_start = lock->start;
                if (lock->end > merged_end)
                    merged_end = lock->end;
                llist_delete(&lock->node);
                free(lock);
            }

            vfs_file_lock_t *new_lock = calloc(1, sizeof(vfs_file_lock_t));
            if (!new_lock) {
                spin_unlock(&node->file_locks_lock);
                file_lock_waiter_remove(node, &waiter);
                return -ENOMEM;
            }

            llist_init_head(&new_lock->node);
            new_lock->start = merged_start;
            new_lock->end = merged_end;
            new_lock->owner = owner;
            new_lock->pid = pid;
            new_lock->type = req->l_type;
            new_lock->ofd = ofd;
            llist_append(&node->file_locks, &new_lock->node);
            spin_unlock(&node->file_locks_lock);
            return 0;
        }

        if (!wait) {
            spin_unlock(&node->file_locks_lock);
            return -EAGAIN;
        }

        if (file_lock_would_deadlock(node, owner, conflict, ofd)) {
            if (!llist_empty(&waiter.node))
                llist_delete(&waiter.node);
            spin_unlock(&node->file_locks_lock);
            return -EDEADLK;
        }

        waiter.blocking_owner = conflict->owner;
        if (llist_empty(&waiter.node))
            llist_append(&node->file_lock_waiters, &waiter.node);

        spin_unlock(&node->file_locks_lock);

        if (task_signal_has_deliverable(current_task)) {
            file_lock_waiter_remove(node, &waiter);
            return ret;
        }

        arch_enable_interrupt();
        arch_wait_for_interrupt();
        arch_disable_interrupt();
    }
}

static int generic_get_fd_path(int fd, struct vfs_path *path) {
    struct vfs_file *file;
    int ret;

    if (!path)
        return -EINVAL;
    memset(path, 0, sizeof(*path));

    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;

    ret = mountfd_get_path(file, path);
    if (ret == -EINVAL)
        ret = fsfd_mount_get_path(file, path);
    if (ret == -EINVAL) {
        ret = vfs_path_copy(path, &file->f_path) ? 0 : -ENOENT;
    }
    vfs_file_put(file);
    return ret;
}

static inline int rw_validate_user_buffer(const void *buf, uint64_t len) {
    /*
     * Leave zero-length requests alone here. For Linux ABI compatibility, a
     * 0-byte read/write should still be able to reach the underlying object,
     * so this helper only validates the user buffer when the call would
     * actually dereference it.
     */
    if (!len)
        return 0;
    if (!buf || check_user_overflow((uint64_t)buf, len))
        return -EFAULT;
    return 0;
}

static uint64_t rw_validate_iovecs(const struct iovec *kiov, uint64_t count,
                                   size_t *total_len) {
    size_t total = 0;

    if (!kiov && count)
        return (uint64_t)-EFAULT;

    for (uint64_t i = 0; i < count; i++) {
        if (kiov[i].len == 0)
            continue;
        if (kiov[i].len > MAX_RW_COUNT)
            return (uint64_t)-EINVAL;
        if (rw_validate_user_buffer(kiov[i].iov_base, kiov[i].len) < 0)
            return (uint64_t)-EFAULT;
        if (kiov[i].len > (uint64_t)MAX_RW_COUNT - total)
            total = MAX_RW_COUNT;
        else
            total += kiov[i].len;
    }

    if (total_len)
        *total_len = total;
    return 0;
}

static inline loff_t rw_pos_from_hilo(uint64_t pos_l, uint64_t pos_h) {
    return (loff_t)((pos_l & 0xffffffffULL) | (pos_h << 32));
}

static void generic_fill_stat_from_kstat(struct stat *buf,
                                         const struct vfs_kstat *stat) {
    if (!buf || !stat)
        return;

    memset(buf, 0, sizeof(*buf));
    buf->st_dev = (long)stat->dev;
    buf->st_ino = (unsigned long)stat->ino;
    buf->st_nlink = (unsigned long)stat->nlink;
    buf->st_mode = (int)stat->mode;
    buf->st_uid = (int)stat->uid;
    buf->st_gid = (int)stat->gid;
    buf->st_rdev = (long)stat->rdev;
    buf->st_blksize = (long)stat->blksize;
    buf->st_size = (long long)stat->size;
    buf->st_blocks = (unsigned long)stat->blocks;
#if defined(__riscv__) || defined(__loongarch64__)
    buf->st_atime = stat->atime.sec;
    buf->st_atime_nsec = (unsigned long)stat->atime.nsec;
    buf->st_mtime = stat->mtime.sec;
    buf->st_mtime_nsec = (unsigned long)stat->mtime.nsec;
    buf->st_ctime = stat->ctime.sec;
    buf->st_ctime_nsec = (unsigned long)stat->ctime.nsec;
#else
    buf->st_atim.tv_sec = stat->atime.sec;
    buf->st_atim.tv_nsec = (long)stat->atime.nsec;
    buf->st_mtim.tv_sec = stat->mtime.sec;
    buf->st_mtim.tv_nsec = (long)stat->mtime.nsec;
    buf->st_ctim.tv_sec = stat->ctime.sec;
    buf->st_ctim.tv_nsec = (long)stat->ctime.nsec;
#endif
}

static void generic_fill_statx_from_kstat(struct statx *buf,
                                          const struct vfs_kstat *stat,
                                          const struct vfs_path *path,
                                          uint64_t mask) {
    if (!buf || !stat)
        return;

    memset(buf, 0, sizeof(*buf));
    buf->stx_mask = STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID;
    buf->stx_blksize = stat->blksize;
    buf->stx_nlink = stat->nlink;
    buf->stx_uid = stat->uid;
    buf->stx_gid = stat->gid;
    buf->stx_mode = stat->mode;
    buf->stx_ino = stat->ino;
    buf->stx_size = stat->size;
    buf->stx_blocks = stat->blocks;
    buf->stx_attributes = 0;
    buf->stx_attributes_mask = 0;
    if (path) {
        buf->stx_attributes_mask |= STATX_ATTR_MOUNT_ROOT;
        if (path->mnt && path->dentry && path->mnt->mnt_root == path->dentry)
            buf->stx_attributes |= STATX_ATTR_MOUNT_ROOT;
    }
    buf->stx_dev_major = (uint32_t)((stat->dev >> 8) & 0xFF);
    buf->stx_dev_minor = (uint32_t)(stat->dev & 0xFF);
    buf->stx_rdev_major = (uint32_t)((stat->rdev >> 8) & 0xFF);
    buf->stx_rdev_minor = (uint32_t)(stat->rdev & 0xFF);
    buf->stx_mnt_id = stat->mnt_id;
    buf->stx_atime.tv_sec = stat->atime.sec;
    buf->stx_atime.tv_nsec = stat->atime.nsec;
    buf->stx_btime.tv_sec = stat->btime.sec;
    buf->stx_btime.tv_nsec = stat->btime.nsec;
    buf->stx_ctime.tv_sec = stat->ctime.sec;
    buf->stx_ctime.tv_nsec = stat->ctime.nsec;
    buf->stx_mtime.tv_sec = stat->mtime.sec;
    buf->stx_mtime.tv_nsec = stat->mtime.nsec;
}

static int generic_setattr_path(const struct vfs_path *path, bool set_mode,
                                umode_t mode, bool set_uid, uid32_t uid,
                                bool set_gid, gid32_t gid, bool set_size,
                                uint64_t size) {
    struct vfs_kstat stat;
    struct vfs_inode *inode;

    if (!path || !path->dentry || !path->dentry->d_inode)
        return -ENOENT;

    inode = path->dentry->d_inode;
    if (!inode->i_op || !inode->i_op->setattr)
        return -EOPNOTSUPP;

    vfs_fill_generic_kstat(path, &stat);
    stat.mask = 0;
    if (set_mode) {
        stat.mode = (stat.mode & S_IFMT) | (mode & 07777);
        stat.mask |= STATX_MODE;
    }
    if (set_uid) {
        stat.uid = uid;
        stat.mask |= STATX_UID;
    }
    if (set_gid) {
        stat.gid = gid;
        stat.mask |= STATX_GID;
    }
    if (set_size) {
        stat.size = size;
        stat.mask |= STATX_SIZE;
    }
    return inode->i_op->setattr(path->dentry, &stat);
}

static struct vfs_timespec64 generic_realtime_now(void) {
    uint64_t now = boot_get_boottime() * 1000000000ULL + nano_time();

    return (struct vfs_timespec64){
        .sec = (int64_t)(now / 1000000000ULL),
        .nsec = (uint32_t)(now % 1000000000ULL),
    };
}

static bool generic_inode_owner_or_root(const struct vfs_inode *inode) {
    uid32_t fsuid;

    if (!inode)
        return false;

    fsuid = vfs_current_fsuid();
    return fsuid == 0 || fsuid == inode->i_uid;
}

static int generic_utimens_path(const struct vfs_path *path,
                                const struct timespec *times, bool set_atime,
                                bool set_mtime, bool all_now) {
    struct vfs_inode *inode;
    struct vfs_kstat stat;
    struct vfs_timespec64 now;
    int ret;

    if (!set_atime && !set_mtime)
        return 0;
    if (!path || !path->dentry || !path->dentry->d_inode)
        return -ENOENT;

    /*
     * Read-only mount enforcement must be shared by all metadata mutation
     * paths.  Enforcing it only here made utimensat fail on paths where the
     * same mount still allowed create, rename, and truncate operations.
     */

    inode = path->dentry->d_inode;
    if (all_now) {
        ret = vfs_inode_permission(inode, VFS_MAY_WRITE);
        if (ret < 0 && !generic_inode_owner_or_root(inode))
            return -EACCES;
    } else if (!generic_inode_owner_or_root(inode)) {
        return -EPERM;
    }

    now = generic_realtime_now();
    vfs_fill_generic_kstat(path, &stat);
    stat.mask = STATX_CTIME;
    stat.ctime = now;
    if (set_atime) {
        if (!times || times[0].tv_nsec == UTIME_NOW) {
            stat.atime = now;
        } else {
            stat.atime.sec = times[0].tv_sec;
            stat.atime.nsec = (uint32_t)times[0].tv_nsec;
        }
        stat.mask |= STATX_ATIME;
    }
    if (set_mtime) {
        if (!times || times[1].tv_nsec == UTIME_NOW) {
            stat.mtime = now;
        } else {
            stat.mtime.sec = times[1].tv_sec;
            stat.mtime.nsec = (uint32_t)times[1].tv_nsec;
        }
        stat.mask |= STATX_MTIME;
    }

    if (inode->i_op && inode->i_op->setattr) {
        ret = inode->i_op->setattr(path->dentry, &stat);
        if (ret < 0)
            return ret;
    }

    spin_lock(&inode->i_lock);
    if (set_atime)
        inode->i_atime = stat.atime;
    if (set_mtime)
        inode->i_mtime = stat.mtime;
    inode->i_ctime = stat.ctime;
    spin_unlock(&inode->i_lock);

    notifyfs_queue_inode_event(inode, inode, NULL, IN_ATTRIB, 0);
    return 0;
}

static int generic_get_empty_path(int dfd, struct vfs_path *path) {
    const struct vfs_path *pwd;

    if (!path)
        return -EINVAL;

    if (dfd != AT_FDCWD)
        return generic_get_fd_path(dfd, path);

    pwd = task_fs_pwd_path(current_task);
    if (!pwd || !pwd->mnt || !pwd->dentry)
        return -ENOENT;

    return vfs_path_copy(path, pwd) ? 0 : -ENOENT;
}

static int generic_do_utimensat(int dfd, const char *pathname,
                                const struct timespec *times, int flags) {
    struct vfs_path path = {0};
    char pathname_buf[512] = {0};
    bool set_atime = true;
    bool set_mtime = true;
    bool all_now = !times;
    int ret;

    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;

    if (times) {
        for (int i = 0; i < 2; i++) {
            long nsec = times[i].tv_nsec;

            if (nsec == UTIME_OMIT) {
                if (i == 0)
                    set_atime = false;
                else
                    set_mtime = false;
                continue;
            }

            if (nsec == UTIME_NOW)
                continue;

            if (times[i].tv_sec < 0 || nsec < 0 || nsec >= 1000000000L)
                return -EINVAL;
        }

        all_now =
            times[0].tv_nsec == UTIME_NOW && times[1].tv_nsec == UTIME_NOW;
        if (!set_atime && !set_mtime)
            return 0;
    }

    if (!pathname) {
        if (dfd == AT_FDCWD)
            return -EFAULT;
        if (flags & AT_SYMLINK_NOFOLLOW)
            return -EINVAL;
        ret = generic_get_fd_path(dfd, &path);
    } else {
        if (copy_from_user_str(pathname_buf, pathname, sizeof(pathname_buf)))
            return -EFAULT;

        if ((flags & AT_EMPTY_PATH) && pathname_buf[0] == '\0') {
            ret = generic_get_empty_path(dfd, &path);
        } else {
            unsigned int lookup_flags =
                (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;
            ret = vfs_filename_lookup(dfd, pathname_buf, lookup_flags, &path);
        }
    }
    if (ret < 0)
        return ret;

    ret = generic_utimens_path(&path, times, set_atime, set_mtime, all_now);
    vfs_path_put(&path);
    return ret;
}

static int generic_chown_path(const struct vfs_path *path, uint64_t uid,
                              uint64_t gid) {
    struct vfs_inode *inode;
    uid32_t new_uid = (uid32_t)uid;
    gid32_t new_gid = (gid32_t)gid;
    uid32_t fsuid;
    bool set_uid = new_uid != (uid32_t)-1;
    bool set_gid = new_gid != (gid32_t)-1;

    if (!path || !path->dentry || !path->dentry->d_inode)
        return -ENOENT;

    inode = path->dentry->d_inode;

    if (!set_uid && !set_gid)
        return 0;

    /*
     * uid_t and gid_t are 32-bit Linux syscall arguments.  In particular,
     * userspace passes (uid_t)-1 as 0xffffffff (normally zero-extended in the
     * syscall register), not as UINT64_MAX.  Treating only UINT64_MAX as the
     * sentinel stored uid 4294967295 in the inode instead of leaving the
     * owner unchanged.
     *
     * Without capabilities/user namespaces, fsuid 0 is the privileged case.
     * An unprivileged owner may keep the current uid and may select one of its
     * own groups, matching Linux chown permission semantics.
     */
    fsuid = vfs_current_fsuid();
    if (fsuid != 0) {
        if (fsuid != inode->i_uid)
            return -EPERM;
        if (set_uid && new_uid != inode->i_uid)
            return -EPERM;
        if (set_gid && new_gid != vfs_current_fsgid() &&
            !task_in_supplementary_group(current_task, new_gid))
            return -EPERM;
    }

    return generic_setattr_path(path, false, 0, set_uid, new_uid, set_gid,
                                new_gid, false, 0);
}

static int generic_link_pathat(const struct vfs_path *old_path, int newdfd,
                               const char *newname) {
    struct vfs_path new_parent = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *existing = NULL;
    struct vfs_dentry *new_dentry = NULL;
    struct vfs_inode *new_dir;
    int ret;

    if (!old_path || !old_path->dentry || !old_path->dentry->d_inode)
        return -ENOENT;

    ret = vfs_path_parent_lookup(newdfd, newname, LOOKUP_PARENT, &new_parent,
                                 &last, NULL);
    if (ret < 0)
        return ret;

    new_dir = new_parent.dentry ? new_parent.dentry->d_inode : NULL;
    if (!new_dir || !new_dir->i_op || !new_dir->i_op->link) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    ret = vfs_inode_permission(new_dir, VFS_MAY_WRITE | VFS_MAY_EXEC);
    if (ret < 0)
        goto out;

    existing = vfs_d_lookup(new_parent.dentry, &last);
    if (existing) {
        if (existing->d_inode) {
            ret = -EEXIST;
            goto out;
        }
        new_dentry = existing;
        existing = NULL;
    } else {
        new_dentry =
            vfs_d_alloc(new_parent.dentry->d_sb, new_parent.dentry, &last);
        if (!new_dentry) {
            ret = -ENOMEM;
            goto out;
        }
    }

    ret = new_dir->i_op->link(old_path->dentry, new_dir, new_dentry);
    if (ret == 0 && !(new_dentry->d_flags & VFS_DENTRY_HASHED))
        vfs_d_add(new_parent.dentry, new_dentry);

out:
    if (existing)
        vfs_dput(existing);
    if (new_dentry)
        vfs_dput(new_dentry);
    vfs_path_put(&new_parent);
    vfs_qstr_destroy(&last);
    return ret;
}

uint64_t sys_mount(char *dev_name, char *dir_name, char *type_user,
                   uint64_t flags, void *data) {
    unsigned long mnt_flags = generic_mount_flags_to_vfs(flags);
    unsigned long propagation_flags =
        flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE);
    char devname[128] = "none";
    char dirname[512] = {0};
    char type[128] = "tmpfs";
    char data_string[512] = {0};
    void *mount_data = data;

    if (copy_from_user_str(dirname, dir_name, sizeof(dirname)))
        return (uint64_t)-EFAULT;

    if (flags & MS_MOVE) {
        if (!dev_name)
            return (uint64_t)-EINVAL;
        if (copy_from_user_str(devname, dev_name, sizeof(devname)))
            return (uint64_t)-EFAULT;
        return (uint64_t)vfs_do_move_mount(AT_FDCWD, devname, AT_FDCWD,
                                           dirname);
    }

    if (flags & MS_REMOUNT) {
        if (propagation_flags)
            return (uint64_t)-EINVAL;
        if (flags & ~GENERIC_MS_REMOUNT_ALLOWED_MASK)
            return (uint64_t)-EOPNOTSUPP;
        return (uint64_t)vfs_do_remount(AT_FDCWD, dirname, mnt_flags);
    }

    if (propagation_flags) {
        struct vfs_path target = {0};
        struct vfs_mount *mnt = NULL;
        int ret;

        if ((propagation_flags & (propagation_flags - 1)) != 0)
            return (uint64_t)-EINVAL;

        ret = vfs_filename_lookup(AT_FDCWD, dirname, LOOKUP_FOLLOW, &target);
        if (ret < 0)
            return (uint64_t)ret;

        mnt = vfs_path_mount(&target);
        if (!mnt && target.mnt)
            mnt = vfs_mntget(target.mnt);
        if (!mnt) {
            vfs_path_put(&target);
            return (uint64_t)-EINVAL;
        }

        ret = vfs_mount_set_propagation(mnt, propagation_flags,
                                        (flags & MS_REC) != 0);
        vfs_mntput(mnt);
        vfs_path_put(&target);
        return (uint64_t)ret;
    }

    if (flags & MS_BIND) {
        if (!dev_name)
            return (uint64_t)-EINVAL;
        if (flags & ~(MS_BIND | MS_REC))
            return (uint64_t)-EOPNOTSUPP;
        if (copy_from_user_str(devname, dev_name, sizeof(devname)))
            return (uint64_t)-EFAULT;
        return (uint64_t)vfs_do_bind_mount(AT_FDCWD, devname, AT_FDCWD, dirname,
                                           (flags & MS_REC) != 0);
    }

    if (type_user) {
        if (copy_from_user_str(type, type_user, sizeof(type)))
            return (uint64_t)-EFAULT;
    }
    if (dev_name) {
        if (copy_from_user_str(devname, dev_name, sizeof(devname)))
            return (uint64_t)-EFAULT;
    }
    if (data) {
        if (copy_from_user_str(data_string, data, sizeof(data_string)))
            return (uint64_t)-EFAULT;
        mount_data = data_string;
    }

    return (uint64_t)vfs_do_mount(AT_FDCWD, dirname, type, mnt_flags, devname,
                                  mount_data);
}

uint64_t sys_mount_setattr(int dfd, const char *path, uint64_t flags,
                           const mount_attr_t *attr_user, size_t size) {
    static const uint64_t allowed_attr =
        MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |
        MOUNT_ATTR_NOEXEC | MOUNT_ATTR_NOATIME | MOUNT_ATTR_STRICTATIME |
        MOUNT_ATTR_NODIRATIME | MOUNT_ATTR_NOSYMFOLLOW | MOUNT_ATTR__ATIME |
        MOUNT_ATTR_IDMAP;
    mount_attr_t attr;
    char pathname[512] = {0};
    struct vfs_path target = {0};
    struct vfs_mount *mnt = NULL;
    unsigned long current_flags, new_flags, attr_mask;
    bool idmap_requested;
    int ret;

    if (!attr_user || size < MOUNT_ATTR_SIZE_VER0)
        return (uint64_t)-EINVAL;
    if (copy_from_user(&attr, attr_user, sizeof(attr)))
        return (uint64_t)-EFAULT;
    if ((attr.attr_set | attr.attr_clr) & ~allowed_attr)
        return (uint64_t)-EINVAL;
    if (attr.attr_clr & MOUNT_ATTR_IDMAP)
        return (uint64_t)-EINVAL;

    idmap_requested = (attr.attr_set & MOUNT_ATTR_IDMAP) != 0;
    if (idmap_requested) {
        if ((int64_t)attr.userns_fd < 0)
            return (uint64_t)-EINVAL;
    } else {
        /*
         * Linux callers commonly pass negative sentinels such as -EBADF here
         * when no idmapped mount is requested. Ignore the field unless the
         * IDMAP attribute is actually set.
         */
        attr.userns_fd = (uint64_t)-1;
    }

    if (path && copy_from_user_str(pathname, path, sizeof(pathname)))
        return (uint64_t)-EFAULT;

    if (!path || pathname[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH))
            return (uint64_t)-ENOENT;
        ret = generic_get_fd_path(dfd, &target);
    } else {
        int lookup_flags =
            (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;
        ret = vfs_filename_lookup(dfd, pathname, lookup_flags, &target);
    }
    if (ret < 0)
        return (uint64_t)ret;

    mnt = vfs_path_mount(&target);
    if (!mnt && target.mnt)
        mnt = vfs_mntget(target.mnt);
    if (!mnt) {
        vfs_path_put(&target);
        return (uint64_t)-EINVAL;
    }

    attr_mask = 0;
    if (attr.attr_set & MOUNT_ATTR_RDONLY)
        attr_mask |= VFS_MNT_READONLY;
    if (attr.attr_set & MOUNT_ATTR_NOSUID)
        attr_mask |= VFS_MNT_NOSUID;
    if (attr.attr_set & MOUNT_ATTR_NODEV)
        attr_mask |= VFS_MNT_NODEV;
    if (attr.attr_set & MOUNT_ATTR_NOEXEC)
        attr_mask |= VFS_MNT_NOEXEC;
    if (attr.attr_set & MOUNT_ATTR_NOSYMFOLLOW)
        attr_mask |= VFS_MNT_NOSYMFOLLOW;

    if (attr.attr_clr & MOUNT_ATTR_RDONLY)
        attr_mask |= VFS_MNT_READONLY;
    if (attr.attr_clr & MOUNT_ATTR_NOSUID)
        attr_mask |= VFS_MNT_NOSUID;
    if (attr.attr_clr & MOUNT_ATTR_NODEV)
        attr_mask |= VFS_MNT_NODEV;
    if (attr.attr_clr & MOUNT_ATTR_NOEXEC)
        attr_mask |= VFS_MNT_NOEXEC;
    if (attr.attr_clr & MOUNT_ATTR_NOSYMFOLLOW)
        attr_mask |= VFS_MNT_NOSYMFOLLOW;

    if (attr_mask != 0) {
        spin_lock(&mnt->mnt_lock);
        current_flags = mnt->mnt_flags;
        new_flags = current_flags & ~attr_mask;
        if (attr.attr_set & MOUNT_ATTR_RDONLY)
            new_flags |= VFS_MNT_READONLY;
        if (attr.attr_set & MOUNT_ATTR_NOSUID)
            new_flags |= VFS_MNT_NOSUID;
        if (attr.attr_set & MOUNT_ATTR_NODEV)
            new_flags |= VFS_MNT_NODEV;
        if (attr.attr_set & MOUNT_ATTR_NOEXEC)
            new_flags |= VFS_MNT_NOEXEC;
        if (attr.attr_set & MOUNT_ATTR_NOSYMFOLLOW)
            new_flags |= VFS_MNT_NOSYMFOLLOW;
        mnt->mnt_flags = new_flags;
        spin_unlock(&mnt->mnt_lock);
    }

    if (idmap_requested) {
        vfs_mntput(mnt);
        vfs_path_put(&target);
        return (uint64_t)-EOPNOTSUPP;
    }

    if (attr.propagation != 0) {
        ret = vfs_mount_set_propagation(mnt, (unsigned long)attr.propagation,
                                        (flags & AT_RECURSIVE) != 0);
        if (ret < 0) {
            vfs_mntput(mnt);
            vfs_path_put(&target);
            return (uint64_t)ret;
        }
    }

    vfs_mntput(mnt);
    vfs_path_put(&target);
    return 0;
}

uint64_t sys_umount2(const char *target, uint64_t flags) {
    char target_k[512];
    if (copy_from_user_str(target_k, target, sizeof(target_k)))
        return (uint64_t)-EFAULT;
    return (uint64_t)vfs_do_umount(AT_FDCWD, target_k, (int)flags);
}

extern int mountfd_create_file(struct vfs_mount *mnt, struct vfs_dentry *root,
                               bool detached, unsigned int open_flags,
                               struct vfs_file **out_file);

uint64_t sys_open_tree(int dfd, const char *pathname, unsigned int flags) {
    static const unsigned int allowed_flags =
        OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH | AT_NO_AUTOMOUNT |
        AT_SYMLINK_NOFOLLOW | AT_RECURSIVE;

    struct vfs_path path = {0};
    struct vfs_mount *mnt = NULL;
    struct vfs_file *file = NULL;
    char pathname_k[VFS_PATH_MAX];
    unsigned int lookup_flags;
    int fd_flags = 0;
    int ret;

    if (flags & ~allowed_flags)
        return (uint64_t)-EINVAL;
    if ((flags & AT_RECURSIVE) && !(flags & OPEN_TREE_CLONE))
        return (uint64_t)-EINVAL;
    if (!pathname)
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(pathname_k, pathname, sizeof(pathname_k)))
        return (uint64_t)-EFAULT;

    if ((flags & AT_EMPTY_PATH) && pathname_k[0] == '\0') {
        ret = generic_get_fd_path(dfd, &path);
        if (ret < 0)
            return (uint64_t)ret;
    } else {
        lookup_flags =
            (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;
        ret = vfs_filename_lookup(dfd, pathname_k, lookup_flags, &path);
        if (ret < 0)
            return (uint64_t)ret;
    }

    if (flags & OPEN_TREE_CLONE) {
        ret =
            vfs_create_bind_mount_err(&path, (flags & AT_RECURSIVE) != 0, &mnt);
        if (ret < 0) {
            vfs_path_put(&path);
            return (uint64_t)ret;
        }
        ret = mountfd_create_file(mnt, mnt->mnt_root, true, flags, &file);
    } else {
        mnt = vfs_mntget(path.mnt);
        if (!mnt) {
            vfs_path_put(&path);
            return (uint64_t)-EINVAL;
        }
        ret = mountfd_create_file(mnt, path.dentry, false, flags, &file);
    }

    vfs_path_put(&path);
    if (ret < 0) {
        if (mnt) {
            if (flags & OPEN_TREE_CLONE)
                vfs_put_mount_tree(mnt);
            else
                vfs_mntput(mnt);
        }
        return (uint64_t)ret;
    }

    if (flags & OPEN_TREE_CLOEXEC)
        fd_flags |= FD_CLOEXEC;
    ret = task_install_file(current_task, file, (unsigned int)fd_flags, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

static struct vfs_mount *generic_namespace_root_mount(void) {
    struct vfs_mount *mnt = task_mount_namespace_root(current_task);

    if (mnt)
        return mnt;
    if (vfs_init_mnt_ns.root)
        return vfs_init_mnt_ns.root;
    return vfs_root_path.mnt;
}

static struct vfs_mount *
generic_find_mount_by_dev_recursive(struct vfs_mount *mnt, dev64_t dev) {
    struct vfs_mount *child, *tmp;

    if (!mnt)
        return NULL;
    if (mnt->mnt_sb && mnt->mnt_sb->s_dev == dev)
        return vfs_mntget(mnt);

    llist_for_each(child, tmp, &mnt->mnt_mounts, mnt_child) {
        struct vfs_mount *found =
            generic_find_mount_by_dev_recursive(child, dev);
        if (found)
            return found;
    }

    return NULL;
}

static int generic_quotactl_resolve_sb(const char *special,
                                       struct vfs_super_block **sb_out) {
    char special_k[VFS_PATH_MAX];
    struct vfs_path path = {0};
    struct vfs_mount *mnt = NULL;
    int ret;

    if (!special || !sb_out)
        return -EFAULT;
    if (copy_from_user_str(special_k, special, sizeof(special_k)))
        return -EFAULT;

    ret = vfs_filename_lookup(AT_FDCWD, special_k, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return ret;

    mnt = vfs_path_mount(&path);
    if (!mnt && path.mnt)
        mnt = vfs_mntget(path.mnt);
    if (!mnt && path.dentry && path.dentry->d_inode &&
        S_ISBLK(path.dentry->d_inode->i_mode) && path.dentry->d_inode->i_rdev) {
        mnt = generic_find_mount_by_dev_recursive(
            generic_namespace_root_mount(), path.dentry->d_inode->i_rdev);
    }

    *sb_out =
        mnt ? mnt->mnt_sb
            : (path.dentry && path.dentry->d_inode ? path.dentry->d_inode->i_sb
                                                   : NULL);
    if (!*sb_out)
        ret = -ENOENT;

    if (mnt)
        vfs_mntput(mnt);
    vfs_path_put(&path);
    return ret;
}

static bool generic_quotactl_can_get_user_quota(uint32_t id) {
    task_t *task = current_task;

    if (!task)
        return false;
    if (task->uid == 0 || task->euid == 0)
        return true;
    return task->uid == (int64_t)id || task->euid == (int64_t)id;
}

uint64_t sys_quotactl(uint32_t cmd, const char *special, uint32_t id,
                      struct dqblk *addr) {
    struct vfs_super_block *sb = NULL;
    struct dqblk dq = {0};
    uint32_t subcmd = cmd >> SUBCMDSHIFT;
    uint32_t type = cmd & SUBCMDMASK;
    int ret;

    if (subcmd != Q_GETQUOTA || type != USRQUOTA)
        return (uint64_t)-EINVAL;
    if (!addr)
        return (uint64_t)-EFAULT;
    if (!generic_quotactl_can_get_user_quota(id))
        return (uint64_t)-EPERM;

    ret = generic_quotactl_resolve_sb(special, &sb);
    if (ret < 0)
        return (uint64_t)ret;
    if (!sb || !sb->s_op || !sb->s_op->get_quota)
        return (uint64_t)-EOPNOTSUPP;

    ret = sb->s_op->get_quota(sb, type, id, &dq.dqb_bhardlimit,
                              &dq.dqb_bsoftlimit, &dq.dqb_valid);
    if (ret < 0)
        return (uint64_t)ret;

    if (check_user_overflow((uint64_t)addr, sizeof(dq)) ||
        copy_to_user(addr, &dq, sizeof(dq))) {
        return (uint64_t)-EFAULT;
    }

    return 0;
}

uint64_t sys_umask(uint64_t mask) {
    task_t *self = current_task;
    uint16_t old = 0022;

    if (!self || !self->fs)
        return old;

    old = self->fs->umask & 0777;
    self->fs->umask = mask & 0777;
    return old;
}

uint64_t do_sys_open(const char *name, uint64_t flags, uint64_t mode) {
    struct vfs_open_how how = {
        .flags = flags,
        .mode = mode,
    };

    return (uint64_t)vfs_sys_openat(AT_FDCWD, name, &how);
}

uint64_t sys_creat(const char *path, uint64_t mode) {
    char name[512];
    if (copy_from_user_str(name, path, sizeof(name)))
        return (uint64_t)-EFAULT;

    return do_sys_open(name, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

uint64_t sys_open(const char *path, uint64_t flags, uint64_t mode) {
    char name[512];
    uint64_t ret;
    if (copy_from_user_str(name, path, sizeof(name)))
        return (uint64_t)-EFAULT;

    ret = do_sys_open(name, flags, mode);
    return ret;
}

uint64_t sys_openat(uint64_t dirfd, const char *name, uint64_t flags,
                    uint64_t mode) {
    struct vfs_open_how how = {
        .flags = flags,
        .mode = mode,
    };
    char name_k[512];
    uint64_t ret;
    if (!name || copy_from_user_str(name_k, name, sizeof(name_k))) {
        return (uint64_t)-EFAULT;
    }
    ret = (uint64_t)vfs_sys_openat((int)dirfd, name_k, &how);
    return ret;
}

#define XATTR_NAME_MAX 255u
#define XATTR_SIZE_MAX 65536u
#define XATTR_CREATE 0x1
#define XATTR_REPLACE 0x2

static int xattr_copy_name(char name_k[XATTR_NAME_MAX + 1], const char *name) {
    if (!name || copy_from_user_str(name_k, name, XATTR_NAME_MAX + 1))
        return -EFAULT;
    if (!name_k[0])
        return -EINVAL;
    return 0;
}

static uint64_t xattr_get_inode(struct vfs_inode *inode, const char *name,
                                void *value, uint64_t size) {
    char name_k[XATTR_NAME_MAX + 1];
    void *value_k = NULL;
    ssize_t ret;
    int err = xattr_copy_name(name_k, name);

    if (err)
        return (uint64_t)err;
    if (size > XATTR_SIZE_MAX)
        return (uint64_t)-E2BIG;
    if (size && !value)
        return (uint64_t)-EFAULT;
    if (size) {
        value_k = malloc((size_t)size);
        if (!value_k)
            return (uint64_t)-ENOMEM;
    }

    ret = vfs_getxattr(inode, name_k, value_k, (size_t)size);
    if (ret > 0 && value_k && copy_to_user(value, value_k, (size_t)ret))
        ret = -EFAULT;
    free(value_k);
    return (uint64_t)ret;
}

static uint64_t xattr_get_path(const char *path_user, const char *name,
                               void *value, uint64_t size, bool follow) {
    struct vfs_path path = {0};
    char path_k[VFS_PATH_MAX];
    uint64_t ret;

    if (!path_user || copy_from_user_str(path_k, path_user, sizeof(path_k)))
        return (uint64_t)-EFAULT;
    int err = vfs_filename_lookup(
        AT_FDCWD, path_k, follow ? LOOKUP_FOLLOW : LOOKUP_NOFOLLOW, &path);
    if (err < 0)
        return (uint64_t)err;
    ret = xattr_get_inode(path.dentry->d_inode, name, value, size);
    vfs_path_put(&path);
    return ret;
}

uint64_t sys_getxattr(const char *path, const char *name, void *value,
                      uint64_t size) {
    return xattr_get_path(path, name, value, size, true);
}

uint64_t sys_lgetxattr(const char *path, const char *name, void *value,
                       uint64_t size) {
    return xattr_get_path(path, name, value, size, false);
}

uint64_t sys_fgetxattr(int fd, const char *name, void *value, uint64_t size) {
    struct vfs_file *file = task_get_file(current_task, fd);
    uint64_t ret;
    if (!file)
        return (uint64_t)-EBADF;
    ret = xattr_get_inode(file->f_inode, name, value, size);
    vfs_file_put(file);
    return ret;
}

static uint64_t xattr_list_inode(struct vfs_inode *inode, char *list,
                                 uint64_t size) {
    char *list_k = NULL;
    ssize_t ret;

    if (size > XATTR_SIZE_MAX)
        return (uint64_t)-E2BIG;
    if (size && !list)
        return (uint64_t)-EFAULT;
    if (size) {
        list_k = malloc((size_t)size);
        if (!list_k)
            return (uint64_t)-ENOMEM;
    }
    ret = vfs_listxattr(inode, list_k, (size_t)size);
    if (ret > 0 && list_k && copy_to_user(list, list_k, (size_t)ret))
        ret = -EFAULT;
    free(list_k);
    return (uint64_t)ret;
}

static uint64_t xattr_list_path(const char *path_user, char *list,
                                uint64_t size, bool follow) {
    struct vfs_path path = {0};
    char path_k[VFS_PATH_MAX];
    uint64_t ret;
    int err;

    if (!path_user || copy_from_user_str(path_k, path_user, sizeof(path_k)))
        return (uint64_t)-EFAULT;
    err = vfs_filename_lookup(AT_FDCWD, path_k,
                              follow ? LOOKUP_FOLLOW : LOOKUP_NOFOLLOW, &path);
    if (err < 0)
        return (uint64_t)err;
    ret = xattr_list_inode(path.dentry->d_inode, list, size);
    vfs_path_put(&path);
    return ret;
}

uint64_t sys_listxattr(const char *path, char *list, uint64_t size) {
    return xattr_list_path(path, list, size, true);
}

uint64_t sys_llistxattr(const char *path, char *list, uint64_t size) {
    return xattr_list_path(path, list, size, false);
}

uint64_t sys_flistxattr(int fd, char *list, uint64_t size) {
    struct vfs_file *file = task_get_file(current_task, fd);
    uint64_t ret;
    if (!file)
        return (uint64_t)-EBADF;
    ret = xattr_list_inode(file->f_inode, list, size);
    vfs_file_put(file);
    return ret;
}

static uint64_t xattr_set_inode(struct vfs_inode *inode, const char *name,
                                const void *value, uint64_t size, int flags) {
    char name_k[XATTR_NAME_MAX + 1];
    void *value_k = NULL;
    int ret = xattr_copy_name(name_k, name);

    if (ret)
        return (uint64_t)ret;
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE))
        return (uint64_t)-EINVAL;
    if (size > XATTR_SIZE_MAX)
        return (uint64_t)-E2BIG;
    if (size && !value)
        return (uint64_t)-EFAULT;
    if (size) {
        value_k = malloc((size_t)size);
        if (!value_k)
            return (uint64_t)-ENOMEM;
        if (copy_from_user(value_k, value, (size_t)size)) {
            free(value_k);
            return (uint64_t)-EFAULT;
        }
    }
    ret = vfs_setxattr(inode, name_k, value_k, (size_t)size, flags);
    free(value_k);
    return (uint64_t)ret;
}

static uint64_t xattr_set_path(const char *path_user, const char *name,
                               const void *value, uint64_t size, int flags,
                               bool follow) {
    struct vfs_path path = {0};
    char path_k[VFS_PATH_MAX];
    uint64_t ret;
    int err;

    if (!path_user || copy_from_user_str(path_k, path_user, sizeof(path_k)))
        return (uint64_t)-EFAULT;
    err = vfs_filename_lookup(AT_FDCWD, path_k,
                              follow ? LOOKUP_FOLLOW : LOOKUP_NOFOLLOW, &path);
    if (err < 0)
        return (uint64_t)err;
    ret = xattr_set_inode(path.dentry->d_inode, name, value, size, flags);
    vfs_path_put(&path);
    return ret;
}

uint64_t sys_setxattr(const char *path, const char *name, const void *value,
                      uint64_t size, int flags) {
    return xattr_set_path(path, name, value, size, flags, true);
}

uint64_t sys_lsetxattr(const char *path, const char *name, const void *value,
                       uint64_t size, int flags) {
    return xattr_set_path(path, name, value, size, flags, false);
}

uint64_t sys_fsetxattr(int fd, const char *name, const void *value,
                       uint64_t size, int flags) {
    struct vfs_file *file = task_get_file(current_task, fd);
    uint64_t ret;
    if (!file)
        return (uint64_t)-EBADF;
    ret = xattr_set_inode(file->f_inode, name, value, size, flags);
    vfs_file_put(file);
    return ret;
}

static uint64_t xattr_remove_inode(struct vfs_inode *inode, const char *name) {
    char name_k[XATTR_NAME_MAX + 1];
    int ret = xattr_copy_name(name_k, name);
    if (ret)
        return (uint64_t)ret;
    return (uint64_t)vfs_removexattr(inode, name_k);
}

static uint64_t xattr_remove_path(const char *path_user, const char *name,
                                  bool follow) {
    struct vfs_path path = {0};
    char path_k[VFS_PATH_MAX];
    uint64_t ret;
    int err;

    if (!path_user || copy_from_user_str(path_k, path_user, sizeof(path_k)))
        return (uint64_t)-EFAULT;
    err = vfs_filename_lookup(AT_FDCWD, path_k,
                              follow ? LOOKUP_FOLLOW : LOOKUP_NOFOLLOW, &path);
    if (err < 0)
        return (uint64_t)err;
    ret = xattr_remove_inode(path.dentry->d_inode, name);
    vfs_path_put(&path);
    return ret;
}

uint64_t sys_removexattr(const char *path, const char *name) {
    return xattr_remove_path(path, name, true);
}

uint64_t sys_lremovexattr(const char *path, const char *name) {
    return xattr_remove_path(path, name, false);
}

uint64_t sys_fremovexattr(int fd, const char *name) {
    struct vfs_file *file = task_get_file(current_task, fd);
    uint64_t ret;
    if (!file)
        return (uint64_t)-EBADF;
    ret = xattr_remove_inode(file->f_inode, name);
    vfs_file_put(file);
    return ret;
}

static int openat2_copy_how_from_user(struct vfs_open_how *how,
                                      const struct vfs_open_how *user_how,
                                      uint64_t size) {
    uint8_t extra[32];
    uint64_t offset;

    if (!how || !user_how)
        return -EFAULT;
    if (size < sizeof(*how))
        return -EINVAL;
    if (check_user_overflow((uint64_t)user_how, size))
        return -EFAULT;

    memset(how, 0, sizeof(*how));
    if (copy_from_user(how, user_how, sizeof(*how)))
        return -EFAULT;

    for (offset = sizeof(*how); offset < size; offset += sizeof(extra)) {
        uint64_t chunk = MIN((uint64_t)sizeof(extra), size - offset);
        if (copy_from_user(extra, (const uint8_t *)user_how + offset, chunk))
            return -EFAULT;
        for (uint64_t i = 0; i < chunk; i++) {
            if (extra[i] != 0)
                return -E2BIG;
        }
    }

    return 0;
}

static int openat2_validate_how(const struct vfs_open_how *how) {
    static const uint64_t allowed_flags =
        O_ACCMODE_FLAGS | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND |
        O_NONBLOCK | O_DSYNC | O_SYNC | O_RSYNC | O_DIRECTORY | O_NOFOLLOW |
        O_ASYNC | O_DIRECT | O_LARGEFILE | O_NOATIME | O_CLOEXEC | O_PATH |
        O_TMPFILE;
    static const uint64_t allowed_resolve =
        RESOLVE_NO_XDEV | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS |
        RESOLVE_BENEATH | RESOLVE_IN_ROOT | RESOLVE_CACHED;

    if (!how)
        return -EINVAL;
    if (how->flags & ~allowed_flags)
        return -EINVAL;
    if (how->mode & ~07777ULL)
        return -EINVAL;
    if (how->resolve & ~allowed_resolve)
        return -EINVAL;
    if (how->mode != 0 && !(how->flags & (O_CREAT | O_TMPFILE)))
        return -EINVAL;
    if (how->resolve & RESOLVE_CACHED)
        return -EAGAIN;

    return 0;
}

uint64_t sys_openat2(uint64_t dirfd, const char *name,
                     const struct vfs_open_how *how_user, uint64_t size) {
    struct vfs_open_how how = {0};
    char name_k[VFS_PATH_MAX];
    int ret;

    if (!name)
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(name_k, name, sizeof(name_k)))
        return (uint64_t)-EFAULT;

    ret = openat2_copy_how_from_user(&how, how_user, size);
    if (ret < 0)
        return (uint64_t)ret;

    ret = openat2_validate_how(&how);
    if (ret < 0)
        return (uint64_t)ret;

    return (uint64_t)vfs_sys_openat((int)dirfd, name_k, &how);
}

uint64_t sys_name_to_handle_at(int dfd, const char *name,
                               struct file_handle *handle, int *mnt_id,
                               int flag) {
    linux_file_handle_prefix_t prefix;
    struct vfs_path path = {0};
    struct vfs_kstat stat;
    int ret;
    const unsigned int required_size = sizeof(uint64_t);

    if (!name || !handle || check_user_overflow((uint64_t)name, 1) ||
        check_user_overflow((uint64_t)handle, sizeof(prefix))) {
        return (uint64_t)-EFAULT;
    }

    if (flag & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW))
        return (uint64_t)-EINVAL;

    ret = generic_copy_file_handle_prefix_from_user(handle, &prefix);
    if (ret < 0)
        return (uint64_t)ret;

    if (prefix.handle_bytes < required_size) {
        prefix.handle_bytes = required_size;
        (void)generic_copy_file_handle_prefix_to_user(handle, &prefix);
        return (uint64_t)-EOVERFLOW;
    }

    if ((flag & AT_EMPTY_PATH) && name[0] == '\0') {
        ret = generic_get_empty_path(dfd, &path);
        if (ret < 0)
            return (uint64_t)ret;

        if (path.dentry->d_inode && path.dentry->d_inode->i_op &&
            path.dentry->d_inode->i_op->getattr) {
            ret = path.dentry->d_inode->i_op->getattr(&path, &stat, 0, 0);
        } else {
            vfs_fill_generic_kstat(&path, &stat);
            ret = 0;
        }
        vfs_path_put(&path);
    } else {
        /* name_to_handle_at() does not follow the final symlink unless
         * AT_SYMLINK_FOLLOW is explicitly requested. vfs_statx() expresses
         * the inverse policy through AT_SYMLINK_NOFOLLOW. */
        ret = vfs_statx(dfd, name,
                        (flag & AT_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW, 0,
                        &stat);
    }
    if (ret < 0)
        return (uint64_t)ret;

    prefix.handle_bytes = required_size;
    prefix.handle_type = 1;
    ret = generic_copy_file_handle_prefix_to_user(handle, &prefix);
    if (ret < 0)
        return (uint64_t)ret;
    ret =
        generic_copy_handle_bytes_to_user(handle, &stat.ino, sizeof(uint64_t));
    if (ret < 0)
        return (uint64_t)ret;

    int mount_id_value = (int)stat.mnt_id;

    if (mnt_id) {
        if (check_user_overflow((uint64_t)mnt_id, sizeof(int))) {
            return (uint64_t)-EFAULT;
        }
        if (copy_to_user(mnt_id, &mount_id_value, sizeof(int))) {
            return (uint64_t)-EFAULT;
        }
    }

    return 0;
}

uint64_t sys_open_by_handle_at(int mountdirfd, struct file_handle *handle,
                               int flags) {
    linux_file_handle_prefix_t prefix;
    struct vfs_path mount_path = {0};
    struct vfs_path open_path = {0};
    struct vfs_inode *inode = NULL;
    struct vfs_dentry *dentry = NULL;
    struct vfs_file *file = NULL;
    uint64_t ino = 0;
    int ret;
    int fd;

    if (!handle)
        return (uint64_t)-EFAULT;

    ret = generic_copy_file_handle_prefix_from_user(handle, &prefix);
    if (ret < 0)
        return (uint64_t)ret;
    if (prefix.handle_type != 1 || prefix.handle_bytes != sizeof(uint64_t))
        return (uint64_t)-EINVAL;

    ret = generic_copy_handle_bytes_from_user(handle, &ino, sizeof(ino));
    if (ret < 0)
        return (uint64_t)ret;

    ret = generic_get_fd_path(mountdirfd, &mount_path);
    if (ret < 0)
        return (uint64_t)ret;

    inode = generic_find_inode_by_ino(
        mount_path.mnt ? mount_path.mnt->mnt_sb : NULL, ino);
    if (!inode) {
        vfs_path_put(&mount_path);
        return (uint64_t)-ESTALE;
    }

    dentry = generic_inode_alias(inode);
    if (!dentry) {
        vfs_iput(inode);
        vfs_path_put(&mount_path);
        return (uint64_t)-ESTALE;
    }

    open_path.mnt = mount_path.mnt ? vfs_mntget(mount_path.mnt) : NULL;
    open_path.dentry = dentry;
    file = vfs_alloc_file(&open_path, (unsigned int)flags);
    vfs_path_put(&open_path);
    vfs_iput(inode);
    vfs_path_put(&mount_path);
    if (!file)
        return (uint64_t)-ENOMEM;

    if (file->f_op && file->f_op->open) {
        ret = file->f_op->open(file->f_inode, file);
        if (ret < 0) {
            vfs_file_put(file);
            return (uint64_t)ret;
        }
    }

    fd = task_install_file(current_task, file, 0, 0);
    vfs_file_put(file);
    return fd < 0 ? (uint64_t)fd : (uint64_t)fd;
}

uint64_t sys_inotify_init() { return sys_inotify_init1(0); }

uint64_t sys_inotify_init1(uint64_t flags) {
    struct vfs_file *file = NULL;
    int ret;

    if (flags & ~(uint64_t)(IN_CLOEXEC | IN_NONBLOCK))
        return (uint64_t)-EINVAL;

    ret = notifyfs_create_handle_file(&file, (unsigned int)flags, NULL);
    if (ret < 0)
        return (uint64_t)ret;

    ret = task_install_file(current_task, file,
                            (flags & IN_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_inotify_add_watch(uint64_t notifyfd, const char *path,
                               uint64_t mask) {
    struct vfs_file *notify_file = NULL;
    notifyfs_handle_t *handle;
    struct vfs_path target = {0};
    unsigned int lookup_flags;
    uint64_t wd = 0;
    int ret;

    if (!path)
        return (uint64_t)-EINVAL;

    notify_file = task_get_file(current_task, (int)notifyfd);
    if (!notify_file)
        return (uint64_t)-EBADF;
    if (!notifyfs_is_file(notify_file)) {
        vfs_file_put(notify_file);
        return (uint64_t)-EINVAL;
    }

    handle = notifyfs_file_handle(notify_file);
    if (!handle || notifyfs_handle_is_fanotify(handle)) {
        vfs_file_put(notify_file);
        return (uint64_t)-EINVAL;
    }

    lookup_flags = (mask & IN_DONT_FOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;
    ret = vfs_filename_lookup(AT_FDCWD, path, lookup_flags, &target);
    if (ret < 0) {
        vfs_file_put(notify_file);
        return (uint64_t)ret;
    }
    if (!target.dentry || !target.dentry->d_inode) {
        vfs_path_put(&target);
        vfs_file_put(notify_file);
        return (uint64_t)-ENOENT;
    }

    ret = notifyfs_handle_add_watch(handle, notify_file->f_inode,
                                    target.dentry->d_inode, mask, &wd);
    vfs_path_put(&target);
    vfs_file_put(notify_file);
    if (ret < 0)
        return (uint64_t)ret;
    return wd;
}

uint64_t sys_inotify_rm_watch(uint64_t watchfd, uint64_t wd) {
    struct vfs_file *notify_file = NULL;
    notifyfs_handle_t *handle;
    int ret;

    notify_file = task_get_file(current_task, (int)watchfd);
    if (!notify_file)
        return (uint64_t)-EBADF;
    if (!notifyfs_is_file(notify_file)) {
        vfs_file_put(notify_file);
        return (uint64_t)-EINVAL;
    }

    handle = notifyfs_file_handle(notify_file);
    if (!handle || notifyfs_handle_is_fanotify(handle)) {
        vfs_file_put(notify_file);
        return (uint64_t)-EINVAL;
    }

    ret = notifyfs_handle_remove_watch(handle, wd);
    vfs_file_put(notify_file);
    return (uint64_t)ret;
}

uint64_t sys_fanotify_init(uint64_t flags, uint64_t event_f_flags) {
    const uint64_t allowed_flags =
        FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_FID | FAN_REPORT_DFID_NAME;
    const uint64_t allowed_event_flags = O_ACCMODE_FLAGS | O_LARGEFILE |
                                         O_CLOEXEC | O_NONBLOCK | O_APPEND |
                                         O_DSYNC | O_NOATIME | O_SYNC;
    struct vfs_file *file = NULL;
    notifyfs_handle_t *handle = NULL;
    int ret;

    if ((flags & FAN_ALL_CLASS_BITS) != FAN_CLASS_NOTIF)
        return (uint64_t)-EINVAL;
    if (flags & ~allowed_flags)
        return (uint64_t)-EINVAL;
    if ((flags & FAN_REPORT_NAME) && !(flags & FAN_REPORT_DIR_FID))
        return (uint64_t)-EINVAL;
    if ((flags & FAN_REPORT_DIR_FID) && !(flags & FAN_REPORT_NAME))
        return (uint64_t)-EINVAL;
    if (event_f_flags & ~allowed_event_flags)
        return (uint64_t)-EINVAL;

    ret = notifyfs_create_handle_file(
        &file, (flags & FAN_NONBLOCK) ? O_NONBLOCK : 0, &handle);
    if (ret < 0)
        return (uint64_t)ret;

    ret = notifyfs_handle_set_fanotify(handle, (unsigned int)flags);
    if (ret < 0) {
        vfs_file_put(file);
        return (uint64_t)ret;
    }

    ret = task_install_file(current_task, file,
                            (flags & FAN_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_fanotify_mark(uint64_t fanotify_fd, uint64_t flags, uint64_t mask,
                           int dfd, const char *path) {
    const uint64_t allowed_flags = FAN_MARK_ADD | FAN_MARK_REMOVE |
                                   FAN_MARK_DONT_FOLLOW | FAN_MARK_ONLYDIR;
    const uint64_t allowed_mask =
        IN_ALL_EVENTS | FAN_EVENT_ON_CHILD | FAN_ONDIR;
    struct vfs_file *notify_file = NULL;
    notifyfs_handle_t *handle;
    struct vfs_path target = {0};
    unsigned int lookup_flags;
    uint64_t wd = 0;
    int ret;

    if (!path)
        return (uint64_t)-EFAULT;
    if (flags & ~allowed_flags)
        return (uint64_t)-EINVAL;
    if (!!(flags & FAN_MARK_ADD) == !!(flags & FAN_MARK_REMOVE))
        return (uint64_t)-EINVAL;
    if (!mask || (mask & ~allowed_mask))
        return (uint64_t)-EINVAL;

    notify_file = task_get_file(current_task, (int)fanotify_fd);
    if (!notify_file)
        return (uint64_t)-EBADF;
    handle = notifyfs_file_handle(notify_file);
    if (!notifyfs_is_file(notify_file) ||
        !notifyfs_handle_is_fanotify(handle)) {
        vfs_file_put(notify_file);
        return (uint64_t)-EINVAL;
    }

    lookup_flags =
        (flags & FAN_MARK_DONT_FOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;
    ret = vfs_filename_lookup(dfd, path, lookup_flags, &target);
    if (ret < 0) {
        vfs_file_put(notify_file);
        return (uint64_t)ret;
    }
    if (!target.dentry || !target.dentry->d_inode) {
        vfs_path_put(&target);
        vfs_file_put(notify_file);
        return (uint64_t)-ENOENT;
    }
    if ((flags & FAN_MARK_ONLYDIR) &&
        !S_ISDIR(target.dentry->d_inode->i_mode)) {
        vfs_path_put(&target);
        vfs_file_put(notify_file);
        return (uint64_t)-ENOTDIR;
    }

    if (flags & FAN_MARK_ADD) {
        uint64_t inotify_mask = mask & IN_ALL_EVENTS;
        if (flags & FAN_MARK_ONLYDIR)
            inotify_mask |= IN_ONLYDIR;
        ret = notifyfs_handle_add_watch(handle, notify_file->f_inode,
                                        target.dentry->d_inode, inotify_mask,
                                        &wd);
    } else {
        ret =
            notifyfs_handle_remove_watch_inode(handle, target.dentry->d_inode);
    }

    vfs_path_put(&target);
    vfs_file_put(notify_file);
    return (uint64_t)ret;
}

uint64_t sys_fsync(uint64_t fd) {
    struct vfs_file *file = task_get_file(current_task, (int)fd);
    int ret;

    if (!file)
        return (uint64_t)-EBADF;

    ret = vfs_fsync_file(file);
    vfs_file_put(file);
    return ret < 0 ? (uint64_t)ret : 0;
}

uint64_t sys_fdatasync(uint64_t fd) {
    struct vfs_file *file = task_get_file(current_task, (int)fd);
    int ret;

    if (!file)
        return (uint64_t)-EBADF;

    ret = vfs_fsync_file_range(file, 0, (loff_t)file->f_inode->i_size, 1);
    vfs_file_put(file);
    return ret < 0 ? (uint64_t)ret : 0;
}

uint64_t sys_syncfs(int fd) {
    struct vfs_file *file;
    struct vfs_super_block *sb;
    struct vfs_inode *inode, *tmp;
    struct vfs_inode **inodes = NULL;
    size_t capacity = 0;
    size_t count = 0;
    size_t i;
    int first_error = 0;

    file = task_get_file(current_task, fd);
    if (!file)
        return (uint64_t)-EBADF;
    sb = file->f_path.mnt ? file->f_path.mnt->mnt_sb
                          : (file->f_inode ? file->f_inode->i_sb : NULL);
    if (!sb) {
        vfs_file_put(file);
        return (uint64_t)-EINVAL;
    }
    if (!vfs_get_super(sb)) {
        vfs_file_put(file);
        return (uint64_t)-EIO;
    }
    vfs_file_put(file);

    spin_lock(&sb->s_inode_lock);
    llist_for_each(inode, tmp, &sb->s_inodes, i_sb_list) { capacity++; }
    spin_unlock(&sb->s_inode_lock);

    if (capacity) {
        inodes = calloc(capacity, sizeof(*inodes));
        if (!inodes) {
            vfs_put_super(sb);
            return (uint64_t)-ENOMEM;
        }

        spin_lock(&sb->s_inode_lock);
        llist_for_each(inode, tmp, &sb->s_inodes, i_sb_list) {
            if (count >= capacity)
                break;
            inodes[count] = vfs_igrab(inode);
            if (inodes[count])
                count++;
        }
        spin_unlock(&sb->s_inode_lock);
    }

    for (i = 0; i < count; ++i) {
        int ret = 0;

        if (inodes[i]->i_mapping.dirty_pages)
            ret = page_cache_writeback_range(&inodes[i]->i_mapping, 0,
                                             UINT64_MAX, false);
        if (ret < 0 && first_error == 0)
            first_error = ret;
        vfs_iput(inodes[i]);
    }
    free(inodes);

    if (sb->s_op && sb->s_op->sync_fs) {
        int ret = sb->s_op->sync_fs(sb, 1);
        if (ret < 0 && first_error == 0)
            first_error = ret;
    }
    vfs_put_super(sb);
    return first_error < 0 ? (uint64_t)first_error : 0;
}

uint64_t sys_close(uint64_t fd) {
    task_t *self = current_task;
    fd_info_t *fd_info;

    fd_t *entry = NULL;
    uint64_t ret = (uint64_t)-EBADF;
    fd_info = task_fd_info_get(self);
    if (!fd_info)
        return (uint64_t)-EBADF;

    with_fd_info_lock(fd_info, {
        if (fd >= fd_info->max_fds)
            break;
        entry = task_fd_slot_take(fd_info, (int)fd, NULL);
        if (!entry)
            break;
        ret = 0;
    });

    task_fd_info_put(fd_info, self);

    if (ret)
        return ret;

    on_close_file_call(self, fd, entry);

    vfs_close_file_for_task(entry, self);

    return 0;
}

static int close_range_unshare_fd_table(task_t *self) {
    if (!self) {
        return -EINVAL;
    }

    fd_info_t *old = task_fd_info_get(self);
    if (!old)
        return -EINVAL;
    if (task_fd_info_ref_read(old) <= 2) {
        task_fd_info_put(old, self);
        return 0;
    }

    fd_info_t *new_info = task_fd_info_clone(old);
    if (!new_info) {
        task_fd_info_put(old, self);
        return -ENOMEM;
    }

    fd_info_t *replaced = task_fd_info_replace(self, new_info);
    task_fd_info_put(old, self);
    old = replaced;
    task_fd_info_put(old, self);
    return 0;
}

uint64_t sys_close_range(uint64_t fd, uint64_t maxfd, uint64_t flags) {
    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) {
        return (uint64_t)-EINVAL;
    }

    if (fd > maxfd) {
        return (uint64_t)-EINVAL;
    }

    task_t *self = current_task;

    if (flags & CLOSE_RANGE_UNSHARE) {
        int ret = close_range_unshare_fd_table(self);
        if (ret < 0) {
            return (uint64_t)ret;
        }
    }

    fd_info_t *fd_info = task_fd_info_get(self);
    if (!fd_info)
        return (uint64_t)-EBADF;

    size_t table_max = fd_info->max_fds;
    if (fd >= table_max) {
        task_fd_info_put(fd_info, self);
        return 0;
    }

    if (maxfd >= table_max) {
        maxfd = table_max - 1;
    }

    if (flags & CLOSE_RANGE_CLOEXEC) {
        with_fd_info_lock(fd_info, {
            for (size_t fd_ = fd; fd_ <= maxfd; fd_++) {
                if (!fd_info->fds[fd_].file)
                    continue;
                fd_info->fds[fd_].flags |= FD_CLOEXEC;
            }
        });
        task_fd_info_put(fd_info, self);
        return 0;
    }

    fd_t **to_close = calloc(table_max, sizeof(*to_close));
    if (!to_close) {
        task_fd_info_put(fd_info, self);
        return (uint64_t)-ENOMEM;
    }

    with_fd_info_lock(fd_info, {
        for (size_t fd_ = fd; fd_ <= maxfd; fd_++) {
            fd_t *entry = task_fd_slot_take(fd_info, (int)fd_, NULL);
            if (!entry)
                continue;
            to_close[fd_] = entry;
        }
    });
    task_fd_info_put(fd_info, self);

    for (size_t fd_ = fd; fd_ <= maxfd; fd_++) {
        fd_t *entry = to_close[fd_];
        if (!entry)
            continue;
        on_close_file_call(self, fd_, entry);
        vfs_close_file_for_task(entry, self);
    }
    free(to_close);

    return 0;
}

uint64_t sys_copy_file_range(uint64_t fd_in, int *offset_in_user,
                             uint64_t fd_out, int *offset_out_user,
                             uint64_t len, uint64_t flags) {
    loff_t src_pos;
    loff_t dst_pos;
    fd_t *in_fd = task_get_file(current_task, (int)fd_in);
    fd_t *out_fd = task_get_file(current_task, (int)fd_out);
    if (!in_fd || !out_fd) {
        vfs_file_put(in_fd);
        vfs_file_put(out_fd);
        return (uint64_t)-EBADF;
    }

    if (flags) {
        vfs_file_put(in_fd);
        vfs_file_put(out_fd);
        return (uint64_t)-EINVAL;
    }

    if (fd_get_offset(out_fd) >= (uint64_t)out_fd->f_inode->i_size &&
        out_fd->f_inode->i_size > 0) {
        vfs_file_put(in_fd);
        vfs_file_put(out_fd);
        return 0;
    }

    int offset_in = 0;
    int offset_out = 0;
    if (offset_in_user) {
        if (copy_from_user(&offset_in, offset_in_user, sizeof(int))) {
            vfs_file_put(in_fd);
            vfs_file_put(out_fd);
            return (uint64_t)-EFAULT;
        }
    }
    if (offset_out_user) {
        if (copy_from_user(&offset_out, offset_out_user, sizeof(int))) {
            vfs_file_put(in_fd);
            vfs_file_put(out_fd);
            return (uint64_t)-EFAULT;
        }
    }

    uint64_t src_offset =
        offset_in_user ? (uint64_t)offset_in : fd_get_offset(in_fd);
    uint64_t dst_offset =
        offset_out_user ? (uint64_t)offset_out : fd_get_offset(out_fd);
    if (fd_get_flags(out_fd) & O_APPEND)
        dst_offset = out_fd->f_inode->i_size;

    if (src_offset >= in_fd->f_inode->i_size) {
        vfs_file_put(in_fd);
        vfs_file_put(out_fd);
        return 0;
    }

    uint64_t length = MIN(len, in_fd->f_inode->i_size - src_offset);
    uint8_t *buffer = (uint8_t *)alloc_frames_bytes(length);
    size_t copy_total = 0;

    src_pos = (loff_t)src_offset;
    dst_pos = (loff_t)dst_offset;

    ssize_t ret = vfs_read_file(in_fd, buffer, length, &src_pos);
    if (ret < 0) {
        free_frames_bytes(buffer, length);
        vfs_file_put(in_fd);
        vfs_file_put(out_fd);
        return (uint64_t)ret;
    }

    ret = vfs_write_file(out_fd, buffer, (size_t)ret, &dst_pos);
    if (ret < 0) {
        free_frames_bytes(buffer, length);
        vfs_file_put(in_fd);
        vfs_file_put(out_fd);
        return (uint64_t)ret;
    }
    copy_total = (size_t)ret;
    free_frames_bytes(buffer, length);
    if (!offset_in_user && copy_total > 0)
        fd_add_offset(in_fd, (int64_t)copy_total);
    if (!offset_out_user && copy_total > 0)
        fd_add_offset(out_fd, (int64_t)copy_total);

    vfs_file_put(in_fd);
    vfs_file_put(out_fd);
    return copy_total;
}

uint64_t sys_read(uint64_t fd, void *buf, uint64_t len) {
    struct vfs_file *file;
    bool needs_put;

    if (rw_validate_user_buffer(buf, len) < 0) {
        return (uint64_t)-EFAULT;
    }
    file = task_get_file_for_io(current_task, (int)fd, &needs_put);
    if (!file)
        return (uint64_t)-EBADF;
    if (S_ISDIR(file->f_inode->i_mode)) {
        if (needs_put)
            vfs_file_put(file);
        return (uint64_t)-EISDIR;
    }

    ssize_t ret = (ssize_t)vfs_read_file(file, buf, len, NULL);

    if (needs_put)
        vfs_file_put(file);

    return ret;
}

uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len) {
    struct vfs_file *file;
    bool needs_put;

    if (len != 0 && rw_validate_user_buffer(buf, len) < 0) {
        return (uint64_t)-EFAULT;
    }

    file = task_get_file_for_io(current_task, (int)fd, &needs_put);
    if (!file)
        return (uint64_t)-EBADF;
    if (S_ISDIR(file->f_inode->i_mode)) {
        if (needs_put)
            vfs_file_put(file);
        return (uint64_t)-EISDIR;
    }

    ssize_t ret = (ssize_t)vfs_write_file(file, buf, len, NULL);

    if (needs_put)
        vfs_file_put(file);
    return ret;
}

uint64_t sys_pread64(int fd, void *buf, size_t len, uint64_t offset) {
    struct vfs_file *file;
    void *kbuf = NULL;
    loff_t pos = (loff_t)offset;
    ssize_t ret;

    if (rw_validate_user_buffer(buf, len) < 0) {
        return (uint64_t)-EFAULT;
    }

    file = task_get_file(current_task, fd);
    if (!file)
        return (uint64_t)-EBADF;
    if (S_ISDIR(file->f_inode->i_mode)) {
        vfs_file_put(file);
        return (uint64_t)-EISDIR;
    }
    /*
     * pread/pwrite follow the same rule. The explicit offset does not change
     * the ABI detail that a 0-byte request should still reach the backend.
     */

    ret = vfs_read_file(file, buf, len, &pos);

    free(kbuf);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_pwrite64(int fd, const void *buf, size_t len, uint64_t offset) {
    struct vfs_file *file;
    void *kbuf = NULL;
    loff_t pos = (loff_t)offset;
    ssize_t ret;

    if (len != 0 && rw_validate_user_buffer(buf, len) < 0) {
        return (uint64_t)-EFAULT;
    }

    file = task_get_file(current_task, fd);
    if (!file)
        return (uint64_t)-EBADF;
    if (S_ISDIR(file->f_inode->i_mode)) {
        vfs_file_put(file);
        return (uint64_t)-EISDIR;
    }

    ret = vfs_write_file(file, buf, len, &pos);

    vfs_file_put(file);
    return (uint64_t)ret;
}

// Use a large sendfile buffer to optimize the speed
#define SENDFILE_BUFFER_SIZE (1 * 1024 * 1024)

uint64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, int *offset_ptr,
                      size_t count) {
    fd_t *out_handle = task_get_file(current_task, (int)out_fd);
    fd_t *in_handle = task_get_file(current_task, (int)in_fd);
    loff_t read_pos = 0;
    if (out_handle == NULL || in_handle == NULL) {
        vfs_file_put(out_handle);
        vfs_file_put(in_handle);
        return -EBADF;
    }

    uint64_t current_offset = fd_get_offset(in_handle);
    size_t total_sent = 0;
    size_t remaining = count;

    if (offset_ptr != NULL) {
        int user_offset = 0;

        if (check_user_overflow((uint64_t)offset_ptr, sizeof(user_offset)) ||
            check_unmapped((uint64_t)offset_ptr, sizeof(user_offset))) {
            vfs_file_put(out_handle);
            vfs_file_put(in_handle);
            return -EFAULT;
        }
        if (copy_from_user(&user_offset, offset_ptr, sizeof(user_offset))) {
            vfs_file_put(out_handle);
            vfs_file_put(in_handle);
            return -EFAULT;
        }
        if (user_offset < 0) {
            vfs_file_put(out_handle);
            vfs_file_put(in_handle);
            return -EINVAL;
        }

        current_offset = (uint64_t)user_offset;
    }

    char *buffer = (char *)alloc_frames_bytes(SENDFILE_BUFFER_SIZE);
    if (buffer == NULL) {
        vfs_file_put(out_handle);
        vfs_file_put(in_handle);
        return -ENOMEM;
    }

    while (remaining > 0) {
        size_t bytes_to_read = MIN(remaining, SENDFILE_BUFFER_SIZE);
        ssize_t bytes_read = 0;
        ssize_t bytes_written = 0;
        read_pos = (loff_t)current_offset;
        bytes_read = vfs_read_file(in_handle, buffer, bytes_to_read, &read_pos);
        if (bytes_read <= 0) {
            if (bytes_read < 0 && total_sent == 0) {
                free_frames_bytes(buffer, SENDFILE_BUFFER_SIZE);
                vfs_file_put(out_handle);
                vfs_file_put(in_handle);
                return (uint64_t)bytes_read;
            }
            break;
        }
        bytes_written = vfs_write_file(out_handle, buffer, bytes_read, NULL);
        if (bytes_written <= 0) {
            if (total_sent == 0) {
                free_frames_bytes(buffer, SENDFILE_BUFFER_SIZE);
                vfs_file_put(out_handle);
                vfs_file_put(in_handle);
                return bytes_written < 0 ? (uint64_t)bytes_written : 0;
            }
            break;
        }
        if (bytes_written < bytes_read) {
            bytes_read = bytes_written;
        }
        current_offset += bytes_read;
        total_sent += bytes_read;
        remaining -= bytes_read;
    }
    free_frames_bytes(buffer, SENDFILE_BUFFER_SIZE);
    if (offset_ptr != NULL) {
        int out_offset =
            current_offset > (uint64_t)INT_MAX ? INT_MAX : (int)current_offset;
        if (copy_to_user(offset_ptr, &out_offset, sizeof(out_offset))) {
            vfs_file_put(out_handle);
            vfs_file_put(in_handle);
            return -EFAULT;
        }
    } else {
        fd_set_offset(in_handle, current_offset);
    }
    vfs_file_put(out_handle);
    vfs_file_put(in_handle);
    return total_sent;
}

static ssize_t generic_splice_fallback(struct vfs_file *in,
                                       struct vfs_file *out, size_t len,
                                       loff_t *in_pos, loff_t *out_pos,
                                       bool nonblock) {
    uint8_t *buf = NULL;
    size_t total = 0;

    if (len == 0)
        return 0;
    buf = (uint8_t *)alloc_frames_bytes(MIN(len, (size_t)(1 << 20)));
    if (!buf)
        return -ENOMEM;

    while (total < len) {
        size_t chunk = MIN(len - total, (size_t)(1 << 20));
        ssize_t rd = vfs_read_file(in, buf, chunk, in_pos);
        ssize_t wr;

        if (rd <= 0) {
            if (rd < 0 && total == 0) {
                free_frames_bytes(buf, MIN(len, (size_t)(1 << 20)));
                return rd;
            }
            break;
        }
        wr = vfs_write_kernel_file(out, buf, (size_t)rd, out_pos);
        if (wr <= 0) {
            if (total == 0) {
                free_frames_bytes(buf, MIN(len, (size_t)(1 << 20)));
                return wr < 0 ? wr : 0;
            }
            break;
        }
        total += (size_t)wr;
        if ((size_t)wr < (size_t)rd)
            break;
    }

    free_frames_bytes(buf, MIN(len, (size_t)(1 << 20)));
    (void)nonblock;
    return (ssize_t)total;
}

uint64_t sys_splice(uint64_t fd_in, int64_t *off_in, uint64_t fd_out,
                    int64_t *off_out, size_t len, uint64_t flags) {
    struct vfs_file *in = NULL;
    struct vfs_file *out = NULL;
    ssize_t ret;
    loff_t in_pos = 0;
    loff_t out_pos = 0;
    bool in_pos_valid = false;
    bool out_pos_valid = false;
    bool nonblock = !!(flags & SPLICE_F_NONBLOCK);

    if (flags & ~SPLICE_F_SUPPORTED)
        return -EINVAL;

    in = task_get_file(current_task, (int)fd_in);
    out = task_get_file(current_task, (int)fd_out);
    if (!in || !out) {
        vfs_file_put(in);
        vfs_file_put(out);
        return -EBADF;
    }
    if (S_ISDIR(in->f_inode->i_mode) || S_ISDIR(out->f_inode->i_mode)) {
        vfs_file_put(in);
        vfs_file_put(out);
        return -EISDIR;
    }

    if (off_in) {
        if (check_user_overflow((uint64_t)off_in, sizeof(*off_in)) ||
            copy_from_user(&in_pos, off_in, sizeof(in_pos))) {
            vfs_file_put(in);
            vfs_file_put(out);
            return -EFAULT;
        }
        in_pos_valid = true;
    }
    if (off_out) {
        if (check_user_overflow((uint64_t)off_out, sizeof(*off_out)) ||
            copy_from_user(&out_pos, off_out, sizeof(out_pos))) {
            vfs_file_put(in);
            vfs_file_put(out);
            return -EFAULT;
        }
        out_pos_valid = true;
    }

    if (pipefs_is_pipe(in)) {
        if (off_in) {
            vfs_file_put(in);
            vfs_file_put(out);
            return -ESPIPE;
        }
        ret = pipefs_splice_to(in, out, len, nonblock);
    } else if (pipefs_is_pipe(out)) {
        if (off_out) {
            vfs_file_put(in);
            vfs_file_put(out);
            return -ESPIPE;
        }
        ret = generic_splice_fallback(
            in, out, len, in_pos_valid ? &in_pos : NULL, NULL, nonblock);
    } else {
        ret =
            generic_splice_fallback(in, out, len, in_pos_valid ? &in_pos : NULL,
                                    out_pos_valid ? &out_pos : NULL, nonblock);
    }

    if (ret > 0) {
        if (in_pos_valid) {
            in_pos += ret;
            if (copy_to_user(off_in, &in_pos, sizeof(in_pos)))
                ret = -EFAULT;
        }
        if (out_pos_valid) {
            out_pos += ret;
            if (copy_to_user(off_out, &out_pos, sizeof(out_pos)))
                ret = -EFAULT;
        }
    }

    vfs_file_put(in);
    vfs_file_put(out);
    return (uint64_t)ret;
}

uint64_t sys_vmsplice(uint64_t fd, const struct iovec *iov, uint64_t nr_segs,
                      uint64_t flags) {
    struct vfs_file *file;
    struct iovec *kiov = NULL;
    size_t iov_bytes = 0;
    size_t total_len = 0;
    ssize_t ret;
    uint64_t vret;

    if (flags & ~SPLICE_F_SUPPORTED)
        return -EINVAL;
    if (!iov && nr_segs)
        return -EFAULT;
    if (nr_segs > UIO_MAXIOV)
        return -EINVAL;
    if (nr_segs && check_user_array_overflow((uint64_t)iov, (size_t)nr_segs,
                                             sizeof(struct iovec), &iov_bytes))
        return -EFAULT;

    if (nr_segs) {
        kiov = malloc(iov_bytes);
        if (!kiov)
            return -ENOMEM;
        if (copy_from_user(kiov, iov, iov_bytes)) {
            free(kiov);
            return -EFAULT;
        }
        vret = rw_validate_iovecs(kiov, nr_segs, &total_len);
        if ((int64_t)vret < 0) {
            free(kiov);
            return vret;
        }
    }

    file = task_get_file(current_task, (int)fd);
    if (!file) {
        free(kiov);
        return -EBADF;
    }
    if (!pipefs_is_pipe(file)) {
        vfs_file_put(file);
        free(kiov);
        return -EINVAL;
    }

    ret = pipefs_splice_from_user(file, kiov, nr_segs, total_len,
                                  !!(flags & SPLICE_F_NONBLOCK));
    vfs_file_put(file);
    free(kiov);
    return (uint64_t)ret;
}

uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    struct vfs_file *file;
    vfs_node_t *node;

    whence &= 0xffffffff;

    file = task_get_file(current_task, (int)fd);
    if (!file) {
        return (uint64_t)-EBADF;
    }

    node = file->f_inode;
    if (fd_get_flags(file) & O_PATH) {
        vfs_file_put(file);
        return (uint64_t)-EBADF;
    }
    if (S_ISSOCK(node->i_mode) || S_ISFIFO(node->i_mode) ||
        S_ISCHR(node->i_mode)) {
        vfs_file_put(file);
        return (uint64_t)-ESPIPE;
    }

    if (whence == SEEK_DATA || whence == SEEK_HOLE) {
        uint64_t new_offset;
        if ((int64_t)offset < 0) {
            vfs_file_put(file);
            return (uint64_t)-EINVAL;
        }
        if (offset >= node->i_size) {
            vfs_file_put(file);
            return (uint64_t)-ENXIO;
        }
        new_offset = whence == SEEK_DATA ? offset : node->i_size;
        fd_set_offset(file, new_offset);
        vfs_file_put(file);
        return new_offset;
    }

    loff_t result = vfs_llseek_file(file, (loff_t)offset, (int)whence);
    vfs_file_put(file);
    return result < 0 ? (uint64_t)result : (uint64_t)result;
}

uint64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    task_t *self = current_task;
    fd_t *f;
    int ret = -ENOSYS;

    f = task_get_file(self, (int)fd);
    if (!f) {
        return (uint64_t)-EBADF;
    }

    if (fd_get_flags(f) & O_PATH) {
        ret = -EBADF;
        goto ret;
    }

    switch (cmd) {
    case FIONBIO:
        if (!arg) {
            ret = -EFAULT;
            goto ret;
        }
        int value = 0;
        if (copy_from_user(&value, (void *)arg, sizeof(value))) {
            ret = -EFAULT;
            goto ret;
        }
        uint64_t file_flags = fd_get_flags(f);
        if (value)
            file_flags |= O_NONBLOCK;
        else
            file_flags &= ~O_NONBLOCK;
        fd_set_flags(f, file_flags);
        ret = 0;
        if (S_ISSOCK(f->f_inode->i_mode)) {
            ret = vfs_ioctl_file(f, cmd, arg);
            if (ret == -ENOTTY || ret == -ENOSYS)
                ret = 0;
        }
        break;
    case FIOCLEX:
        ret = task_set_fd_flags_mask_for_file(self, (int)fd, f, FD_CLOEXEC, 0);
        break;
    case FIONCLEX:
        ret = task_set_fd_flags_mask_for_file(self, (int)fd, f, 0, FD_CLOEXEC);
        break;

    default:
        ret = vfs_ioctl_file(f, cmd, arg);
        break;
    }

    // if (ret == -ENOTTY) {
    //     const char *sb_type =
    //         (f->f_inode && f->f_inode->i_sb && f->f_inode->i_sb->s_type &&
    //          f->f_inode->i_sb->s_type->name)
    //             ? f->f_inode->i_sb->s_type->name
    //             : "<unknown>";
    //     const char *dentry_name =
    //         (f->f_path.dentry && f->f_path.dentry->d_name.name &&
    //          f->f_path.dentry->d_name.name[0])
    //             ? f->f_path.dentry->d_name.name
    //             : "<anonymous>";
    //     const char *inode_type =
    //         f->f_inode ? generic_file_type_name(f->f_inode->type) : "<none>";
    //     char *full_path = NULL;

    //     if (current_task && f->f_path.dentry)
    //         full_path =
    //             vfs_path_to_string(&f->f_path,
    //             task_fs_root_path(current_task));

    //     printk("Ioctl not implemented: fd=%d sb_type=%s inode_type=%s "
    //            "ino=%llu dentry=%s path=%s cmd=%#010x "
    //            "[dir=%u size=%u type=%#x nr=%#x]\n",
    //            fd, sb_type, inode_type,
    //            (unsigned long long)(f->f_inode ? f->f_inode->i_ino : 0),
    //            dentry_name, full_path ? full_path : "<unresolved>", cmd,
    //            (unsigned int)((cmd >> 30) & 0x3),
    //            (unsigned int)((cmd >> 16) & 0x3fff),
    //            (unsigned int)((cmd >> 8) & 0xff), (unsigned int)(cmd &
    //            0xff));
    //     free(full_path);
    // }

ret:
    vfs_file_put(f);
    return ret;
}

static uint64_t generic_rw_copy_iovecs(struct iovec *iovec, uint64_t count,
                                       struct iovec **out_iov,
                                       size_t *out_total) {
    size_t iov_bytes;
    struct iovec *kiov;
    uint64_t ret;

    if (!out_iov || !out_total)
        return (uint64_t)-EINVAL;
    *out_iov = NULL;
    *out_total = 0;

    if (count == 0)
        return 0;
    if (count > UIO_MAXIOV)
        return (uint64_t)-EINVAL;
    if (!iovec || check_user_array_overflow((uint64_t)iovec, (size_t)count,
                                            sizeof(struct iovec), &iov_bytes))
        return (uint64_t)-EFAULT;

    kiov = malloc(iov_bytes);
    if (!kiov)
        return (uint64_t)-ENOMEM;
    if (copy_from_user(kiov, iovec, iov_bytes)) {
        free(kiov);
        return (uint64_t)-EFAULT;
    }

    ret = rw_validate_iovecs(kiov, count, out_total);
    if ((int64_t)ret < 0) {
        free(kiov);
        return ret;
    }

    *out_iov = kiov;
    return 0;
}

static uint64_t generic_do_preadv(uint64_t fd, struct iovec *iovec,
                                  uint64_t count, loff_t pos, bool use_f_pos,
                                  uint64_t flags) {
    struct vfs_file *file;
    struct iovec *kiov;
    size_t total_len;
    ssize_t total_read = 0;
    uint64_t ret;
    bool update_f_pos = false;

    if (flags & ~RWF_SUPPORTED)
        return (uint64_t)-EOPNOTSUPP;
    if (count == 0)
        return 0;

    ret = generic_rw_copy_iovecs(iovec, count, &kiov, &total_len);
    if ((int64_t)ret < 0)
        return ret;
    if (!total_len) {
        free(kiov);
        return 0;
    }

    file = task_get_file(current_task, (int)fd);
    if (!file) {
        free(kiov);
        return (uint64_t)-EBADF;
    }
    if (S_ISDIR(file->f_inode->i_mode)) {
        vfs_file_put(file);
        free(kiov);
        return (uint64_t)-EISDIR;
    }
    if (S_ISSOCK(file->f_inode->i_mode)) {
        int64_t io_ret;

        if (!use_f_pos) {
            vfs_file_put(file);
            free(kiov);
            return (uint64_t)-ESPIPE;
        }
        vfs_file_put(file);
        io_ret = socket_recv_iov((int)fd, kiov, count, 0);
        free(kiov);
        return (uint64_t)io_ret;
    }

    if (use_f_pos && !(file->f_mode & VFS_FMODE_NO_POS_LOCK)) {
        vfs_file_pos_lock(file);
        pos = file->f_pos;
        update_f_pos = true;
    }

    for (uint64_t i = 0; i < count; i++) {
        size_t len = kiov[i].len;
        size_t iov_done = 0;
        loff_t *ppos = use_f_pos ? (update_f_pos ? &pos : NULL) : &pos;

        if (len > MAX_RW_COUNT - (size_t)total_read)
            len = MAX_RW_COUNT - (size_t)total_read;
        if (kiov[i].iov_base == NULL)
            continue;

        while (iov_done < len) {
            ssize_t io_ret = (ssize_t)vfs_read_file(
                file, (uint8_t *)kiov[i].iov_base + iov_done, len - iov_done,
                ppos);
            if (io_ret < 0) {
                if (total_read > 0 &&
                    (io_ret == -EAGAIN || io_ret == -EWOULDBLOCK ||
                     io_ret == -EINTR)) {
                    goto out_readv;
                }
                total_read = total_read ? total_read : io_ret;
                goto out_readv;
            }
            if (io_ret == 0)
                break;
            iov_done += (size_t)io_ret;
            total_read += io_ret;
            if ((size_t)total_read >= MAX_RW_COUNT)
                break;
        }
        if (iov_done < len || (size_t)total_read >= MAX_RW_COUNT)
            break;
    }
out_readv:
    if (update_f_pos) {
        if (total_read > 0)
            file->f_pos = pos;
        vfs_file_pos_unlock(file);
    }
    vfs_file_put(file);
    free(kiov);
    return total_read;
}

static uint64_t generic_do_pwritev(uint64_t fd, struct iovec *iovec,
                                   uint64_t count, loff_t pos, bool use_f_pos,
                                   uint64_t flags) {
    struct vfs_file *file;
    struct iovec *kiov;
    size_t total_len;
    ssize_t total_written = 0;
    uint64_t ret;
    bool append;
    bool pos_locked = false;
    bool update_f_pos = false;

    if (flags & ~RWF_SUPPORTED)
        return (uint64_t)-EOPNOTSUPP;
    if (count == 0)
        return 0;

    ret = generic_rw_copy_iovecs(iovec, count, &kiov, &total_len);
    if ((int64_t)ret < 0)
        return ret;
    if (!total_len) {
        free(kiov);
        return 0;
    }

    file = task_get_file(current_task, (int)fd);
    if (!file) {
        free(kiov);
        return (uint64_t)-EBADF;
    }
    if (S_ISDIR(file->f_inode->i_mode)) {
        vfs_file_put(file);
        free(kiov);
        return (uint64_t)-EISDIR;
    }
    if (S_ISSOCK(file->f_inode->i_mode)) {
        int64_t io_ret;

        if (!use_f_pos) {
            vfs_file_put(file);
            free(kiov);
            return (uint64_t)-ESPIPE;
        }
        vfs_file_put(file);
        io_ret = socket_send_iov((int)fd, kiov, count, 0);
        free(kiov);
        return (uint64_t)io_ret;
    }

    append = (flags & RWF_APPEND) || (file->f_flags & O_APPEND);
    if ((use_f_pos || append) && !(file->f_mode & VFS_FMODE_NO_POS_LOCK)) {
        vfs_file_pos_lock(file);
        pos_locked = true;
    }
    if (use_f_pos && pos_locked) {
        pos = file->f_pos;
        update_f_pos = true;
    }
    if (append)
        pos = (loff_t)file->f_inode->i_size;

    for (uint64_t i = 0; i < count; i++) {
        size_t len = kiov[i].len;
        loff_t *ppos =
            (use_f_pos || append) ? (pos_locked ? &pos : NULL) : &pos;
        size_t iov_done = 0;

        if (len > MAX_RW_COUNT - (size_t)total_written)
            len = MAX_RW_COUNT - (size_t)total_written;
        if (kiov[i].iov_base == NULL)
            continue;
        while (iov_done < len) {
            ssize_t io_ret = (ssize_t)vfs_write_file(
                file, (uint8_t *)kiov[i].iov_base + iov_done, len - iov_done,
                ppos);
            if (io_ret < 0) {
                if (total_written > 0 &&
                    (io_ret == -EAGAIN || io_ret == -EWOULDBLOCK ||
                     io_ret == -EINTR)) {
                    goto out_writev;
                }
                total_written = total_written ? total_written : io_ret;
                goto out_writev;
            }
            iov_done += (size_t)io_ret;
            total_written += io_ret;
            if (io_ret == 0 || (size_t)total_written >= MAX_RW_COUNT)
                break;
        }
        if (iov_done < len || (size_t)total_written >= MAX_RW_COUNT)
            break;
    }
out_writev:
    if (update_f_pos && total_written > 0)
        file->f_pos = pos;
    if (pos_locked)
        vfs_file_pos_unlock(file);
    vfs_file_put(file);
    free(kiov);
    return total_written;
}

uint64_t sys_readv(uint64_t fd, struct iovec *iovec, uint64_t count) {
    return generic_do_preadv(fd, iovec, count, 0, true, 0);
}

uint64_t sys_writev(uint64_t fd, struct iovec *iovec, uint64_t count) {
    return generic_do_pwritev(fd, iovec, count, 0, true, 0);
}

uint64_t sys_preadv(uint64_t fd, struct iovec *iovec, uint64_t count,
                    uint64_t pos_l, uint64_t pos_h) {
    loff_t pos = rw_pos_from_hilo(pos_l, pos_h);

    if (pos < 0)
        return (uint64_t)-EINVAL;
    return generic_do_preadv(fd, iovec, count, pos, false, 0);
}

uint64_t sys_pwritev(uint64_t fd, struct iovec *iovec, uint64_t count,
                     uint64_t pos_l, uint64_t pos_h) {
    loff_t pos = rw_pos_from_hilo(pos_l, pos_h);

    if (pos < 0)
        return (uint64_t)-EINVAL;
    return generic_do_pwritev(fd, iovec, count, pos, false, 0);
}

uint64_t sys_preadv2(uint64_t fd, struct iovec *iovec, uint64_t count,
                     uint64_t pos_l, uint64_t pos_h, uint64_t flags) {
    loff_t pos = rw_pos_from_hilo(pos_l, pos_h);

    if (pos < -1)
        return (uint64_t)-EINVAL;
    return generic_do_preadv(fd, iovec, count, pos, pos == -1, flags);
}

uint64_t sys_pwritev2(uint64_t fd, struct iovec *iovec, uint64_t count,
                      uint64_t pos_l, uint64_t pos_h, uint64_t flags) {
    loff_t pos = rw_pos_from_hilo(pos_l, pos_h);

    if (pos < -1)
        return (uint64_t)-EINVAL;
    return generic_do_pwritev(fd, iovec, count, pos, pos == -1, flags);
}

#define DIRENT_HEADER_SIZE offsetof(struct dirent, d_name)

static inline size_t dirent_reclen(size_t name_len) {
    return (DIRENT_HEADER_SIZE + name_len + 1 + 7) & ~7;
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define DIRENT64_HEADER_SIZE offsetof(struct linux_dirent64, d_name)

static inline size_t dirent64_reclen(size_t name_len) {
    return (DIRENT64_HEADER_SIZE + name_len + 1 + 7) & ~7;
}

struct getdents_state {
    uint8_t *buf;
    uint64_t size;
    uint64_t written;
};

static int getdents_actor(struct vfs_dir_context *ctx, const char *name,
                          int namelen, loff_t next, ino64_t ino,
                          unsigned d_type) {
    struct getdents_state *state = (struct getdents_state *)ctx->private;
    struct dirent *dent;
    size_t reclen;

    if (!state || !name || namelen < 0)
        return -EINVAL;

    reclen = dirent_reclen((size_t)namelen);
    if (state->written + reclen > state->size)
        return -EINVAL;

    dent = (struct dirent *)(state->buf + state->written);
    memset(dent, 0, reclen);
    dent->d_ino = ino;
    dent->d_reclen = reclen;
    dent->d_off = next;
    dent->d_type = d_type;
    memcpy(dent->d_name, name, (size_t)namelen);
    dent->d_name[namelen] = '\0';
    state->written += reclen;
    return 0;
}

static int getdents64_actor(struct vfs_dir_context *ctx, const char *name,
                            int namelen, loff_t next, ino64_t ino,
                            unsigned d_type) {
    struct getdents_state *state = (struct getdents_state *)ctx->private;
    struct linux_dirent64 *dent;
    size_t reclen;

    if (!state || !name || namelen < 0)
        return -EINVAL;

    reclen = dirent64_reclen((size_t)namelen);
    if (state->written + reclen > state->size)
        return -EINVAL;

    dent = (struct linux_dirent64 *)(state->buf + state->written);
    memset(dent, 0, reclen);
    dent->d_ino = (uint64_t)ino;
    dent->d_reclen = (unsigned short)reclen;
    dent->d_off = (int64_t)next;
    dent->d_type = (unsigned char)d_type;
    memcpy(dent->d_name, name, (size_t)namelen);
    dent->d_name[namelen] = '\0';
    state->written += reclen;
    return 0;
}

uint64_t sys_getdents(uint64_t fd, uint64_t buf, uint64_t size) {
    struct vfs_file *file;
    struct vfs_dir_context ctx = {0};
    struct getdents_state state = {0};
    int ret;

    if (check_user_overflow(buf, size) || check_unmapped(buf, size)) {
        return (uint64_t)-EFAULT;
    }

    file = task_get_file(current_task, (int)fd);
    if (!file)
        return (uint64_t)-EBADF;
    if (!S_ISDIR(file->f_inode->i_mode)) {
        vfs_file_put(file);
        return (uint64_t)-ENOTDIR;
    }

    state.buf = (uint8_t *)buf;
    state.size = size;
    ctx.pos = file->f_pos;
    ctx.actor = getdents_actor;
    ctx.private = &state;

    ret = vfs_iterate_dir(file, &ctx);
    vfs_file_put(file);
    if (ret < 0 && state.written == 0)
        return (uint64_t)ret;
    return state.written;
}

uint64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t size) {
    struct vfs_file *file;
    struct vfs_dir_context ctx = {0};
    struct getdents_state state = {0};
    int ret;

    if (check_user_overflow(buf, size) || check_unmapped(buf, size))
        return (uint64_t)-EFAULT;

    file = task_get_file(current_task, (int)fd);
    if (!file)
        return (uint64_t)-EBADF;
    if (!S_ISDIR(file->f_inode->i_mode)) {
        vfs_file_put(file);
        return (uint64_t)-ENOTDIR;
    }

    state.buf = (uint8_t *)buf;
    state.size = size;
    ctx.pos = file->f_pos;
    ctx.actor = getdents64_actor;
    ctx.private = &state;

    ret = vfs_iterate_dir(file, &ctx);
    vfs_file_put(file);
    if (ret < 0 && state.written == 0)
        return (uint64_t)ret;
    return state.written;
}

uint64_t sys_chdir(const char *dname) {
    char dirname[512];
    struct vfs_path path = {0};
    int ret;

    if (copy_from_user_str(dirname, dname, sizeof(dirname)))
        return (uint64_t)-EFAULT;

    ret = vfs_filename_lookup(AT_FDCWD, dirname, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    if (!path.dentry->d_inode || !S_ISDIR(path.dentry->d_inode->i_mode)) {
        vfs_path_put(&path);
        return (uint64_t)-ENOTDIR;
    }
    ret = task_fs_chdir(current_task, &path);
    vfs_path_put(&path);
    return ret;
}

uint64_t sys_pivot_root(const char *new_root_user, const char *put_old_user) {
    char new_root_name[VFS_PATH_MAX];
    char put_old_name[VFS_PATH_MAX];
    struct vfs_path old_root = {0};
    struct vfs_path new_root = {0};
    struct vfs_path put_old = {0};
    int ret;

    if (!new_root_user || !put_old_user || !current_task)
        return (uint64_t)-EINVAL;
    if (copy_from_user_str(new_root_name, new_root_user, sizeof(new_root_name)))
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(put_old_name, put_old_user, sizeof(put_old_name)))
        return (uint64_t)-EFAULT;

    old_root = *task_fs_root_path(current_task);
    if (!old_root.mnt || !old_root.dentry)
        return (uint64_t)-EINVAL;
    if (!vfs_path_get(&old_root))
        return (uint64_t)-ENOENT;

    ret =
        vfs_filename_lookup(AT_FDCWD, new_root_name, LOOKUP_FOLLOW, &new_root);
    if (ret < 0)
        goto out;

    ret = vfs_filename_lookup(AT_FDCWD, put_old_name,
                              LOOKUP_FOLLOW | LOOKUP_NO_LAST_MOUNT, &put_old);
    if (ret < 0)
        goto out;

    if (!old_root.dentry->d_inode || !new_root.dentry ||
        !new_root.dentry->d_inode || !put_old.dentry ||
        !put_old.dentry->d_inode) {
        ret = -ENOENT;
        goto out;
    }
    if (!S_ISDIR(new_root.dentry->d_inode->i_mode) ||
        !S_ISDIR(put_old.dentry->d_inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    if (old_root.dentry != old_root.mnt->mnt_root) {
        ret = -EINVAL;
        goto out;
    }
    if (task_mount_namespace_root(current_task) != old_root.mnt ||
        old_root.mnt->mnt_parent != old_root.mnt) {
        ret = -EINVAL;
        goto out;
    }
    if (new_root.dentry != new_root.mnt->mnt_root) {
        ret = -EINVAL;
        goto out;
    }
    if (!vfs_path_is_ancestor(&old_root, &new_root) ||
        !vfs_path_is_ancestor(&old_root, &put_old) ||
        !vfs_path_is_ancestor(&new_root, &put_old)) {
        ret = -EINVAL;
        goto out;
    }
    if (new_root.mnt == old_root.mnt || put_old.mnt == old_root.mnt) {
        ret = -EBUSY;
        goto out;
    }
    if (new_root.mnt->mnt_parent && new_root.mnt->mnt_parent != new_root.mnt &&
        vfs_mount_is_shared(new_root.mnt->mnt_parent)) {
        ret = -EINVAL;
        goto out;
    }
    if (old_root.mnt->mnt_parent && old_root.mnt->mnt_parent != old_root.mnt &&
        vfs_mount_is_shared(old_root.mnt->mnt_parent)) {
        ret = -EINVAL;
        goto out;
    }

    ret = vfs_pivot_root_mounts(old_root.mnt, new_root.mnt, &put_old);
    if (ret < 0)
        goto out;

    ret = task_mount_namespace_pivot_root(current_task, &old_root, &new_root);

out:
    vfs_path_put(&put_old);
    vfs_path_put(&new_root);
    vfs_path_put(&old_root);
    return ret < 0 ? (uint64_t)ret : 0;
}

uint64_t sys_chroot(const char *dname) {
    char dirname[512];
    struct vfs_path path = {0};
    int ret;

    if (copy_from_user_str(dirname, dname, sizeof(dirname)))
        return (uint64_t)-EFAULT;

    ret = vfs_filename_lookup(AT_FDCWD, dirname, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    if (!path.dentry->d_inode || !S_ISDIR(path.dentry->d_inode->i_mode)) {
        vfs_path_put(&path);
        return (uint64_t)-ENOTDIR;
    }

    ret = task_fs_chroot(current_task, &path);
    if (ret < 0) {
        vfs_path_put(&path);
        return (uint64_t)ret;
    }

    vfs_path_put(&path);

    return 0;
}

uint64_t sys_getcwd(char *cwd, uint64_t size) {
    static const char unreachable_prefix[] = "(unreachable)";
    const struct vfs_path *pwd = task_fs_pwd_path(current_task);
    const struct vfs_path *root = task_fs_root_path(current_task);
    struct vfs_path ns_root = {0};
    char *path = NULL;
    char *str = NULL;
    uint64_t to_copy;

    if (vfs_path_is_ancestor(root, pwd)) {
        str = vfs_path_to_string(pwd, root);
    } else {
        ns_root.mnt = task_mount_namespace_root(current_task);
        ns_root.dentry = ns_root.mnt ? ns_root.mnt->mnt_root : NULL;
        path = vfs_path_to_string(pwd, ns_root.mnt ? &ns_root : NULL);
        if (!path)
            return (uint64_t)-ENOMEM;

        str = malloc(sizeof(unreachable_prefix) + strlen(path));
        if (!str) {
            free(path);
            return (uint64_t)-ENOMEM;
        }

        memcpy(str, unreachable_prefix, sizeof(unreachable_prefix) - 1);
        strcpy(str + sizeof(unreachable_prefix) - 1, path);
        free(path);
    }

    if (!str)
        return (uint64_t)-ENOMEM;
    to_copy = strlen(str) + 1;
    if (size < to_copy) {
        free(str);
        return (uint64_t)-ERANGE;
    }
    if (copy_to_user_str(cwd, str, size)) {
        free(str);
        return (uint64_t)-EFAULT;
    }
    free(str);
    return to_copy;
}

extern int unix_socket_fsid;
extern int unix_accept_fsid;

static uint64_t dup_to_exact(task_t *self, uint64_t fd, uint64_t newfd,
                             bool cloexec, bool allow_same_fd) {
    if (!self)
        return (uint64_t)-EBADF;
    if (fd >= self->rlim[RLIMIT_NOFILE].rlim_cur)
        return (uint64_t)-EBADF;
    if (newfd >= self->rlim[RLIMIT_NOFILE].rlim_cur)
        return (uint64_t)-EBADF;

    uint64_t ret = newfd;
    bool replaced_existing = false;
    bool installed_new = false;
    fd_t *replaced_fd = NULL;
    unsigned int new_fd_flags = 0;
    fd_info_t *fd_info = task_fd_info_get(self);
    if (!fd_info)
        return (uint64_t)-EBADF;
    with_fd_info_lock(fd_info, {
        if (fd >= fd_info->max_fds) {
            ret = (uint64_t)-EBADF;
            break;
        }
        if (newfd >= fd_info->max_fds &&
            task_fd_info_expand(fd_info, (size_t)newfd + 1) < 0) {
            ret = (uint64_t)-ENOMEM;
            break;
        }

        if (!fd_info->fds[fd].file) {
            ret = (uint64_t)-EBADF;
            break;
        }

        if (fd == newfd) {
            if (allow_same_fd) {
                ret = newfd;
            } else {
                ret = (uint64_t)-EINVAL;
            }
            break;
        }

        fd_t *newf = vfs_file_get(fd_info->fds[fd].file);
        if (!newf) {
            ret = (uint64_t)-ENOSPC;
            break;
        }

        if (fd_info->fds[newfd].file) {
            replaced_fd = task_fd_slot_take(fd_info, (int)newfd, NULL);
            replaced_existing = true;
        }

        new_fd_flags = cloexec ? FD_CLOEXEC : 0;
        if (task_fd_slot_install(fd_info, (int)newfd, newf, new_fd_flags) < 0) {
            vfs_file_put(newf);
            if (replaced_fd)
                task_fd_slot_install(fd_info, (int)newfd, replaced_fd, 0);
            ret = (uint64_t)-ENOMEM;
            break;
        }
        vfs_file_put(newf);
        installed_new = true;
    });
    task_fd_info_put(fd_info, self);

    if ((int64_t)ret >= 0 && installed_new) {
        if (replaced_existing) {
            on_close_file_call(self, newfd, replaced_fd);
            vfs_close_file(replaced_fd);
        }
        on_open_file_call(self, newfd);
    }

    return ret;
}

uint64_t sys_dup2(uint64_t fd, uint64_t newfd) {
    task_t *self = current_task;
    return dup_to_exact(self, fd, newfd, false, true);
}

static uint64_t dup_to_free_slot(task_t *self, uint64_t fd, uint64_t start,
                                 bool cloexec) {
    if (!self)
        return (uint64_t)-EBADF;
    if (fd >= self->rlim[RLIMIT_NOFILE].rlim_cur)
        return (uint64_t)-EBADF;
    if (start >= self->rlim[RLIMIT_NOFILE].rlim_cur)
        return (uint64_t)-EINVAL;

    uint64_t ret = (uint64_t)-EBADF;
    int installed_fd = -1;
    fd_info_t *fd_info = task_fd_info_get(self);
    if (!fd_info)
        return (uint64_t)-EBADF;
    with_fd_info_lock(fd_info, {
        if (fd >= fd_info->max_fds) {
            ret = (uint64_t)-EBADF;
            break;
        }

        if (!fd_info->fds[fd].file) {
            ret = (uint64_t)-EBADF;
            break;
        }

        uint64_t i;
        size_t limit =
            MIN(fd_info->max_fds, self->rlim[RLIMIT_NOFILE].rlim_cur);
        for (i = MAX(start, fd_info->next_fd); i < limit; i++) {
            if (!fd_info->fds[i].file)
                break;
        }

        if (i == limit &&
            self->rlim[RLIMIT_NOFILE].rlim_cur > fd_info->max_fds) {
            size_t old_max = fd_info->max_fds;
            size_t new_max = MIN(self->rlim[RLIMIT_NOFILE].rlim_cur,
                                 MAX(old_max * 2, old_max + 1));
            if (start >= new_max)
                new_max = start + 1;
            if (task_fd_info_expand(fd_info, new_max) < 0) {
                ret = (uint64_t)-ENOMEM;
                break;
            }
            limit = MIN(fd_info->max_fds, self->rlim[RLIMIT_NOFILE].rlim_cur);
            for (i = MAX(start, old_max); i < limit; i++) {
                if (!fd_info->fds[i].file)
                    break;
            }
        }

        if (i == limit) {
            ret = (uint64_t)-EMFILE;
            break;
        }

        fd_t *newf = vfs_file_get(fd_info->fds[fd].file);
        if (!newf) {
            ret = (uint64_t)-ENOSPC;
            break;
        }

        if (task_fd_slot_install(fd_info, (int)i, newf,
                                 cloexec ? FD_CLOEXEC : 0) < 0) {
            vfs_file_put(newf);
            ret = (uint64_t)-ENOMEM;
            break;
        }
        vfs_file_put(newf);
        installed_fd = (int)i;
        ret = i;
    });
    task_fd_info_put(fd_info, self);

    if (installed_fd >= 0)
        on_open_file_call(self, installed_fd);

    return ret;
}

static uint64_t fcntl_dupfd_query(task_t *self, fd_t *file, uint64_t arg) {
    int query_fd = (int)arg;
    fd_t *query_file;
    uint64_t ret;

    if (!self || !file)
        return (uint64_t)-EBADF;

    query_file = task_get_file(self, query_fd);
    if (!query_file)
        return (uint64_t)-EBADF;

    ret = (query_file == file) ? 1 : 0;
    vfs_file_put(query_file);
    return ret;
}

uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    if (flags & ~O_CLOEXEC)
        return (uint64_t)-EINVAL;
    task_t *self = current_task;
    return dup_to_exact(self, oldfd, newfd, !!(flags & O_CLOEXEC), false);
}

uint64_t sys_dup(uint64_t fd) {
    task_t *self = current_task;
    return dup_to_free_slot(self, fd, 0, false);
}

#define RWF_WRITE_LIFE_NOT_SET 0
#define RWH_WRITE_LIFE_NONE 1
#define RWH_WRITE_LIFE_SHORT 2
#define RWH_WRITE_LIFE_MEDIUM 3
#define RWH_WRITE_LIFE_LONG 4
#define RWH_WRITE_LIFE_EXTREME 5

uint64_t sys_fcntl(uint64_t fd, uint64_t command, uint64_t arg) {
    task_t *self = current_task;
    fd_t *file = task_get_file(self, (int)fd);
    uint64_t out = (uint64_t)-EINVAL;
    unsigned int fd_flags = 0;
    if (!file)
        return (uint64_t)-EBADF;

    switch (command) {
    case F_GETFD:
        if (task_get_fd_flags_for_file(self, (int)fd, file, &fd_flags) < 0) {
            out = (uint64_t)-EBADF;
            break;
        }
        out = (fd_flags & FD_CLOEXEC) ? FD_CLOEXEC : 0;
        break;
    case F_SETFD:
        out = task_set_fd_flags_mask_for_file(
            self, (int)fd, file, (arg & FD_CLOEXEC) ? FD_CLOEXEC : 0,
            FD_CLOEXEC);
        break;
    case F_DUPFD_CLOEXEC:
        out = dup_to_free_slot(self, fd, arg, true);
        break;
    case F_DUPFD:
        out = dup_to_free_slot(self, fd, arg, false);
        break;
    case F_DUPFD_QUERY:
        out = fcntl_dupfd_query(self, file, arg);
        break;
    case F_GETFL:
        out = fd_get_flags(file);
        break;
    case F_SETFL:
        if (fd_get_flags(file) & O_PATH) {
            out = (uint64_t)-EBADF;
            break;
        }
        uint64_t file_flags = fd_get_flags(file);
        uint64_t valid_flags = O_SETFL_FLAGS;
        file_flags = (file_flags & ~valid_flags) | (arg & valid_flags);
        fd_set_flags(file, file_flags);
        int ret = 0;
        if (S_ISSOCK(file->f_inode->i_mode)) {
            int nonblock = !!(file_flags & O_NONBLOCK);
            ret = vfs_ioctl_file(file, FIONBIO,
                                 nonblock ? FIONBIO_INTERNAL_ENABLE
                                          : FIONBIO_INTERNAL_DISABLE);
            if (ret == -ENOTTY || ret == -ENOSYS)
                ret = 0;
        }
        out = ret;
        break;
    case F_GETLK: {
        flock_t lock;
        if (check_user_overflow(arg, sizeof(lock))) {
            out = -EFAULT;
            break;
        }
        memcpy(&lock, (void *)arg, sizeof(lock));
        int getlk_ret = file_lock_getlk(file, &lock, false);
        if (getlk_ret < 0) {
            out = getlk_ret;
            break;
        }
        if (copy_to_user((void *)arg, &lock, sizeof(lock))) {
            out = -EFAULT;
            break;
        }
        out = 0;
        break;
    }
    case F_OFD_GETLK: {
        flock_t lock;
        if (check_user_overflow(arg, sizeof(lock))) {
            out = -EFAULT;
            break;
        }
        memcpy(&lock, (void *)arg, sizeof(lock));
        int getlk_ret = file_lock_getlk(file, &lock, true);
        if (getlk_ret < 0) {
            out = getlk_ret;
            break;
        }
        if (copy_to_user((void *)arg, &lock, sizeof(lock))) {
            out = -EFAULT;
            break;
        }
        out = 0;
        break;
    }
    case F_SETLKW:
    case F_SETLK: {
        flock_t lock;
        if (check_user_overflow(arg, sizeof(lock))) {
            out = -EFAULT;
            break;
        }
        memcpy(&lock, (void *)arg, sizeof(lock));

        if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK &&
            lock.l_type != F_UNLCK) {
            out = -EINVAL;
            break;
        }

        out = file_lock_setlk(file, &lock, command == F_SETLKW, false);
        break;
    }
    case F_OFD_SETLKW:
    case F_OFD_SETLK: {
        flock_t lock;
        if (check_user_overflow(arg, sizeof(lock))) {
            out = -EFAULT;
            break;
        }
        memcpy(&lock, (void *)arg, sizeof(lock));

        if (lock.l_type != F_RDLCK && lock.l_type != F_WRLCK &&
            lock.l_type != F_UNLCK) {
            out = -EINVAL;
            break;
        }

        out = file_lock_setlk(file, &lock, command == F_OFD_SETLKW, true);
        break;
    }
    case F_GETPIPE_SZ:
        out = 512 * 1024;
        break;
    case F_SETPIPE_SZ:
        out = 0;
        break;
    case F_GET_SEALS:
        if (file->private_data) {
            struct memfd_ctx {
                vfs_node_t *node;
                char name[64];
                uint64_t *pages;
                size_t page_slots;
                uint64_t len;
                int flags;
                unsigned int seals;
                spinlock_t lock;
            };
            struct memfd_ctx *ctx = file->private_data;
            out = ctx->seals;
        } else {
            out = -EINVAL;
        }
        break;
    case F_ADD_SEALS:
        if (file->private_data) {
            struct memfd_ctx {
                vfs_node_t *node;
                char name[64];
                uint64_t *pages;
                size_t page_slots;
                uint64_t len;
                int flags;
                unsigned int seals;
                spinlock_t lock;
            };
            struct memfd_ctx *ctx = file->private_data;
            unsigned int seals = (unsigned int)arg;
            unsigned int allowed =
                F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
            if (seals & ~allowed) {
                out = -EINVAL;
                break;
            }
            if (!(ctx->flags & 0x0002U)) {
                out = -EPERM;
                break;
            }
            if (ctx->seals & F_SEAL_SEAL) {
                out = -EPERM;
                break;
            }
            ctx->seals |= seals;
            out = 0;
        } else {
            out = -EINVAL;
        }
        break;
    case F_GET_RW_HINT:
        if (!file->f_inode->rw_hint) {
            out = 0;
            break;
        }
        out = file->f_inode->rw_hint;
        break;

    case F_SET_RW_HINT:
        if (arg < RWH_WRITE_LIFE_NONE || arg > RWH_WRITE_LIFE_EXTREME) {
            out = -EINVAL;
            break;
        }
        file->f_inode->rw_hint = arg;
        out = 0;
        break;
    default:
        printk("Unsupported fcntl command %#010lx\n", command);
        out = (uint64_t)-EINVAL;
        break;
    }

    vfs_file_put(file);
    return out;
}

uint64_t do_stat_path(const char *path, struct stat *buf) {
    struct vfs_kstat stat;
    int ret;

    ret = vfs_statx(AT_FDCWD, path, 0, 0, &stat);
    if (ret < 0)
        return (uint64_t)ret;

    generic_fill_stat_from_kstat(buf, &stat);
    return 0;
}

uint64_t sys_stat(const char *fn, struct stat *user_buf) {
    char path[512];
    if (copy_from_user_str(path, fn, sizeof(path)))
        return (uint64_t)-EFAULT;

    struct stat buf;
    uint64_t ret = do_stat_path(path, &buf);
    if ((int64_t)ret < 0)
        return ret;

    if (copy_to_user(user_buf, &buf, sizeof(struct stat)))
        return (uint64_t)-EFAULT;

    return 0;
}

uint64_t do_stat_fd(int fd, struct stat *buf) {
    struct vfs_path path = {0};
    struct vfs_kstat stat;
    int ret;

    ret = generic_get_fd_path(fd, &path);
    if (ret < 0)
        return ret;
    vfs_fill_generic_kstat(&path, &stat);
    generic_fill_stat_from_kstat(buf, &stat);
    vfs_path_put(&path);
    return 0;
}

uint64_t sys_fstat(uint64_t fd, struct stat *user_buf) {
    struct stat res;
    int ret = do_stat_fd(fd, &res);
    if (ret < 0)
        return ret;

    if (copy_to_user(user_buf, &res, sizeof(struct stat)))
        return (uint64_t)-EFAULT;

    return 0;
}

uint64_t sys_newfstatat(uint64_t dirfd, const char *pathname_user,
                        struct stat *buf_user, uint64_t flags) {
    char pathname[512];
    if (copy_from_user_str(pathname, pathname_user, sizeof(pathname))) {
        if (pathname_user &&
            !check_user_overflow((uint64_t)pathname_user, sizeof(pathname)))
            return (uint64_t)-ENAMETOOLONG;
        return (uint64_t)-EFAULT;
    }

    if ((flags & AT_EMPTY_PATH) && pathname[0] == '\0') {
        return sys_fstat(dirfd, buf_user);
    }

    struct stat buf;
    struct vfs_kstat stat;
    uint64_t ret = (uint64_t)vfs_statx(
        (int)dirfd, pathname,
        (flags & AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0, 0, &stat);
    if ((int64_t)ret < 0)
        return ret;
    generic_fill_stat_from_kstat(&buf, &stat);

    if (copy_to_user(buf_user, &buf, sizeof(struct stat)))
        return (uint64_t)-EFAULT;

    return 0;
}

uint64_t sys_statx(uint64_t dirfd, const char *pathname_user, uint64_t flags,
                   uint64_t mask, struct statx *buff_user) {
    struct vfs_path path = {0};
    struct vfs_kstat stat;
    char pathname[512] = {0};
    if (pathname_user &&
        copy_from_user_str(pathname, pathname_user, sizeof(pathname))) {
        if (!check_user_overflow((uint64_t)pathname_user, sizeof(pathname)))
            return (uint64_t)-ENAMETOOLONG;
        return (uint64_t)-EFAULT;
    }

    struct statx res;
    struct statx *buff = &res;
    int ret;

    if ((flags & AT_EMPTY_PATH) && pathname[0] == '\0') {
        ret = generic_get_fd_path((int)dirfd, &path);
        if (ret < 0)
            return (uint64_t)ret;
        vfs_fill_generic_kstat(&path, &stat);
    } else {
        ret = vfs_filename_lookup(
            (int)dirfd, pathname,
            (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW,
            &path);
        if (ret < 0)
            return (uint64_t)ret;
        if (path.dentry->d_inode && path.dentry->d_inode->i_op &&
            path.dentry->d_inode->i_op->getattr) {
            ret =
                path.dentry->d_inode->i_op->getattr(&path, &stat, mask, flags);
            if (ret < 0) {
                vfs_path_put(&path);
                return (uint64_t)ret;
            }
        } else {
            vfs_fill_generic_kstat(&path, &stat);
        }
    }

    generic_fill_statx_from_kstat(buff, &stat, &path, mask);
    if (copy_to_user(buff_user, buff, sizeof(struct statx))) {
        vfs_path_put(&path);
        return (uint64_t)-EFAULT;
    }
    vfs_path_put(&path);

    return 0;
}

size_t do_access(char *filename, int mode) {
    struct vfs_path path = {0};
    int ret;

    ret = vfs_filename_lookup(AT_FDCWD, filename, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return ret;
    if (path.dentry->d_inode && path.dentry->d_inode->i_op &&
        path.dentry->d_inode->i_op->permission) {
        ret =
            path.dentry->d_inode->i_op->permission(path.dentry->d_inode, mode);
        vfs_path_put(&path);
        return ret;
    }
    vfs_path_put(&path);
    return 0;
}

size_t sys_access(char *filename_user, int mode) {
    (void)mode;
    char filename[512];
    if (copy_from_user_str(filename, filename_user, sizeof(filename)))
        return (size_t)-EFAULT;

    return do_access(filename, mode);
}

uint64_t sys_faccessat(uint64_t dirfd, const char *pathname_user,
                       uint64_t mode) {
    struct vfs_path path = {0};
    int ret;

    char pathname[512];
    if (copy_from_user_str(pathname, pathname_user, sizeof(pathname)))
        return (uint64_t)-EFAULT;

    if (pathname[0] == '\0') { // by fd
        return 0;
    }

    ret = vfs_filename_lookup((int)dirfd, pathname, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    if (path.dentry->d_inode && path.dentry->d_inode->i_op &&
        path.dentry->d_inode->i_op->permission) {
        ret = path.dentry->d_inode->i_op->permission(path.dentry->d_inode,
                                                     (int)mode);
    } else {
        ret = 0;
    }
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_faccessat2(uint64_t dirfd, const char *pathname_user,
                        uint64_t mode, uint64_t flags) {
    struct vfs_path path = {0};
    int ret;

    char pathname[512];
    if (copy_from_user_str(pathname, pathname_user, sizeof(pathname)))
        return (uint64_t)-EFAULT;

    if ((flags & AT_EMPTY_PATH) && pathname[0] == '\0') { // by fd
        return 0;
    }

    ret = vfs_filename_lookup(
        (int)dirfd, pathname,
        (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    if (path.dentry->d_inode && path.dentry->d_inode->i_op &&
        path.dentry->d_inode->i_op->permission) {
        ret = path.dentry->d_inode->i_op->permission(path.dentry->d_inode,
                                                     (int)mode);
    } else {
        ret = 0;
    }
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t do_readlink(char *path, char *buf, uint64_t size) {
    struct vfs_path vpath = {0};
    const char *target;
    size_t len;
    int ret;

    ret = vfs_filename_lookup(AT_FDCWD, path, LOOKUP_NOFOLLOW, &vpath);
    if (ret < 0)
        return (uint64_t)ret;
    if (!vpath.dentry->d_inode || !S_ISLNK(vpath.dentry->d_inode->i_mode) ||
        !vpath.dentry->d_inode->i_op ||
        !vpath.dentry->d_inode->i_op->get_link) {
        vfs_path_put(&vpath);
        return (uint64_t)-EINVAL;
    }

    target = vpath.dentry->d_inode->i_op->get_link(vpath.dentry,
                                                   vpath.dentry->d_inode, NULL);
    if (IS_ERR_OR_NULL(target)) {
        vfs_path_put(&vpath);
        return target ? (uint64_t)PTR_ERR(target) : (uint64_t)-ENOENT;
    }

    len = MIN((size_t)size, strlen(target));
    memcpy(buf, target, len);
    if (vpath.dentry->d_inode->i_op->put_link)
        vpath.dentry->d_inode->i_op->put_link(vpath.dentry->d_inode, target);
    vfs_path_put(&vpath);
    return len;
}

uint64_t sys_readlink(char *path_user, char *buf_user, uint64_t size) {
    if (path_user == NULL || buf_user == NULL || size == 0) {
        return (uint64_t)-EFAULT;
    }

    char path[512];
    if (copy_from_user_str(path, path_user, sizeof(path)))
        return (uint64_t)-EFAULT;

    char *buf = malloc(size);
    if (!buf)
        return (uint64_t)-ENOMEM;
    memset(buf, 0, size);

    ssize_t result = do_readlink(path, buf, size);

    if (result < 0) {
        free(buf);
        return (uint64_t)result;
    }

    if (copy_to_user(buf_user, buf, result)) {
        free(buf);
        return (uint64_t)-EFAULT;
    }

    free(buf);

    return result;
}

uint64_t sys_readlinkat(int dfd, char *path_user, char *buf_user,
                        uint64_t size) {
    struct vfs_path vpath = {0};
    const char *target;
    size_t len;
    int ret;

    if (path_user == NULL || buf_user == NULL || size == 0) {
        return (uint64_t)-EFAULT;
    }

    char path[512];

    if (copy_from_user_str(path, path_user, sizeof(path)))
        return (uint64_t)-EFAULT;

    if (path[0] == '\0') {
        fd_t *file;

        if (dfd == AT_FDCWD)
            return (uint64_t)-ENOENT;

        file = task_get_file(current_task, dfd);
        if (!file)
            return (uint64_t)-EBADF;
        if (!vfs_path_copy(&vpath, &file->f_path))
            ret = -ENOENT;
        else
            ret = 0;
        vfs_file_put(file);
    } else {
        ret = vfs_filename_lookup(dfd, path, LOOKUP_NOFOLLOW, &vpath);
    }
    if (ret < 0)
        return (uint64_t)ret;
    if (!vpath.dentry->d_inode || !S_ISLNK(vpath.dentry->d_inode->i_mode) ||
        !vpath.dentry->d_inode->i_op ||
        !vpath.dentry->d_inode->i_op->get_link) {
        vfs_path_put(&vpath);
        return (uint64_t)-EINVAL;
    }

    target = vpath.dentry->d_inode->i_op->get_link(vpath.dentry,
                                                   vpath.dentry->d_inode, NULL);
    if (IS_ERR_OR_NULL(target)) {
        vfs_path_put(&vpath);
        return target ? (uint64_t)PTR_ERR(target) : (uint64_t)-ENOENT;
    }
    len = MIN((size_t)size, strlen(target));
    ret = copy_to_user(buf_user, target, len);
    if (vpath.dentry->d_inode->i_op->put_link)
        vpath.dentry->d_inode->i_op->put_link(vpath.dentry->d_inode, target);
    vfs_path_put(&vpath);
    return ret ? (uint64_t)-EFAULT : (uint64_t)len;
}

uint64_t sys_rmdir(const char *name_user) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name))) {
        return (uint64_t)-EFAULT;
    }
    return (uint64_t)vfs_unlinkat(AT_FDCWD, name, AT_REMOVEDIR, false);
}

static uint64_t do_unlink(const char *name) {
    return (uint64_t)vfs_unlinkat(AT_FDCWD, name, 0, false);
}

uint64_t sys_unlink(const char *name_user) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name))) {
        return (uint64_t)-EFAULT;
    }

    return do_unlink(name);
}

uint64_t sys_unlinkat(uint64_t dirfd, const char *name_user, uint64_t flags) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name))) {
        return (uint64_t)-EFAULT;
    }
    return (uint64_t)vfs_unlinkat((int)dirfd, name, (int)flags, false);
}

uint64_t do_rename(const char *old, const char *new) {
    return (uint64_t)vfs_renameat2(AT_FDCWD, old, AT_FDCWD, new, 0, false);
}

uint64_t sys_rename(const char *old_user, const char *new_user) {
    char old[512];
    char new[512];

    if (copy_from_user_str(old, old_user, sizeof(old)))
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(new, new_user, sizeof(new)))
        return (uint64_t)-EFAULT;

    return do_rename(old, new);
}

uint64_t sys_renameat(uint64_t oldfd, const char *old_user, uint64_t newfd,
                      const char *new_user) {
    char old[512];
    char new[512];

    if (copy_from_user_str(old, old_user, sizeof(old)))
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(new, new_user, sizeof(new)))
        return (uint64_t)-EFAULT;

    return (uint64_t)vfs_renameat2((int)oldfd, old, (int)newfd, new, 0, false);
}

uint64_t sys_renameat2(uint64_t oldfd, const char *old_user, uint64_t newfd,
                       const char *new_user, uint64_t flags) {
    char old[512];
    char new[512];

    if (copy_from_user_str(old, old_user, sizeof(old)))
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(new, new_user, sizeof(new)))
        return (uint64_t)-EFAULT;

    return (uint64_t)vfs_renameat2((int)oldfd, old, (int)newfd, new,
                                   (unsigned int)flags, false);
}

uint64_t sys_fchdir(uint64_t fd) {
    struct vfs_path path = {0};
    int ret = generic_get_fd_path((int)fd, &path);
    if (ret < 0)
        return ret;
    if (!path.dentry->d_inode || !S_ISDIR(path.dentry->d_inode->i_mode)) {
        vfs_path_put(&path);
        return -ENOTDIR;
    }
    ret = task_fs_chdir(current_task, &path);
    vfs_path_put(&path);
    return ret;
}

uint64_t do_mkdir(const char *name, uint64_t mode) {
    umode_t dir_mode = (umode_t)(mode & 0777);
    if (current_task && current_task->fs)
        dir_mode &= ~current_task->fs->umask;
    return (uint64_t)vfs_mkdirat(AT_FDCWD, name, dir_mode, false);
}

uint64_t sys_mkdir(const char *name_user, uint64_t mode) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;

    return do_mkdir(name, mode);
}

uint64_t sys_mkdirat(int dfd, const char *name_user, uint64_t mode) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    umode_t dir_mode = (umode_t)(mode & 0777);
    if (current_task && current_task->fs)
        dir_mode &= ~current_task->fs->umask;
    return (uint64_t)vfs_mkdirat((int)dfd, name, dir_mode, false);
}

uint64_t do_link(const char *name, const char *new) {
    return (uint64_t)vfs_linkat(AT_FDCWD, name, AT_FDCWD, new, 0, false);
}

static int resolve_linkat_source_path(uint64_t olddirfd, const char *oldpath,
                                      int flags, struct vfs_path *source_out) {
    if (!source_out)
        return -EINVAL;
    memset(source_out, 0, sizeof(*source_out));

    if ((flags & AT_EMPTY_PATH) && oldpath && oldpath[0] == '\0') {
        fd_t *file = task_get_file(current_task, (int)olddirfd);
        if (!file)
            return -EBADF;
        if (!vfs_path_copy(source_out, &file->f_path)) {
            vfs_file_put(file);
            return -ENOENT;
        }
        vfs_file_put(file);
        return 1;
    }

    if ((flags & AT_SYMLINK_FOLLOW) && oldpath) {
        int ret = vfs_filename_lookup((int)olddirfd, oldpath, LOOKUP_FOLLOW,
                                      source_out);
        if (ret < 0)
            return ret;
        return 1;
    }

    return 0;
}

uint64_t sys_link(const char *name_user, const char *new_user) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    char new[512];
    if (copy_from_user_str(new, new_user, sizeof(new)))
        return (uint64_t)-EFAULT;

    return do_link(name, new);
}

uint64_t do_symlink(const char *name, const char *new) {
    return (uint64_t)vfs_symlinkat(name, AT_FDCWD, new, false);
}

uint64_t sys_symlink(const char *name_user, const char *target_name_user) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    char target_name[512];
    if (copy_from_user_str(target_name, target_name_user, sizeof(target_name)))
        return (uint64_t)-EFAULT;

    return do_symlink(name, target_name);
}

uint64_t sys_linkat(uint64_t olddirfd, const char *oldpath_user,
                    uint64_t newdirfd, const char *newpath_user, int flags) {
    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW))
        return (uint64_t)-EINVAL;

    char oldpath[512];
    if (copy_from_user_str(oldpath, oldpath_user, sizeof(oldpath)))
        return (uint64_t)-EFAULT;
    char newpath[512];
    if (copy_from_user_str(newpath, newpath_user, sizeof(newpath)))
        return (uint64_t)-EFAULT;

    struct vfs_path source = {0};
    int source_ret =
        resolve_linkat_source_path(olddirfd, oldpath, flags, &source);
    int ret = 0;

    if (source_ret < 0) {
        return (uint64_t)source_ret;
    }

    if (source_ret > 0) {
        ret = generic_link_pathat(&source, (int)newdirfd, newpath);
        vfs_path_put(&source);
    } else {
        ret = vfs_linkat((int)olddirfd, oldpath, (int)newdirfd, newpath,
                         flags & AT_SYMLINK_FOLLOW, false);
    }

    return ret;
}

uint64_t sys_symlinkat(const char *name_user, int dfd, const char *new_user) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    char new[512];
    if (copy_from_user_str(new, new_user, sizeof(new)))
        return (uint64_t)-EFAULT;
    return (uint64_t)vfs_symlinkat(name, dfd, new, false);
}

uint64_t sys_mknod(const char *name_user, uint16_t umode, int dev) {
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;

    uint16_t masked_mode = (umode & S_IFMT) | (umode & 0777);
    if (current_task && current_task->fs)
        masked_mode =
            (umode & S_IFMT) | ((umode & 0777) & ~current_task->fs->umask);

    return (uint64_t)vfs_mknodat(AT_FDCWD, name, masked_mode, (dev64_t)dev,
                                 false);
}

uint64_t sys_mknodat(uint64_t fd, const char *path_user, uint16_t umode,
                     int dev) {
    char path[512];
    if (copy_from_user_str(path, path_user, sizeof(path)))
        return (uint64_t)-EFAULT;

    uint16_t masked_mode = (umode & S_IFMT) | (umode & 0777);
    if (current_task && current_task->fs)
        masked_mode =
            (umode & S_IFMT) | ((umode & 0777) & ~current_task->fs->umask);

    return (uint64_t)vfs_mknodat((int)fd, path, masked_mode, (dev64_t)dev,
                                 false);
}

uint64_t sys_chmod(const char *name_user, uint16_t mode) {
    struct vfs_path path = {0};
    int ret;
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    ret = vfs_filename_lookup(AT_FDCWD, name, LOOKUP_NOFOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    ret = generic_setattr_path(&path, true, mode, false, 0, false, 0, false, 0);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_fchmod(int fd, uint16_t mode) {
    struct vfs_path path = {0};
    int ret = generic_get_fd_path(fd, &path);
    if (ret < 0)
        return ret;
    ret = generic_setattr_path(&path, true, mode, false, 0, false, 0, false, 0);
    vfs_path_put(&path);
    return ret;
}

uint64_t sys_fchmodat(int dfd, const char *name_user, uint16_t mode) {
    struct vfs_path path = {0};
    int ret;
    char name[512];
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    ret = vfs_filename_lookup(dfd, name, LOOKUP_NOFOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    ret = generic_setattr_path(&path, true, mode, false, 0, false, 0, false, 0);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_fchmodat2(int dfd, const char *name_user, uint16_t mode,
                       int flags) {
    struct vfs_path path = {0};
    int ret;
    char name[512];
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return (uint64_t)-EINVAL;
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    if (name[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH))
            return (uint64_t)-ENOENT;
        ret = generic_get_fd_path(dfd, &path);
    } else {
        ret = vfs_filename_lookup(
            dfd, name,
            (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW,
            &path);
    }
    if (ret < 0)
        return (uint64_t)ret;
    ret = generic_setattr_path(&path, true, mode, false, 0, false, 0, false, 0);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_chown(const char *filename_user, uint64_t uid, uint64_t gid) {
    struct vfs_path path = {0};
    int ret;
    char filename[512];
    if (copy_from_user_str(filename, filename_user, sizeof(filename)))
        return (uint64_t)-EFAULT;
    ret = vfs_filename_lookup(AT_FDCWD, filename, LOOKUP_NOFOLLOW, &path);
    if (ret < 0)
        return (uint64_t)ret;
    ret = generic_chown_path(&path, uid, gid);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_fchown(int fd, uint64_t uid, uint64_t gid) {
    struct vfs_path path = {0};
    int ret = generic_get_fd_path(fd, &path);
    if (ret < 0)
        return ret;
    ret = generic_chown_path(&path, uid, gid);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_fchownat(int dfd, const char *name_user, uint64_t uid,
                      uint64_t gid, int flags) {
    struct vfs_path path = {0};
    int ret;
    char name[512];
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return (uint64_t)-EINVAL;
    if (copy_from_user_str(name, name_user, sizeof(name)))
        return (uint64_t)-EFAULT;
    if (name[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH))
            return (uint64_t)-ENOENT;
        ret = generic_get_fd_path(dfd, &path);
    } else {
        ret = vfs_filename_lookup(
            dfd, name,
            (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW,
            &path);
    }
    if (ret < 0)
        return (uint64_t)ret;
    ret = generic_chown_path(&path, uid, gid);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_fallocate(int fd, int mode, uint64_t offset, uint64_t len) {
    struct vfs_path path = {0};
    int ret;
    if (mode != 0)
        return -EOPNOTSUPP;
    if (offset > UINT64_MAX - len)
        return -EFBIG;
    ret = generic_get_fd_path(fd, &path);
    if (ret < 0)
        return ret;
    ret = generic_setattr_path(&path, false, 0, false, 0, false, 0, true,
                               offset + len);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_truncate(const char *path_user, uint64_t length) {
    struct vfs_path vpath = {0};
    int ret;
    char path[512];
    if (copy_from_user_str(path, path_user, sizeof(path)))
        return (uint64_t)-EFAULT;
    ret = vfs_filename_lookup(AT_FDCWD, path, LOOKUP_FOLLOW, &vpath);
    if (ret < 0)
        return (uint64_t)ret;
    ret = generic_setattr_path(&vpath, false, 0, false, 0, false, 0, true,
                               length);
    vfs_path_put(&vpath);
    return (uint64_t)ret;
}

uint64_t sys_ftruncate(int fd, uint64_t length) {
    struct vfs_path path = {0};
    int ret = generic_get_fd_path(fd, &path);
    if (ret < 0)
        return ret;
    ret =
        generic_setattr_path(&path, false, 0, false, 0, false, 0, true, length);
    vfs_path_put(&path);
    return (uint64_t)ret;
}

uint64_t sys_flock(int fd, uint64_t operation) {
    fd_t *file = task_get_file(current_task, fd);

    if (fd < 0 || !file)
        return -EBADF;

    vfs_node_t *node = file->f_inode;
    vfs_bsd_lock_t *lock = &node->flock_lock;
    uintptr_t owner = (uintptr_t)file;

    switch (operation & ~LOCK_NB) {
    case LOCK_SH:
    case LOCK_EX:
    case LOCK_UN:
        break;
    default:
        vfs_file_put(file);
        return -EINVAL;
    }

    switch (operation & ~LOCK_NB) {
    case LOCK_SH:
    case LOCK_EX:
        for (;;) {
            bool conflict;

            spin_lock(&lock->spin);
            conflict = lock->l_type != F_UNLCK && lock->owner != owner;
            if (!conflict) {
                lock->l_type = (operation & LOCK_EX) ? F_WRLCK : F_RDLCK;
                lock->owner = owner;
                spin_unlock(&lock->spin);
                break;
            }
            spin_unlock(&lock->spin);

            if (operation & LOCK_NB) {
                vfs_file_put(file);
                return -EWOULDBLOCK;
            }
            arch_pause();
        }
        break;

    case LOCK_UN:
        spin_lock(&lock->spin);
        if (lock->owner != owner) {
            spin_unlock(&lock->spin);
            vfs_file_put(file);
            return -EACCES;
        }
        lock->l_type = F_UNLCK;
        lock->owner = 0;
        spin_unlock(&lock->spin);
        break;
    }

    vfs_file_put(file);
    return 0;
}

uint64_t sys_fadvise64(int fd, uint64_t offset, uint64_t len, int advice) {
    fd_t *file = task_get_file(current_task, fd);
    if (fd < 0 || !file)
        return -EBADF;

    if (advice == POSIX_FADV_WILLNEED || advice == POSIX_FADV_SEQUENTIAL)
        (void)page_cache_readahead(file, offset, len ? len : PAGE_SIZE * 32);
    if (advice == POSIX_FADV_DONTNEED && file->f_inode)
        (void)page_cache_invalidate_range(&file->f_inode->i_mapping, offset,
                                          len ? offset + len : UINT64_MAX,
                                          true);

    vfs_file_put(file);
    return 0;
}

uint64_t sys_readahead(int fd, uint64_t offset, uint64_t count) {
    fd_t *file = task_get_file(current_task, fd);
    int ret;

    if (fd < 0 || !file)
        return (uint64_t)-EBADF;
    if (!file->f_inode || !S_ISREG(file->f_inode->i_mode)) {
        vfs_file_put(file);
        return 0;
    }

    ret = page_cache_readahead(file, offset, count);
    vfs_file_put(file);
    return ret < 0 ? (uint64_t)ret : 0;
}

uint64_t sys_utimensat(int dfd, const char *pathname, struct timespec *ntimes,
                       int flags) {
    struct timespec user_times[2];

    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return (uint64_t)-EINVAL;

    if (ntimes) {
        if (copy_from_user(user_times, ntimes, sizeof(user_times)))
            return (uint64_t)-EFAULT;
    }

    return (uint64_t)generic_do_utimensat(dfd, pathname,
                                          ntimes ? user_times : NULL, flags);
}

uint64_t sys_futimesat(int dfd, const char *pathname, struct timeval *utimes) {
    struct timespec ntimes[2];
    struct timeval tv[2];

    if (!utimes)
        return sys_utimensat(dfd, pathname, NULL, 0);

    if (copy_from_user(tv, utimes, sizeof(tv)))
        return (uint64_t)-EFAULT;

    for (int i = 0; i < 2; i++) {
        if (tv[i].tv_sec < 0 || tv[i].tv_usec < 0 ||
            tv[i].tv_usec >= 1000000L) {
            return (uint64_t)-EINVAL;
        }
        ntimes[i].tv_sec = tv[i].tv_sec;
        ntimes[i].tv_nsec = tv[i].tv_usec * 1000L;
    }

    return (uint64_t)generic_do_utimensat(dfd, pathname, ntimes, 0);
}

extern uint64_t memory_size;

uint64_t sys_sysinfo(struct sysinfo *info_user) {
    struct sysinfo res;
    struct sysinfo *info = &res;

    memset(info, 0, sizeof(struct sysinfo));
    info->uptime = boot_get_boottime();
    info->loads[0] = 0;
    info->loads[1] = 0;
    info->loads[2] = 0;
    info->totalram = memory_size / PAGE_SIZE;
    info->mem_unit = PAGE_SIZE;
    info->freeram = 0;
    info->procs = task_count();

    if (copy_to_user(info_user, info, sizeof(struct sysinfo)))
        return (uint64_t)-EFAULT;

    return 0;
}
