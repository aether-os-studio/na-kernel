#include <fs/vfs/vfs.h>
#include <mm/mm.h>

#define MOUNTFDFS_MAGIC 0x6d6e746664ULL

typedef struct mountfd_ctx {
    struct vfs_mount *mnt;
    struct vfs_dentry *root;
    bool detached;
    bool attached;
} mountfd_ctx_t;

typedef struct mountfdfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} mountfdfs_info_t;

static struct vfs_file_system_type mountfdfs_fs_type;
static const struct vfs_super_operations mountfdfs_super_ops;
static const struct vfs_file_operations mountfdfs_dir_file_ops;
static const struct vfs_file_operations mountfdfs_file_ops;
static spinlock_t mountfdfs_mount_lock;
static struct vfs_mount *mountfdfs_internal_mnt;

static inline mountfdfs_info_t *mountfdfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (mountfdfs_info_t *)sb->s_fs_info : NULL;
}

static mountfd_ctx_t *mountfd_file_ctx(struct vfs_file *file) {
    if (!file || !file->f_inode || !file->f_inode->i_sb ||
        !file->f_inode->i_sb->s_type ||
        file->f_inode->i_sb->s_type != &mountfdfs_fs_type) {
        return NULL;
    }

    if (file->private_data)
        return (mountfd_ctx_t *)file->private_data;
    return (mountfd_ctx_t *)file->f_inode->i_private;
}

int mountfd_get_path(struct vfs_file *file, struct vfs_path *path) {
    mountfd_ctx_t *ctx;

    if (!path)
        return -EINVAL;

    ctx = mountfd_file_ctx(file);
    if (!ctx || !ctx->mnt || !ctx->root)
        return -EINVAL;

    memset(path, 0, sizeof(*path));
    return vfs_path_set(path, ctx->mnt, ctx->root) ? 0 : -ENOENT;
}

void mountfd_mark_attached(struct vfs_file *file) {
    mountfd_ctx_t *ctx;

    ctx = mountfd_file_ctx(file);
    if (!ctx || !ctx->detached)
        return;

    ctx->attached = true;
}

static loff_t mountfd_llseek(struct vfs_file *file, loff_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static ssize_t mountfd_read(struct vfs_file *file, void *buf, size_t count,
                            loff_t *ppos) {
    (void)file;
    (void)buf;
    (void)count;
    (void)ppos;
    return -EBADF;
}

static ssize_t mountfd_write(struct vfs_file *file, const void *buf,
                             size_t count, loff_t *ppos) {
    (void)file;
    (void)buf;
    (void)count;
    (void)ppos;
    return -EBADF;
}

static int mountfd_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    if (!file)
        return -EINVAL;
    file->private_data = inode ? inode->i_private : NULL;
    return 0;
}

static int mountfd_release(struct vfs_inode *inode, struct vfs_file *file) {
    mountfd_ctx_t *ctx = mountfd_file_ctx(file);

    (void)inode;
    if (!ctx)
        return 0;

    if (ctx->mnt) {
        if (ctx->detached && !ctx->attached)
            vfs_put_mount_tree(ctx->mnt);
        else if (!ctx->detached)
            vfs_mntput(ctx->mnt);
    }
    if (ctx->root)
        vfs_dput(ctx->root);

    free(ctx);
    file->private_data = NULL;
    if (file->f_inode)
        file->f_inode->i_private = NULL;
    return 0;
}

static size_t mountfd_procfs_fdinfo_render(struct vfs_file *file, char *buf,
                                           size_t size) {
    mountfd_ctx_t *ctx;
    int len;

    if (!file || !buf || size == 0)
        return 0;

    ctx = mountfd_file_ctx(file);
    if (!ctx || !ctx->mnt)
        return 0;

    len = snprintf(buf, size, "mnt_id:\t%u\ndetached:\t%s\n", ctx->mnt->mnt_id,
                   ctx->detached ? "yes" : "no");
    if (len < 0)
        return 0;
    if ((size_t)len >= size)
        return size - 1;
    return (size_t)len;
}

static const struct vfs_file_operations mountfdfs_dir_file_ops = {
    .llseek = mountfd_llseek,
    .open = mountfd_open_file,
    .release = mountfd_release,
    .show_fdinfo = mountfd_procfs_fdinfo_render,
};

static const struct vfs_file_operations mountfdfs_file_ops = {
    .llseek = mountfd_llseek,
    .read = mountfd_read,
    .write = mountfd_write,
    .open = mountfd_open_file,
    .release = mountfd_release,
    .show_fdinfo = mountfd_procfs_fdinfo_render,
};

static struct vfs_mount *mountfdfs_get_internal_mount(void) {
    int ret;

    spin_lock(&mountfdfs_mount_lock);
    if (!mountfdfs_internal_mnt) {
        ret =
            vfs_kern_mount("mountfdfs", 0, NULL, NULL, &mountfdfs_internal_mnt);
        if (ret < 0)
            mountfdfs_internal_mnt = NULL;
    }
    if (mountfdfs_internal_mnt)
        vfs_mntget(mountfdfs_internal_mnt);
    spin_unlock(&mountfdfs_mount_lock);
    return mountfdfs_internal_mnt;
}

static int mountfdfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int mountfdfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    mountfdfs_info_t *fsi;
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
    sb->s_magic = MOUNTFDFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &mountfdfs_super_ops;
    sb->s_type = &mountfdfs_fs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &mountfdfs_dir_file_ops;

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

static void mountfdfs_kill_sb(struct vfs_super_block *sb) {
    if (!sb)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations mountfdfs_super_ops = {
    .put_super = mountfdfs_kill_sb,
};

static struct vfs_file_system_type mountfdfs_fs_type = {
    .name = "mountfdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = mountfdfs_init_fs_context,
    .get_tree = mountfdfs_get_tree,
};

int mountfd_create_file(struct vfs_mount *mnt, struct vfs_dentry *root,
                        bool detached, unsigned int open_flags,
                        struct vfs_file **out_file) {
    struct vfs_mount *internal_mnt;
    struct vfs_super_block *sb;
    mountfdfs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    mountfd_ctx_t *ctx;
    char namebuf[32];

    if (!mnt || !out_file)
        return -EINVAL;
    if (!root)
        root = mnt->mnt_root;
    if (!root)
        return -EINVAL;

    internal_mnt = mountfdfs_get_internal_mount();
    if (!internal_mnt)
        return -ENODEV;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        vfs_mntput(internal_mnt);
        return -ENOMEM;
    }

    ctx->mnt = mnt;
    ctx->root = vfs_dget(root);
    if (!ctx->root) {
        free(ctx);
        vfs_mntput(internal_mnt);
        return -ENOENT;
    }
    ctx->detached = detached;

    sb = internal_mnt->mnt_sb;
    fsi = mountfdfs_sb_info(sb);
    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_dput(ctx->root);
        free(ctx);
        vfs_mntput(internal_mnt);
        return -ENOMEM;
    }

    spin_lock(&fsi->lock);
    inode->i_ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    inode->inode = inode->i_ino;
    /*
     * Linux mount fds are directory-like handles to a mount root and can be
     * reused as dirfds (for example openat(mfd, ".", O_DIRECTORY)). Model
     * them as directories so generic dirfd checks don't spuriously fail with
     * ENOTDIR.
     */
    inode->i_mode = S_IFDIR | 0500;
    inode->i_nlink = 2;
    inode->i_fop = &mountfdfs_dir_file_ops;
    inode->i_private = ctx;

    snprintf(namebuf, sizeof(namebuf), "mountfd-%llu",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        vfs_iput(inode);
        vfs_dput(ctx->root);
        free(ctx);
        vfs_mntput(internal_mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(
        &(struct vfs_path){.mnt = internal_mnt, .dentry = dentry},
        O_PATH | (open_flags & O_CLOEXEC));
    if (!file) {
        vfs_dput(dentry);
        vfs_iput(inode);
        vfs_dput(ctx->root);
        free(ctx);
        vfs_mntput(internal_mnt);
        return -ENOMEM;
    }

    if (!vfs_path_update(&file->f_path,
                         &(struct vfs_path){.mnt = mnt, .dentry = root})) {
        vfs_file_put(file);
        vfs_dput(dentry);
        vfs_iput(inode);
        vfs_mntput(internal_mnt);
        return -ENOENT;
    }

    file->private_data = ctx;
    *out_file = file;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(internal_mnt);
    return 0;
}

void mountfd_init() {
    spin_init(&mountfdfs_mount_lock);
    vfs_register_filesystem(&mountfdfs_fs_type);
}
