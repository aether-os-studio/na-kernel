#include "fs/vfs/vfs_internal.h"
#include "mm/mm.h"
#include "mm/cache.h"
#include "task/task.h"

static uint32_t vfs_mode_to_type(umode_t mode) {
    switch (mode & S_IFMT) {
    case S_IFDIR:
        return file_dir;
    case S_IFLNK:
        return file_symlink;
    case S_IFBLK:
        return file_block;
    case S_IFCHR:
        return file_stream;
    case S_IFIFO:
        return file_fifo;
    case S_IFSOCK:
        return file_socket;
    case S_IFREG:
    default:
        return file_none;
    }
}

void vfs_sync_inode_compat(struct vfs_inode *inode) {
    if (!inode)
        return;

    inode->inode = inode->i_ino;
}

struct vfs_inode *vfs_alloc_inode(struct vfs_super_block *sb) {
    struct vfs_inode *inode = NULL;

    if (!sb)
        return NULL;
    if (!vfs_get_super(sb))
        return NULL;
    if (sb->s_op && sb->s_op->alloc_inode)
        inode = sb->s_op->alloc_inode(sb);
    else
        inode = calloc(1, sizeof(*inode));
    if (!inode) {
        vfs_put_super(sb);
        return NULL;
    }

    inode->i_sb = sb;
    inode->i_blkbits = 12;
    inode->i_state = VFS_I_NEW;
    inode->i_mapping.host = inode;
    spin_init(&inode->i_mapping.lock);
    inode->i_mapping.pages = RB_ROOT_INIT;
    inode->i_mapping.cached_pages = 0;
    inode->i_mapping.dirty_pages = 0;
    inode->i_mapping.mmap_pages = 0;
    inode->i_mapping.readahead_window = 2;
    inode->i_mapping.readahead_last_index = UINT64_MAX;
    inode->inode = 0;
    inode->rw_hint = 0;
    spin_init(&inode->i_lock);
    spin_init(&inode->i_rwsem);
    spin_init(&inode->flock_lock.spin);
    inode->flock_lock.l_type = F_UNLCK;
    inode->flock_lock.owner = 0;
    spin_init(&inode->file_locks_lock);
    llist_init_head(&inode->file_locks);
    llist_init_head(&inode->file_lock_waiters);
    inode->i_write_count = 0;
    inode->i_exec_count = 0;
    llist_init_head(&inode->i_dentry_aliases);
    llist_init_head(&inode->i_sb_list);
    wait_queue_init(&inode->poll_wait);
    vfs_ref_init(&inode->i_ref, 1);

    spin_lock(&sb->s_inode_lock);
    llist_append(&sb->s_inodes, &inode->i_sb_list);
    spin_unlock(&sb->s_inode_lock);

    return inode;
}

struct vfs_inode *vfs_igrab(struct vfs_inode *inode) {
    if (!inode)
        return NULL;
    if (!vfs_ref_try_get(&inode->i_ref))
        return NULL;
    return inode;
}

void vfs_iput(struct vfs_inode *inode) {
    struct vfs_super_block *sb;

    if (!inode)
        return;
    if (!vfs_ref_put(&inode->i_ref))
        return;

    sb = inode->i_sb;
    if (sb && !llist_empty(&inode->i_sb_list)) {
        spin_lock(&sb->s_inode_lock);
        if (!llist_empty(&inode->i_sb_list))
            llist_delete(&inode->i_sb_list);
        spin_unlock(&sb->s_inode_lock);
    }
    if (sb && sb->s_op && sb->s_op->evict_inode)
        sb->s_op->evict_inode(inode);
    /* Filesystems may evict cached data as part of their own teardown, but
     * the VFS must not let an omitted callback leave cache entries pointing
     * at an inode that is about to be destroyed. */
    page_cache_evict(&inode->i_mapping);
    if (sb && sb->s_op && sb->s_op->destroy_inode)
        sb->s_op->destroy_inode(inode);
    else
        free(inode);
    if (sb)
        vfs_put_super(sb);
}

int vfs_inode_get_write_access(struct vfs_inode *inode) {
    int exec_count;

    if (!inode)
        return -EINVAL;
    if (!S_ISREG(inode->i_mode))
        return 0;

    spin_lock(&inode->i_lock);
    exec_count = __atomic_load_n(&inode->i_exec_count, __ATOMIC_ACQUIRE);
    if (exec_count > 0) {
        spin_unlock(&inode->i_lock);
        return -ETXTBSY;
    }
    __atomic_add_fetch(&inode->i_write_count, 1, __ATOMIC_ACQ_REL);
    spin_unlock(&inode->i_lock);
    return 0;
}

void vfs_inode_put_write_access(struct vfs_inode *inode) {
    if (!inode || !S_ISREG(inode->i_mode))
        return;

    spin_lock(&inode->i_lock);
    if (__atomic_load_n(&inode->i_write_count, __ATOMIC_ACQUIRE) > 0)
        __atomic_sub_fetch(&inode->i_write_count, 1, __ATOMIC_ACQ_REL);
    spin_unlock(&inode->i_lock);
}

int vfs_file_get_exec_access(struct vfs_file *file) {
    struct vfs_inode *inode;
    int write_count;

    if (!file || !file->f_inode)
        return -EINVAL;
    if (file->f_mode & VFS_FMODE_EXEC_ACCESS)
        return 0;

    inode = file->f_inode;
    if (!S_ISREG(inode->i_mode)) {
        file->f_mode |= VFS_FMODE_EXEC_ACCESS;
        return 0;
    }

    spin_lock(&inode->i_lock);
    write_count = __atomic_load_n(&inode->i_write_count, __ATOMIC_ACQUIRE);
    if (write_count > 0) {
        spin_unlock(&inode->i_lock);
        return -ETXTBSY;
    }
    __atomic_add_fetch(&inode->i_exec_count, 1, __ATOMIC_ACQ_REL);
    file->f_mode |= VFS_FMODE_EXEC_ACCESS;
    spin_unlock(&inode->i_lock);
    return 0;
}

void vfs_inode_init_owner(struct vfs_inode *inode, struct vfs_inode *dir,
                          umode_t mode) {
    if (!inode)
        return;

    uid32_t uid = 0;
    gid32_t gid = 0;
    vfs_init_new_inode_owner(dir, &mode, &uid, &gid);
    inode->i_mode = mode;
    inode->i_uid = uid;
    inode->i_gid = gid;
}

uid32_t vfs_current_fsuid(void) {
    task_t *task = current_task;

    if (!task)
        return 0;
    return (uid32_t)task->fsuid;
}

gid32_t vfs_current_fsgid(void) {
    task_t *task = current_task;

    if (!task)
        return 0;
    return (gid32_t)task->fsgid;
}

void vfs_init_new_inode_owner(struct vfs_inode *dir, umode_t *mode,
                              uid32_t *uid, gid32_t *gid) {
    uid32_t new_uid = vfs_current_fsuid();
    gid32_t new_gid = vfs_current_fsgid();
    umode_t new_mode = mode ? *mode : 0;

    if (dir && (dir->i_mode & S_ISGID)) {
        new_gid = dir->i_gid;
        if (S_ISDIR(new_mode))
            new_mode |= S_ISGID;
    } else if ((new_mode & S_ISGID) && !S_ISDIR(new_mode) &&
               vfs_current_fsuid() != 0) {
        new_mode &= ~S_ISGID;
    }

    if (mode)
        *mode = new_mode;
    if (uid)
        *uid = new_uid;
    if (gid)
        *gid = new_gid;
}

int vfs_inode_permission(struct vfs_inode *inode, int mask) {
    uid32_t fsuid;
    gid32_t fsgid;
    umode_t mode;
    int granted;

    if (!inode)
        return -ENOENT;
    mask &= VFS_MAY_READ | VFS_MAY_WRITE | VFS_MAY_EXEC;
    if (!mask)
        return 0;

    fsuid = vfs_current_fsuid();
    if (fsuid == 0)
        return 0;

    fsgid = vfs_current_fsgid();
    mode = inode->i_mode;
    if (fsuid == inode->i_uid)
        granted = (mode >> 6) & 7;
    else if (fsgid == inode->i_gid ||
             task_in_supplementary_group(current_task, inode->i_gid))
        granted = (mode >> 3) & 7;
    else
        granted = mode & 7;

    return ((granted & mask) == mask) ? 0 : -EACCES;
}
