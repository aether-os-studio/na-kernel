#include <fs/vfs/vfs.h>
#include <fs/fs_syscall.h>
#include <task/task.h>
#include <init/callbacks.h>

#define FSFDFS_MAGIC 0x66736664ULL

enum fsfd_object_kind {
    FSFD_OBJECT_CONTEXT = 1,
    FSFD_OBJECT_MOUNT = 2,
};

enum {
    FC_STATE_INIT = 0,
    FC_STATE_CONFIG = 1,
    FC_STATE_CREATED = 2,
    FC_STATE_MOUNTED = 3,
};

typedef struct fsfd_object {
    uint32_t kind;
} fsfd_object_t;

typedef struct fs_context {
    fsfd_object_t object;
    struct vfs_file_system_type *fs_type;
    char *source;
    char *data;
    uint64_t mount_flags;
    uint64_t attr_flags;
    struct vfs_mount *reconfigure_mnt;
    int state;
} fs_context_t;

typedef struct mount_handle {
    fsfd_object_t object;
    struct vfs_mount *mnt;
    char *source;
    uint64_t mount_flags;
    bool attached;
} mount_handle_t;

typedef struct fsfdfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} fsfdfs_info_t;

typedef struct fsfdfs_inode_info {
    struct vfs_inode vfs_inode;
} fsfdfs_inode_info_t;

static struct vfs_file_system_type fsfdfs_fs_type;
static const struct vfs_super_operations fsfdfs_super_ops;
static const struct vfs_file_operations fsfdfs_dir_file_ops;
static const struct vfs_file_operations fsfdfs_ctx_file_ops;
static const struct vfs_file_operations fsfdfs_mnt_file_ops;
static spinlock_t fsfdfs_mount_lock;
static struct vfs_mount *fsfdfs_internal_mnt;

static inline fsfdfs_info_t *fsfdfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (fsfdfs_info_t *)sb->s_fs_info : NULL;
}

static inline fsfd_object_t *fsfd_file_object(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (fsfd_object_t *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (fsfd_object_t *)file->f_inode->i_private;
}

static inline fs_context_t *fsfd_file_context(struct vfs_file *file) {
    fsfd_object_t *obj = fsfd_file_object(file);

    if (!obj || obj->kind != FSFD_OBJECT_CONTEXT)
        return NULL;
    if (!file || !file->f_inode || !file->f_inode->i_sb ||
        file->f_inode->i_sb->s_type != &fsfdfs_fs_type) {
        return NULL;
    }
    return (fs_context_t *)obj;
}

static inline mount_handle_t *fsfd_file_mount(struct vfs_file *file) {
    fsfd_object_t *obj = fsfd_file_object(file);

    if (!obj || obj->kind != FSFD_OBJECT_MOUNT)
        return NULL;
    if (!file || !file->f_inode || !file->f_inode->i_sb ||
        file->f_inode->i_sb->s_type != &fsfdfs_fs_type) {
        return NULL;
    }
    return (mount_handle_t *)obj;
}

int fsfd_mount_get_path(struct vfs_file *file, struct vfs_path *path) {
    mount_handle_t *mnt;

    if (!path)
        return -EINVAL;

    mnt = fsfd_file_mount(file);
    if (!mnt || !mnt->mnt || !mnt->mnt->mnt_root)
        return -EINVAL;

    memset(path, 0, sizeof(*path));
    return vfs_path_set(path, mnt->mnt, mnt->mnt->mnt_root) ? 0 : -ENOENT;
}

static struct vfs_inode *fsfdfs_alloc_inode(struct vfs_super_block *sb) {
    fsfdfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void fsfd_destroy_object(void *ptr) {
    fsfd_object_t *obj = (fsfd_object_t *)ptr;

    if (!obj)
        return;

    if (obj->kind == FSFD_OBJECT_CONTEXT) {
        fs_context_t *ctx = (fs_context_t *)obj;

        free(ctx->source);
        free(ctx->data);
        if (ctx->reconfigure_mnt)
            vfs_mntput(ctx->reconfigure_mnt);
        free(ctx);
        return;
    }

    if (obj->kind == FSFD_OBJECT_MOUNT) {
        mount_handle_t *mnt = (mount_handle_t *)obj;

        free(mnt->source);
        if (mnt->mnt && !mnt->attached)
            vfs_mntput(mnt->mnt);
        free(mnt);
    }
}

static void fsfdfs_destroy_inode(struct vfs_inode *inode) {
    if (!inode)
        return;
    if (inode->i_private) {
        fsfd_destroy_object(inode->i_private);
        inode->i_private = NULL;
    }
    free(container_of(inode, fsfdfs_inode_info_t, vfs_inode));
}

static void fsfdfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static int fsfdfs_statfs(struct vfs_path *path, void *buf) {
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
    st->f_fsid.val[0] = (int)(sb->s_dev & 0xffffffffU);
    st->f_fsid.val[1] = (int)((sb->s_dev >> 32) & 0xffffffffU);
    return 0;
}

static int fsfdfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int fsfdfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    fsfdfs_info_t *fsi;
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

    sb->s_magic = FSFDFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &fsfdfs_super_ops;
    sb->s_type = &fsfdfs_fs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &fsfdfs_dir_file_ops;

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

static const struct vfs_super_operations fsfdfs_super_ops = {
    .alloc_inode = fsfdfs_alloc_inode,
    .destroy_inode = fsfdfs_destroy_inode,
    .put_super = fsfdfs_put_super,
    .statfs = fsfdfs_statfs,
};

static struct vfs_file_system_type fsfdfs_fs_type = {
    .name = "fsfdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = fsfdfs_init_fs_context,
    .get_tree = fsfdfs_get_tree,
};

static struct vfs_mount *fsfdfs_get_internal_mount(void) {
    int ret;

    spin_lock(&fsfdfs_mount_lock);
    if (!fsfdfs_internal_mnt) {
        ret = vfs_kern_mount("fsfdfs", 0, NULL, NULL, &fsfdfs_internal_mnt);
        if (ret < 0)
            fsfdfs_internal_mnt = NULL;
    }
    if (fsfdfs_internal_mnt)
        vfs_mntget(fsfdfs_internal_mnt);
    spin_unlock(&fsfdfs_mount_lock);
    return fsfdfs_internal_mnt;
}

static ino64_t fsfdfs_next_ino(struct vfs_super_block *sb) {
    fsfdfs_info_t *fsi = fsfdfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static loff_t fsfdfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int fsfdfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!file)
        return -EINVAL;
    file->private_data = inode ? inode->i_private : NULL;
    return 0;
}

static int fsfdfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (!file)
        return 0;
    file->private_data = NULL;
    return 0;
}

static const struct vfs_file_operations fsfdfs_dir_file_ops = {
    .llseek = fsfdfs_llseek,
    .open = fsfdfs_open,
    .release = fsfdfs_release,
};

static const struct vfs_file_operations fsfdfs_ctx_file_ops = {
    .llseek = fsfdfs_llseek,
    .open = fsfdfs_open,
    .release = fsfdfs_release,
};

static const struct vfs_file_operations fsfdfs_mnt_file_ops = {
    .llseek = fsfdfs_llseek,
    .open = fsfdfs_open,
    .release = fsfdfs_release,
};

static int fsfdfs_create_file(const char *prefix,
                              const struct vfs_file_operations *ops,
                              void *private_data, struct vfs_file **out_file) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    char label[64];

    if (!prefix || !ops || !private_data || !out_file)
        return -EINVAL;

    mnt = fsfdfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = fsfdfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFREG | 0600;
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
                          O_RDWR);
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = private_data;
    *out_file = file;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

static int fsfd_fill_statfs(struct vfs_path *path, struct statfs *buf) {
    struct vfs_super_block *sb;
    int ret = 0;

    if (!path || !path->dentry || !path->dentry->d_inode || !buf)
        return -EINVAL;

    memset(buf, 0, sizeof(*buf));
    sb = path->dentry->d_inode->i_sb;
    if (!sb)
        return 0;

    if (sb->s_op && sb->s_op->statfs) {
        ret = sb->s_op->statfs(path, buf);
        if (ret < 0)
            return ret;
    }

    buf->f_type = sb->s_magic;
    if (!buf->f_bsize)
        buf->f_bsize = PAGE_SIZE;
    if (!buf->f_frsize)
        buf->f_frsize = buf->f_bsize;
    if (!buf->f_namelen)
        buf->f_namelen = 255;
    buf->f_flags = sb->s_flags;
    buf->f_fsid.val[0] = (int)(sb->s_dev & 0xffffffffU);
    buf->f_fsid.val[1] = (int)((sb->s_dev >> 32) & 0xffffffffU);
    return 0;
}

static uint64_t attr_flags_to_ms_flags(uint64_t attr_flags) {
    uint64_t ms_flags = 0;

    if (attr_flags & MOUNT_ATTR_RDONLY)
        ms_flags |= MS_RDONLY;
    if (attr_flags & MOUNT_ATTR_NOSUID)
        ms_flags |= MS_NOSUID;
    if (attr_flags & MOUNT_ATTR_NODEV)
        ms_flags |= MS_NODEV;
    if (attr_flags & MOUNT_ATTR_NOEXEC)
        ms_flags |= MS_NOEXEC;
    if (attr_flags & MOUNT_ATTR_NOATIME)
        ms_flags |= MS_NOATIME;
    if (attr_flags & MOUNT_ATTR_STRICTATIME)
        ms_flags |= MS_STRICTATIME;
    if (attr_flags & MOUNT_ATTR_NODIRATIME)
        ms_flags |= MS_NODIRATIME;
    if (attr_flags & MOUNT_ATTR_NOSYMFOLLOW)
        ms_flags |= MS_NOSYMFOLLOW;

    return ms_flags;
}

static int fsconfig_set_flag(fs_context_t *ctx, const char *key) {
    if (!ctx || !key)
        return -EINVAL;

    if (!strcmp(key, "ro") || !strcmp(key, "rdonly")) {
        ctx->mount_flags |= MS_RDONLY;
        return 0;
    }
    if (!strcmp(key, "rw")) {
        ctx->mount_flags &= ~MS_RDONLY;
        return 0;
    }
    if (!strcmp(key, "nosuid")) {
        ctx->mount_flags |= MS_NOSUID;
        return 0;
    }
    if (!strcmp(key, "suid")) {
        ctx->mount_flags &= ~MS_NOSUID;
        return 0;
    }
    if (!strcmp(key, "nodev")) {
        ctx->mount_flags |= MS_NODEV;
        return 0;
    }
    if (!strcmp(key, "dev")) {
        ctx->mount_flags &= ~MS_NODEV;
        return 0;
    }
    if (!strcmp(key, "noexec")) {
        ctx->mount_flags |= MS_NOEXEC;
        return 0;
    }
    if (!strcmp(key, "exec")) {
        ctx->mount_flags &= ~MS_NOEXEC;
        return 0;
    }
    if (!strcmp(key, "sync") || !strcmp(key, "synchronous")) {
        ctx->mount_flags |= MS_SYNCHRONOUS;
        return 0;
    }
    if (!strcmp(key, "async")) {
        ctx->mount_flags &= ~MS_SYNCHRONOUS;
        return 0;
    }
    if (!strcmp(key, "remount")) {
        ctx->mount_flags |= MS_REMOUNT;
        return 0;
    }
    if (!strcmp(key, "mand") || !strcmp(key, "mandlock")) {
        ctx->mount_flags |= MS_MANDLOCK;
        return 0;
    }
    if (!strcmp(key, "nomand")) {
        ctx->mount_flags &= ~MS_MANDLOCK;
        return 0;
    }
    if (!strcmp(key, "dirsync")) {
        ctx->mount_flags |= MS_DIRSYNC;
        return 0;
    }
    if (!strcmp(key, "nosymfollow")) {
        ctx->mount_flags |= MS_NOSYMFOLLOW;
        return 0;
    }
    if (!strcmp(key, "symfollow")) {
        ctx->mount_flags &= ~MS_NOSYMFOLLOW;
        return 0;
    }
    if (!strcmp(key, "noatime")) {
        ctx->mount_flags |= MS_NOATIME;
        return 0;
    }
    if (!strcmp(key, "atime")) {
        ctx->mount_flags &= ~MS_NOATIME;
        return 0;
    }
    if (!strcmp(key, "nodiratime")) {
        ctx->mount_flags |= MS_NODIRATIME;
        return 0;
    }
    if (!strcmp(key, "diratime")) {
        ctx->mount_flags &= ~MS_NODIRATIME;
        return 0;
    }
    if (!strcmp(key, "relatime")) {
        ctx->mount_flags |= MS_RELATIME;
        return 0;
    }
    if (!strcmp(key, "norelatime")) {
        ctx->mount_flags &= ~MS_RELATIME;
        return 0;
    }
    if (!strcmp(key, "strictatime")) {
        ctx->mount_flags |= MS_STRICTATIME;
        return 0;
    }
    if (!strcmp(key, "lazytime")) {
        ctx->mount_flags |= MS_LAZYTIME;
        return 0;
    }
    if (!strcmp(key, "nolazytime")) {
        ctx->mount_flags &= ~MS_LAZYTIME;
        return 0;
    }
    if (!strcmp(key, "silent")) {
        ctx->mount_flags |= MS_SILENT;
        return 0;
    }
    if (!strcmp(key, "loud")) {
        ctx->mount_flags &= ~MS_SILENT;
        return 0;
    }

    return 0;
}

static int fsconfig_set_string(fs_context_t *ctx, const char *key,
                               const char *value) {
    if (!ctx || !key)
        return -EINVAL;

    if (!strcmp(key, "source")) {
        char *dup = strdup(value ? value : "");

        if (!dup)
            return -ENOMEM;
        free(ctx->source);
        ctx->source = dup;
        return 0;
    }

    if (!strcmp(key, "data") || !strcmp(key, "options")) {
        char *dup = strdup(value ? value : "");

        if (!dup)
            return -ENOMEM;
        free(ctx->data);
        ctx->data = dup;
        return 0;
    }

    if (value && value[0]) {
        char opt_buf[512];

        snprintf(opt_buf, sizeof(opt_buf), "%s=%s", key, value);
        if (ctx->data) {
            size_t old_len = strlen(ctx->data);
            size_t opt_len = strlen(opt_buf);
            char *new_data = malloc(old_len + opt_len + 2);

            if (!new_data)
                return -ENOMEM;
            strcpy(new_data, ctx->data);
            strcat(new_data, ",");
            strcat(new_data, opt_buf);
            free(ctx->data);
            ctx->data = new_data;
        } else {
            ctx->data = strdup(opt_buf);
            if (!ctx->data)
                return -ENOMEM;
        }
    }

    return 0;
}

static int fsconfig_cmd_create(fs_context_t *ctx) {
    if (!ctx || !ctx->fs_type)
        return -EINVAL;
    if (ctx->state == FC_STATE_CREATED)
        return -EBUSY;
    ctx->state = FC_STATE_CREATED;
    return 0;
}

static int fsconfig_cmd_reconfigure(fs_context_t *ctx) {
    unsigned long preserved_flags;
    unsigned long new_flags = 0;

    if (!ctx)
        return -EINVAL;
    if (ctx->state != FC_STATE_CREATED && ctx->state != FC_STATE_MOUNTED)
        return -EINVAL;
    if (!ctx->reconfigure_mnt)
        return 0;

    if (ctx->mount_flags & MS_RDONLY)
        new_flags |= VFS_MNT_READONLY;
    if (ctx->mount_flags & MS_NOSUID)
        new_flags |= VFS_MNT_NOSUID;
    if (ctx->mount_flags & MS_NODEV)
        new_flags |= VFS_MNT_NODEV;
    if (ctx->mount_flags & MS_NOEXEC)
        new_flags |= VFS_MNT_NOEXEC;
    if (ctx->mount_flags & MS_NOSYMFOLLOW)
        new_flags |= VFS_MNT_NOSYMFOLLOW;

    spin_lock(&ctx->reconfigure_mnt->mnt_lock);
    preserved_flags = ctx->reconfigure_mnt->mnt_flags &
                      ~(VFS_MNT_READONLY | VFS_MNT_NOSUID | VFS_MNT_NODEV |
                        VFS_MNT_NOEXEC | VFS_MNT_NOSYMFOLLOW);
    ctx->reconfigure_mnt->mnt_flags = preserved_flags | new_flags;
    spin_unlock(&ctx->reconfigure_mnt->mnt_lock);
    return 0;
}

static void fsconfig_note_parameter_update(fs_context_t *ctx) {
    if (!ctx)
        return;

    /* Parameter updates before FSCONFIG_CMD_CREATE transition the context into
     * the configuration phase. Once the filesystem has been created or mounted,
     * preserve that state so callers can update parameters and then issue
     * FSCONFIG_CMD_RECONFIGURE, which is exactly how systemd flips credential
     * tmpfs mounts read-only. */
    if (ctx->state == FC_STATE_INIT || ctx->state == FC_STATE_CONFIG)
        ctx->state = FC_STATE_CONFIG;
}

static struct vfs_mount *fsfd_path_mount(const struct vfs_path *path) {
    return vfs_path_mount(path);
}

static int fsfd_get_fd_path(int fd, struct vfs_path *path) {
    struct vfs_file *file;

    if (!path)
        return -EINVAL;
    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;
    memset(path, 0, sizeof(*path));
    if (!vfs_path_copy(path, &file->f_path)) {
        vfs_file_put(file);
        return -ENOENT;
    }
    vfs_file_put(file);
    return 0;
}

uint64_t sys_fsopen(const char *fsname_user, unsigned int flags) {
    struct vfs_file_system_type *fs_type;
    struct vfs_file *file = NULL;
    fs_context_t *ctx;
    char fsname[256];
    int ret;

    if (flags & ~FSOPEN_CLOEXEC)
        return (uint64_t)-EINVAL;
    if (copy_from_user_str(fsname, fsname_user, sizeof(fsname)))
        return (uint64_t)-EFAULT;

    fs_type = vfs_get_fs_type(fsname);
    if (!fs_type)
        return (uint64_t)-ENOENT;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return (uint64_t)-ENOMEM;

    ctx->object.kind = FSFD_OBJECT_CONTEXT;
    ctx->fs_type = fs_type;
    ctx->state = FC_STATE_INIT;

    ret = fsfdfs_create_file("fsctx", &fsfdfs_ctx_file_ops, ctx, &file);
    if (ret < 0) {
        fsfd_destroy_object(ctx);
        return (uint64_t)ret;
    }

    ret = task_install_file(current_task, file,
                            (flags & FSOPEN_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_fspick(int dfd, const char *path_user, unsigned int flags) {
    static const unsigned int allowed_flags =
        FSPICK_CLOEXEC | FSPICK_SYMLINK_NOFOLLOW | FSPICK_NO_AUTOMOUNT |
        FSPICK_EMPTY_PATH;
    struct vfs_path path = {0};
    struct vfs_mount *mnt = NULL;
    struct vfs_file *file = NULL;
    fs_context_t *ctx = NULL;
    char pathname[VFS_PATH_MAX];
    int ret;

    if (flags & ~allowed_flags)
        return (uint64_t)-EINVAL;
    if (!path_user)
        return (uint64_t)-EFAULT;
    if (copy_from_user_str(pathname, path_user, sizeof(pathname)))
        return (uint64_t)-EFAULT;

    if (pathname[0] == '\0') {
        if (!(flags & FSPICK_EMPTY_PATH))
            return (uint64_t)-ENOENT;
        ret = fsfd_get_fd_path(dfd, &path);
    } else {
        ret = vfs_filename_lookup(
            dfd, pathname,
            (flags & FSPICK_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW,
            &path);
    }
    if (ret < 0)
        return (uint64_t)ret;

    mnt = fsfd_path_mount(&path);
    if (!mnt && path.mnt)
        mnt = vfs_mntget(path.mnt);
    vfs_path_put(&path);
    if (!mnt || !mnt->mnt_sb || !mnt->mnt_sb->s_type) {
        if (mnt)
            vfs_mntput(mnt);
        return (uint64_t)-EINVAL;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        vfs_mntput(mnt);
        return (uint64_t)-ENOMEM;
    }
    ctx->object.kind = FSFD_OBJECT_CONTEXT;
    ctx->fs_type = mnt->mnt_sb->s_type;
    ctx->reconfigure_mnt = mnt;
    ctx->state = FC_STATE_CREATED;
    if (mnt->mnt_flags & VFS_MNT_READONLY)
        ctx->mount_flags |= MS_RDONLY;
    if (mnt->mnt_flags & VFS_MNT_NOSUID)
        ctx->mount_flags |= MS_NOSUID;
    if (mnt->mnt_flags & VFS_MNT_NODEV)
        ctx->mount_flags |= MS_NODEV;
    if (mnt->mnt_flags & VFS_MNT_NOEXEC)
        ctx->mount_flags |= MS_NOEXEC;
    if (mnt->mnt_flags & VFS_MNT_NOSYMFOLLOW)
        ctx->mount_flags |= MS_NOSYMFOLLOW;

    ret = fsfdfs_create_file("fspick", &fsfdfs_ctx_file_ops, ctx, &file);
    if (ret < 0) {
        fsfd_destroy_object(ctx);
        return (uint64_t)ret;
    }
    ret = task_install_file(current_task, file,
                            (flags & FSPICK_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

uint64_t sys_statfs(const char *path, struct statfs *buf) {
    struct vfs_path vpath = {0};
    char path_k[VFS_PATH_MAX];
    struct statfs kbuf;
    int ret;

    if (!path || !buf)
        return -EINVAL;

    if (copy_from_user_str(path_k, path, sizeof(path_k))) {
        if (!check_user_overflow((uint64_t)path, sizeof(path_k)))
            return -ENAMETOOLONG;
        return -EFAULT;
    }

    ret = vfs_filename_lookup(AT_FDCWD, path_k, LOOKUP_FOLLOW, &vpath);
    if (ret < 0)
        return ret;

    ret = fsfd_fill_statfs(&vpath, &kbuf);
    vfs_path_put(&vpath);
    if (ret < 0)
        return ret;
    return copy_to_user(buf, &kbuf, sizeof(kbuf)) ? -EFAULT : 0;
}

uint64_t sys_fstatfs(int fd, struct statfs *buf) {
    struct vfs_file *file;
    struct vfs_path path = {0};
    struct statfs kbuf;
    int ret;

    if (!buf)
        return -EINVAL;

    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;

    if (!vfs_path_copy(&path, &file->f_path)) {
        vfs_file_put(file);
        return -ENOENT;
    }
    vfs_file_put(file);

    ret = fsfd_fill_statfs(&path, &kbuf);
    vfs_path_put(&path);
    if (ret < 0)
        return ret;
    return copy_to_user(buf, &kbuf, sizeof(kbuf)) ? -EFAULT : 0;
}

uint64_t sys_fsconfig(int fd, uint32_t cmd, const char *key_user,
                      const void *value_user, int aux) {
    struct vfs_file *file;
    fs_context_t *ctx;
    char key[256];
    char value[1024];
    int ret = 0;

    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;

    ctx = fsfd_file_context(file);
    if (!ctx) {
        vfs_file_put(file);
        return -EINVAL;
    }

    switch (cmd) {
    case FSCONFIG_SET_FLAG:
        if (!key_user) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user_str(key, key_user, sizeof(key))) {
            ret = -EFAULT;
            break;
        }
        fsconfig_note_parameter_update(ctx);
        ret = fsconfig_set_flag(ctx, key);
        break;

    case FSCONFIG_SET_STRING:
        if (!key_user) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user_str(key, key_user, sizeof(key))) {
            ret = -EFAULT;
            break;
        }
        if (value_user) {
            if (copy_from_user_str(value, value_user, sizeof(value))) {
                ret = -EFAULT;
                break;
            }
        } else {
            value[0] = '\0';
        }
        fsconfig_note_parameter_update(ctx);
        ret = fsconfig_set_string(ctx, key, value);
        break;

    case FSCONFIG_SET_BINARY:
        if (!key_user) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user_str(key, key_user, sizeof(key))) {
            ret = -EFAULT;
            break;
        }
        if (aux > 0 && value_user) {
            size_t copy_len =
                aux < (int)sizeof(value) - 1 ? (size_t)aux : sizeof(value) - 1;

            if (copy_from_user(value, value_user, copy_len)) {
                ret = -EFAULT;
                break;
            }
            value[copy_len] = '\0';
        } else {
            value[0] = '\0';
        }
        fsconfig_note_parameter_update(ctx);
        ret = fsconfig_set_string(ctx, key, value);
        break;

    case FSCONFIG_SET_PATH:
    case FSCONFIG_SET_PATH_EMPTY:
        if (!key_user) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user_str(key, key_user, sizeof(key))) {
            ret = -EFAULT;
            break;
        }
        if (value_user) {
            if (copy_from_user_str(value, value_user, sizeof(value))) {
                ret = -EFAULT;
                break;
            }
        } else if (cmd == FSCONFIG_SET_PATH_EMPTY) {
            value[0] = '\0';
        } else {
            ret = -EINVAL;
            break;
        }
        fsconfig_note_parameter_update(ctx);
        if (!strcmp(key, "source")) {
            char *resolved = at_resolve_pathname(aux, value);

            ret = fsconfig_set_string(ctx, key, resolved ? resolved : value);
            free(resolved);
        } else {
            ret = fsconfig_set_string(ctx, key, value);
        }
        break;

    case FSCONFIG_SET_FD:
        if (!key_user) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user_str(key, key_user, sizeof(key))) {
            ret = -EFAULT;
            break;
        }
        if (aux < 0) {
            ret = -EBADF;
            break;
        }
        fsconfig_note_parameter_update(ctx);
        if (!strcmp(key, "source")) {
            struct vfs_file *source_file = task_get_file(current_task, aux);
            char *source_path;

            if (!source_file) {
                ret = -EBADF;
                break;
            }
            source_path = vfs_path_to_string(&source_file->f_path,
                                             task_fs_root_path(current_task));
            vfs_file_put(source_file);
            if (!source_path) {
                ret = -ENOMEM;
                break;
            }
            ret = fsconfig_set_string(ctx, key, source_path);
            free(source_path);
        } else {
            ret = -EOPNOTSUPP;
        }
        break;

    case FSCONFIG_CMD_CREATE:
        ret = fsconfig_cmd_create(ctx);
        break;

    case FSCONFIG_CMD_CREATE_EXCL:
        if (ctx->state == FC_STATE_CREATED || ctx->state == FC_STATE_MOUNTED) {
            ret = -EBUSY;
            break;
        }
        ret = fsconfig_cmd_create(ctx);
        break;

    case FSCONFIG_CMD_RECONFIGURE:
        ret = fsconfig_cmd_reconfigure(ctx);
        break;

    default:
        ret = -EOPNOTSUPP;
        break;
    }

    vfs_file_put(file);
    return ret;
}

uint64_t sys_fsmount(int fd, uint32_t flags, uint32_t attr_flags) {
    struct vfs_file *file;
    fs_context_t *ctx;
    mount_handle_t *handle;
    struct vfs_file *mnt_file = NULL;
    struct vfs_mount *mnt = NULL;
    uint64_t combined_flags;
    int ret;

    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;

    ctx = fsfd_file_context(file);
    if (!ctx) {
        vfs_file_put(file);
        return -EINVAL;
    }
    if (!ctx->fs_type) {
        vfs_file_put(file);
        return -ENOENT;
    }
    if (ctx->state != FC_STATE_CREATED) {
        vfs_file_put(file);
        return -EINVAL;
    }

    combined_flags = ctx->mount_flags | attr_flags_to_ms_flags(attr_flags);
    ret = vfs_kern_mount(ctx->fs_type->name, combined_flags, ctx->source,
                         ctx->data, &mnt);
    if (ret < 0) {
        vfs_file_put(file);
        return ret;
    }

    handle = calloc(1, sizeof(*handle));
    if (!handle) {
        vfs_mntput(mnt);
        vfs_file_put(file);
        return -ENOMEM;
    }

    handle->object.kind = FSFD_OBJECT_MOUNT;
    handle->mnt = mnt;
    handle->mount_flags = combined_flags;
    handle->source = ctx->source ? strdup(ctx->source) : NULL;
    if (ctx->source && !handle->source) {
        fsfd_destroy_object(handle);
        vfs_file_put(file);
        return -ENOMEM;
    }

    ret = fsfdfs_create_file("mnt", &fsfdfs_mnt_file_ops, handle, &mnt_file);
    if (ret < 0) {
        fsfd_destroy_object(handle);
        vfs_file_put(file);
        return ret;
    }

    ret = task_install_file(current_task, mnt_file,
                            (flags & FSMOUNT_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(mnt_file);
    if (ret >= 0)
        ctx->state = FC_STATE_MOUNTED;
    vfs_file_put(file);
    return ret;
}

uint64_t sys_move_mount(int from_dfd, const char *from_pathname_user,
                        int to_dfd, const char *to_pathname_user,
                        uint32_t flags) {
    struct vfs_file *from_file = NULL;
    struct vfs_file *to_file = NULL;
    struct vfs_path from_path = {0};
    struct vfs_path to_path = {0};
    struct vfs_mount *source_mnt = NULL;
    mount_handle_t *mnt_handle = NULL;
    char from_pathname[512];
    char to_pathname[512];
    bool from_empty = (flags & MOVE_MOUNT_F_EMPTY_PATH) != 0;
    bool to_empty = (flags & MOVE_MOUNT_T_EMPTY_PATH) != 0;
    int ret = 0;

    if (!from_empty) {
        if (!from_pathname_user)
            return -EINVAL;
        if (copy_from_user_str(from_pathname, from_pathname_user,
                               sizeof(from_pathname))) {
            return -EFAULT;
        }
        ret = vfs_filename_lookup(from_dfd, from_pathname, LOOKUP_FOLLOW,
                                  &from_path);
        if (ret < 0)
            return ret;
    } else {
        from_file = task_get_file(current_task, from_dfd);
        if (!from_file)
            return -EBADF;
        mnt_handle = fsfd_file_mount(from_file);
        if (mnt_handle) {
            if (!mnt_handle->mnt || mnt_handle->attached) {
                ret = -EINVAL;
                goto out;
            }
        } else {
            source_mnt = fsfd_path_mount(&from_file->f_path);
            if (!source_mnt) {
                ret = -EINVAL;
                goto out;
            }
        }
    }

    if (!to_empty) {
        if (!to_pathname_user) {
            ret = -EINVAL;
            goto out;
        }
        if (copy_from_user_str(to_pathname, to_pathname_user,
                               sizeof(to_pathname))) {
            ret = -EFAULT;
            goto out;
        }
        ret = vfs_filename_lookup(to_dfd, to_pathname, LOOKUP_FOLLOW, &to_path);
        if (ret < 0)
            goto out;
    } else {
        to_file = task_get_file(current_task, to_dfd);
        if (!to_file) {
            ret = -EBADF;
            goto out;
        }
        if (!vfs_path_copy(&to_path, &to_file->f_path)) {
            ret = -ENOENT;
            goto out;
        }
    }

    if (!to_path.dentry || !to_path.dentry->d_inode ||
        !S_ISDIR(to_path.dentry->d_inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    if (mnt_handle) {
        ret = vfs_reconfigure_mount(mnt_handle->mnt, &to_path, true);
        if (ret == 0)
            mnt_handle->attached = true;
        goto out;
    }

    if (!source_mnt)
        source_mnt = fsfd_path_mount(&from_path);
    if (!source_mnt) {
        ret = -EINVAL;
        goto out;
    }

    ret = vfs_reconfigure_mount(source_mnt, &to_path, false);
    if (ret == 0 && from_file)
        mountfd_mark_attached(from_file);

out:
    if (source_mnt)
        vfs_mntput(source_mnt);
    if (to_file)
        vfs_file_put(to_file);
    if (from_file)
        vfs_file_put(from_file);
    vfs_path_put(&to_path);
    vfs_path_put(&from_path);
    return ret;
}

void fsfdfs_init() {
    spin_init(&fsfdfs_mount_lock);
    vfs_register_filesystem(&fsfdfs_fs_type);
}
