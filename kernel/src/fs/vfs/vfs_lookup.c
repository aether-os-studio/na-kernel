#include "fs/vfs/vfs_internal.h"
#include "task/task.h"
#include <fs/fs_syscall.h>

static inline bool vfs_path_is_absolute(const char *name) {
    return name && name[0] == '/';
}

static inline bool vfs_has_remaining_components(const char *rest) {
    if (!rest)
        return false;
    while (*rest) {
        if (*rest != '/')
            return true;
        rest++;
    }
    return false;
}

static inline bool vfs_lookup_is_scoped(unsigned int lookup_flags) {
    return (lookup_flags & (LOOKUP_BENEATH | LOOKUP_IN_ROOT)) != 0;
}

static bool vfs_mount_is_same_or_descendant(const struct vfs_mount *root,
                                            const struct vfs_mount *mnt);

static bool vfs_process_path_get(struct vfs_process_fs *fs, bool root,
                                 struct vfs_path *out) {
    struct vfs_path *src;

    if (!fs || !out)
        return false;

    memset(out, 0, sizeof(*out));
    spin_lock(&fs->lock);
    src = root ? &fs->root : &fs->pwd;
    if (src->mnt && src->dentry)
        (void)vfs_path_copy(out, src);
    spin_unlock(&fs->lock);

    return out->mnt && out->dentry;
}

static bool vfs_process_path_get_if_visible(struct vfs_process_fs *fs,
                                            bool root,
                                            struct vfs_mount *ns_root_mnt,
                                            struct vfs_path *out) {
    if (!vfs_process_path_get(fs, root, out))
        return false;

    if (ns_root_mnt && ns_root_mnt->mnt_root &&
        !vfs_mount_is_same_or_descendant(ns_root_mnt, out->mnt)) {
        vfs_path_put(out);
        return false;
    }

    return true;
}

static bool vfs_mount_is_same_or_descendant(const struct vfs_mount *root,
                                            const struct vfs_mount *mnt) {
    const struct vfs_mount *cursor;

    if (!root || !mnt)
        return false;

    for (cursor = mnt; cursor; cursor = cursor->mnt_parent) {
        if (cursor == root)
            return true;
        if (!cursor->mnt_parent || cursor->mnt_parent == cursor)
            break;
    }

    return false;
}

static void vfs_path_replace(struct vfs_path *dst, struct vfs_mount *mnt,
                             struct vfs_dentry *dentry) {
    struct vfs_path tmp = {0};

    if (!dst)
        return;
    if (!vfs_path_set(&tmp, mnt, dentry))
        return;
    vfs_path_move(dst, &tmp);
}

static int vfs_follow_mount_checked(struct vfs_path *path,
                                    unsigned int lookup_flags) {
    struct vfs_mount *orig_mnt;
    struct vfs_dentry *orig_dentry;

    if (!path)
        return -EINVAL;

    orig_mnt = path->mnt;
    orig_dentry = path->dentry;
    vfs_follow_mount(path);

    if ((lookup_flags & LOOKUP_NO_XDEV) &&
        (path->mnt != orig_mnt || path->dentry != orig_dentry)) {
        return -EXDEV;
    }

    return 0;
}

static int vfs_get_scoped_start(struct vfs_process_fs *fs,
                                struct vfs_mount *ns_root_mnt, int dfd,
                                struct vfs_path *scoped) {
    struct vfs_file *file;
    int ret;
    struct vfs_path fs_path = {0};

    if (!scoped)
        return -EINVAL;
    memset(scoped, 0, sizeof(*scoped));

    if (dfd == AT_FDCWD) {
        if (vfs_process_path_get_if_visible(fs, false, ns_root_mnt, &fs_path)) {
            *scoped = fs_path;
            return 0;
        }
        if (vfs_process_path_get(fs, true, &fs_path)) {
            *scoped = fs_path;
            return 0;
        }
        return -ENOENT;
    }

    file = task_get_file(current_task, dfd);
    if (!file)
        return -EBADF;

    ret = mountfd_get_path(file, scoped);
    if (ret == -EINVAL)
        ret = fsfd_mount_get_path(file, scoped);
    if (ret == -EINVAL) {
        ret = vfs_path_copy(scoped, &file->f_path) ? 0 : -ENOENT;
    }
    vfs_file_put(file);
    return ret;
}

int vfs_get_fs_start(int dfd, const char *name, unsigned int lookup_flags,
                     struct vfs_path *start, struct vfs_path *root) {
    struct vfs_process_fs *fs;
    struct vfs_file *file;
    struct vfs_mount *ns_root_mnt;
    int fd_path_ret;
    struct vfs_path fs_path = {0};

    if (!start || !root)
        return -EINVAL;
    memset(start, 0, sizeof(*start));
    memset(root, 0, sizeof(*root));

    fs = task_current_vfs_fs();
    ns_root_mnt = current_task ? task_mount_namespace_root(current_task) : NULL;
    if (!fs) {
        if (!vfs_root_path.mnt || !vfs_root_path.dentry)
            return -ENOENT;
        if (!vfs_path_copy(root, &vfs_root_path))
            return -ENOENT;
        if (vfs_follow_mount_checked(root, lookup_flags) < 0) {
            vfs_path_put(root);
            return -EXDEV;
        }
        if (vfs_lookup_is_scoped(lookup_flags)) {
            if ((lookup_flags & LOOKUP_BENEATH) && vfs_path_is_absolute(name)) {
                vfs_path_put(root);
                return -EXDEV;
            }
            if (!vfs_path_copy(start, root)) {
                vfs_path_put(root);
                return -ENOENT;
            }
            return 0;
        }
        if (vfs_path_is_absolute(name) || dfd == AT_FDCWD) {
            if (!vfs_path_copy(start, root)) {
                vfs_path_put(root);
                return -ENOENT;
            }
            return 0;
        }
        vfs_path_put(root);
        return -EBADF;
    }

    if (vfs_process_path_get_if_visible(fs, true, ns_root_mnt, &fs_path)) {
        *root = fs_path;
    } else if (ns_root_mnt && ns_root_mnt->mnt_root) {
        if (!vfs_path_set(root, ns_root_mnt, ns_root_mnt->mnt_root))
            return -ENOENT;
    } else if (vfs_root_path.mnt && vfs_root_path.dentry) {
        if (!vfs_path_copy(root, &vfs_root_path))
            return -ENOENT;
    } else {
        return -ENOENT;
    }

    if (vfs_follow_mount_checked(root, lookup_flags) < 0) {
        vfs_path_put(root);
        return -EXDEV;
    }

    if (vfs_lookup_is_scoped(lookup_flags)) {
        struct vfs_path scoped = {0};
        int scoped_ret;

        if ((lookup_flags & LOOKUP_BENEATH) && vfs_path_is_absolute(name)) {
            vfs_path_put(root);
            return -EXDEV;
        }

        scoped_ret = vfs_get_scoped_start(fs, ns_root_mnt, dfd, &scoped);
        if (scoped_ret < 0) {
            vfs_path_put(root);
            return scoped_ret;
        }
        if (vfs_follow_mount_checked(&scoped, lookup_flags) < 0) {
            vfs_path_put(&scoped);
            vfs_path_put(root);
            return -EXDEV;
        }

        vfs_path_move(root, &scoped);
        if (!vfs_path_copy(start, root)) {
            vfs_path_put(root);
            return -ENOENT;
        }
        return 0;
    }

    if (vfs_path_is_absolute(name)) {
        if (!vfs_path_copy(start, root)) {
            vfs_path_put(root);
            return -ENOENT;
        }
        return 0;
    }

    if (dfd == AT_FDCWD) {
        if (vfs_process_path_get_if_visible(fs, false, ns_root_mnt, &fs_path)) {
            *start = fs_path;
        } else {
            if (!vfs_path_copy(start, root)) {
                vfs_path_put(root);
                return -ENOENT;
            }
        }
        return 0;
    }

    file = task_get_file(current_task, dfd);
    if (!file)
        goto err;

    fd_path_ret = mountfd_get_path(file, start);
    if (fd_path_ret == -EINVAL)
        fd_path_ret = fsfd_mount_get_path(file, start);
    if (fd_path_ret == -EINVAL) {
        fd_path_ret = vfs_path_copy(start, &file->f_path) ? 0 : -ENOENT;
    }
    vfs_file_put(file);
    if (fd_path_ret < 0)
        goto err;
    return 0;

err:
    vfs_path_put(root);
    return -EBADF;
}

struct vfs_dentry *vfs_lookup_component(struct vfs_path *parent,
                                        const char *component,
                                        unsigned int flags) {
    struct vfs_qstr qstr;
    struct vfs_dentry *dentry;
    struct vfs_inode *dir;

    if (!parent || !parent->dentry || !component || !component[0])
        return ERR_PTR(-ENOENT);

    vfs_qstr_make(&qstr, component);
    dentry = vfs_d_lookup(parent->dentry, &qstr);
    if (dentry) {
        if (dentry->d_op && dentry->d_op->d_revalidate) {
            int ret = dentry->d_op->d_revalidate(dentry, flags);
            if (ret < 0) {
                vfs_dput(dentry);
                return ERR_PTR(ret);
            }
            if (ret == 0) {
                vfs_dentry_unhash(dentry);
                vfs_dput(dentry);
                dentry = NULL;
            }
        }
        if (dentry && !dentry->d_inode) {
            vfs_dentry_unhash(dentry);
            vfs_dput(dentry);
            dentry = NULL;
        }
        if (dentry)
            return dentry;
    }

    dir = parent->dentry->d_inode;
    if (!dir || !dir->i_op || !dir->i_op->lookup)
        return ERR_PTR(-ENOENT);

    dentry = vfs_d_alloc(parent->dentry->d_sb, parent->dentry, &qstr);
    if (!dentry)
        return ERR_PTR(-ENOMEM);

    {
        struct vfs_dentry *lookup = dir->i_op->lookup(dir, dentry, flags);
        if (IS_ERR(lookup)) {
            vfs_dput(dentry);
            return lookup;
        }
        if (!lookup) {
            vfs_dput(dentry);
            return ERR_PTR(-ENOENT);
        }
        if (lookup != dentry)
            vfs_dput(dentry);
        dentry = lookup;
    }
    if (!(dentry->d_flags & VFS_DENTRY_HASHED))
        vfs_d_add(parent->dentry, dentry);
    return dentry;
}

void vfs_follow_mount(struct vfs_path *path) {
    while (path && path->dentry) {
        struct vfs_mount *mounted = vfs_child_mount_at(path->mnt, path->dentry);
        if (!mounted)
            break;
        if (!mounted->mnt_root) {
            vfs_mntput(mounted);
            break;
        }
        vfs_path_replace(path, mounted, mounted->mnt_root);
        vfs_mntput(mounted);
    }
}

static int __vfs_filename_lookup(struct vfs_path *start,
                                 const struct vfs_path *root, const char *name,
                                 unsigned int lookup_flags, unsigned int depth,
                                 struct vfs_path *out);

static int vfs_lookup_apply_scope(struct vfs_path *path,
                                  const struct vfs_path *root,
                                  unsigned int lookup_flags) {
    if (!path || !root)
        return -EINVAL;

    if ((lookup_flags & (LOOKUP_BENEATH | LOOKUP_IN_ROOT)) &&
        !vfs_path_is_ancestor(root, path)) {
        if (lookup_flags & LOOKUP_IN_ROOT) {
            vfs_path_replace(path, root->mnt, root->dentry);
        } else {
            return -EXDEV;
        }
    }

    return 0;
}

static int vfs_walk_dotdot(struct vfs_path *path, const struct vfs_path *root,
                           unsigned int lookup_flags) {
    struct vfs_mount *next_mnt = NULL;
    struct vfs_dentry *next_dentry = NULL;

    if (!path || !path->mnt || !path->dentry || !root)
        return -EINVAL;

    if (vfs_path_equal(path, root))
        return 0;

    if (path->dentry == path->mnt->mnt_root && path->mnt->mnt_parent &&
        path->mnt->mnt_parent != path->mnt && path->mnt->mnt_mountpoint) {
        if (lookup_flags & LOOKUP_NO_XDEV)
            return -EXDEV;

        struct vfs_dentry *mountpoint = path->mnt->mnt_mountpoint;
        next_mnt = vfs_mntget(path->mnt->mnt_parent);

        next_dentry = mountpoint->d_parent ? vfs_dget(mountpoint->d_parent)
                                           : vfs_dget(mountpoint);
        if (!next_mnt || !next_dentry) {
            if (next_dentry)
                vfs_dput(next_dentry);
            if (next_mnt)
                vfs_mntput(next_mnt);
            return -ENOENT;
        }

        vfs_path_replace(path, next_mnt, next_dentry);
        vfs_dput(next_dentry);
        vfs_mntput(next_mnt);
    } else if (path->dentry->d_parent &&
               path->dentry != path->dentry->d_parent) {
        next_dentry = vfs_dget(path->dentry->d_parent);
        if (!next_dentry)
            return -ENOENT;

        vfs_path_replace(path, path->mnt, next_dentry);
        vfs_dput(next_dentry);
    }

    return vfs_lookup_apply_scope(path, root, lookup_flags);
}

static int vfs_parent_resolve_dotdot(struct vfs_path *parent,
                                     const struct vfs_path *root,
                                     struct vfs_qstr *last,
                                     unsigned int lookup_flags) {
    int ret;

    if (!vfs_qstr_is_dotdot(last))
        return 0;

    ret = vfs_walk_dotdot(parent, root, lookup_flags);
    if (ret < 0)
        return ret;

    // Parent lookup normally leaves the final component for its caller.
    // Resolve a final ".." here because dentry-only lookup has no mount
    // context and cannot cross from a mounted root into its parent mount.
    vfs_qstr_destroy(last);
    vfs_qstr_dup(last, ".");
    return last->name ? 0 : -ENOMEM;
}

static int vfs_follow_symlink(struct vfs_path *parent,
                              const struct vfs_path *root,
                              struct vfs_dentry *link_dentry,
                              const char *remaining, unsigned int lookup_flags,
                              unsigned int depth, struct vfs_path *out) {
    const char *target;
    char *pathbuf;
    size_t target_len, rest_len;
    int ret;
    struct vfs_path next;
    struct vfs_nameidata nd = {0};
    struct vfs_inode *inode;

    if (depth >= VFS_MAX_SYMLINKS)
        return -ELOOP;
    if (!link_dentry || !link_dentry->d_inode || !link_dentry->d_inode->i_op ||
        !link_dentry->d_inode->i_op->get_link) {
        return -ELOOP;
    }

    inode = link_dentry->d_inode;
    target = inode->i_op->get_link(link_dentry, inode, &nd);
    if (IS_ERR_OR_NULL(target))
        return target ? (int)PTR_ERR(target) : -ENOENT;

    if (nd.path.mnt && nd.path.dentry) {
        if (!remaining || !remaining[0]) {
            *out = nd.path;
            if (inode->i_op->put_link)
                inode->i_op->put_link(inode, target);
            return 0;
        }

        ret = __vfs_filename_lookup(&nd.path, &nd.path, remaining, lookup_flags,
                                    depth + 1, out);
        vfs_path_put(&nd.path);
        if (inode->i_op->put_link)
            inode->i_op->put_link(inode, target);
        return ret;
    }

    target_len = strlen(target);
    rest_len = remaining ? strlen(remaining) : 0;
    pathbuf = malloc(target_len + (rest_len ? 1 + rest_len : 0) + 1);
    if (!pathbuf) {
        if (inode->i_op->put_link)
            inode->i_op->put_link(inode, target);
        return -ENOMEM;
    }

    memcpy(pathbuf, target, target_len);
    if (rest_len) {
        pathbuf[target_len] = '/';
        memcpy(pathbuf + target_len + 1, remaining, rest_len);
        pathbuf[target_len + 1 + rest_len] = '\0';
    } else {
        pathbuf[target_len] = '\0';
    }

    memset(&next, 0, sizeof(next));
    if (target[0] == '/') {
        if (lookup_flags & LOOKUP_BENEATH) {
            if (inode->i_op->put_link)
                inode->i_op->put_link(inode, target);
            free(pathbuf);
            return -EXDEV;
        }
        if (!vfs_path_copy(&next, root)) {
            if (inode->i_op->put_link)
                inode->i_op->put_link(inode, target);
            free(pathbuf);
            return -ENOENT;
        }
    } else {
        if (!vfs_path_copy(&next, parent)) {
            if (inode->i_op->put_link)
                inode->i_op->put_link(inode, target);
            free(pathbuf);
            return -ENOENT;
        }
        ret = vfs_follow_mount_checked(&next, lookup_flags);
        if (ret < 0) {
            vfs_path_put(&next);
            if (inode->i_op->put_link)
                inode->i_op->put_link(inode, target);
            free(pathbuf);
            return ret;
        }
    }

    ret = __vfs_filename_lookup(&next, root, pathbuf, lookup_flags, depth + 1,
                                out);
    vfs_path_put(&next);
    if (inode->i_op->put_link)
        inode->i_op->put_link(inode, target);
    free(pathbuf);
    return ret;
}

static int __vfs_filename_lookup(struct vfs_path *start,
                                 const struct vfs_path *root, const char *name,
                                 unsigned int lookup_flags, unsigned int depth,
                                 struct vfs_path *out) {
    struct vfs_path path;
    char *walk = NULL;
    char *cursor = NULL;
    char *component = NULL;
    int ret = 0;

    if (!start || !root || !name || !out)
        return -EINVAL;

    if (lookup_flags & LOOKUP_EMPTY) {
        if (!name[0]) {
            if (!vfs_path_copy(out, start))
                return -ENOENT;
            return 0;
        }
    } else if (!name[0]) {
        return -ENOENT;
    }

    memset(&path, 0, sizeof(path));
    if (!vfs_path_copy(&path, start))
        return -ENOENT;
    ret = vfs_follow_mount_checked(&path, lookup_flags);
    if (ret < 0)
        goto out_no_path;

    walk = strdup(vfs_path_is_absolute(name) ? name + 1 : name);
    if (!walk) {
        ret = -ENOMEM;
        goto out;
    }

    if (!walk[0]) {
        *out = path;
        free(walk);
        return 0;
    }

    cursor = walk;
    while ((component = pathtok(&cursor))) {
        struct vfs_dentry *next;
        bool has_remaining;

        ret = vfs_follow_mount_checked(&path, lookup_flags);
        if (ret < 0)
            goto out;

        if (!path.dentry || !path.dentry->d_inode) {
            ret = -ENOENT;
            goto out;
        }
        if (!S_ISDIR(path.dentry->d_inode->i_mode)) {
            ret = -ENOTDIR;
            goto out;
        }

        if (streq(component, ".")) {
            ret = vfs_lookup_apply_scope(&path, root, lookup_flags);
            if (ret < 0)
                goto out;
            continue;
        }

        if (streq(component, "..")) {
            ret = vfs_walk_dotdot(&path, root, lookup_flags);
            if (ret < 0)
                goto out;
            continue;
        }

        next = vfs_lookup_component(&path, component, lookup_flags);
        if (IS_ERR(next)) {
            ret = (int)PTR_ERR(next);
            goto out;
        }
        if (!next->d_inode) {
            vfs_dput(next);
            ret = -ENOENT;
            goto out;
        }

        has_remaining = vfs_has_remaining_components(cursor);
        if ((lookup_flags & LOOKUP_NO_SYMLINKS) &&
            S_ISLNK(next->d_inode->i_mode)) {
            vfs_dput(next);
            ret = -ELOOP;
            goto out;
        }
        if (S_ISLNK(next->d_inode->i_mode) &&
            (!(lookup_flags & LOOKUP_NOFOLLOW) || has_remaining ||
             (lookup_flags & LOOKUP_FOLLOW))) {
            ret = vfs_follow_symlink(&path, root, next, cursor, lookup_flags,
                                     depth, out);
            vfs_dput(next);
            goto out_no_path_put;
        }

        vfs_path_replace(&path, path.mnt, next);
        vfs_dput(next);
        ret = vfs_lookup_apply_scope(&path, root, lookup_flags);
        if (ret < 0)
            goto out;
        if (!(lookup_flags & LOOKUP_NO_LAST_MOUNT) || has_remaining) {
            ret = vfs_follow_mount_checked(&path, lookup_flags);
            if (ret < 0)
                goto out;
        }
        ret = vfs_lookup_apply_scope(&path, root, lookup_flags);
        if (ret < 0)
            goto out;
    }

    if ((lookup_flags & LOOKUP_DIRECTORY) && path.dentry &&
        path.dentry->d_inode && !S_ISDIR(path.dentry->d_inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    *out = path;
    free(walk);
    return 0;

out:
    vfs_path_put(&path);
out_no_path:
out_no_path_put:
    free(walk);
    return ret;
}

int vfs_filename_lookup(int dfd, const char *name, unsigned int lookup_flags,
                        struct vfs_path *path) {
    struct vfs_path start, root;
    int ret;

    if (!name || !path)
        return -EINVAL;

    memset(&start, 0, sizeof(start));
    memset(&root, 0, sizeof(root));
    ret = vfs_get_fs_start(dfd, name, lookup_flags, &start, &root);
    if (ret < 0)
        return ret;

    ret = __vfs_filename_lookup(&start, &root, name, lookup_flags, 0, path);
    vfs_path_put(&start);
    vfs_path_put(&root);
    return ret;
}

int vfs_filename_lookup_from(const struct vfs_path *start,
                             const struct vfs_path *root, const char *name,
                             unsigned int lookup_flags, struct vfs_path *path) {
    const struct vfs_path *lookup_start;

    if (!start || !root || !name || !path)
        return -EINVAL;

    lookup_start = vfs_path_is_absolute(name) ? root : start;
    return __vfs_filename_lookup((struct vfs_path *)lookup_start, root, name,
                                 lookup_flags, 0, path);
}

int vfs_path_parent_lookup(int dfd, const char *name, unsigned int lookup_flags,
                           struct vfs_path *parent, struct vfs_qstr *last,
                           unsigned int *type) {
    struct vfs_path start = {0};
    struct vfs_path root = {0};
    int ret;

    if (!name || !parent || !last)
        return -EINVAL;

    ret = vfs_get_fs_start(dfd, name, lookup_flags, &start, &root);
    if (ret < 0)
        return ret;

    ret = vfs_path_parent_lookup_from(&start, &root, name, lookup_flags, parent,
                                      last, type);
    vfs_path_put(&start);
    vfs_path_put(&root);
    return ret;
}

int vfs_path_parent_lookup_from(const struct vfs_path *start,
                                const struct vfs_path *root, const char *name,
                                unsigned int lookup_flags,
                                struct vfs_path *parent, struct vfs_qstr *last,
                                unsigned int *type) {
    char *copy, *basename, *slash;
    int ret;

    if (!start || !root || !name || !parent || !last)
        return -EINVAL;

    copy = strdup(name);
    if (!copy)
        return -ENOMEM;

    while (strlen(copy) > 1 && copy[strlen(copy) - 1] == '/')
        copy[strlen(copy) - 1] = '\0';

    slash = strrchr(copy, '/');
    if (!slash) {
        if (!vfs_path_copy(parent, start)) {
            free(copy);
            return -ENOENT;
        }
        ret = vfs_follow_mount_checked(parent, lookup_flags);
        if (ret < 0) {
            vfs_path_put(parent);
            free(copy);
            return ret;
        }
        vfs_qstr_dup(last, copy);
        ret = vfs_parent_resolve_dotdot(parent, root, last, lookup_flags);
        if (ret < 0) {
            vfs_path_put(parent);
            vfs_qstr_destroy(last);
            free(copy);
            return ret;
        }
        if (type)
            *type = 0;
        free(copy);
        return 0;
    }

    if (slash == copy) {
        basename = slash + 1;
        while (*basename == '/')
            basename++;
        vfs_qstr_dup(last, basename[0] ? basename : ".");
        slash[1] = '\0';
    } else {
        basename = slash + 1;
        while (*basename == '/')
            basename++;
        vfs_qstr_dup(last, basename[0] ? basename : ".");
        *slash = '\0';
    }
    ret = vfs_filename_lookup_from(start, root, copy[0] ? copy : "/",
                                   lookup_flags, parent);
    if (ret == 0) {
        vfs_follow_mount(parent);
        ret = vfs_parent_resolve_dotdot(parent, root, last, lookup_flags);
    }
    if (type)
        *type = 0;
    free(copy);
    return ret;
}
