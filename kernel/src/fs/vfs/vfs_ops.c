#include "fs/vfs/vfs_internal.h"
#include "fs/vfs/notify.h"

static wait_mutex_t vfs_rename_lock;
static volatile uint64_t vfs_rename_seq = 1;

void vfs_ops_init(void) { wait_mutex_init(&vfs_rename_lock); }

ssize_t vfs_getxattr(struct vfs_inode *inode, const char *name, void *value,
                     size_t size) {
    if (!inode || !name)
        return -EINVAL;
    if (!inode->i_op || !inode->i_op->getxattr)
        return -EOPNOTSUPP;
    return inode->i_op->getxattr(inode, name, value, size);
}

ssize_t vfs_listxattr(struct vfs_inode *inode, char *list, size_t size) {
    if (!inode)
        return -EINVAL;
    if (!inode->i_op || !inode->i_op->listxattr)
        return -EOPNOTSUPP;
    return inode->i_op->listxattr(inode, list, size);
}

int vfs_setxattr(struct vfs_inode *inode, const char *name, const void *value,
                 size_t size, int flags) {
    if (!inode || !name)
        return -EINVAL;
    if (!inode->i_op || !inode->i_op->setxattr)
        return -EOPNOTSUPP;
    return inode->i_op->setxattr(inode, name, value, size, flags);
}

int vfs_removexattr(struct vfs_inode *inode, const char *name) {
    if (!inode || !name)
        return -EINVAL;
    if (!inode->i_op || !inode->i_op->removexattr)
        return -EOPNOTSUPP;
    return inode->i_op->removexattr(inode, name);
}

static int vfs_may_write_dir(struct vfs_inode *dir) {
    if (!dir)
        return -ENOENT;
    if (!S_ISDIR(dir->i_mode))
        return -ENOTDIR;
    return vfs_inode_permission(dir, VFS_MAY_WRITE | VFS_MAY_EXEC);
}

static int vfs_may_delete(struct vfs_inode *dir, struct vfs_inode *victim) {
    uid32_t fsuid;
    int ret = vfs_may_write_dir(dir);
    if (ret < 0)
        return ret;
    if (!victim)
        return -ENOENT;
    if (!(dir->i_mode & S_ISVTX))
        return 0;

    fsuid = vfs_current_fsuid();
    if (fsuid == 0 || fsuid == dir->i_uid || fsuid == victim->i_uid)
        return 0;
    return -EPERM;
}

int vfs_mkdirat(int dfd, const char *pathname, umode_t mode, bool kernel) {
    struct vfs_path parent = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *dentry;
    struct vfs_inode *dir;
    int ret;

    ret = vfs_path_parent_lookup(dfd, pathname, LOOKUP_PARENT, &parent, &last,
                                 NULL);
    if (ret < 0)
        return ret;

    if (vfs_qstr_is_dot(&last) || vfs_qstr_is_dotdot(&last)) {
        ret = -EEXIST;
        goto out;
    }

    dir = parent.dentry ? parent.dentry->d_inode : NULL;
    if (!dir || !dir->i_op || !dir->i_op->mkdir) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    dentry = vfs_d_lookup(parent.dentry, &last);
    if (dentry) {
        if (dentry->d_inode) {
            vfs_dput(dentry);
            ret = -EEXIST;
            goto out;
        }
    }

    if (!kernel) {
        ret = vfs_may_write_dir(dir);
        if (ret < 0) {
            if (dentry)
                vfs_dput(dentry);
            goto out;
        }
    }

    if (!dentry) {
        dentry = vfs_d_alloc(parent.dentry->d_sb, parent.dentry, &last);
        if (!dentry) {
            ret = -ENOMEM;
            goto out;
        }
    }

    ret = dir->i_op->mkdir(dir, dentry, mode);
    if (ret == 0) {
        if (!(dentry->d_flags & VFS_DENTRY_HASHED))
            vfs_d_add(parent.dentry, dentry);
        notifyfs_queue_inode_event(dir, dentry->d_inode, last.name, IN_CREATE,
                                   0);
    }
    vfs_dput(dentry);

out:
    vfs_path_put(&parent);
    vfs_qstr_destroy(&last);
    return ret;
}

int vfs_mknodat(int dfd, const char *pathname, umode_t mode, dev64_t dev,
                bool kernel) {
    struct vfs_path parent = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *dentry;
    struct vfs_inode *dir;
    int ret;

    ret = vfs_path_parent_lookup(dfd, pathname, LOOKUP_PARENT, &parent, &last,
                                 NULL);
    if (ret < 0)
        return ret;

    if (vfs_qstr_is_dot(&last) || vfs_qstr_is_dotdot(&last)) {
        ret = -EEXIST;
        goto out;
    }

    dir = parent.dentry ? parent.dentry->d_inode : NULL;
    if (!dir || !dir->i_op || !dir->i_op->mknod) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    if (!kernel) {
        if ((S_ISCHR(mode) || S_ISBLK(mode)) && vfs_current_fsuid() != 0) {
            ret = -EPERM;
            goto out;
        }
        ret = vfs_may_write_dir(dir);
        if (ret < 0)
            goto out;
    }

    dentry = vfs_d_lookup(parent.dentry, &last);
    if (dentry) {
        if (dentry->d_inode) {
            vfs_dput(dentry);
            ret = -EEXIST;
            goto out;
        }
    } else {
        dentry = vfs_d_alloc(parent.dentry->d_sb, parent.dentry, &last);
        if (!dentry) {
            ret = -ENOMEM;
            goto out;
        }
    }

    ret = dir->i_op->mknod(dir, dentry, mode, dev);
    if (ret == 0) {
        if (!(dentry->d_flags & VFS_DENTRY_HASHED))
            vfs_d_add(parent.dentry, dentry);
        notifyfs_queue_inode_event(dir, dentry->d_inode, last.name, IN_CREATE,
                                   0);
    }
    vfs_dput(dentry);

out:
    vfs_path_put(&parent);
    vfs_qstr_destroy(&last);
    return ret;
}

int vfs_unlinkat(int dfd, const char *pathname, int flags, bool kernel) {
    struct vfs_path parent = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *victim = NULL;
    struct vfs_mount *mounted = NULL;
    struct vfs_inode *dir;
    struct vfs_inode *victim_inode = NULL;
    int ret;

    ret = vfs_path_parent_lookup(dfd, pathname, LOOKUP_PARENT, &parent, &last,
                                 NULL);
    if (ret < 0)
        return ret;

    if (vfs_qstr_is_dot(&last) || vfs_qstr_is_dotdot(&last)) {
        ret = -EINVAL;
        goto out;
    }

    dir = parent.dentry ? parent.dentry->d_inode : NULL;
    victim = vfs_lookup_component(&parent, last.name, 0);
    if (IS_ERR(victim)) {
        ret = PTR_ERR(victim);
        victim = NULL;
        goto out;
    }
    if (!dir || !victim || !victim->d_inode) {
        ret = -ENOENT;
        goto out;
    }

    if ((mounted = vfs_child_mount_at(parent.mnt, victim))) {
        vfs_mntput(mounted);
        ret = -EBUSY;
        goto out;
    }

    ret = vfs_may_delete(dir, victim->d_inode);
    if (ret < 0)
        goto out;

    victim_inode = vfs_igrab(victim->d_inode);
    if (!victim_inode) {
        ret = -ENOENT;
        goto out;
    }

    if ((flags & AT_REMOVEDIR) || S_ISDIR(victim->d_inode->i_mode)) {
        if (!dir->i_op || !dir->i_op->rmdir) {
            ret = -EOPNOTSUPP;
            goto out;
        }
        ret = dir->i_op->rmdir(dir, victim);
    } else {
        if (!dir->i_op || !dir->i_op->unlink) {
            ret = -EOPNOTSUPP;
            goto out;
        }
        ret = dir->i_op->unlink(dir, victim);
    }

    if (ret == 0) {
        notifyfs_queue_inode_event(dir, victim_inode, victim->d_name.name,
                                   IN_DELETE, 0);
        notifyfs_queue_inode_event(victim_inode, victim_inode, NULL,
                                   IN_DELETE_SELF, 0);
        vfs_dentry_unhash(victim);
        victim->d_flags |= VFS_DENTRY_NEGATIVE;
    }

out:
    if (victim_inode)
        vfs_iput(victim_inode);
    if (victim)
        vfs_dput(victim);
    vfs_path_put(&parent);
    vfs_qstr_destroy(&last);
    return ret;
}

int vfs_linkat(int olddfd, const char *oldname, int newdfd, const char *newname,
               int flags, bool kernel) {
    struct vfs_path old_path = {0}, new_parent = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *new_dentry = NULL;
    struct vfs_inode *new_dir;
    int ret;

    ret = vfs_filename_lookup(olddfd, oldname,
                              (flags & AT_SYMLINK_FOLLOW) ? LOOKUP_FOLLOW : 0,
                              &old_path);
    if (ret < 0)
        return ret;

    ret = vfs_path_parent_lookup(newdfd, newname, LOOKUP_PARENT, &new_parent,
                                 &last, NULL);
    if (ret < 0)
        goto out_old;

    if (vfs_qstr_is_dot(&last) || vfs_qstr_is_dotdot(&last)) {
        ret = -EEXIST;
        goto out;
    }

    new_dir = new_parent.dentry ? new_parent.dentry->d_inode : NULL;
    if (!new_dir || !new_dir->i_op || !new_dir->i_op->link) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    ret = vfs_may_write_dir(new_dir);
    if (ret < 0)
        goto out;

    new_dentry = vfs_d_lookup(new_parent.dentry, &last);
    if (new_dentry) {
        if (new_dentry->d_inode) {
            ret = -EEXIST;
            goto out;
        }
    } else {
        new_dentry =
            vfs_d_alloc(new_parent.dentry->d_sb, new_parent.dentry, &last);
        if (!new_dentry) {
            ret = -ENOMEM;
            goto out;
        }
    }

    ret = new_dir->i_op->link(old_path.dentry, new_dir, new_dentry);
    if (ret == 0) {
        if (!(new_dentry->d_flags & VFS_DENTRY_HASHED))
            vfs_d_add(new_parent.dentry, new_dentry);
        notifyfs_queue_inode_event(new_dir, old_path.dentry->d_inode, last.name,
                                   IN_CREATE, 0);
        notifyfs_queue_inode_event(old_path.dentry->d_inode,
                                   old_path.dentry->d_inode, NULL, IN_ATTRIB,
                                   0);
    }

out:
    if (new_dentry)
        vfs_dput(new_dentry);
    vfs_path_put(&new_parent);
    vfs_qstr_destroy(&last);
out_old:
    vfs_path_put(&old_path);
    return ret;
}

int vfs_symlinkat(const char *target, int newdfd, const char *newname,
                  bool kernel) {
    struct vfs_path parent = {0};
    struct vfs_qstr last = {0};
    struct vfs_dentry *dentry = NULL;
    struct vfs_inode *dir;
    int ret;

    ret = vfs_path_parent_lookup(newdfd, newname, LOOKUP_PARENT, &parent, &last,
                                 NULL);
    if (ret < 0)
        return ret;

    if (vfs_qstr_is_dot(&last) || vfs_qstr_is_dotdot(&last)) {
        ret = -EEXIST;
        goto out;
    }

    dir = parent.dentry ? parent.dentry->d_inode : NULL;
    if (!dir || !dir->i_op || !dir->i_op->symlink) {
        ret = -EOPNOTSUPP;
        goto out;
    }

    if (!kernel) {
        ret = vfs_may_write_dir(dir);
        if (ret < 0)
            goto out;
    }

    dentry = vfs_d_lookup(parent.dentry, &last);
    if (dentry) {
        if (dentry->d_inode) {
            ret = -EEXIST;
            goto out;
        }
    } else {
        dentry = vfs_d_alloc(parent.dentry->d_sb, parent.dentry, &last);
        if (!dentry) {
            ret = -ENOMEM;
            goto out;
        }
    }

    ret = dir->i_op->symlink(dir, dentry, target);
    if (ret == 0) {
        if (!(dentry->d_flags & VFS_DENTRY_HASHED))
            vfs_d_add(parent.dentry, dentry);
        notifyfs_queue_inode_event(dir, dentry->d_inode, last.name, IN_CREATE,
                                   0);
    }

out:
    if (dentry)
        vfs_dput(dentry);
    vfs_path_put(&parent);
    vfs_qstr_destroy(&last);
    return ret;
}

int vfs_renameat2(int olddfd, const char *oldname, int newdfd,
                  const char *newname, unsigned int flags, bool kernel) {
    struct vfs_path old_parent = {0}, new_parent = {0};
    struct vfs_qstr old_last = {0}, new_last = {0};
    struct vfs_dentry *old_dentry = NULL, *new_dentry = NULL;
    struct vfs_rename_ctx ctx;
    struct vfs_inode *moved_inode = NULL;
    int ret;

    wait_mutex_lock(&vfs_rename_lock);

    ret = vfs_path_parent_lookup(olddfd, oldname, LOOKUP_PARENT, &old_parent,
                                 &old_last, NULL);
    if (ret < 0)
        goto out_unlock;
    ret = vfs_path_parent_lookup(newdfd, newname, LOOKUP_PARENT, &new_parent,
                                 &new_last, NULL);
    if (ret < 0)
        goto out;

    old_dentry = vfs_d_lookup(old_parent.dentry, &old_last);
    if (!old_dentry || !old_dentry->d_inode) {
        ret = -ENOENT;
        goto out;
    }

    new_dentry = vfs_d_lookup(new_parent.dentry, &new_last);
    if (!new_dentry) {
        new_dentry =
            vfs_d_alloc(new_parent.dentry->d_sb, new_parent.dentry, &new_last);
        if (!new_dentry) {
            ret = -ENOMEM;
            goto out;
        }
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.old_dir = old_parent.dentry->d_inode;
    ctx.old_dentry = old_dentry;
    ctx.new_dir = new_parent.dentry->d_inode;
    ctx.new_dentry = new_dentry;
    ctx.flags = flags;
    moved_inode = vfs_igrab(old_dentry->d_inode);

    if (!ctx.old_dir || !ctx.old_dir->i_op || !ctx.old_dir->i_op->rename) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    ret = vfs_may_delete(ctx.old_dir, moved_inode);
    if (ret < 0)
        goto out;
    if (new_dentry->d_inode)
        ret = vfs_may_delete(ctx.new_dir, new_dentry->d_inode);
    else
        ret = vfs_may_write_dir(ctx.new_dir);
    if (ret < 0)
        goto out;

    ret = ctx.old_dir->i_op->rename(&ctx);
    if (ret == 0) {
        uint32_t cookie = notifyfs_next_cookie();

        if (old_dentry != new_dentry) {
            if (!(new_dentry->d_flags & VFS_DENTRY_HASHED))
                vfs_d_add(new_parent.dentry, new_dentry);
            vfs_d_instantiate(new_dentry, moved_inode);
            vfs_dentry_unhash(old_dentry);
            vfs_d_instantiate(old_dentry, NULL);
        }

        __atomic_add_fetch(&vfs_rename_seq, 1, __ATOMIC_ACQ_REL);
        notifyfs_queue_inode_event(ctx.old_dir, moved_inode, old_last.name,
                                   IN_MOVED_FROM, cookie);
        notifyfs_queue_inode_event(ctx.new_dir, moved_inode, new_last.name,
                                   IN_MOVED_TO, cookie);
        notifyfs_queue_inode_event(moved_inode, moved_inode, NULL, IN_MOVE_SELF,
                                   cookie);
    }

out:
    if (moved_inode)
        vfs_iput(moved_inode);
    if (old_dentry)
        vfs_dput(old_dentry);
    if (new_dentry)
        vfs_dput(new_dentry);
    vfs_path_put(&old_parent);
    vfs_path_put(&new_parent);
    vfs_qstr_destroy(&old_last);
    vfs_qstr_destroy(&new_last);
out_unlock:
    wait_mutex_unlock(&vfs_rename_lock);
    return ret;
}

int vfs_statx(int dfd, const char *pathname, int flags, uint32_t mask,
              struct vfs_kstat *stat) {
    struct vfs_path path = {0};
    int ret;

    ret = vfs_filename_lookup(
        dfd, pathname,
        (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return ret;

    if (path.dentry->d_inode && path.dentry->d_inode->i_op &&
        path.dentry->d_inode->i_op->getattr) {
        ret = path.dentry->d_inode->i_op->getattr(&path, stat, mask, flags);
    } else {
        vfs_fill_generic_kstat(&path, stat);
        ret = 0;
    }

    vfs_path_put(&path);
    return ret;
}

int vfs_kern_mount(const char *fs_name, unsigned long mnt_flags,
                   const char *source, void *data, struct vfs_mount **out) {
    struct vfs_file_system_type *fstype;
    struct vfs_fs_context fc;
    struct vfs_mount *mnt;
    int ret;

    if (!fs_name || !out)
        return -EINVAL;

    fstype = vfs_get_fs_type(fs_name);
    if (!fstype)
        return -ENODEV;

    memset(&fc, 0, sizeof(fc));
    fc.fs_type = fstype;
    fc.mnt_flags = mnt_flags;
    fc.source = source;
    fc.data = data;

    if (fstype->init_fs_context) {
        ret = fstype->init_fs_context(&fc);
        if (ret < 0)
            return ret;
    }

    ret = fstype->get_tree(&fc);
    if (ret < 0)
        return ret;
    if (!fc.sb || !fc.sb->s_root)
        return -EINVAL;

    mnt = vfs_mount_alloc(fc.sb, mnt_flags);
    if (!mnt)
        return -ENOMEM;

    *out = mnt;
    return 0;
}

int vfs_do_mount(int dfd, const char *pathname, const char *fs_name,
                 unsigned long mnt_flags, const char *source, void *data) {
    struct vfs_mount *mnt = NULL;
    struct vfs_path target = {0};
    int ret;

    ret = vfs_kern_mount(fs_name, mnt_flags, source, data, &mnt);
    if (ret < 0)
        return ret;

    ret = vfs_filename_lookup(dfd, pathname,
                              LOOKUP_FOLLOW | LOOKUP_NO_LAST_MOUNT, &target);
    if (ret < 0)
        goto out;

    ret = vfs_mount_attach(target.mnt, target.dentry, mnt);
    if (ret == 0 && !vfs_root_path.mnt) {
        if (!vfs_path_set(&vfs_root_path, mnt, mnt->mnt_root))
            ret = -ENOENT;
    }

out:
    vfs_path_put(&target);
    if (ret < 0 && mnt)
        vfs_mntput(mnt);
    return ret;
}

int vfs_do_bind_mount(int from_dfd, const char *from_pathname, int to_dfd,
                      const char *to_pathname, bool recursive) {
    struct vfs_path from = {0};
    struct vfs_path to = {0};
    struct vfs_mount *mnt = NULL;
    int ret;

    if (!from_pathname || !to_pathname)
        return -EINVAL;

    ret = vfs_filename_lookup(from_dfd, from_pathname, LOOKUP_FOLLOW, &from);
    if (ret < 0)
        return ret;

    ret = vfs_filename_lookup(to_dfd, to_pathname,
                              LOOKUP_FOLLOW | LOOKUP_NO_LAST_MOUNT, &to);
    if (ret < 0)
        goto out;

    ret = vfs_create_bind_mount_err(&from, recursive, &mnt);
    if (ret < 0)
        goto out;

    ret = vfs_reconfigure_mount(mnt, &to, true);
    if (ret < 0)
        vfs_put_mount_tree(mnt);

    mnt = NULL;

out:
    if (mnt)
        vfs_mntput(mnt);
    vfs_path_put(&to);
    vfs_path_put(&from);
    return ret;
}

int vfs_do_remount(int dfd, const char *pathname, unsigned long mnt_flags) {
    struct vfs_path target = {0};
    struct vfs_mount *mnt = NULL;
    unsigned long preserved_flags;
    int ret;

    if (!pathname)
        return -EINVAL;

    ret = vfs_filename_lookup(dfd, pathname, LOOKUP_FOLLOW, &target);
    if (ret < 0)
        return ret;

    mnt = vfs_path_mount(&target);
    if (!mnt && target.mnt)
        mnt = vfs_mntget(target.mnt);
    if (!mnt) {
        ret = -EINVAL;
        goto out;
    }

    spin_lock(&mnt->mnt_lock);
    preserved_flags =
        mnt->mnt_flags & ~(VFS_MNT_READONLY | VFS_MNT_NOSUID | VFS_MNT_NODEV |
                           VFS_MNT_NOEXEC | VFS_MNT_NOSYMFOLLOW);
    mnt->mnt_flags = preserved_flags | mnt_flags;
    spin_unlock(&mnt->mnt_lock);
    ret = 0;

out:
    if (mnt)
        vfs_mntput(mnt);
    vfs_path_put(&target);
    return ret;
}

int vfs_do_move_mount(int from_dfd, const char *from_pathname, int to_dfd,
                      const char *to_pathname) {
    struct vfs_path from = {0};
    struct vfs_path to = {0};
    struct vfs_mount *mnt = NULL;
    int ret;

    if (!from_pathname || !to_pathname)
        return -EINVAL;

    ret = vfs_filename_lookup(from_dfd, from_pathname, LOOKUP_FOLLOW, &from);
    if (ret < 0)
        return ret;

    ret = vfs_filename_lookup(to_dfd, to_pathname,
                              LOOKUP_FOLLOW | LOOKUP_NO_LAST_MOUNT, &to);
    if (ret < 0)
        goto out;

    mnt = vfs_path_mount(&from);
    if (!mnt) {
        ret = -EINVAL;
        goto out;
    }

    ret = vfs_reconfigure_mount(mnt, &to, false);

out:
    if (mnt)
        vfs_mntput(mnt);
    vfs_path_put(&to);
    vfs_path_put(&from);
    return ret;
}

int vfs_do_umount(int dfd, const char *pathname, int flags) {
    struct vfs_path target = {0};
    struct vfs_mount *mnt;
    int ret;

    (void)flags;
    ret = vfs_filename_lookup(dfd, pathname, LOOKUP_FOLLOW, &target);
    if (ret < 0)
        return ret;

    mnt = vfs_path_mount(&target);
    if (!mnt) {
        vfs_path_put(&target);
        return -EINVAL;
    }

    vfs_mount_detach(mnt);
    vfs_path_put(&target);
    vfs_mntput(mnt);
    return 0;
}
