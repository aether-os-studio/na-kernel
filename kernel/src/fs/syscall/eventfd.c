#include <fs/fs_syscall.h>
#include <fs/proc.h>
#include <fs/vfs/vfs.h>
#include <mod/dlinker.h>
#include <init/callbacks.h>

#define EVENTFDFS_MAGIC 0x65766466ULL

typedef struct eventfdfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} eventfdfs_info_t;

typedef struct eventfdfs_inode_info {
    struct vfs_inode vfs_inode;
} eventfdfs_inode_info_t;

static struct vfs_file_system_type eventfdfs_fs_type;
static const struct vfs_super_operations eventfdfs_super_ops;
static const struct vfs_file_operations eventfdfs_dir_file_ops;
static const struct vfs_file_operations eventfdfs_file_ops;
static spinlock_t eventfdfs_mount_lock;
static struct vfs_mount *eventfdfs_internal_mnt;

int eventfdfs_id = 0;

static inline eventfdfs_info_t *eventfdfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (eventfdfs_info_t *)sb->s_fs_info : NULL;
}

eventfd_t *eventfd_file_handle(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (eventfd_t *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (eventfd_t *)file->f_inode->i_private;
}

int eventfd_is_file(struct vfs_file *file) {
    eventfd_t *efd = eventfd_file_handle(file);

    if (!efd || !file || !file->f_inode || !file->f_inode->i_sb ||
        !file->f_inode->i_sb->s_type)
        return 0;

    return file->f_inode->i_sb->s_type == &eventfdfs_fs_type;
}

static struct vfs_inode *eventfdfs_alloc_inode(struct vfs_super_block *sb) {
    eventfdfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void eventfdfs_destroy_inode(struct vfs_inode *inode) {
    if (!inode)
        return;
    if (inode->i_private) {
        free((eventfd_t *)inode->i_private);
        inode->i_private = NULL;
    }
    free(container_of(inode, eventfdfs_inode_info_t, vfs_inode));
}

static int eventfdfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int eventfdfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    eventfdfs_info_t *fsi;
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
    sb->s_magic = EVENTFDFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &eventfdfs_super_ops;
    sb->s_type = &eventfdfs_fs_type;

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
    inode->i_fop = &eventfdfs_dir_file_ops;

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

static void eventfdfs_put_super(struct vfs_super_block *sb) {
    if (!sb)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations eventfdfs_super_ops = {
    .alloc_inode = eventfdfs_alloc_inode,
    .destroy_inode = eventfdfs_destroy_inode,
    .put_super = eventfdfs_put_super,
};

static struct vfs_file_system_type eventfdfs_fs_type = {
    .name = "eventfdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = eventfdfs_init_fs_context,
    .get_tree = eventfdfs_get_tree,
};

static struct vfs_mount *eventfdfs_get_internal_mount(void) {
    int ret;

    spin_lock(&eventfdfs_mount_lock);
    if (!eventfdfs_internal_mnt) {
        ret =
            vfs_kern_mount("eventfdfs", 0, NULL, NULL, &eventfdfs_internal_mnt);
        if (ret < 0)
            eventfdfs_internal_mnt = NULL;
    }
    if (eventfdfs_internal_mnt)
        vfs_mntget(eventfdfs_internal_mnt);
    spin_unlock(&eventfdfs_mount_lock);
    return eventfdfs_internal_mnt;
}

static ino64_t eventfdfs_next_ino(struct vfs_super_block *sb) {
    eventfdfs_info_t *fsi = eventfdfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static loff_t eventfdfs_llseek(struct vfs_file *file, loff_t offset,
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

static ssize_t eventfdfs_read(struct vfs_file *file, void *buf, size_t count,
                              loff_t *ppos) {
    eventfd_t *efd = eventfd_file_handle(file);
    vfs_node_t *node;
    uint64_t value;

    (void)ppos;
    if (!efd || count < sizeof(uint64_t))
        return -EINVAL;
    node = efd->node ? efd->node : file->f_inode;
    if (!node)
        return -EINVAL;

    while (true) {
        spin_lock(&efd->lock);
        uint64_t old_count = efd->count;
        if (old_count != 0) {
            value = (efd->flags & EFD_SEMAPHORE) ? 1 : old_count;
            efd->count = old_count - value;
            spin_unlock(&efd->lock);

            vfs_poll_notify_inode(node, EPOLLOUT | EPOLLWRNORM);
            memcpy(buf, &value, sizeof(uint64_t));
            return sizeof(uint64_t);
        }
        spin_unlock(&efd->lock);

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        int reason = vfs_poll_wait_interruptible(
            file, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLNVAL);
        if (reason < 0)
            return reason;
    }
}

static ssize_t eventfdfs_write(struct vfs_file *file, const void *buf,
                               size_t count, loff_t *ppos) {
    eventfd_t *efd = eventfd_file_handle(file);
    vfs_node_t *node;
    uint64_t value;

    (void)ppos;
    if (!efd || count < sizeof(uint64_t))
        return -EINVAL;
    node = efd->node ? efd->node : file->f_inode;
    if (!node)
        return -EINVAL;

    memcpy(&value, buf, sizeof(uint64_t));
    if (value == UINT64_MAX)
        return -EINVAL;

    while (true) {
        spin_lock(&efd->lock);
        uint64_t old_count = efd->count;
        if (old_count > UINT64_MAX - 1 - value) {
            spin_unlock(&efd->lock);
            if (file->f_flags & O_NONBLOCK)
                return -EAGAIN;

            int reason = vfs_poll_wait_interruptible(
                file, EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLNVAL);
            if (reason < 0)
                return reason;

            continue;
        }

        efd->count = old_count + value;
        spin_unlock(&efd->lock);
        break;
    }

    vfs_poll_notify_inode(node, EPOLLIN | EPOLLRDNORM);
    return sizeof(uint64_t);
}

static __poll_t eventfdfs_poll(struct vfs_file *file,
                               struct vfs_poll_table *pt) {
    eventfd_t *efd = eventfd_file_handle(file);
    uint64_t count;

    (void)pt;
    if (!efd)
        return EPOLLNVAL;

    spin_lock(&efd->lock);
    count = efd->count;
    spin_unlock(&efd->lock);

    __poll_t revents = 0;
    if (count > 0)
        revents |= EPOLLIN | EPOLLRDNORM;
    if (count < UINT64_MAX - 1)
        revents |= EPOLLOUT | EPOLLWRNORM;
    return revents;
}

static int eventfdfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    file->private_data = inode->i_private;
    return 0;
}

static int eventfdfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (file)
        file->private_data = NULL;
    return 0;
}

static const struct vfs_file_operations eventfdfs_dir_file_ops = {
    .llseek = eventfdfs_llseek,
    .open = eventfdfs_open,
    .release = eventfdfs_release,
};

static const struct vfs_file_operations eventfdfs_file_ops = {
    .llseek = eventfdfs_llseek,
    .read = eventfdfs_read,
    .write = eventfdfs_write,
    .poll = eventfdfs_poll,
    .open = eventfdfs_open,
    .release = eventfdfs_release,
};

int eventfd_create_file(struct vfs_file **out_file, uint64_t initial_val,
                        unsigned int flags, eventfd_t **out_efd) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    eventfd_t *efd;
    char namebuf[32];

    if (!out_file)
        return -EINVAL;

    mnt = eventfdfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    efd = calloc(1, sizeof(*efd));
    if (!efd) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(efd);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = eventfdfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFCHR | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &eventfdfs_file_ops;
    inode->i_private = efd;

    efd->count = initial_val;
    spin_init(&efd->lock);
    efd->flags = flags & EFD_SEMAPHORE;
    efd->node = inode;

    snprintf(namebuf, sizeof(namebuf), "eventfd-%llu",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        inode->i_private = NULL;
        vfs_iput(inode);
        free(efd);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDWR | (flags & EFD_NONBLOCK));
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        free(efd);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = efd;
    file->f_mode |= VFS_FMODE_NO_POS_LOCK;
    *out_file = file;
    if (out_efd)
        *out_efd = efd;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

uint64_t sys_eventfd2(uint64_t initial_val, uint64_t flags) {
    struct vfs_file *file = NULL;
    int ret;

    if (flags & ~(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE))
        return (uint64_t)-EINVAL;

    ret = eventfd_create_file(&file, initial_val, (unsigned int)flags, NULL);
    if (ret < 0)
        return (uint64_t)ret;

    ret = task_install_file(current_task, file,
                            (flags & EFD_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_eventfd(uint64_t arg1) { return sys_eventfd2(arg1, 0); }

void eventfd_init() {
    spin_init(&eventfdfs_mount_lock);
    vfs_register_filesystem(&eventfdfs_fs_type);
    eventfdfs_id = 1;
}
