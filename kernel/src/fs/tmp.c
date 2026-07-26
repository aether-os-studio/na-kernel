#include <fs/tmp.h>
#include <fs/pipe.h>
#include <fs/vfs/tmpfs_limit.h>
#include <mm/mm.h>
#include <mm/mm_syscall.h>
#include <fs/fs_syscall.h>

#define MAX_TMPFS_FILE_SIZE (64 * 1024 * 1024)

spinlock_t tmpfs_mem_limit_lock = SPIN_INIT;
uint64_t tmpfs_mem_used = 0;

static struct vfs_file_system_type tmpfs_fs_type;
static const struct vfs_super_operations tmpfs_super_ops;
static const struct vfs_inode_operations tmpfs_inode_ops;
static const struct vfs_file_operations tmpfs_dir_ops;
static const struct vfs_file_operations tmpfs_file_ops;
static void tmpfs_free_dirent(tmpfs_dirent_t *de);

static inline tmpfs_fs_info_t *tmpfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (tmpfs_fs_info_t *)sb->s_fs_info : NULL;
}

static inline tmpfs_inode_info_t *tmpfs_i(struct vfs_inode *inode) {
    return inode ? container_of(inode, tmpfs_inode_info_t, vfs_inode) : NULL;
}

static int tmpfs_parse_mount_options(tmpfs_fs_info_t *fsi,
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

static tmpfs_dirent_t *tmpfs_find_dirent_locked(tmpfs_inode_info_t *info,
                                                const char *name) {
    tmpfs_dirent_t *de, *tmp;

    if (!info || !name)
        return NULL;

    llist_for_each(de, tmp, &info->children, node) {
        if (de->name && streq(de->name, name))
            return de;
    }

    return NULL;
}

static void tmpfs_lock_dir_infos(tmpfs_inode_info_t *a, tmpfs_inode_info_t *b) {
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

static void tmpfs_unlock_dir_infos(tmpfs_inode_info_t *a,
                                   tmpfs_inode_info_t *b) {
    if (!a)
        return;

    if (!b || a == b) {
        spin_unlock(&a->lock);
        return;
    }

    spin_unlock(&a->lock);
    spin_unlock(&b->lock);
}

static unsigned char tmpfs_dtype(umode_t mode) {
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

int tmpfs_mem_resize_reserve(uint64_t old_size, uint64_t new_size) {
    uint64_t old_aligned = tmpfs_mem_align(old_size);
    uint64_t new_aligned = tmpfs_mem_align(new_size);

    spin_lock(&tmpfs_mem_limit_lock);

    if (new_aligned > old_aligned) {
        uint64_t delta = new_aligned - old_aligned;
        uint64_t limit = tmpfs_mem_limit();
        if (delta > limit || tmpfs_mem_used > limit - delta) {
            spin_unlock(&tmpfs_mem_limit_lock);
            return -ENOMEM;
        }
        tmpfs_mem_used += delta;
    } else {
        uint64_t delta = old_aligned - new_aligned;
        tmpfs_mem_used =
            (delta >= tmpfs_mem_used) ? 0 : (tmpfs_mem_used - delta);
    }

    spin_unlock(&tmpfs_mem_limit_lock);
    return 0;
}

void tmpfs_mem_release(uint64_t size) {
    uint64_t aligned;

    if (!size)
        return;

    aligned = tmpfs_mem_align(size);
    spin_lock(&tmpfs_mem_limit_lock);
    tmpfs_mem_used =
        (aligned >= tmpfs_mem_used) ? 0 : (tmpfs_mem_used - aligned);
    spin_unlock(&tmpfs_mem_limit_lock);
}

static ino64_t tmpfs_next_ino(struct vfs_super_block *sb) {
    tmpfs_fs_info_t *fs = tmpfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fs->lock);
    ino = ++fs->next_ino;
    spin_unlock(&fs->lock);
    return ino;
}

static tmpfs_dirent_t *tmpfs_find_dirent(struct vfs_inode *dir,
                                         const char *name) {
    tmpfs_inode_info_t *info = tmpfs_i(dir);
    tmpfs_dirent_t *de;

    if (!info || !S_ISDIR(dir->i_mode) || !name)
        return NULL;

    spin_lock(&info->lock);
    de = tmpfs_find_dirent_locked(info, name);
    spin_unlock(&info->lock);
    return de;
}

static int tmpfs_add_dirent(struct vfs_inode *dir, const char *name,
                            struct vfs_inode *inode) {
    tmpfs_inode_info_t *info = tmpfs_i(dir);
    tmpfs_dirent_t *de;

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
    if (tmpfs_find_dirent_locked(info, name)) {
        spin_unlock(&info->lock);
        tmpfs_free_dirent(de);
        return -EEXIST;
    }
    llist_append(&info->children, &de->node);
    spin_unlock(&info->lock);
    return 0;
}

static tmpfs_dirent_t *tmpfs_detach_dirent(struct vfs_inode *dir,
                                           const char *name) {
    tmpfs_inode_info_t *info = tmpfs_i(dir);
    tmpfs_dirent_t *de, *tmp;

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

static void tmpfs_free_dirent(tmpfs_dirent_t *de) {
    if (!de)
        return;
    if (de->inode)
        vfs_iput(de->inode);
    if (de->name)
        free(de->name);
    free(de);
}

static int tmpfs_resize_inode(struct vfs_inode *inode, uint64_t new_size) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    uint64_t old_size;
    size_t old_pages;
    size_t new_pages;
    int ret;

    if (!info || S_ISDIR(inode->i_mode))
        return -EINVAL;
    if (new_size > MAX_TMPFS_FILE_SIZE)
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

static struct vfs_inode *tmpfs_new_inode(struct vfs_super_block *sb,
                                         struct vfs_inode *dir, umode_t mode) {
    struct vfs_inode *inode = vfs_alloc_inode(sb);
    tmpfs_inode_info_t *info = tmpfs_i(inode);

    if (!inode)
        return NULL;

    spin_init(&info->lock);
    llist_init_head(&info->children);
    llist_init_head(&info->xattrs);
    inode->i_ino = tmpfs_next_ino(sb);
    vfs_inode_init_owner(inode, dir, mode);
    inode->inode = inode->i_ino;
    inode->i_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_size = 0;
    inode->i_blocks = 0;
    inode->i_blkbits = 12;
    inode->i_atime.sec = inode->i_btime.sec = inode->i_ctime.sec =
        inode->i_mtime.sec = (int64_t)(nano_time() / 1000000000ULL);
    inode->i_mapping.host = inode;
    return inode;
}

static struct vfs_dentry *tmpfs_lookup(struct vfs_inode *dir,
                                       struct vfs_dentry *dentry,
                                       unsigned int flags) {
    tmpfs_inode_info_t *info;
    struct vfs_inode *inode = NULL;

    (void)flags;
    if (!dir || !dentry)
        return ERR_PTR(-EINVAL);

    info = tmpfs_i(dir);
    if (!info)
        return ERR_PTR(-EINVAL);

    spin_lock(&info->lock);
    tmpfs_dirent_t *de = tmpfs_find_dirent_locked(info, dentry->d_name.name);
    if (de)
        inode = vfs_igrab(de->inode);
    spin_unlock(&info->lock);

    vfs_d_instantiate(dentry, inode);
    if (inode)
        vfs_iput(inode);
    return dentry;
}

static int tmpfs_create_common(struct vfs_inode *dir, struct vfs_dentry *dentry,
                               umode_t mode, const char *symlink_target,
                               dev64_t rdev) {
    struct vfs_inode *inode;
    tmpfs_inode_info_t *info;
    int ret;

    if (!dir || !dentry || !S_ISDIR(dir->i_mode))
        return -ENOTDIR;
    if (tmpfs_find_dirent(dir, dentry->d_name.name))
        return -EEXIST;

    inode = tmpfs_new_inode(dir->i_sb, dir, mode);
    if (!inode)
        return -ENOMEM;

    info = tmpfs_i(inode);
    inode->i_rdev = rdev;
    inode->i_op = dir->i_op;

    ret = tmpfs_add_dirent(dir, dentry->d_name.name, inode);
    if (ret < 0) {
        vfs_iput(inode);
        return ret;
    }

    if (S_ISDIR(mode)) {
        inode->i_fop = dir->i_fop;
        dir->i_nlink++;
    } else {
        inode->i_fop = &tmpfs_file_ops;
    }

    if (S_ISLNK(mode) && symlink_target) {
        ret = tmpfs_resize_inode(inode, strlen(symlink_target));
        if (ret < 0) {
            tmpfs_free_dirent(tmpfs_detach_dirent(dir, dentry->d_name.name));
            vfs_iput(inode);
            return ret;
        }
        info->link_target = strdup(symlink_target);
        if (!info->link_target) {
            tmpfs_free_dirent(tmpfs_detach_dirent(dir, dentry->d_name.name));
            vfs_iput(inode);
            return -ENOMEM;
        }

        loff_t pos = 0;
        spin_lock(&info->lock);
        ret = (int)paged_file_store_write_locked(&info->store, symlink_target,
                                                 strlen(symlink_target), &pos);
        spin_unlock(&info->lock);
        if (ret < 0) {
            free(info->link_target);
            info->link_target = NULL;
            tmpfs_free_dirent(tmpfs_detach_dirent(dir, dentry->d_name.name));
            vfs_iput(inode);
            return ret;
        }
    }

    vfs_d_instantiate(dentry, inode);
    vfs_iput(inode);
    return 0;
}

static int tmpfs_create(struct vfs_inode *dir, struct vfs_dentry *dentry,
                        umode_t mode, bool excl) {
    (void)excl;
    return tmpfs_create_common(dir, dentry, (mode & 07777) | S_IFREG, NULL, 0);
}

static int tmpfs_tmpfile(struct vfs_inode *dir, struct vfs_file *file,
                         umode_t mode) {
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {.name = "", .len = 0, .hash = 0};
    struct vfs_path old_path;
    struct vfs_path new_path;
    struct vfs_inode *new_inode;

    if (!dir || !file || !file->f_path.dentry || !S_ISDIR(dir->i_mode))
        return -EINVAL;

    inode = tmpfs_new_inode(dir->i_sb, dir, (mode & 07777) | S_IFREG);
    if (!inode)
        return -ENOMEM;

    inode->i_op = dir->i_op;
    inode->i_fop = &tmpfs_file_ops;
    inode->i_nlink = 0;

    dentry = vfs_d_alloc(dir->i_sb, file->f_path.dentry, &name);
    if (!dentry) {
        vfs_iput(inode);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    vfs_iput(inode);

    if (!vfs_path_set(&new_path, file->f_path.mnt, dentry)) {
        vfs_dput(dentry);
        return -ENOENT;
    }
    new_inode = vfs_igrab(dentry->d_inode);
    if (!new_inode) {
        vfs_path_put(&new_path);
        vfs_dput(dentry);
        return -ENOENT;
    }

    old_path = file->f_path;
    file->f_path = new_path;
    vfs_path_put(&old_path);
    if (file->f_inode)
        vfs_iput(file->f_inode);
    file->f_inode = new_inode;
    file->node = file->f_inode;
    file->f_op = file->f_inode->i_fop;
    vfs_dput(dentry);
    return 0;
}

static int tmpfs_mkdir(struct vfs_inode *dir, struct vfs_dentry *dentry,
                       umode_t mode) {
    return tmpfs_create_common(dir, dentry, (mode & 07777) | S_IFDIR, NULL, 0);
}

static int tmpfs_mknod(struct vfs_inode *dir, struct vfs_dentry *dentry,
                       umode_t mode, dev64_t dev) {
    return tmpfs_create_common(dir, dentry, mode, NULL, dev);
}

static int tmpfs_symlink(struct vfs_inode *dir, struct vfs_dentry *dentry,
                         const char *target) {
    return tmpfs_create_common(dir, dentry, S_IFLNK | 0777, target, 0);
}

static int tmpfs_link(struct vfs_dentry *old_dentry, struct vfs_inode *dir,
                      struct vfs_dentry *new_dentry) {
    int ret;

    if (!old_dentry || !old_dentry->d_inode || !dir || !new_dentry)
        return -EINVAL;
    if (S_ISDIR(old_dentry->d_inode->i_mode))
        return -EPERM;
    if (old_dentry->d_inode->i_sb != dir->i_sb)
        return -EXDEV;

    ret = tmpfs_add_dirent(dir, new_dentry->d_name.name, old_dentry->d_inode);
    if (ret < 0)
        return ret;

    old_dentry->d_inode->i_nlink++;
    vfs_d_instantiate(new_dentry, old_dentry->d_inode);
    return 0;
}

static int tmpfs_unlink(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    tmpfs_dirent_t *de;

    if (!dir || !dentry || !dentry->d_inode)
        return -ENOENT;
    if (S_ISDIR(dentry->d_inode->i_mode))
        return -EISDIR;

    de = tmpfs_detach_dirent(dir, dentry->d_name.name);
    if (!de)
        return -ENOENT;

    if (de->inode->i_nlink)
        de->inode->i_nlink--;
    tmpfs_free_dirent(de);
    return 0;
}

static int tmpfs_rmdir(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    tmpfs_dirent_t *de;
    tmpfs_inode_info_t *info;
    bool has_children;

    if (!dir || !dentry || !dentry->d_inode ||
        !S_ISDIR(dentry->d_inode->i_mode))
        return -ENOTDIR;

    info = tmpfs_i(dentry->d_inode);
    spin_lock(&info->lock);
    has_children = !llist_empty(&info->children);
    spin_unlock(&info->lock);
    if (has_children)
        return -ENOTEMPTY;

    de = tmpfs_detach_dirent(dir, dentry->d_name.name);
    if (!de)
        return -ENOENT;

    if (dir->i_nlink)
        dir->i_nlink--;
    if (de->inode->i_nlink >= 2)
        de->inode->i_nlink -= 2;
    tmpfs_free_dirent(de);
    return 0;
}

static int tmpfs_rename(struct vfs_rename_ctx *ctx) {
    tmpfs_inode_info_t *old_info;
    tmpfs_inode_info_t *new_info;
    tmpfs_dirent_t *old_de;
    tmpfs_dirent_t *victim = NULL;
    char *new_name;
    char *old_name = NULL;

    if (!ctx || !ctx->old_dir || !ctx->new_dir || !ctx->old_dentry ||
        !ctx->new_dentry) {
        return -EINVAL;
    }

    if (ctx->flags & (VFS_RENAME_EXCHANGE | VFS_RENAME_WHITEOUT))
        return -EOPNOTSUPP;

    old_info = tmpfs_i(ctx->old_dir);
    new_info = tmpfs_i(ctx->new_dir);
    if (!old_info || !new_info)
        return -EINVAL;

    new_name = strdup(ctx->new_dentry->d_name.name);
    if (!new_name)
        return -ENOMEM;

    tmpfs_lock_dir_infos(old_info, new_info);
    old_de = tmpfs_find_dirent_locked(old_info, ctx->old_dentry->d_name.name);
    if (!old_de) {
        tmpfs_unlock_dir_infos(old_info, new_info);
        free(new_name);
        return -ENOENT;
    }
    llist_delete(&old_de->node);

    victim = tmpfs_find_dirent_locked(new_info, ctx->new_dentry->d_name.name);
    if (victim)
        llist_delete(&victim->node);
    if (victim) {
        if (S_ISDIR(victim->inode->i_mode) != S_ISDIR(old_de->inode->i_mode)) {
            llist_init_head(&old_de->node);
            llist_append(&old_info->children, &old_de->node);
            llist_init_head(&victim->node);
            llist_append(&new_info->children, &victim->node);
            tmpfs_unlock_dir_infos(old_info, new_info);
            free(new_name);
            return -ENOTEMPTY;
        }
    } else if (ctx->flags & VFS_RENAME_NOREPLACE) {
        llist_init_head(&old_de->node);
        llist_append(&old_info->children, &old_de->node);
        tmpfs_unlock_dir_infos(old_info, new_info);
        free(new_name);
        return -EEXIST;
    }

    old_name = old_de->name;
    old_de->name = new_name;
    llist_init_head(&old_de->node);
    llist_append(&new_info->children, &old_de->node);
    tmpfs_unlock_dir_infos(old_info, new_info);
    free(old_name);
    tmpfs_free_dirent(victim);
    return 0;
}

static const char *tmpfs_get_link(struct vfs_dentry *dentry,
                                  struct vfs_inode *inode,
                                  struct vfs_nameidata *nd) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    (void)dentry;
    (void)nd;
    return info && info->link_target ? info->link_target : ERR_PTR(-EINVAL);
}

static int tmpfs_permission(struct vfs_inode *inode, int mask) {
    return vfs_inode_permission(inode, mask);
}

static tmpfs_xattr_t *tmpfs_find_xattr_locked(tmpfs_inode_info_t *info,
                                              const char *name) {
    tmpfs_xattr_t *xattr, *tmp;
    llist_for_each(xattr, tmp, &info->xattrs, node) {
        if (!strcmp(xattr->name, name))
            return xattr;
    }
    return NULL;
}

static ssize_t tmpfs_getxattr(struct vfs_inode *inode, const char *name,
                              void *value, size_t size) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    tmpfs_xattr_t *xattr;
    ssize_t ret;
    if (!info || !name)
        return -EINVAL;
    spin_lock(&info->lock);
    xattr = tmpfs_find_xattr_locked(info, name);
    if (!xattr)
        ret = -ENODATA;
    else if (size && size < xattr->size)
        ret = -ERANGE;
    else {
        if (value && xattr->size)
            memcpy(value, xattr->value, xattr->size);
        ret = (ssize_t)xattr->size;
    }
    spin_unlock(&info->lock);
    return ret;
}

static ssize_t tmpfs_listxattr(struct vfs_inode *inode, char *list,
                               size_t size) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    tmpfs_xattr_t *xattr, *tmp;
    size_t total = 0;
    ssize_t ret;
    if (!info)
        return -EINVAL;
    spin_lock(&info->lock);
    llist_for_each(xattr, tmp, &info->xattrs, node) {
        size_t len = strlen(xattr->name) + 1;
        if (size && total + len > size) {
            ret = -ERANGE;
            goto out;
        }
        if (list)
            memcpy(list + total, xattr->name, len);
        total += len;
    }
    ret = (ssize_t)total;
out:
    spin_unlock(&info->lock);
    return ret;
}

static int tmpfs_setxattr(struct vfs_inode *inode, const char *name,
                          const void *value, size_t size, int flags) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    tmpfs_xattr_t *xattr;
    void *new_value = NULL;
    if (!info || !name || (size && !value))
        return -EINVAL;
    if (vfs_current_fsuid() != 0 && vfs_current_fsuid() != inode->i_uid)
        return -EPERM;
    if (size) {
        new_value = malloc(size);
        if (!new_value)
            return -ENOMEM;
        memcpy(new_value, value, size);
    }
    spin_lock(&info->lock);
    xattr = tmpfs_find_xattr_locked(info, name);
    if ((flags & 1) && xattr) {
        spin_unlock(&info->lock);
        free(new_value);
        return -EEXIST;
    }
    if ((flags & 2) && !xattr) {
        spin_unlock(&info->lock);
        free(new_value);
        return -ENODATA;
    }
    if (!xattr) {
        xattr = calloc(1, sizeof(*xattr));
        if (!xattr) {
            spin_unlock(&info->lock);
            free(new_value);
            return -ENOMEM;
        }
        xattr->name = strdup(name);
        if (!xattr->name) {
            spin_unlock(&info->lock);
            free(xattr);
            free(new_value);
            return -ENOMEM;
        }
        llist_append(&info->xattrs, &xattr->node);
    }
    free(xattr->value);
    xattr->value = new_value;
    xattr->size = size;
    inode->i_ctime.sec = (int64_t)(nano_time() / 1000000000ULL);
    inode->i_version++;
    spin_unlock(&info->lock);
    return 0;
}

static int tmpfs_removexattr(struct vfs_inode *inode, const char *name) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    tmpfs_xattr_t *xattr;
    if (!info || !name)
        return -EINVAL;
    if (vfs_current_fsuid() != 0 && vfs_current_fsuid() != inode->i_uid)
        return -EPERM;
    spin_lock(&info->lock);
    xattr = tmpfs_find_xattr_locked(info, name);
    if (!xattr) {
        spin_unlock(&info->lock);
        return -ENODATA;
    }
    llist_delete(&xattr->node);
    spin_unlock(&info->lock);
    free(xattr->name);
    free(xattr->value);
    free(xattr);
    inode->i_ctime.sec = (int64_t)(nano_time() / 1000000000ULL);
    inode->i_version++;
    return 0;
}

static int tmpfs_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                         uint32_t request_mask, unsigned int flags) {
    (void)request_mask;
    (void)flags;
    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static int tmpfs_setattr(struct vfs_dentry *dentry,
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
        stat->size != dentry->d_inode->i_size)
        ret = tmpfs_resize_inode(dentry->d_inode, stat->size);
    dentry->d_inode->inode = dentry->d_inode->i_ino;
    return ret;
}

static loff_t tmpfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
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

static ssize_t tmpfs_read(struct vfs_file *file, void *buf, size_t count,
                          loff_t *ppos) {
    tmpfs_inode_info_t *info;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (S_ISFIFO(file->f_inode->i_mode))
        return pipefs_named_read(file, buf, count, ppos);
    info = tmpfs_i(file->f_inode);
    if (!info)
        return -EINVAL;

    spin_lock(&info->lock);
    ssize_t ret = paged_file_store_read_locked(&info->store, buf, count, ppos);
    spin_unlock(&info->lock);
    return ret;
}

static ssize_t tmpfs_write(struct vfs_file *file, const void *buf, size_t count,
                           loff_t *ppos) {
    tmpfs_inode_info_t *info;
    uint64_t end;
    ssize_t ret;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (S_ISFIFO(file->f_inode->i_mode))
        return pipefs_named_write(file, buf, count, ppos);

    info = tmpfs_i(file->f_inode);
    if (*ppos < 0)
        return -EINVAL;
    if ((uint64_t)*ppos > UINT64_MAX - count)
        return -EFBIG;

    end = (uint64_t)*ppos + count;
    ret = tmpfs_resize_inode(file->f_inode, MAX(info->store.size, end));
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

static int tmpfs_iterate_shared(struct vfs_file *file,
                                struct vfs_dir_context *ctx) {
    tmpfs_inode_info_t *info;
    tmpfs_dirent_t *de, *tmp;
    loff_t index = 0;

    if (!file || !file->f_inode || !ctx || !S_ISDIR(file->f_inode->i_mode))
        return -ENOTDIR;

    info = tmpfs_i(file->f_inode);
    spin_lock(&info->lock);
    llist_for_each(de, tmp, &info->children, node) {
        index++;
        if (index <= ctx->pos)
            continue;
        if (ctx->actor(ctx, de->name, (int)strlen(de->name), index,
                       de->inode->i_ino, tmpfs_dtype(de->inode->i_mode))) {
            break;
        }
        ctx->pos = index;
    }
    spin_unlock(&info->lock);

    file->f_pos = ctx->pos;
    return 0;
}

static int tmpfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    if (S_ISFIFO(inode->i_mode))
        return pipefs_named_open(inode, file);
    file->f_op = inode->i_fop;
    return 0;
}

static int tmpfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    if (inode && S_ISFIFO(inode->i_mode))
        return pipefs_named_release(inode, file);

    (void)inode;
    (void)file;
    return 0;
}

static int tmpfs_fsync(struct vfs_file *file, loff_t start, loff_t end,
                       int datasync) {
    (void)file;
    (void)start;
    (void)end;
    (void)datasync;
    return 0;
}

static void *tmpfs_mmap(struct vfs_file *file, void *addr, size_t offset,
                        size_t size, size_t prot, uint64_t flags) {
    tmpfs_inode_info_t *info;
    uint64_t pt_flags = PT_FLAG_U;
    uint64_t *pgdir;
    int ret;

    if (!file || !file->f_inode)
        return (void *)(int64_t)-EINVAL;
    if (!S_ISREG(file->f_inode->i_mode))
        return (void *)(int64_t)-ENODEV;
    if ((flags & MAP_TYPE) == MAP_PRIVATE)
        return general_map(file, (uint64_t)addr, size, prot, flags, offset);
    if (offset > SIZE_MAX - size)
        return (void *)(int64_t)-EINVAL;

    info = tmpfs_i(file->f_inode);
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

static __poll_t tmpfs_poll(struct vfs_file *file, struct vfs_poll_table *pt) {
    if (file && file->f_inode && S_ISFIFO(file->f_inode->i_mode))
        return pipefs_named_poll(file, pt);

    (void)file;
    (void)pt;
    return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
}

static struct vfs_inode *tmpfs_alloc_inode(struct vfs_super_block *sb) {
    tmpfs_inode_info_t *info = calloc(1, sizeof(*info));
    if (!info)
        return NULL;
    return &info->vfs_inode;
}

static void tmpfs_destroy_inode(struct vfs_inode *inode) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    free(info);
}

static void tmpfs_evict_inode(struct vfs_inode *inode) {
    tmpfs_inode_info_t *info = tmpfs_i(inode);
    tmpfs_dirent_t *de, *tmp;
    tmpfs_xattr_t *xattr, *xattr_tmp;

    if (!info)
        return;

    tmpfs_mem_release(info->store.size);
    paged_file_store_destroy(&info->store);
    free(info->link_target);
    info->link_target = NULL;

    spin_lock(&info->lock);
    llist_for_each(de, tmp, &info->children, node) {
        llist_delete(&de->node);
        tmpfs_free_dirent(de);
    }
    llist_for_each(xattr, xattr_tmp, &info->xattrs, node) {
        llist_delete(&xattr->node);
        free(xattr->name);
        free(xattr->value);
        free(xattr);
    }
    spin_unlock(&info->lock);

    if (S_ISFIFO(inode->i_mode))
        pipefs_named_evict_inode(inode);
}

static void tmpfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static int tmpfs_statfs(struct vfs_path *path, void *buf) {
    struct statfs *st = (struct statfs *)buf;
    struct vfs_super_block *sb;
    uint64_t mem_limit;
    uint64_t mem_used;
    uint64_t inode_count = 0;
    struct llist_header *node;

    if (!path || !path->dentry || !path->dentry->d_inode || !st)
        return -EINVAL;

    sb = path->dentry->d_inode->i_sb;
    if (!sb)
        return -EINVAL;

    memset(st, 0, sizeof(*st));

    mem_limit = tmpfs_mem_limit();

    spin_lock(&tmpfs_mem_limit_lock);
    mem_used = tmpfs_mem_used;
    spin_unlock(&tmpfs_mem_limit_lock);

    spin_lock(&sb->s_inode_lock);
    for (node = sb->s_inodes.next; node != &sb->s_inodes; node = node->next)
        inode_count++;
    spin_unlock(&sb->s_inode_lock);

    st->f_type = sb->s_magic;
    st->f_bsize = PAGE_SIZE;
    st->f_frsize = PAGE_SIZE;
    st->f_blocks = mem_limit / PAGE_SIZE;
    st->f_bfree =
        (mem_limit > mem_used) ? ((mem_limit - mem_used) / PAGE_SIZE) : 0;
    st->f_bavail = st->f_bfree;
    st->f_files = inode_count;
    st->f_namelen = VFS_NAME_MAX;
    st->f_flags = sb->s_flags;
    st->f_fsid.val[0] = (int)(sb->s_dev & 0xffffffffU);
    st->f_fsid.val[1] = (int)((sb->s_dev >> 32) & 0xffffffffU);
    return 0;
}

static int tmpfs_init_fs_context(struct vfs_fs_context *fc) {
    fc->sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!fc->sb)
        return -ENOMEM;
    return 0;
}

static int tmpfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb = fc->sb;
    tmpfs_fs_info_t *fsi;
    struct vfs_inode *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};
    int opt_ret;

    if (!sb)
        return -EINVAL;

    fsi = calloc(1, sizeof(*fsi));
    if (!fsi)
        return -ENOMEM;
    spin_init(&fsi->lock);
    fsi->next_ino = 1;
    fsi->dev = 0;
    fsi->root_uid = vfs_current_fsuid();
    fsi->root_gid = vfs_current_fsgid();
    fsi->root_mode = S_IFDIR | 01777;
    opt_ret = tmpfs_parse_mount_options(fsi, fc->data);
    if (opt_ret < 0) {
        free(fsi);
        return opt_ret;
    }

    sb->s_fs_info = fsi;
    sb->s_op = &tmpfs_super_ops;
    sb->s_type = &tmpfs_fs_type;
    sb->s_magic = 0x01021994;

    root_inode = tmpfs_new_inode(sb, NULL, fsi->root_mode);
    if (!root_inode)
        return -ENOMEM;

    root_inode->i_uid = fsi->root_uid;
    root_inode->i_gid = fsi->root_gid;
    root_inode->i_op = &tmpfs_inode_ops;
    root_inode->i_fop = &tmpfs_dir_ops;

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

static const struct vfs_super_operations tmpfs_super_ops = {
    .alloc_inode = tmpfs_alloc_inode,
    .destroy_inode = tmpfs_destroy_inode,
    .evict_inode = tmpfs_evict_inode,
    .put_super = tmpfs_put_super,
    .statfs = tmpfs_statfs,
};

static const struct vfs_file_operations tmpfs_dir_ops = {
    .llseek = tmpfs_llseek,
    .iterate_shared = tmpfs_iterate_shared,
    .open = tmpfs_open,
    .release = tmpfs_release,
    .fsync = tmpfs_fsync,
    .poll = tmpfs_poll,
};

static const struct vfs_file_operations tmpfs_file_ops = {
    .llseek = tmpfs_llseek,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .mmap = tmpfs_mmap,
    .open = tmpfs_open,
    .release = tmpfs_release,
    .fsync = tmpfs_fsync,
    .poll = tmpfs_poll,
};

static const struct vfs_inode_operations tmpfs_inode_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .link = tmpfs_link,
    .unlink = tmpfs_unlink,
    .symlink = tmpfs_symlink,
    .mkdir = tmpfs_mkdir,
    .rmdir = tmpfs_rmdir,
    .mknod = tmpfs_mknod,
    .rename = tmpfs_rename,
    .get_link = tmpfs_get_link,
    .permission = tmpfs_permission,
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
    .tmpfile = tmpfs_tmpfile,
    .getxattr = tmpfs_getxattr,
    .listxattr = tmpfs_listxattr,
    .setxattr = tmpfs_setxattr,
    .removexattr = tmpfs_removexattr,
};

static struct vfs_file_system_type tmpfs_fs_type = {
    .name = "tmpfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = tmpfs_init_fs_context,
    .get_tree = tmpfs_get_tree,
};

void tmpfs_init(void) { vfs_register_filesystem(&tmpfs_fs_type); }
