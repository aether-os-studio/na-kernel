#include <fs/fs_syscall.h>
#include <fs/proc.h>
#include <fs/vfs/vfs.h>
#include <task/signal.h>
#include <init/callbacks.h>

#define SIGNALFDFS_MAGIC 0x73676664ULL

typedef struct signalfdfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} signalfdfs_info_t;

typedef struct signalfdfs_inode_info {
    struct vfs_inode vfs_inode;
} signalfdfs_inode_info_t;

static struct vfs_file_system_type signalfdfs_fs_type;
static const struct vfs_super_operations signalfdfs_super_ops;
static const struct vfs_file_operations signalfdfs_dir_file_ops;
static const struct vfs_file_operations signalfdfs_file_ops;
static spinlock_t signalfdfs_mount_lock;
static struct vfs_mount *signalfdfs_internal_mnt;

int signalfdfs_id = 0;
int signalfd_id = 0;

static inline signalfdfs_info_t *
signalfdfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (signalfdfs_info_t *)sb->s_fs_info : NULL;
}

struct signalfd_ctx *signalfd_file_handle(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (struct signalfd_ctx *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (struct signalfd_ctx *)file->f_inode->i_private;
}

int signalfd_is_file(struct vfs_file *file) {
    struct signalfd_ctx *ctx = signalfd_file_handle(file);

    if (!ctx || !file || !file->f_inode || !file->f_inode->i_sb ||
        !file->f_inode->i_sb->s_type)
        return 0;
    return file->f_inode->i_sb->s_type == &signalfdfs_fs_type;
}

bool signalfd_mask_contains(struct vfs_file *file, int sig) {
    struct signalfd_ctx *ctx;

    if (!signal_sig_in_range(sig))
        return false;

    ctx = signalfd_file_handle(file);
    return ctx && (sigset_user_to_kernel(ctx->sigmask) & signal_sigbit(sig));
}

static inline void signalfd_apply_flags(int fd_num, fd_t *fd, int flags) {
    uint64_t file_flags = fd_get_flags(fd);
    file_flags &= ~O_NONBLOCK;
    if (flags & O_NONBLOCK)
        file_flags |= O_NONBLOCK;
    fd_set_flags(fd, file_flags);

    task_set_fd_flags_mask_for_file(current_task, fd_num, fd,
                                    (flags & O_CLOEXEC) ? FD_CLOEXEC : 0,
                                    FD_CLOEXEC);
}

static struct vfs_inode *signalfdfs_alloc_inode(struct vfs_super_block *sb) {
    signalfdfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void signalfdfs_destroy_inode(struct vfs_inode *inode) {
    struct signalfd_ctx *ctx;

    if (!inode)
        return;
    ctx = (struct signalfd_ctx *)inode->i_private;
    if (ctx) {
        free(ctx->queue);
        free(ctx);
        inode->i_private = NULL;
    }
    free(container_of(inode, signalfdfs_inode_info_t, vfs_inode));
}

static int signalfdfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int signalfdfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    signalfdfs_info_t *fsi;
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
    sb->s_magic = SIGNALFDFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &signalfdfs_super_ops;
    sb->s_type = &signalfdfs_fs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &signalfdfs_dir_file_ops;

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

static void signalfdfs_put_super(struct vfs_super_block *sb) {
    if (!sb)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations signalfdfs_super_ops = {
    .alloc_inode = signalfdfs_alloc_inode,
    .destroy_inode = signalfdfs_destroy_inode,
    .put_super = signalfdfs_put_super,
};

static struct vfs_file_system_type signalfdfs_fs_type = {
    .name = "signalfdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = signalfdfs_init_fs_context,
    .get_tree = signalfdfs_get_tree,
};

static struct vfs_mount *signalfdfs_get_internal_mount(void) {
    int ret;

    spin_lock(&signalfdfs_mount_lock);
    if (!signalfdfs_internal_mnt) {
        ret = vfs_kern_mount("signalfdfs", 0, NULL, NULL,
                             &signalfdfs_internal_mnt);
        if (ret < 0)
            signalfdfs_internal_mnt = NULL;
    }
    if (signalfdfs_internal_mnt)
        vfs_mntget(signalfdfs_internal_mnt);
    spin_unlock(&signalfdfs_mount_lock);
    return signalfdfs_internal_mnt;
}

static ino64_t signalfdfs_next_ino(struct vfs_super_block *sb) {
    signalfdfs_info_t *fsi = signalfdfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static loff_t signalfdfs_llseek(struct vfs_file *file, loff_t offset,
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

static ssize_t signalfdfs_read(struct vfs_file *file, void *addr, size_t size,
                               loff_t *ppos) {
    struct signalfd_ctx *ctx = signalfd_file_handle(file);
    struct signalfd_siginfo ev = {0};

    (void)ppos;
    if (!ctx)
        return -EINVAL;

    while (!task_signal_take_signalfd(current_task, ctx->sigmask, &ev) &&
           ctx->queue_head == ctx->queue_tail) {
        if (file->f_flags & O_NONBLOCK)
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(
            file, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLNVAL);
        if (reason < 0)
            return reason;
    }

    if (ev.ssi_signo == 0) {
        memcpy(&ev, &ctx->queue[ctx->queue_tail], sizeof(ev));
        ctx->queue_tail = (ctx->queue_tail + 1) % ctx->queue_size;
    }

    size_t copy_len = size < sizeof(ev) ? size : sizeof(ev);
    memcpy(addr, &ev, copy_len);
    return (ssize_t)copy_len;
}

static long signalfdfs_ioctl(struct vfs_file *file, unsigned long cmd,
                             unsigned long arg) {
    struct signalfd_ctx *ctx = signalfd_file_handle(file);
    if (!ctx)
        return -EBADF;
    switch (cmd) {
    case SIGNALFD_IOC_MASK:
        memcpy(&ctx->sigmask, (sigset_t *)(uintptr_t)arg, sizeof(sigset_t));
        return 0;
    default:
        return -ENOTTY;
    }
}

static __poll_t signalfdfs_poll(struct vfs_file *file,
                                struct vfs_poll_table *pt) {
    struct signalfd_ctx *ctx = signalfd_file_handle(file);

    (void)pt;
    if (!ctx)
        return EPOLLNVAL;
    return task_signal_has_pending_in_set(current_task, ctx->sigmask) ||
                   ctx->queue_head != ctx->queue_tail
               ? (EPOLLIN | EPOLLRDNORM)
               : 0;
}

static int signalfdfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    file->private_data = inode->i_private;
    return 0;
}

static int signalfdfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (file)
        file->private_data = NULL;
    return 0;
}

static const struct vfs_file_operations signalfdfs_dir_file_ops = {
    .llseek = signalfdfs_llseek,
    .open = signalfdfs_open,
    .release = signalfdfs_release,
};

static const struct vfs_file_operations signalfdfs_file_ops = {
    .llseek = signalfdfs_llseek,
    .read = signalfdfs_read,
    .unlocked_ioctl = signalfdfs_ioctl,
    .poll = signalfdfs_poll,
    .open = signalfdfs_open,
    .release = signalfdfs_release,
};

static int signalfd_create_file(struct vfs_file **out_file, sigset_t sigmask,
                                unsigned int flags,
                                struct signalfd_ctx **out_ctx) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    struct signalfd_ctx *ctx;
    char buf[32];

    if (!out_file)
        return -EINVAL;

    mnt = signalfdfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }
    ctx->sigmask = sigmask;
    ctx->queue_size = 64;
    ctx->queue = calloc(ctx->queue_size, sizeof(struct signalfd_siginfo));
    if (!ctx->queue) {
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(ctx->queue);
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = signalfdfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFCHR | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &signalfdfs_file_ops;
    inode->i_private = ctx;
    ctx->node = inode;

    snprintf(buf, sizeof(buf), "signalfd-%d", signalfd_id++);
    vfs_qstr_make(&name, buf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        inode->i_private = NULL;
        vfs_iput(inode);
        free(ctx->queue);
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDONLY | (flags & O_NONBLOCK));
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        free(ctx->queue);
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = ctx;
    file->f_mode |= VFS_FMODE_NO_POS_LOCK;
    *out_file = file;
    if (out_ctx)
        *out_ctx = ctx;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

uint64_t sys_signalfd4(int ufd, const sigset_t *mask, size_t sizemask,
                       int flags) {
    sigset_t sigmask = 0;

    if (ufd < -1)
        return -EINVAL;
    if (!signal_sigset_size_valid(sizemask))
        return -EINVAL;
    if (flags & ~(O_NONBLOCK | O_CLOEXEC))
        return -EINVAL;
    if (copy_from_user(&sigmask, mask, sizemask))
        return (uint64_t)-EFAULT;

    if (ufd >= 0) {
        struct vfs_file *fd = task_get_file(current_task, ufd);
        struct signalfd_ctx *ctx;

        if (!fd)
            return (uint64_t)-EBADF;
        if (!signalfd_is_file(fd)) {
            vfs_file_put(fd);
            return (uint64_t)-EINVAL;
        }

        ctx = signalfd_file_handle(fd);
        if (!ctx) {
            vfs_file_put(fd);
            return (uint64_t)-EINVAL;
        }
        ctx->sigmask = sigmask;
        signalfd_apply_flags(ufd, fd, flags);
        vfs_file_put(fd);
        return (uint64_t)ufd;
    }

    struct vfs_file *file = NULL;
    int ret = signalfd_create_file(&file, sigmask, (unsigned int)flags, NULL);
    if (ret < 0)
        return (uint64_t)ret;

    ret = task_install_file(current_task, file,
                            (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_signalfd(int ufd, const sigset_t *mask, size_t sizemask) {
    return sys_signalfd4(ufd, mask, sizemask, 0);
}

void signalfd_init() {
    spin_init(&signalfdfs_mount_lock);
    vfs_register_filesystem(&signalfdfs_fs_type);
    signalfdfs_id = 1;
}
