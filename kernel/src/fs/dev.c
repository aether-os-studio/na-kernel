#include <libs/keys.h>
#include <fs/vfs/vfs.h>
#include <fs/sys.h>
#include <block/partition.h>
#include <dev/device.h>
#include <drivers/logger.h>
#include <drivers/serial.h>
#include <drivers/tty.h>
#include <drivers/pty.h>
#include <net/netlink.h>
#include <net/netdev.h>
#include <boot/boot.h>
#include <arch/arch.h>
#include <fs/vfs/paged_store.h>
#include <fs/vfs/tmpfs_limit.h>
#include <mm/mm_syscall.h>
#include <task/task.h>

#define DEVTMPFS_MAGIC 0x01021994ULL
#define MAX_DEVTMPFS_FILE_SIZE (128 * 1024 * 1024)

typedef struct devtmpfs_dirent {
    struct llist_header node;
    char *name;
    struct vfs_inode *inode;
} devtmpfs_dirent_t;

typedef struct devtmpfs_inode_info {
    struct vfs_inode vfs_inode;
    spinlock_t lock;
    struct llist_header children;
    paged_file_store_t store;
    char *link_target;
} devtmpfs_inode_info_t;

typedef struct devtmpfs_fs_info {
    spinlock_t populate_lock;
    bool populated;
    bool populating;
    uid32_t root_uid;
    gid32_t root_gid;
    umode_t root_mode;
} devtmpfs_fs_info_t;

static struct vfs_file_system_type devtmpfs_fs_type;
static const struct vfs_super_operations devtmpfs_super_ops;
static const struct vfs_inode_operations devtmpfs_inode_ops;
static const struct vfs_file_operations devtmpfs_dir_file_ops;
static const struct vfs_file_operations devtmpfs_file_ops;
static void devtmpfs_free_dirent(devtmpfs_dirent_t *de);

vfs_node_t *devtmpfs_root = NULL;
bool devfs_initialized = false;

static inline devtmpfs_inode_info_t *devtmpfs_i(vfs_node_t *inode) {
    return inode ? container_of(inode, devtmpfs_inode_info_t, vfs_inode) : NULL;
}

static int devtmpfs_parse_mount_options(devtmpfs_fs_info_t *fsi,
                                        const char *options) {
    char buf[512];
    char *cursor;

    if (!fsi)
        return -EINVAL;
    if (!options || !options[0])
        return 0;

    strncpy(buf, options, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    cursor = buf;
    while (*cursor) {
        char *opt = cursor;
        char *comma = strchr(opt, ',');
        char *value;
        char *end = NULL;
        uint64_t parsed;

        if (comma) {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = opt + strlen(opt);
        }
        if (!*opt)
            continue;

        value = strchr(opt, '=');
        if (!value)
            continue;
        *value++ = '\0';

        if (!strcmp(opt, "mode")) {
            parsed = strtoul(value, &end, 8);
            if (!end || *end || parsed > 07777)
                return -EINVAL;
            fsi->root_mode = S_IFDIR | (umode_t)parsed;
        } else if (!strcmp(opt, "uid")) {
            parsed = strtoul(value, &end, 10);
            if (!end || *end || parsed > 0xffffffffULL)
                return -EINVAL;
            fsi->root_uid = (uid32_t)parsed;
        } else if (!strcmp(opt, "gid")) {
            parsed = strtoul(value, &end, 10);
            if (!end || *end || parsed > 0xffffffffULL)
                return -EINVAL;
            fsi->root_gid = (gid32_t)parsed;
        }
    }

    return 0;
}

static devtmpfs_dirent_t *
devtmpfs_find_dirent_locked(devtmpfs_inode_info_t *info, const char *name) {
    devtmpfs_dirent_t *de, *tmp;

    if (!info || !name)
        return NULL;

    llist_for_each(de, tmp, &info->children, node) {
        if (de->name && streq(de->name, name))
            return de;
    }

    return NULL;
}

static void devtmpfs_lock_dir_infos(devtmpfs_inode_info_t *a,
                                    devtmpfs_inode_info_t *b) {
    if (!a)
        return;

    if (!b || a == b) {
        spin_lock(&a->lock);
        return;
    }

    if ((uintptr_t)a < (uintptr_t)b) {
        spin_lock(&a->lock);
        spin_lock(&b->lock);
    } else {
        spin_lock(&b->lock);
        spin_lock(&a->lock);
    }
}

static void devtmpfs_unlock_dir_infos(devtmpfs_inode_info_t *a,
                                      devtmpfs_inode_info_t *b) {
    if (!a)
        return;

    if (!b || a == b) {
        spin_unlock(&a->lock);
        return;
    }

    spin_unlock(&a->lock);
    spin_unlock(&b->lock);
}

static inline devtmpfs_fs_info_t *devtmpfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (devtmpfs_fs_info_t *)sb->s_fs_info : NULL;
}

static void devtmpfs_ensure_populated(struct vfs_super_block *sb);
static vfs_node_t *devtmpfs_lookup_inode_path(const char *path,
                                              unsigned int flags);
static void setup_consoles(void);

static char ttydev_magic;
static char consoledev_magic;
static tty_t *ttydev_resolve(void *data);
static pty_pair_t *ttydev_resolve_pty(void *data);

static void ttydev_bind_path(tty_t *tty, const char *path) {
    if (!tty || !path)
        return;

    vfs_node_t *inode = devtmpfs_lookup_inode_path(path, LOOKUP_FOLLOW);
    if (!inode)
        return;

    tty_bind_devnode(tty, inode);
    vfs_iput(inode);
}

static void ttydev_bind_file(struct vfs_file *file) {
    device_t *device;

    if (!file || !file->f_inode || !S_ISCHR(file->f_inode->i_mode))
        return;

    device = device_get(file->f_inode->i_rdev);
    if (!device || !device->ptr)
        return;

    if (device->subtype == DEV_TTY) {
        tty_bind_devnode((tty_t *)device->ptr, file->f_inode);
    } else if (device->subtype == DEV_SYSDEV &&
               (streq(device->name, "tty") || streq(device->name, "console"))) {
        tty_t *tty = ttydev_resolve(device->ptr);
        if (tty)
            tty_bind_devnode(tty, file->f_inode);
        else {
            pty_pair_t *pair = ttydev_resolve_pty(device->ptr);
            if (pair)
                pty_session_bind_devnode(pair, file->f_inode);
        }
    }
}

static unsigned char devtmpfs_dtype(umode_t mode) {
    if (S_ISDIR(mode))
        return DT_DIR;
    if (S_ISLNK(mode))
        return DT_LNK;
    if (S_ISCHR(mode))
        return DT_CHR;
    if (S_ISBLK(mode))
        return DT_BLK;
    if (S_ISFIFO(mode))
        return DT_FIFO;
    if (S_ISSOCK(mode))
        return DT_SOCK;
    return DT_REG;
}

static int devtmpfs_resize_inode(struct vfs_inode *inode, uint64_t new_size) {
    devtmpfs_inode_info_t *info = devtmpfs_i(inode);
    uint64_t old_size;
    size_t old_pages;
    size_t new_pages;
    int ret;

    if (!info || S_ISDIR(inode->i_mode) || S_ISCHR(inode->i_mode) ||
        S_ISBLK(inode->i_mode))
        return -EINVAL;
    if (new_size > MAX_DEVTMPFS_FILE_SIZE)
        return -EFBIG;

    old_size = info->store.size;
    if (old_size == new_size)
        return 0;

    old_pages = paged_file_store_pages_for_size(old_size);
    new_pages = paged_file_store_pages_for_size(new_size);

    ret = tmpfs_mem_resize_reserve(old_size, new_size);
    if (ret < 0)
        return ret;

    spin_lock(&info->lock);
    if (new_size > old_size) {
        ret = paged_file_store_ensure_slots(&info->store, new_pages);
        if (ret < 0) {
            spin_unlock(&info->lock);
            tmpfs_mem_resize_reserve(new_size, old_size);
            return ret;
        }
    } else {
        paged_file_store_zero_tail(&info->store, new_size);
    }

    info->store.size = new_size;
    inode->i_size = new_size;
    inode->i_blocks = tmpfs_mem_align(new_size) >> 9;
    inode->i_version++;
    spin_unlock(&info->lock);

    if (new_size < old_size) {
        uint64_t zap_start = PADDING_UP(new_size, PAGE_SIZE);
        uint64_t zap_end = PADDING_UP(old_size, PAGE_SIZE);

        if (zap_start < zap_end)
            paged_file_store_zap_shared_mappings(inode, zap_start, zap_end);

        for (size_t i = new_pages; i < old_pages; i++) {
            uint64_t paddr = 0;

            spin_lock(&info->lock);
            if (i < info->store.page_slots) {
                paddr = info->store.pages[i];
                info->store.pages[i] = 0;
            }
            spin_unlock(&info->lock);

            if (paddr)
                address_release(paddr);
        }
    }

    return 0;
}

static devtmpfs_dirent_t *devtmpfs_find_dirent(struct vfs_inode *dir,
                                               const char *name) {
    devtmpfs_inode_info_t *info = devtmpfs_i(dir);
    devtmpfs_dirent_t *de;

    if (!info || !S_ISDIR(dir->i_mode) || !name)
        return NULL;

    spin_lock(&info->lock);
    de = devtmpfs_find_dirent_locked(info, name);
    spin_unlock(&info->lock);
    return de;
}

static int devtmpfs_add_dirent(struct vfs_inode *dir, const char *name,
                               struct vfs_inode *inode) {
    devtmpfs_inode_info_t *info = devtmpfs_i(dir);
    devtmpfs_dirent_t *de;

    if (!info || !inode || !name || !name[0])
        return -EINVAL;

    de = calloc(1, sizeof(*de));
    if (!de)
        return -ENOMEM;

    de->name = strdup(name);
    if (!de->name) {
        free(de);
        return -ENOMEM;
    }

    de->inode = vfs_igrab(inode);
    llist_init_head(&de->node);
    spin_lock(&info->lock);
    if (devtmpfs_find_dirent_locked(info, name)) {
        spin_unlock(&info->lock);
        devtmpfs_free_dirent(de);
        return -EEXIST;
    }
    llist_append(&info->children, &de->node);
    spin_unlock(&info->lock);
    return 0;
}

static devtmpfs_dirent_t *devtmpfs_detach_dirent(struct vfs_inode *dir,
                                                 const char *name) {
    devtmpfs_inode_info_t *info = devtmpfs_i(dir);
    devtmpfs_dirent_t *de, *tmp;

    if (!info || !name)
        return NULL;

    spin_lock(&info->lock);
    llist_for_each(de, tmp, &info->children, node) {
        if (!de->name || !streq(de->name, name))
            continue;
        llist_delete(&de->node);
        spin_unlock(&info->lock);
        return de;
    }
    spin_unlock(&info->lock);
    return NULL;
}

static void devtmpfs_free_dirent(devtmpfs_dirent_t *de) {
    if (!de)
        return;
    if (de->inode)
        vfs_iput(de->inode);
    free(de->name);
    free(de);
}

static struct vfs_inode *devtmpfs_new_inode(struct vfs_super_block *sb,
                                            struct vfs_inode *dir,
                                            umode_t mode) {
    struct vfs_inode *inode = vfs_alloc_inode(sb);
    devtmpfs_inode_info_t *info = devtmpfs_i(inode);

    if (!inode)
        return NULL;

    spin_init(&info->lock);
    llist_init_head(&info->children);
    inode->i_op = &devtmpfs_inode_ops;
    inode->i_fop = S_ISDIR(mode) ? &devtmpfs_dir_file_ops : &devtmpfs_file_ops;
    vfs_inode_init_owner(inode, dir, mode);
    inode->i_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_blkbits = 12;
    inode->i_ino = (ino64_t)(uintptr_t)inode;
    inode->inode = inode->i_ino;
    inode->i_atime.sec = inode->i_btime.sec = inode->i_ctime.sec =
        inode->i_mtime.sec = (int64_t)(nano_time() / 1000000000ULL);
    return inode;
}

static struct vfs_dentry *devtmpfs_lookup(struct vfs_inode *dir,
                                          struct vfs_dentry *dentry,
                                          unsigned int flags) {
    devtmpfs_inode_info_t *info;
    struct vfs_inode *inode = NULL;

    (void)flags;
    if (!dir || !dentry)
        return ERR_PTR(-EINVAL);

    devtmpfs_ensure_populated(dir->i_sb);

    info = devtmpfs_i(dir);
    if (!info)
        return ERR_PTR(-EINVAL);

    spin_lock(&info->lock);
    devtmpfs_dirent_t *de =
        devtmpfs_find_dirent_locked(info, dentry->d_name.name);
    if (de)
        inode = vfs_igrab(de->inode);
    spin_unlock(&info->lock);

    vfs_d_instantiate(dentry, inode);
    if (inode)
        vfs_iput(inode);
    return dentry;
}

static int devtmpfs_create_common(struct vfs_inode *dir,
                                  struct vfs_dentry *dentry, umode_t mode,
                                  const char *symlink_target, dev64_t rdev) {
    struct vfs_inode *inode;
    devtmpfs_inode_info_t *info;
    int ret;

    if (!dir || !dentry || !S_ISDIR(dir->i_mode))
        return -ENOTDIR;
    if (devtmpfs_find_dirent(dir, dentry->d_name.name))
        return -EEXIST;

    inode = devtmpfs_new_inode(dir->i_sb, dir, mode);
    if (!inode)
        return -ENOMEM;

    info = devtmpfs_i(inode);
    inode->i_rdev = rdev;

    if (S_ISLNK(mode) && symlink_target) {
        info->link_target = strdup(symlink_target);
        if (!info->link_target) {
            vfs_iput(inode);
            return -ENOMEM;
        }
        ret = devtmpfs_resize_inode(inode, strlen(symlink_target));
        if (ret < 0) {
            free(info->link_target);
            info->link_target = NULL;
            vfs_iput(inode);
            return ret;
        }

        loff_t pos = 0;
        spin_lock(&info->lock);
        ret = (int)paged_file_store_write_locked(&info->store, symlink_target,
                                                 strlen(symlink_target), &pos);
        spin_unlock(&info->lock);
        if (ret < 0) {
            free(info->link_target);
            info->link_target = NULL;
            vfs_iput(inode);
            return ret;
        }
    }

    ret = devtmpfs_add_dirent(dir, dentry->d_name.name, inode);
    if (ret < 0) {
        vfs_iput(inode);
        return ret;
    }

    if (S_ISDIR(mode))
        dir->i_nlink++;

    vfs_d_instantiate(dentry, inode);
    vfs_iput(inode);
    return 0;
}

static int devtmpfs_create(struct vfs_inode *dir, struct vfs_dentry *dentry,
                           umode_t mode, bool excl) {
    (void)excl;
    return devtmpfs_create_common(dir, dentry, (mode & 07777) | S_IFREG, NULL,
                                  0);
}

static int devtmpfs_mkdir(struct vfs_inode *dir, struct vfs_dentry *dentry,
                          umode_t mode) {
    return devtmpfs_create_common(dir, dentry, (mode & 07777) | S_IFDIR, NULL,
                                  0);
}

static int devtmpfs_mknod(struct vfs_inode *dir, struct vfs_dentry *dentry,
                          umode_t mode, dev64_t dev) {
    return devtmpfs_create_common(dir, dentry, mode, NULL, dev);
}

static int devtmpfs_symlink(struct vfs_inode *dir, struct vfs_dentry *dentry,
                            const char *target) {
    return devtmpfs_create_common(dir, dentry, S_IFLNK | 0777, target, 0);
}

static int devtmpfs_link(struct vfs_dentry *old_dentry, struct vfs_inode *dir,
                         struct vfs_dentry *new_dentry) {
    int ret;

    if (!old_dentry || !old_dentry->d_inode || !dir || !new_dentry)
        return -EINVAL;
    if (S_ISDIR(old_dentry->d_inode->i_mode))
        return -EPERM;
    if (old_dentry->d_inode->i_sb != dir->i_sb)
        return -EXDEV;

    ret =
        devtmpfs_add_dirent(dir, new_dentry->d_name.name, old_dentry->d_inode);
    if (ret < 0)
        return ret;

    old_dentry->d_inode->i_nlink++;
    vfs_d_instantiate(new_dentry, old_dentry->d_inode);
    return 0;
}

static int devtmpfs_unlink(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    devtmpfs_dirent_t *de;

    if (!dir || !dentry || !dentry->d_inode)
        return -ENOENT;
    if (S_ISDIR(dentry->d_inode->i_mode))
        return -EISDIR;

    de = devtmpfs_detach_dirent(dir, dentry->d_name.name);
    if (!de)
        return -ENOENT;

    if (de->inode->i_nlink)
        de->inode->i_nlink--;
    devtmpfs_free_dirent(de);
    return 0;
}

static int devtmpfs_rmdir(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    devtmpfs_dirent_t *de;
    devtmpfs_inode_info_t *info;
    bool has_children;

    if (!dir || !dentry || !dentry->d_inode ||
        !S_ISDIR(dentry->d_inode->i_mode))
        return -ENOTDIR;

    info = devtmpfs_i(dentry->d_inode);
    spin_lock(&info->lock);
    has_children = !llist_empty(&info->children);
    spin_unlock(&info->lock);
    if (has_children)
        return -ENOTEMPTY;

    de = devtmpfs_detach_dirent(dir, dentry->d_name.name);
    if (!de)
        return -ENOENT;

    if (dir->i_nlink)
        dir->i_nlink--;
    if (de->inode->i_nlink >= 2)
        de->inode->i_nlink -= 2;
    devtmpfs_free_dirent(de);
    return 0;
}

static int devtmpfs_rename(struct vfs_rename_ctx *ctx) {
    devtmpfs_inode_info_t *old_info;
    devtmpfs_inode_info_t *new_info;
    devtmpfs_dirent_t *old_de;
    devtmpfs_dirent_t *victim = NULL;
    char *new_name;
    char *old_name = NULL;

    if (!ctx || !ctx->old_dir || !ctx->new_dir || !ctx->old_dentry ||
        !ctx->new_dentry)
        return -EINVAL;

    if (ctx->flags & (VFS_RENAME_EXCHANGE | VFS_RENAME_WHITEOUT))
        return -EOPNOTSUPP;

    old_info = devtmpfs_i(ctx->old_dir);
    new_info = devtmpfs_i(ctx->new_dir);
    if (!old_info || !new_info)
        return -EINVAL;

    new_name = strdup(ctx->new_dentry->d_name.name);
    if (!new_name)
        return -ENOMEM;

    devtmpfs_lock_dir_infos(old_info, new_info);
    old_de =
        devtmpfs_find_dirent_locked(old_info, ctx->old_dentry->d_name.name);
    if (!old_de) {
        devtmpfs_unlock_dir_infos(old_info, new_info);
        free(new_name);
        return -ENOENT;
    }
    llist_delete(&old_de->node);

    victim =
        devtmpfs_find_dirent_locked(new_info, ctx->new_dentry->d_name.name);
    if (victim)
        llist_delete(&victim->node);
    if (victim) {
        if (S_ISDIR(victim->inode->i_mode) != S_ISDIR(old_de->inode->i_mode)) {
            llist_init_head(&old_de->node);
            llist_append(&old_info->children, &old_de->node);
            llist_init_head(&victim->node);
            llist_append(&new_info->children, &victim->node);
            devtmpfs_unlock_dir_infos(old_info, new_info);
            free(new_name);
            return -ENOTEMPTY;
        }
    } else if (ctx->flags & VFS_RENAME_NOREPLACE) {
        llist_init_head(&old_de->node);
        llist_append(&old_info->children, &old_de->node);
        devtmpfs_unlock_dir_infos(old_info, new_info);
        free(new_name);
        return -EEXIST;
    }

    old_name = old_de->name;
    old_de->name = new_name;
    llist_init_head(&old_de->node);
    llist_append(&new_info->children, &old_de->node);
    devtmpfs_unlock_dir_infos(old_info, new_info);
    free(old_name);
    devtmpfs_free_dirent(victim);
    return 0;
}

static const char *devtmpfs_get_link(struct vfs_dentry *dentry,
                                     struct vfs_inode *inode,
                                     struct vfs_nameidata *nd) {
    devtmpfs_inode_info_t *info = devtmpfs_i(inode);
    (void)dentry;
    (void)nd;
    return info && info->link_target ? info->link_target : ERR_PTR(-EINVAL);
}

static int devtmpfs_permission(struct vfs_inode *inode, int mask) {
    return vfs_inode_permission(inode, mask);
}

static int devtmpfs_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                            uint32_t request_mask, unsigned int flags) {
    (void)request_mask;
    (void)flags;
    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static int devtmpfs_setattr(struct vfs_dentry *dentry,
                            const struct vfs_kstat *stat) {
    int ret = 0;

    if (!dentry || !dentry->d_inode || !stat)
        return -EINVAL;

    if (stat->mode)
        dentry->d_inode->i_mode =
            (dentry->d_inode->i_mode & S_IFMT) | (stat->mode & 07777);
    dentry->d_inode->i_uid = stat->uid;
    dentry->d_inode->i_gid = stat->gid;
    if (!S_ISDIR(dentry->d_inode->i_mode) &&
        !S_ISCHR(dentry->d_inode->i_mode) &&
        !S_ISBLK(dentry->d_inode->i_mode) &&
        stat->size != dentry->d_inode->i_size) {
        ret = devtmpfs_resize_inode(dentry->d_inode, stat->size);
    }
    dentry->d_inode->inode = dentry->d_inode->i_ino;
    return ret;
}

static loff_t devtmpfs_llseek(struct vfs_file *file, loff_t offset,
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

static ssize_t devtmpfs_read(struct vfs_file *file, void *buf, size_t count,
                             loff_t *ppos) {
    devtmpfs_inode_info_t *info;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (S_ISCHR(file->f_inode->i_mode) || S_ISBLK(file->f_inode->i_mode))
        return device_read(file->f_inode->i_rdev, buf, (uint64_t)*ppos, count,
                           file);

    info = devtmpfs_i(file->f_inode);
    if (!info)
        return -EINVAL;

    spin_lock(&info->lock);
    ssize_t ret = paged_file_store_read_locked(&info->store, buf, count, ppos);
    spin_unlock(&info->lock);
    return ret;
}

static ssize_t devtmpfs_write(struct vfs_file *file, const void *buf,
                              size_t count, loff_t *ppos) {
    devtmpfs_inode_info_t *info;
    uint64_t end;
    ssize_t ret;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (S_ISCHR(file->f_inode->i_mode) || S_ISBLK(file->f_inode->i_mode))
        return device_write(file->f_inode->i_rdev, (void *)buf, (uint64_t)*ppos,
                            count, file);

    info = devtmpfs_i(file->f_inode);
    if (*ppos < 0)
        return -EINVAL;
    if ((uint64_t)*ppos > UINT64_MAX - count)
        return -EFBIG;

    end = (uint64_t)*ppos + count;
    ret = devtmpfs_resize_inode(file->f_inode, MAX(info->store.size, end));
    if (ret < 0)
        return ret;

    spin_lock(&info->lock);
    ret = paged_file_store_write_locked(&info->store, buf, count, ppos);
    if (ret > 0 && end > info->store.size)
        info->store.size = end;
    file->f_inode->i_size = info->store.size;
    file->f_inode->i_version++;
    spin_unlock(&info->lock);
    return ret;
}

static int devtmpfs_iterate_shared(struct vfs_file *file,
                                   struct vfs_dir_context *ctx) {
    devtmpfs_inode_info_t *info;
    devtmpfs_dirent_t *de, *tmp;
    loff_t index = 0;

    if (!file || !file->f_inode || !ctx || !S_ISDIR(file->f_inode->i_mode))
        return -ENOTDIR;

    devtmpfs_ensure_populated(file->f_inode->i_sb);

    info = devtmpfs_i(file->f_inode);
    spin_lock(&info->lock);
    llist_for_each(de, tmp, &info->children, node) {
        index++;
        if (index <= ctx->pos)
            continue;
        if (ctx->actor(ctx, de->name, (int)strlen(de->name), index,
                       de->inode->i_ino, devtmpfs_dtype(de->inode->i_mode))) {
            break;
        }
        ctx->pos = index;
    }
    spin_unlock(&info->lock);

    file->f_pos = ctx->pos;
    return 0;
}

static int devtmpfs_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    ssize_t ret;

    if (!inode || !file)
        return -EINVAL;
    if (S_ISDIR(inode->i_mode))
        devtmpfs_ensure_populated(inode->i_sb);
    if ((file->f_flags & O_PATH) == 0 &&
        (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))) {
        ret = device_open(inode->i_rdev, file);
        if (ret < 0)
            return (int)ret;
        ttydev_bind_file(file);
    }
    file->f_op = inode->i_fop;
    return 0;
}

static int devtmpfs_release_file(struct vfs_inode *inode,
                                 struct vfs_file *file) {
    (void)file;
    if (inode && (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)))
        device_close(inode->i_rdev, file);
    return 0;
}

static long devtmpfs_ioctl(struct vfs_file *file, unsigned long cmd,
                           unsigned long arg) {
    if (!file || !file->f_inode)
        return -EINVAL;
    if (S_ISCHR(file->f_inode->i_mode) || S_ISBLK(file->f_inode->i_mode))
        return device_ioctl(file->f_inode->i_rdev, (int)cmd, (void *)arg, file);
    return -EINVAL;
}

static void *devtmpfs_mmap(struct vfs_file *file, void *addr, size_t offset,
                           size_t size, size_t prot, uint64_t flags) {
    devtmpfs_inode_info_t *info;
    uint64_t pt_flags = PT_FLAG_U;
    uint64_t *pgdir;
    int ret;

    if (!file || !file->f_inode)
        return (void *)(int64_t)-EINVAL;
    if (S_ISCHR(file->f_inode->i_mode) || S_ISBLK(file->f_inode->i_mode)) {
        return device_map(file->f_inode->i_rdev, addr, offset, size, prot,
                          file);
    }
    if (!S_ISREG(file->f_inode->i_mode))
        return (void *)(int64_t)-ENODEV;
    if ((flags & MAP_TYPE) == MAP_PRIVATE)
        return general_map(file, (uint64_t)addr, size, prot, flags, offset);
    if (offset > SIZE_MAX - size)
        return (void *)(int64_t)-EINVAL;

    info = devtmpfs_i(file->f_inode);
    if (!info)
        return (void *)(int64_t)-EINVAL;

    if (prot & PROT_READ)
        pt_flags |= PT_FLAG_R;
    if (prot & PROT_WRITE)
        pt_flags |= PT_FLAG_W;
    if (prot & PROT_EXEC)
        pt_flags |= PT_FLAG_X;
    if (!(pt_flags & (PT_FLAG_R | PT_FLAG_W | PT_FLAG_X)))
        pt_flags |= PT_FLAG_R;

    pgdir = get_current_page_dir(true);
    spin_lock(&info->lock);
    ret = paged_file_store_map_shared_locked(&info->store, pgdir,
                                             (uint64_t)addr, (uint64_t)offset,
                                             (uint64_t)size, pt_flags);
    spin_unlock(&info->lock);
    if (ret < 0)
        return (void *)(int64_t)ret;

    return addr;
}

static __poll_t devtmpfs_poll_file(struct vfs_file *file,
                                   struct vfs_poll_table *pt) {
    if (!file || !file->f_inode)
        return EPOLLNVAL;
    if (S_ISCHR(file->f_inode->i_mode) || S_ISBLK(file->f_inode->i_mode)) {
        ttydev_bind_file(file);
        uint32_t events = pt ? pt->events
                             : (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR |
                                EPOLLHUP | EPOLLNVAL);
        return (__poll_t)device_poll(file->f_inode->i_rdev, events, file);
    }
    return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
}

static struct vfs_inode *devtmpfs_alloc_inode(struct vfs_super_block *sb) {
    devtmpfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void devtmpfs_destroy_inode(struct vfs_inode *inode) {
    free(devtmpfs_i(inode));
}

static void devtmpfs_evict_inode(struct vfs_inode *inode) {
    devtmpfs_inode_info_t *info = devtmpfs_i(inode);
    devtmpfs_dirent_t *de, *tmp;

    if (!info)
        return;
    tmpfs_mem_release(info->store.size);
    paged_file_store_destroy(&info->store);
    free(info->link_target);
    spin_lock(&info->lock);
    llist_for_each(de, tmp, &info->children, node) {
        llist_delete(&de->node);
        devtmpfs_free_dirent(de);
    }
    spin_unlock(&info->lock);
}

static int devtmpfs_init_fs_context(struct vfs_fs_context *fc) {
    if (!fc)
        return -EINVAL;
    fc->fs_private = calloc(1, sizeof(devtmpfs_fs_info_t));
    if (!fc->fs_private)
        return -ENOMEM;
    spin_init(&((devtmpfs_fs_info_t *)fc->fs_private)->populate_lock);
    return 0;
}

static int devtmpfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    devtmpfs_fs_info_t *fsi;
    vfs_node_t *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};
    int opt_ret;

    if (!fc)
        return -EINVAL;

    fsi = fc->fs_private;
    sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!sb) {
        free(fsi);
        fc->fs_private = NULL;
        return -ENOMEM;
    }

    sb->s_dev = (0UL << 8) | 4;
    sb->s_op = &devtmpfs_super_ops;
    sb->s_type = &devtmpfs_fs_type;
    sb->s_magic = DEVTMPFS_MAGIC;
    sb->s_fs_info = fsi;
    fc->fs_private = NULL;
    fsi->root_uid = vfs_current_fsuid();
    fsi->root_gid = vfs_current_fsgid();
    fsi->root_mode = S_IFDIR | 0755;
    opt_ret = devtmpfs_parse_mount_options(fsi, fc->data);
    if (opt_ret < 0) {
        vfs_put_super(sb);
        return opt_ret;
    }

    root_inode = devtmpfs_new_inode(sb, NULL, fsi->root_mode);
    if (!root_inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    root_inode->i_uid = fsi->root_uid;
    root_inode->i_gid = fsi->root_gid;
    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    fc->sb = sb;
    vfs_iput(root_inode);
    return 0;
}

static void devtmpfs_put_super(struct vfs_super_block *sb) {
    if (!sb || !sb->s_fs_info)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations devtmpfs_super_ops = {
    .alloc_inode = devtmpfs_alloc_inode,
    .destroy_inode = devtmpfs_destroy_inode,
    .evict_inode = devtmpfs_evict_inode,
    .put_super = devtmpfs_put_super,
};

static const struct vfs_inode_operations devtmpfs_inode_ops = {
    .lookup = devtmpfs_lookup,
    .create = devtmpfs_create,
    .link = devtmpfs_link,
    .unlink = devtmpfs_unlink,
    .symlink = devtmpfs_symlink,
    .mkdir = devtmpfs_mkdir,
    .rmdir = devtmpfs_rmdir,
    .mknod = devtmpfs_mknod,
    .rename = devtmpfs_rename,
    .get_link = devtmpfs_get_link,
    .permission = devtmpfs_permission,
    .getattr = devtmpfs_getattr,
    .setattr = devtmpfs_setattr,
};

static const struct vfs_file_operations devtmpfs_dir_file_ops = {
    .llseek = devtmpfs_llseek,
    .iterate_shared = devtmpfs_iterate_shared,
    .open = devtmpfs_open_file,
    .release = devtmpfs_release_file,
    .poll = devtmpfs_poll_file,
};

static const struct vfs_file_operations devtmpfs_file_ops = {
    .llseek = devtmpfs_llseek,
    .read = devtmpfs_read,
    .write = devtmpfs_write,
    .unlocked_ioctl = devtmpfs_ioctl,
    .mmap = devtmpfs_mmap,
    .open = devtmpfs_open_file,
    .release = devtmpfs_release_file,
    .poll = devtmpfs_poll_file,
};

static struct vfs_file_system_type devtmpfs_fs_type = {
    .name = "devtmpfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = devtmpfs_init_fs_context,
    .get_tree = devtmpfs_get_tree,
};

static vfs_node_t *devtmpfs_lookup_inode_path(const char *path,
                                              unsigned int lookup_flags) {
    struct vfs_path p = {0};
    vfs_node_t *inode = NULL;

    if (!path)
        return NULL;
    if (vfs_filename_lookup(AT_FDCWD, path, lookup_flags, &p) < 0)
        return NULL;
    if (p.dentry && p.dentry->d_inode)
        inode = vfs_igrab(p.dentry->d_inode);
    vfs_path_put(&p);
    return inode;
}

static void devtmpfs_refresh_root(void) {
    vfs_node_t *root = devtmpfs_lookup_inode_path("/dev", LOOKUP_FOLLOW);

    if (!root)
        return;
    if (devtmpfs_root)
        vfs_iput(devtmpfs_root);
    devtmpfs_root = root;
}

static void devfs_ensure_parent_dirs(const char *path) {
    char *copy;
    char *slash;

    if (!path || path[0] != '/')
        return;

    copy = strdup(path);
    if (!copy)
        return;

    for (slash = copy + 1; (slash = strchr(slash, '/')) != NULL; slash++) {
        *slash = '\0';
        (void)vfs_mkdirat(AT_FDCWD, copy, 0755, true);
        *slash = '/';
    }

    free(copy);
}

static void devfs_register_existing_devices(void) {
    int subtype;

    for (subtype = 0; subtype < DEV_MAX; subtype++) {
        uint64_t index = 0;

        while (true) {
            device_t *device = device_find(subtype, index++);
            if (!device)
                break;
            devfs_register_device(device);
        }
    }
}

static void devtmpfs_populate_nodes(void) {
    pty_init();

    (void)vfs_mkdirat(AT_FDCWD, "/dev/shm", 0755, true);
    (void)vfs_mkdirat(AT_FDCWD, "/dev/pts", 0755, true);
    (void)vfs_mkdirat(AT_FDCWD, "/dev/bus", 0755, true);
    (void)vfs_mkdirat(AT_FDCWD, "/dev/bus/usb", 0755, true);

    devfs_initialized = true;
    devfs_register_existing_devices();
    setup_consoles();
    ptmx_init();
    pts_init();

    devtmpfs_refresh_root();
}

static void devtmpfs_ensure_populated(struct vfs_super_block *sb) {
    devtmpfs_fs_info_t *fsi = devtmpfs_sb_info(sb);

    if (!fsi)
        return;

    spin_lock(&fsi->populate_lock);
    if (fsi->populated || fsi->populating) {
        spin_unlock(&fsi->populate_lock);
        return;
    }
    fsi->populating = true;
    spin_unlock(&fsi->populate_lock);

    devtmpfs_populate_nodes();

    spin_lock(&fsi->populate_lock);
    fsi->populating = false;
    fsi->populated = true;
    spin_unlock(&fsi->populate_lock);
}

typedef struct inputdev_file {
    struct llist_header node;
    struct input_event *event_queue;
    size_t event_queue_capacity;
    size_t event_queue_head;
    size_t event_queue_tail;
    size_t event_queue_count;
    bool event_queue_overflow;
    spinlock_t event_queue_lock;
    int clock_id;
} inputdev_file_t;

ssize_t inputdev_open(void *data, void *arg) {
    dev_input_event_t *event = data;
    fd_t *fd = arg;
    inputdev_file_t *file;

    if (!event || !fd)
        return -EINVAL;

    file = calloc(1, sizeof(*file));
    if (!file)
        return -ENOMEM;
    file->event_queue_capacity =
        INPUT_EVENT_RING_SIZE / sizeof(struct input_event);
    if (file->event_queue_capacity == 0)
        file->event_queue_capacity = 128;
    file->event_queue =
        calloc(file->event_queue_capacity, sizeof(struct input_event));
    if (!file->event_queue) {
        free(file);
        return -ENOMEM;
    }
    spin_init(&file->event_queue_lock);
    file->clock_id = CLOCK_MONOTONIC;
    if (device_file_set_private(fd, file) < 0) {
        free(file->event_queue);
        free(file);
        return -EBADF;
    }

    spin_lock(&event->open_files_lock);
    llist_append(&event->open_files, &file->node);
    __atomic_add_fetch(&event->timesOpened, 1, __ATOMIC_ACQ_REL);
    spin_unlock(&event->open_files_lock);
    return 0;
}

ssize_t inputdev_close(void *data, void *arg) {
    dev_input_event_t *event = data;
    fd_t *fd = arg;
    inputdev_file_t *file = device_file_private(fd);

    if (!event || !file)
        return 0;

    spin_lock(&event->open_files_lock);
    llist_delete(&file->node);
    __atomic_sub_fetch(&event->timesOpened, 1, __ATOMIC_ACQ_REL);
    spin_unlock(&event->open_files_lock);

    device_file_set_private(fd, NULL);
    free(file->event_queue);
    free(file);
    return 0;
}

static bool input_event_queue_push(inputdev_file_t *file,
                                   const struct input_event *in,
                                   bool *wake_needed) {
    struct input_event queued;

    if (!file || !in || !file->event_queue || !file->event_queue_capacity)
        return false;

    queued = *in;
    if (file->clock_id == CLOCK_REALTIME)
        queued.sec += boot_get_boottime();

    spin_lock(&file->event_queue_lock);
    bool was_empty =
        file->event_queue_count == 0 && !file->event_queue_overflow;

    if (file->event_queue_count >= file->event_queue_capacity) {
        file->event_queue_head =
            (file->event_queue_head + 1) % file->event_queue_capacity;
        file->event_queue_count--;
        file->event_queue_overflow = true;
    }

    file->event_queue[file->event_queue_tail] = queued;
    file->event_queue_tail =
        (file->event_queue_tail + 1) % file->event_queue_capacity;
    file->event_queue_count++;

    if (wake_needed)
        *wake_needed = was_empty || file->event_queue_overflow;

    spin_unlock(&file->event_queue_lock);
    return true;
}

static void inputdev_notify_readable(dev_input_event_t *event) {
    if (!event || !event->devnode)
        return;

    vfs_poll_notify_inode(event->devnode, EPOLLIN | EPOLLRDNORM);
}

static size_t input_event_queue_pop(inputdev_file_t *file,
                                    struct input_event *out,
                                    size_t max_events) {
    if (!file || !out || max_events == 0)
        return 0;

    size_t produced = 0;
    spin_lock(&file->event_queue_lock);

    if (file->event_queue_overflow && produced < max_events) {
        memset(&out[produced], 0, sizeof(struct input_event));
        uint64_t now_ns = nano_time();
        out[produced].sec = now_ns / 1000000000ULL;
        out[produced].usec = (now_ns % 1000000000ULL) / 1000ULL;
        out[produced].type = EV_SYN;
        out[produced].code = 3;
        out[produced].value = 0;
        file->event_queue_overflow = false;
        produced++;
    }

    while (produced < max_events && file->event_queue_count > 0 &&
           file->event_queue_capacity > 0) {
        out[produced++] = file->event_queue[file->event_queue_head];
        file->event_queue_head =
            (file->event_queue_head + 1) % file->event_queue_capacity;
        file->event_queue_count--;
    }

    spin_unlock(&file->event_queue_lock);
    return produced;
}

static bool input_event_queue_has_data(inputdev_file_t *file) {
    bool has_data = false;

    if (!file)
        return false;

    spin_lock(&file->event_queue_lock);
    has_data = (file->event_queue_count > 0) || file->event_queue_overflow;
    spin_unlock(&file->event_queue_lock);
    return has_data;
}

ssize_t inputdev_event_read(void *data, void *buf, uint64_t offset,
                            uint64_t len, fd_t *fd) {
    dev_input_event_t *event = data;
    inputdev_file_t *file = device_file_private(fd);
    (void)offset;
    if (!event || !file || !buf || !fd)
        return -EINVAL;
    if (len == 0)
        return 0;
    if (len < sizeof(struct input_event))
        return -EINVAL;

    len = (len / sizeof(struct input_event)) * sizeof(struct input_event);
    struct input_event *events = (struct input_event *)buf;
    size_t max_events = len / sizeof(struct input_event);

    while (true) {
        size_t cnt = input_event_queue_pop(file, events, max_events);
        if (cnt > 0)
            return cnt * sizeof(struct input_event);
        if (fd && (fd_get_flags(fd) & O_NONBLOCK))
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(fd, EPOLLIN | EPOLLERR |
                                                         EPOLLHUP | EPOLLNVAL);
        if (reason < 0)
            return reason;
    }
}

ssize_t inputdev_event_write(void *data, const void *buf, uint64_t offset,
                             uint64_t len, uint64_t flags) {
    (void)data;
    (void)buf;
    (void)offset;
    (void)flags;
    return len;
}

ssize_t inputdev_ioctl(void *data, ssize_t request, ssize_t arg, fd_t *fd) {
    dev_input_event_t *event = data;
    inputdev_file_t *file = device_file_private(fd);
    size_t number = _IOC_NR(request);
    size_t size = _IOC_SIZE(request);
    ssize_t ret = -ENOTTY;

    if (!event || !file)
        return -EBADF;

    if (number >= 0x20 && number < (0x20 + EV_CNT))
        return event->event_bit(data, request, (void *)arg);
    if (number >= 0x40 && number < (0x40 + ABS_CNT))
        return event->event_bit(data, request, (void *)arg);
    if (request == 0x540b)
        return 0;

    switch (number) {
    case 0x01:
        if (!arg || copy_to_user((void *)arg, &(int){0x10001}, sizeof(int)))
            return -EFAULT;
        ret = 0;
        break;
    case 0x02:
        if (!arg ||
            copy_to_user((void *)arg, &event->inputid, sizeof(struct input_id)))
            return -EFAULT;
        ret = 0;
        break;
    case 0x06: {
        int to_copy = MIN(size, (size_t)strlen(event->devname) + 1);
        if (to_copy && (!arg || copy_to_user((void *)arg, event->devname,
                                             (size_t)to_copy)))
            return -EFAULT;
        ret = to_copy;
        break;
    }
    case 0x07: {
        int to_copy = MIN(size, (size_t)strlen(event->physloc) + 1);
        if (to_copy && (!arg || copy_to_user((void *)arg, event->physloc,
                                             (size_t)to_copy)))
            return -EFAULT;
        ret = to_copy;
        break;
    }
    case 0x08:
        if (event->uniq[0]) {
            int to_copy = MIN(size, (size_t)strlen(event->uniq) + 1);
            if (to_copy && (!arg || copy_to_user((void *)arg, event->uniq,
                                                 (size_t)to_copy)))
                return -EFAULT;
            ret = to_copy;
        } else {
            ret = -ENODATA;
        }
        break;
    case 0x09:
        if (size) {
            int to_copy = MIN(sizeof(size_t), size);
            if (!arg ||
                copy_to_user((void *)arg, &event->properties, (size_t)to_copy))
                return -EFAULT;
            ret = to_copy;
        } else {
            ret = 0;
        }
        break;
    case 0x03:
    case 0x18:
    case 0x19:
    case 0x1b:
        ret = event->event_bit(data, request, (void *)arg);
        break;
    case 0xa0: {
        int clock_id;
        if (!arg || copy_from_user(&clock_id, (void *)arg, sizeof(clock_id)))
            return -EFAULT;
        if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME &&
            clock_id != CLOCK_BOOTTIME)
            return -EINVAL;
        file->clock_id = clock_id;
        ret = 0;
        break;
    }
    case 0x91:
        ret = 0;
        break;
    default:
        printk("inputdev_ioctl(): Unsupported ioctl: %#018lx\n", request);
        ret = -ENOTTY;
        break;
    }

    return ret;
}

ssize_t inputdev_poll(void *data, size_t event, fd_t *fd) {
    inputdev_file_t *file = device_file_private(fd);
    (void)data;
    if (input_event_queue_has_data(file) && (event & EPOLLIN))
        return EPOLLIN;
    return 0;
}

void devfs_register_device(device_t *device) {
    char path[128];

    if (!devfs_initialized || !device || !device->name)
        return;

    snprintf(path, sizeof(path), "/dev/%s", device->name);
    devfs_ensure_parent_dirs(path);
    vfs_mknodat(AT_FDCWD, path,
                device->mode | (device->type == DEV_BLOCK ? S_IFBLK : S_IFCHR),
                device->dev, true);

    if (device->subtype == DEV_INPUT && device->ptr) {
        dev_input_event_t *event = device->ptr;
        vfs_node_t *inode = devtmpfs_lookup_inode_path(path, LOOKUP_FOLLOW);
        if (inode) {
            if (event->devnode)
                vfs_iput(event->devnode);
            event->devnode = inode;
        }
    }

    if (device->subtype == DEV_TTY && device->ptr)
        ttydev_bind_path(device->ptr, path);
    if (device->subtype == DEV_SYSDEV && streq(device->name, "console"))
        ttydev_bind_path(kernel_session, path);

    if (device->type == DEV_BLOCK && device->ptr) {
        vfs_node_t *inode = devtmpfs_lookup_inode_path(path, LOOKUP_FOLLOW);
        partition_t *part = device->ptr;
        if (!inode)
            return;
        inode->i_size = 512ULL * (part->ending_lba - part->starting_lba + 1);
        vfs_iput(inode);
    }
}

void devfs_unregister_device(device_t *device) {
    char path[128];

    if (!devfs_initialized || !device || !device->name || !device->name[0])
        return;

    snprintf(path, sizeof(path), "/dev/%s", device->name);
    vfs_unlinkat(AT_FDCWD, path, 0, true);
}

void devtmpfs_init() {
    vfs_register_filesystem(&devtmpfs_fs_type);
    vfs_mkdirat(AT_FDCWD, "/dev", 0755, true);
    vfs_do_mount(AT_FDCWD, "/dev", "devtmpfs", 0, NULL, NULL);
    devtmpfs_refresh_root();
}

void devtmpfs_init_umount() {
    vfs_do_umount(AT_FDCWD, "/dev", 0);
    if (devtmpfs_root) {
        vfs_iput(devtmpfs_root);
        devtmpfs_root = NULL;
    }
}

ssize_t nulldev_read(void *data, void *buf, uint64_t offset, uint64_t len,
                     uint64_t flags) {
    (void)data;
    (void)buf;
    (void)offset;
    (void)len;
    (void)flags;
    return 0;
}

ssize_t nulldev_write(void *data, const void *buf, uint64_t offset,
                      uint64_t len, uint64_t flags) {
    (void)data;
    (void)offset;
    (void)flags;
    return len;
}

ssize_t nulldev_ioctl(void *data, ssize_t request, ssize_t arg, fd_t *fd) {
    (void)data;
    (void)request;
    (void)arg;
    (void)fd;
    return 0;
}

static uint64_t simple_rand() {
    uint32_t seed = boot_get_boottime() * 100 + nano_time() / 10;
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
    return ((uint64_t)seed << 32) | seed;
}

ssize_t zerodev_read(void *data, void *buf, uint64_t offset, uint64_t len,
                     uint64_t flags) {
    (void)data;
    (void)offset;
    (void)flags;
    memset(buf, 0, len);
    return len;
}

ssize_t zerodev_write(void *data, const void *buf, uint64_t offset,
                      uint64_t len, uint64_t flags) {
    (void)data;
    (void)buf;
    (void)offset;
    (void)len;
    (void)flags;
    return len;
}

ssize_t zerodev_ioctl(void *data, ssize_t request, ssize_t arg, fd_t *fd) {
    (void)data;
    (void)request;
    (void)arg;
    (void)fd;
    return 0;
}

ssize_t urandom_read(void *data, void *buf, uint64_t offset, uint64_t len,
                     uint64_t flags) {
    (void)data;
    (void)offset;
    (void)flags;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t rand = simple_rand();
        uint8_t byte = (rand >> 5) & 0xFF;
        ((uint8_t *)buf)[i] = byte;
    }
    return len;
}

ssize_t urandom_write(void *data, const void *buf, uint64_t offset,
                      uint64_t len, uint64_t flags) {
    (void)data;
    (void)buf;
    (void)offset;
    (void)len;
    (void)flags;
    return 0;
}

ssize_t urandom_ioctl(void *data, ssize_t request, ssize_t arg, fd_t *fd) {
    (void)data;
    (void)request;
    (void)arg;
    (void)fd;
    return 0;
}

extern char *default_console;

static tty_t *ttydev_resolve(void *data) {
    if (data == &consoledev_magic)
        return kernel_session;
    if (!current_task)
        return kernel_session;
    return tty_lookup_session_by_sid(current_task->sid);
}

static pty_pair_t *ttydev_resolve_pty(void *data) {
    if (data == &consoledev_magic || !current_task)
        return NULL;
    return pty_lookup_session_by_sid(current_task->sid);
}

static ssize_t ttydev_open(void *data, void *arg) {
    (void)arg;
    return (ttydev_resolve(data) || ttydev_resolve_pty(data)) ? 0 : -ENXIO;
}

static ssize_t ttydev_ioctl(void *data, ssize_t request, ssize_t arg,
                            fd_t *fd) {
    tty_t *tty = ttydev_resolve(data);
    (void)fd;
    if (!tty) {
        pty_pair_t *pair = ttydev_resolve_pty(data);
        return pair ? pts_ioctl(pair, (uint64_t)request, (void *)arg) : -ENXIO;
    }
    return tty_ioctl(tty, (int)request, (void *)arg);
}

static ssize_t ttydev_poll(void *data, int events, fd_t *fd) {
    tty_t *tty = ttydev_resolve(data);
    if (!tty) {
        pty_pair_t *pair = ttydev_resolve_pty(data);
        return pair ? pty_session_poll(pair, NULL) : -ENXIO;
    }
    return tty_poll(tty, events, fd);
}

static ssize_t ttydev_read(void *data, void *buf, uint64_t offset, uint64_t len,
                           fd_t *fd) {
    tty_t *tty = ttydev_resolve(data);
    if (!tty) {
        pty_pair_t *pair = ttydev_resolve_pty(data);
        return pair ? pty_session_read(pair, fd, buf, len) : -ENXIO;
    }
    return tty_read(tty, buf, offset, len, fd);
}

static ssize_t ttydev_write(void *data, void *buf, uint64_t offset,
                            uint64_t len, fd_t *fd) {
    tty_t *tty = ttydev_resolve(data);
    if (!tty) {
        pty_pair_t *pair = ttydev_resolve_pty(data);
        return pair ? pty_session_write(pair, fd, buf, len) : -ENXIO;
    }
    return tty_write(tty, buf, offset, len, fd);
}

void setup_consoles() { ttydev_bind_path(kernel_session, "/dev/console"); }

ssize_t kmsg_read(void *data, void *buf, uint64_t offset, uint64_t len,
                  uint64_t flags) {
    fd_t *fd = (fd_t *)flags;

    (void)data;
    (void)offset;

    return logger_kmsg_read(fd, buf, len, fd ? fd->f_flags : 0);
}

ssize_t kmsg_poll(void *dev, int events, fd_t *fd) {
    (void)dev;
    return logger_kmsg_poll(fd, events);
}

ssize_t kmsg_write(void *data, const void *buf, uint64_t offset, uint64_t len,
                   uint64_t flags) {
    (void)data;
    (void)offset;
    (void)flags;
    return logger_kmsg_write(buf, len);
}

ssize_t kmsg_ioctl(void *data, ssize_t request, ssize_t arg, fd_t *fd) {
    (void)data;
    (void)request;
    (void)arg;
    (void)fd;
    return -ENOTTY;
}

enum {
    RFKILL_TYPE_WLAN = 1,
    RFKILL_OP_ADD = 0,
};

typedef struct rfkill_event {
    uint32_t idx;
    uint8_t type;
    uint8_t op;
    uint8_t soft;
    uint8_t hard;
} rfkill_event_t;

ssize_t rfkill_read(void *data, void *buf, uint64_t offset, uint64_t len,
                    uint64_t flags) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    uint64_t want_event = offset / sizeof(rfkill_event_t);
    uint64_t event_index = 0;
    fd_t *fd = (fd_t *)flags;
    size_t count;

    (void)data;

    if (!buf)
        return -EINVAL;
    if (len < sizeof(rfkill_event_t))
        return -EINVAL;

    count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));
    for (size_t i = 0; i < count; i++) {
        netdev_t *dev = devs[i];

        if (!dev || dev->type != NETDEV_TYPE_WIFI)
            continue;

        if (event_index == want_event) {
            rfkill_event_t event = {
                .idx = dev->id,
                .type = RFKILL_TYPE_WLAN,
                .op = RFKILL_OP_ADD,
                .soft = 0,
                .hard = 0,
            };

            memcpy(buf, &event, sizeof(event));
            for (size_t j = 0; j < count; j++)
                netdev_put(devs[j]);
            return sizeof(event);
        }

        event_index++;
    }

    for (size_t i = 0; i < count; i++)
        netdev_put(devs[i]);

    if (fd && (fd_get_flags(fd) & O_NONBLOCK))
        return -EAGAIN;
    return 0;
}

ssize_t rfkill_poll(void *data, int events, fd_t *fd) {
    (void)data;
    (void)events;
    (void)fd;
    return 0;
}

void devfs_nodes_init() {
    vfs_mkdirat(AT_FDCWD, "/dev/shm", 0755, true);
    vfs_mkdirat(AT_FDCWD, "/dev/bus", 0755, true);
    vfs_mkdirat(AT_FDCWD, "/dev/bus/usb", 0755, true);

    device_install(DEV_CHAR, DEV_SYSDEV, NULL, "null", 0, NULL, nulldev_ioctl,
                   NULL, NULL, nulldev_read, nulldev_write, NULL);
    device_install(DEV_CHAR, DEV_SYSDEV, NULL, "zero", 0, NULL, NULL,
                   zerodev_ioctl, NULL, zerodev_read, zerodev_write, NULL);
    device_install(DEV_CHAR, DEV_SYSDEV, NULL, "urandom", 0, NULL, NULL,
                   urandom_ioctl, NULL, urandom_read, urandom_write, NULL);
    device_install(DEV_CHAR, DEV_SYSDEV, kernel_session, "kmsg", 0, NULL, NULL,
                   kmsg_ioctl, kmsg_poll, kmsg_read, kmsg_write, NULL);
    device_install(DEV_CHAR, DEV_SYSDEV, &consoledev_magic, "console", 0,
                   ttydev_open, NULL, ttydev_ioctl, ttydev_poll, ttydev_read,
                   ttydev_write, NULL);
    device_install(DEV_CHAR, DEV_SYSDEV, &ttydev_magic, "tty", 0, ttydev_open,
                   NULL, ttydev_ioctl, ttydev_poll, ttydev_read, ttydev_write,
                   NULL);
    device_install(DEV_CHAR, DEV_SYSDEV, NULL, "rfkill", 0, NULL, NULL, NULL,
                   rfkill_poll, rfkill_read, NULL, NULL);

    setup_consoles();

    pty_init();
    ptmx_init();
    pts_init();
}

void input_generate_event(void *data, uint16_t type, uint16_t code,
                          int32_t value, uint64_t sec, uint64_t usecs) {
    dev_input_event_t *item = data;
    if (!item)
        return;

    tty_input_event(item, type, code, value);
    if (__atomic_load_n(&item->timesOpened, __ATOMIC_ACQUIRE) == 0)
        return;

    struct input_event event;
    memset(&event, 0, sizeof(struct input_event));
    event.sec = sec;
    event.usec = usecs;
    event.type = type;
    event.code = code;
    event.value = value;

    bool wake_needed = false;
    inputdev_file_t *file, *tmp;
    spin_lock(&item->open_files_lock);
    llist_for_each(file, tmp, &item->open_files, node) {
        bool file_wake = false;
        if (input_event_queue_push(file, &event, &file_wake) && file_wake)
            wake_needed = true;
    }
    spin_unlock(&item->open_files_lock);
    if (wake_needed)
        inputdev_notify_readable(item);
}
