#include <drivers/drm/drmfs.h>
#include <fs/fs_syscall.h>
#include <libs/klibc.h>

typedef struct drmfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} drmfs_info_t;

typedef struct drmfs_inode_info {
    struct vfs_inode vfs_inode;
} drmfs_inode_info_t;

static struct vfs_file_system_type drmfs_type;
static const struct vfs_super_operations drmfs_super_ops;
static const struct vfs_file_operations drmfs_dir_file_ops;
static spinlock_t drmfs_mount_lock;
static struct vfs_mount *drmfs_internal_mnt;
static spinlock_t drmfs_register_lock = SPIN_INIT;
static bool drmfs_registered;

static inline drmfs_info_t *drmfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (drmfs_info_t *)sb->s_fs_info : NULL;
}

static struct vfs_inode *drmfs_alloc_inode(struct vfs_super_block *sb) {
    drmfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void drmfs_destroy_inode(struct vfs_inode *inode) {
    if (!inode)
        return;
    free(container_of(inode, drmfs_inode_info_t, vfs_inode));
}

static void drmfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static int drmfs_statfs(struct vfs_path *path, void *buf) {
    struct statfs *st = (struct statfs *)buf;
    struct vfs_super_block *sb;

    if (!path || !path->dentry || !path->dentry->d_inode || !st)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    sb = path->dentry->d_inode->i_sb;
    if (!sb)
        return 0;
    st->f_type = sb->s_magic;
    st->f_bsize = PAGE_SIZE;
    st->f_frsize = PAGE_SIZE;
    st->f_namelen = 255;
    st->f_flags = sb->s_flags;
    return 0;
}

static int drmfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static loff_t drmfs_dir_llseek(struct vfs_file *file, loff_t offset,
                               int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int drmfs_dir_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!file)
        return -EINVAL;
    file->private_data = inode ? inode->i_private : NULL;
    return 0;
}

static int drmfs_dir_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (file)
        file->private_data = NULL;
    return 0;
}

static int drmfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    drmfs_info_t *fsi;
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
    sb->s_magic = 0x64726d66ULL;
    sb->s_fs_info = fsi;
    sb->s_op = &drmfs_super_ops;
    sb->s_type = &drmfs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &drmfs_dir_file_ops;

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

static const struct vfs_super_operations drmfs_super_ops = {
    .alloc_inode = drmfs_alloc_inode,
    .destroy_inode = drmfs_destroy_inode,
    .put_super = drmfs_put_super,
    .statfs = drmfs_statfs,
};

static const struct vfs_file_operations drmfs_dir_file_ops = {
    .llseek = drmfs_dir_llseek,
    .open = drmfs_dir_open,
    .release = drmfs_dir_release,
};

static struct vfs_file_system_type drmfs_type = {
    .name = "drmfdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = drmfs_init_fs_context,
    .get_tree = drmfs_get_tree,
};

static struct vfs_mount *drmfs_get_internal_mount(void) {
    int ret;

    spin_lock(&drmfs_register_lock);
    if (!drmfs_registered) {
        spin_init(&drmfs_mount_lock);
        vfs_register_filesystem(&drmfs_type);
        drmfs_registered = true;
    }
    spin_unlock(&drmfs_register_lock);

    spin_lock(&drmfs_mount_lock);
    if (!drmfs_internal_mnt) {
        ret = vfs_kern_mount("drmfdfs", 0, NULL, NULL, &drmfs_internal_mnt);
        if (ret < 0)
            drmfs_internal_mnt = NULL;
    }
    if (drmfs_internal_mnt)
        vfs_mntget(drmfs_internal_mnt);
    spin_unlock(&drmfs_mount_lock);
    return drmfs_internal_mnt;
}

static ino64_t drmfs_next_ino(struct vfs_super_block *sb) {
    drmfs_info_t *fsi = drmfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

int drmfs_create_file(const char *prefix, const struct vfs_file_operations *ops,
                      void *private_data, umode_t mode, unsigned int open_flags,
                      struct vfs_file **out_file,
                      struct vfs_inode **out_inode) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    char label[64];

    if (!prefix || !ops || !private_data || !out_file)
        return -EINVAL;

    mnt = drmfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = drmfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = mode;
    if (S_ISREG(mode))
        inode->i_nlink = 1;
    inode->i_fop = ops;
    inode->i_private = private_data;

    snprintf(label, sizeof(label), "%s-%llu", prefix,
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, label);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          open_flags);
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = private_data;
    *out_file = file;
    if (out_inode)
        *out_inode = inode;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

bool drmfs_owns_file(struct vfs_file *file) {
    return file && file->f_inode && file->f_inode->i_sb &&
           file->f_inode->i_sb->s_type == &drmfs_type;
}
