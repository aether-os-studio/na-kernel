#include "fs/vfs/vfs_internal.h"
#include "fs/fs_syscall.h"
#include "init/callbacks.h"
#include "task/task.h"

static wait_mutex_t vfs_mount_lock;
static volatile unsigned int vfs_next_mnt_id = 1;
static volatile unsigned int vfs_next_peer_group_id = 1;
static volatile uint64_t vfs_mount_seq = 1;

uint64_t vfs_mount_seq_read(void) {
    return __atomic_load_n(&vfs_mount_seq, __ATOMIC_ACQUIRE);
}

static inline void vfs_mount_mark_changed(void) {
    __atomic_add_fetch(&vfs_mount_seq, 1, __ATOMIC_ACQ_REL);
    vfs_init_mnt_ns.seq++;
    on_mount_change_call();
}

struct vfs_propagation_target {
    struct vfs_mount *receiver;
    struct vfs_dentry *mountpoint;
    struct vfs_mount *clone;
    size_t depth;
    bool attached;
};

struct vfs_propagation_group {
    unsigned int old_group_id;
    unsigned int new_group_id;
    struct vfs_mount *representative;
};

struct vfs_mount_clone_pair {
    const struct vfs_mount *src;
    struct vfs_mount *dst;
};

struct vfs_mount_group_clone_pair {
    unsigned int src_group_id;
    unsigned int dst_group_id;
};

static int vfs_mount_attach_locked(struct vfs_mount *parent,
                                   struct vfs_dentry *mountpoint,
                                   struct vfs_mount *child, bool propagate);
static void vfs_mount_detach_locked(struct vfs_mount *mnt, bool propagate);
static void vfs_put_mount_tree_locked(struct vfs_mount *root);
static struct vfs_mount *vfs_clone_mount_tree_internal(struct vfs_mount *root,
                                                       bool mark_source_id);
static int vfs_mount_attach_tree(struct vfs_mount *parent,
                                 struct vfs_dentry *mountpoint,
                                 struct vfs_mount *child);

static bool vfs_mountpoint_matches(const struct vfs_dentry *mounted_on,
                                   const struct vfs_dentry *query) {
    if (mounted_on == query)
        return true;
    if (!mounted_on || !query || !mounted_on->d_inode || !query->d_inode)
        return false;
    if (mounted_on->d_inode != query->d_inode)
        return false;

    if (mounted_on->d_parent == query->d_parent)
        return true;
    if (!mounted_on->d_parent || !query->d_parent ||
        !mounted_on->d_parent->d_inode || !query->d_parent->d_inode) {
        return false;
    }

    if (mounted_on->d_parent->d_inode != query->d_parent->d_inode)
        return false;
    if (mounted_on->d_name.len != query->d_name.len ||
        mounted_on->d_name.hash != query->d_name.hash ||
        !mounted_on->d_name.name || !query->d_name.name) {
        return false;
    }

    return strncmp(mounted_on->d_name.name, query->d_name.name,
                   mounted_on->d_name.len) == 0;
}

static bool vfs_mount_is_listed_child(const struct vfs_mount *parent,
                                      const struct vfs_mount *child) {
    const struct vfs_mount *pos;
    const struct vfs_mount *tmp;

    if (!parent || !child)
        return false;

    llist_for_each(pos, tmp, &parent->mnt_mounts, mnt_child) {
        if (pos == child)
            return true;
    }

    return false;
}

static struct vfs_mount *
vfs_find_child_mount_on_aliases(struct vfs_mount *parent,
                                struct vfs_dentry *mountpoint) {
    struct vfs_dentry *alias;
    struct vfs_dentry *tmp_alias;
    struct vfs_mount *best = NULL;

    if (!parent || !mountpoint || !mountpoint->d_inode)
        return NULL;

    llist_for_each(alias, tmp_alias, &mountpoint->d_inode->i_dentry_aliases,
                   d_alias) {
        struct vfs_mount *stack;

        for (stack = alias->d_mounted; stack; stack = stack->mnt_stack_prev) {
            if (stack->mnt_parent != parent)
                continue;
            if (!vfs_mountpoint_matches(stack->mnt_mountpoint, mountpoint))
                continue;
            if (!best || stack->mnt_id > best->mnt_id)
                best = stack;
        }
    }

    return best;
}

int vfs_mount_collect_missing_direct_children(struct vfs_mount *src_parent,
                                              struct vfs_mount ***out,
                                              size_t *out_count) {
    struct vfs_mount *missing[64];
    struct vfs_dentry *root_alias;
    struct vfs_dentry *tmp_root_alias;
    struct vfs_dentry *child_dentry;
    struct vfs_dentry *tmp_dentry;
    size_t count = 0;
    struct vfs_mount **result;
    size_t i;

    if (!out || !out_count)
        return -EINVAL;

    *out = NULL;
    *out_count = 0;

    if (!src_parent || !src_parent->mnt_root || !src_parent->mnt_root->d_inode)
        return 0;

    llist_for_each(root_alias, tmp_root_alias,
                   &src_parent->mnt_root->d_inode->i_dentry_aliases, d_alias) {
        llist_for_each(child_dentry, tmp_dentry, &root_alias->d_subdirs,
                       d_child) {
            struct vfs_mount *stack =
                vfs_find_child_mount_on_aliases(src_parent, child_dentry);

            if (!stack)
                continue;
            if (vfs_mount_is_listed_child(src_parent, stack))
                continue;
            if (count >= sizeof(missing) / sizeof(missing[0]))
                return -EOVERFLOW;
            for (i = 0; i < count; ++i) {
                if (missing[i] == stack)
                    break;
            }
            if (i != count)
                continue;
            missing[count++] = stack;
        }
    }

    if (count == 0)
        return 0;

    result = calloc(count, sizeof(*result));
    if (!result)
        return -ENOMEM;

    for (i = 0; i < count; ++i)
        result[i] = missing[i];

    *out = result;
    *out_count = count;
    return 0;
}

void vfs_mount_subsys_init(void) { wait_mutex_init(&vfs_mount_lock); }

struct vfs_mount *vfs_active_namespace_root_mount(void) {
    struct vfs_mount *mnt = task_mount_namespace_root(current_task);

    if (mnt)
        return mnt;
    if (vfs_init_mnt_ns.root)
        return vfs_init_mnt_ns.root;
    return vfs_root_path.mnt;
}

struct vfs_mount *vfs_child_mount_at(struct vfs_mount *parent,
                                     struct vfs_dentry *mountpoint) {
    struct vfs_mount *mnt;
    struct vfs_mount *tmp;
    struct vfs_mount *best = NULL;

    if (!parent || !mountpoint)
        return NULL;

    wait_mutex_lock(&vfs_mount_lock);
    for (mnt = mountpoint->d_mounted; mnt; mnt = mnt->mnt_stack_prev) {
        if (mnt->mnt_parent != parent)
            continue;
        if (!vfs_mountpoint_matches(mnt->mnt_mountpoint, mountpoint))
            continue;
        if (!best || mnt->mnt_id > best->mnt_id)
            best = mnt;
    }

    mnt = vfs_find_child_mount_on_aliases(parent, mountpoint);
    if (mnt && (!best || mnt->mnt_id > best->mnt_id))
        best = mnt;

    llist_for_each(mnt, tmp, &parent->mnt_mounts, mnt_child) {
        if (!vfs_mountpoint_matches(mnt->mnt_mountpoint, mountpoint))
            continue;
        if (!best || mnt->mnt_id > best->mnt_id)
            best = mnt;
    }

    if (best)
        best = vfs_mntget(best);
    wait_mutex_unlock(&vfs_mount_lock);
    return best;
}

struct vfs_mount *
vfs_find_mount_child_by_source(struct vfs_mount *parent,
                               const struct vfs_mount *src_child) {
    struct vfs_mount *pos;
    struct vfs_mount *tmp;
    struct vfs_dentry *translated_mountpoint;

    if (!parent || !src_child || !src_child->mnt_parent ||
        !src_child->mnt_parent->mnt_root || !src_child->mnt_mountpoint) {
        return NULL;
    }

    llist_for_each(pos, tmp, &parent->mnt_mounts, mnt_child) {
        if (pos->mnt_clone_source_id != src_child->mnt_id)
            continue;
        return vfs_mntget(pos);
    }

    translated_mountpoint = vfs_translate_dentry_between_mounts(
        src_child->mnt_parent->mnt_root, src_child->mnt_mountpoint, parent);
    if (!translated_mountpoint)
        return NULL;

    llist_for_each(pos, tmp, &parent->mnt_mounts, mnt_child) {
        if (pos->mnt_mountpoint == translated_mountpoint &&
            pos->mnt_sb == src_child->mnt_sb) {
            vfs_dput(translated_mountpoint);
            return vfs_mntget(pos);
        }
    }

    vfs_dput(translated_mountpoint);
    return NULL;
}

static void vfs_rebind_task_root_paths(const struct vfs_path *old_root,
                                       struct vfs_mount *new_mnt) {
    struct vfs_process_fs *fs;
    struct vfs_path new_root = {0};
    struct vfs_path new_pwd = {0};
    bool replace_root;
    bool replace_pwd;

    if (!old_root || !old_root->mnt || !old_root->dentry || !new_mnt ||
        !new_mnt->mnt_root) {
        return;
    }

    fs = task_current_vfs_fs();
    if (!fs)
        return;

    spin_lock(&fs->lock);
    replace_root =
        fs->root.mnt == old_root->mnt && fs->root.dentry == old_root->dentry;
    replace_pwd =
        fs->pwd.mnt == old_root->mnt && fs->pwd.dentry == old_root->dentry;
    if (!replace_root && !replace_pwd)
        goto out;

    if (replace_root && !vfs_path_set(&new_root, new_mnt, new_mnt->mnt_root))
        replace_root = false;
    if (replace_pwd && !vfs_path_set(&new_pwd, new_mnt, new_mnt->mnt_root))
        replace_pwd = false;
    if (replace_root)
        vfs_path_move(&fs->root, &new_root);
    if (replace_pwd)
        vfs_path_move(&fs->pwd, &new_pwd);
    fs->seq++;
out:
    spin_unlock(&fs->lock);
}

static void vfs_rebind_namespace_root(const struct vfs_path *old_root,
                                      struct vfs_mount *new_mnt) {
    struct vfs_mount *active_root;

    if (!old_root || !old_root->mnt || !old_root->dentry || !new_mnt)
        return;

    active_root = vfs_active_namespace_root_mount();
    if (active_root != old_root->mnt ||
        old_root->dentry != old_root->mnt->mnt_root) {
        return;
    }

    (void)task_mount_namespace_set_root(current_task, new_mnt);

    if (vfs_root_path.mnt == old_root->mnt &&
        vfs_root_path.dentry == old_root->dentry) {
        struct vfs_path new_root = {0};

        if (vfs_path_set(&new_root, new_mnt, new_mnt->mnt_root))
            vfs_path_move(&vfs_root_path, &new_root);
    }

    if (vfs_init_mnt_ns.root == old_root->mnt) {
        struct vfs_mount *new_root = vfs_mntget(new_mnt);

        if (new_root) {
            vfs_mntput(vfs_init_mnt_ns.root);
            vfs_init_mnt_ns.root = new_root;
            vfs_init_mnt_ns.seq++;
        }
    }
}

static unsigned int vfs_alloc_peer_group_id(void) {
    return __atomic_add_fetch(&vfs_next_peer_group_id, 1, __ATOMIC_ACQ_REL);
}

static struct vfs_mount *
vfs_mount_find_peer_in_group(const struct vfs_mount *mnt,
                             unsigned int group_id) {
    struct vfs_mount *peer;
    struct vfs_mount *tmp;

    if (!mnt || !mnt->mnt_sb || group_id == 0)
        return NULL;

    llist_for_each(peer, tmp, &mnt->mnt_sb->s_mounts, mnt_sb_link) {
        if (peer == mnt)
            continue;
        if (peer->mnt_group_id != group_id)
            continue;
        return peer;
    }

    return NULL;
}

static bool vfs_mount_is_slave_of_group(const struct vfs_mount *mnt,
                                        unsigned int group_id) {
    const struct vfs_mount *cursor;

    if (!mnt || group_id == 0)
        return false;

    for (cursor = mnt->mnt_master; cursor; cursor = cursor->mnt_master) {
        if (cursor->mnt_group_id == group_id)
            return true;
    }

    return false;
}

static void vfs_mount_array_put(struct vfs_mount **mounts, size_t count) {
    size_t i;

    if (!mounts)
        return;

    for (i = 0; i < count; ++i) {
        if (mounts[i])
            vfs_mntput(mounts[i]);
    }
    free(mounts);
}

static int vfs_mount_collect_propagation_targets(struct vfs_mount *origin,
                                                 unsigned int group_id,
                                                 struct vfs_mount ***out,
                                                 size_t *out_count) {
    struct vfs_mount *receiver;
    struct vfs_mount *tmp;
    struct vfs_mount **targets = NULL;
    size_t count = 0;
    size_t capacity = 0;

    if (!origin || !origin->mnt_sb || !out || !out_count)
        return -EINVAL;

    llist_for_each(receiver, tmp, &origin->mnt_sb->s_mounts, mnt_sb_link) {
        struct vfs_mount **new_targets;
        size_t new_capacity;

        if (receiver == origin)
            continue;
        if (receiver->mnt_group_id != group_id &&
            !vfs_mount_is_slave_of_group(receiver, group_id)) {
            continue;
        }

        if (count == capacity) {
            new_capacity = capacity ? capacity * 2 : 8;
            new_targets = realloc(targets, new_capacity * sizeof(*new_targets));
            if (!new_targets) {
                vfs_mount_array_put(targets, count);
                return -ENOMEM;
            }
            targets = new_targets;
            capacity = new_capacity;
        }

        targets[count++] = vfs_mntget(receiver);
    }

    *out = targets;
    *out_count = count;
    return 0;
}

static struct vfs_mount *
vfs_mount_find_propagated_child(struct vfs_mount *parent,
                                struct vfs_dentry *mountpoint,
                                const struct vfs_mount *src_child) {
    struct vfs_mount *cursor;

    if (!parent || !mountpoint || !src_child)
        return NULL;

    for (cursor = mountpoint->d_mounted; cursor;
         cursor = cursor->mnt_stack_prev) {
        if (cursor->mnt_parent != parent)
            continue;
        if (cursor->mnt_propagation_source_id != src_child->mnt_id)
            continue;
        return cursor;
    }

    return NULL;
}

static void vfs_mount_copy_propagation(struct vfs_mount *dst,
                                       const struct vfs_mount *src) {
    if (!dst || !src)
        return;

    dst->mnt_propagation = src->mnt_propagation;
    dst->mnt_group_id = src->mnt_group_id;
    dst->mnt_master = src->mnt_master;
    dst->mnt_clone_source_id = 0;
    dst->mnt_propagation_source_id = 0;
}

static size_t vfs_count_mount_tree_nodes(const struct vfs_mount *root) {
    const struct vfs_mount *child;
    const struct vfs_mount *tmp;
    size_t count = 1;

    if (!root)
        return 0;

    llist_for_each(child, tmp, &root->mnt_mounts, mnt_child) {
        count += vfs_count_mount_tree_nodes(child);
    }

    return count;
}

static size_t
vfs_count_complete_mount_tree_nodes(const struct vfs_mount *root) {
    const struct vfs_mount *child;
    const struct vfs_mount *tmp;
    struct vfs_mount **extra_children = NULL;
    size_t extra_count = 0;
    size_t count = 1;
    size_t i;

    if (!root)
        return 0;

    llist_for_each(child, tmp, &root->mnt_mounts, mnt_child) {
        count += vfs_count_complete_mount_tree_nodes(child);
    }

    if (vfs_mount_collect_missing_direct_children(
            (struct vfs_mount *)root, &extra_children, &extra_count) < 0) {
        return count;
    }

    for (i = 0; i < extra_count; ++i) {
        if (!extra_children[i])
            continue;
        count += vfs_count_complete_mount_tree_nodes(extra_children[i]);
    }

    free(extra_children);
    return count;
}

static int vfs_mount_clone_pair_add(struct vfs_mount_clone_pair *pairs,
                                    size_t capacity, size_t *count,
                                    const struct vfs_mount *src,
                                    struct vfs_mount *dst) {
    if (!pairs || !count || !src || !dst || *count >= capacity)
        return -EINVAL;

    pairs[*count].src = src;
    pairs[*count].dst = dst;
    (*count)++;
    return 0;
}

static struct vfs_mount *
vfs_find_cloned_mount(struct vfs_mount_clone_pair *pairs, size_t count,
                      const struct vfs_mount *src) {
    size_t i;

    if (!pairs || !src)
        return NULL;

    for (i = 0; i < count; ++i) {
        if (pairs[i].src == src)
            return pairs[i].dst;
    }

    return NULL;
}

static unsigned int
vfs_remap_cloned_group_id(struct vfs_mount_group_clone_pair *groups,
                          size_t *group_count, size_t group_capacity,
                          unsigned int src_group_id) {
    size_t i;

    if (!groups || !group_count || src_group_id == 0)
        return 0;

    for (i = 0; i < *group_count; ++i) {
        if (groups[i].src_group_id == src_group_id)
            return groups[i].dst_group_id;
    }

    if (*group_count >= group_capacity)
        return 0;

    groups[*group_count].src_group_id = src_group_id;
    groups[*group_count].dst_group_id = vfs_alloc_peer_group_id();
    (*group_count)++;
    return groups[*group_count - 1].dst_group_id;
}

static void vfs_remap_cloned_tree_metadata(struct vfs_mount_clone_pair *pairs,
                                           size_t count,
                                           bool mark_propagation_source) {
    struct vfs_mount_group_clone_pair *groups;
    size_t group_count = 0;
    size_t i;

    if (!pairs)
        return;

    groups = calloc(count ? count : 1, sizeof(*groups));
    if (!groups)
        return;

    for (i = 0; i < count; ++i) {
        unsigned int remapped_group_id = 0;
        struct vfs_mount *new_master =
            vfs_find_cloned_mount(pairs, count, pairs[i].src->mnt_master);

        vfs_mount_copy_propagation(pairs[i].dst, pairs[i].src);
        pairs[i].dst->mnt_clone_source_id = pairs[i].src->mnt_id;
        if (pairs[i].src->mnt_group_id != 0) {
            remapped_group_id = vfs_remap_cloned_group_id(
                groups, &group_count, count ? count : 1,
                pairs[i].src->mnt_group_id);
            if (remapped_group_id != 0)
                pairs[i].dst->mnt_group_id = remapped_group_id;
        }
        if (new_master) {
            pairs[i].dst->mnt_master = new_master;
        } else {
            pairs[i].dst->mnt_master = NULL;
        }
        if (mark_propagation_source)
            pairs[i].dst->mnt_propagation_source_id = pairs[i].src->mnt_id;
    }

    free(groups);
}

static size_t vfs_mount_depth_from_group(const struct vfs_mount *mnt,
                                         unsigned int group_id) {
    const struct vfs_mount *cursor;
    size_t depth = 0;

    if (!mnt || group_id == 0)
        return SIZE_MAX;
    if (mnt->mnt_group_id == group_id)
        return 0;

    for (cursor = mnt->mnt_master; cursor; cursor = cursor->mnt_master) {
        depth++;
        if (cursor->mnt_group_id == group_id)
            return depth;
    }

    return SIZE_MAX;
}

static struct vfs_propagation_group *
vfs_find_propagation_group(struct vfs_propagation_group *groups, size_t count,
                           unsigned int old_group_id) {
    size_t i;

    if (!groups || old_group_id == 0)
        return NULL;

    for (i = 0; i < count; ++i) {
        if (groups[i].old_group_id == old_group_id)
            return &groups[i];
    }

    return NULL;
}

static struct vfs_mount *vfs_resolve_propagated_master(
    const struct vfs_mount *receiver, struct vfs_mount *origin_child,
    unsigned int origin_group, struct vfs_propagation_group *groups,
    size_t count) {
    struct vfs_propagation_group *group;

    if (!receiver || !receiver->mnt_master)
        return NULL;
    if (receiver->mnt_master->mnt_group_id == origin_group)
        return origin_child;

    group = vfs_find_propagation_group(groups, count,
                                       receiver->mnt_master->mnt_group_id);
    return group ? group->representative : NULL;
}

static int vfs_configure_propagated_mount(struct vfs_mount *clone,
                                          const struct vfs_mount *receiver,
                                          struct vfs_mount *origin_child,
                                          unsigned int origin_group,
                                          struct vfs_propagation_group *groups,
                                          size_t *group_count,
                                          size_t group_capacity) {
    struct vfs_mount *master;
    struct vfs_propagation_group *group;

    if (!clone || !receiver || !origin_child || !groups || !group_count)
        return -EINVAL;

    if (receiver->mnt_group_id == origin_group) {
        vfs_mount_copy_propagation(clone, origin_child);
        return 0;
    }

    master = vfs_resolve_propagated_master(receiver, origin_child, origin_group,
                                           groups, *group_count);
    if (receiver->mnt_master && !master)
        return -ENOENT;

    if (receiver->mnt_group_id != 0) {
        group = vfs_find_propagation_group(groups, *group_count,
                                           receiver->mnt_group_id);
        if (!group) {
            if (*group_count >= group_capacity)
                return -ENOMEM;
            group = &groups[(*group_count)++];
            memset(group, 0, sizeof(*group));
            group->old_group_id = receiver->mnt_group_id;
            group->new_group_id = vfs_alloc_peer_group_id();
        }

        clone->mnt_propagation = VFS_MNT_PROP_SHARED;
        clone->mnt_group_id = group->new_group_id;
        clone->mnt_master = master;
        if (!group->representative)
            group->representative = clone;
        return 0;
    }

    clone->mnt_propagation = VFS_MNT_PROP_SLAVE;
    clone->mnt_group_id = 0;
    clone->mnt_master = master;
    return 0;
}

static int vfs_prepare_mount_for_destination(struct vfs_mount *parent,
                                             struct vfs_mount *child) {
    if (!child)
        return -EINVAL;
    if (!parent || parent->mnt_propagation != VFS_MNT_PROP_SHARED ||
        parent->mnt_group_id == 0) {
        return 0;
    }
    if (child->mnt_propagation == VFS_MNT_PROP_UNBINDABLE)
        return -EINVAL;

    if (child->mnt_propagation == VFS_MNT_PROP_PRIVATE ||
        child->mnt_propagation == VFS_MNT_PROP_SLAVE) {
        child->mnt_propagation = VFS_MNT_PROP_SHARED;
        if (child->mnt_group_id == 0)
            child->mnt_group_id = vfs_alloc_peer_group_id();
    }

    return 0;
}

static void
vfs_mount_apply_propagation(struct vfs_mount *mnt,
                            enum vfs_mount_propagation propagation) {
    unsigned int previous_group;

    if (!mnt)
        return;

    previous_group = mnt->mnt_group_id;
    mnt->mnt_propagation = (uint8_t)propagation;
    switch (propagation) {
    case VFS_MNT_PROP_SHARED:
        if (!mnt->mnt_group_id)
            mnt->mnt_group_id = vfs_alloc_peer_group_id();
        break;
    case VFS_MNT_PROP_SLAVE:
        mnt->mnt_group_id = 0;
        if (previous_group) {
            struct vfs_mount *peer =
                vfs_mount_find_peer_in_group(mnt, previous_group);

            if (peer) {
                mnt->mnt_master = peer;
                break;
            }
        }
        if (mnt->mnt_parent && mnt->mnt_parent != mnt) {
            if (mnt->mnt_parent->mnt_propagation == VFS_MNT_PROP_SHARED)
                mnt->mnt_master = mnt->mnt_parent;
            else
                mnt->mnt_master = mnt->mnt_parent->mnt_master;
        } else {
            mnt->mnt_master = NULL;
        }
        break;
    case VFS_MNT_PROP_UNBINDABLE:
    case VFS_MNT_PROP_PRIVATE:
    default:
        mnt->mnt_group_id = 0;
        mnt->mnt_master = NULL;
        break;
    }
}

static void vfs_mount_apply_propagation_tree(struct vfs_mount *mnt,
                                             enum vfs_mount_propagation prop) {
    struct vfs_mount *child, *tmp;

    if (!mnt)
        return;

    vfs_mount_apply_propagation(mnt, prop);
    llist_for_each(child, tmp, &mnt->mnt_mounts, mnt_child) {
        vfs_mount_apply_propagation_tree(child, prop);
    }
}

struct vfs_mount *vfs_mount_alloc(struct vfs_super_block *sb,
                                  unsigned long mnt_flags) {
    struct vfs_mount *mnt;

    if (!sb || !sb->s_root)
        return NULL;

    mnt = calloc(1, sizeof(*mnt));
    if (!mnt)
        return NULL;

    mnt->mnt_parent = mnt;
    mnt->mnt_root = vfs_dget(sb->s_root);
    if (!mnt->mnt_root) {
        free(mnt);
        return NULL;
    }
    mnt->mnt_sb = sb;
    mnt->mnt_flags = mnt_flags;
    mnt->mnt_propagation = VFS_MNT_PROP_PRIVATE;
    mnt->mnt_id = __atomic_add_fetch(&vfs_next_mnt_id, 1, __ATOMIC_ACQ_REL);
    mnt->mnt_propagation_source_id = 0;
    vfs_ref_init(&mnt->mnt_ref, 1);
    spin_init(&mnt->mnt_lock);
    llist_init_head(&mnt->mnt_sb_link);
    llist_init_head(&mnt->mnt_child);
    llist_init_head(&mnt->mnt_mounts);

    if (!vfs_get_super(sb)) {
        vfs_dput(mnt->mnt_root);
        free(mnt);
        return NULL;
    }
    spin_lock(&sb->s_mount_lock);
    llist_append(&sb->s_mounts, &mnt->mnt_sb_link);
    spin_unlock(&sb->s_mount_lock);

    return mnt;
}

struct vfs_mount *vfs_mntget(struct vfs_mount *mnt) {
    if (!mnt)
        return NULL;
    if (!vfs_ref_try_get(&mnt->mnt_ref))
        return NULL;
    return mnt;
}

static int vfs_propagate_attach_locked(struct vfs_mount *parent,
                                       struct vfs_dentry *mountpoint,
                                       struct vfs_mount *child) {
    struct vfs_mount **receivers = NULL;
    struct vfs_propagation_target *targets = NULL;
    struct vfs_propagation_group *groups = NULL;
    size_t receiver_count = 0;
    size_t target_count = 0;
    size_t group_count = 0;
    size_t i;
    int ret = 0;

    if (!parent || !mountpoint || !child)
        return -EINVAL;
    if (parent->mnt_propagation != VFS_MNT_PROP_SHARED ||
        parent->mnt_group_id == 0) {
        return 0;
    }

    ret = vfs_mount_collect_propagation_targets(parent, parent->mnt_group_id,
                                                &receivers, &receiver_count);
    if (ret < 0)
        return ret;
    if (receiver_count == 0)
        goto out;

    targets = calloc(receiver_count, sizeof(*targets));
    groups = calloc(receiver_count, sizeof(*groups));
    if (!targets || !groups) {
        ret = -ENOMEM;
        goto out;
    }

    for (i = 0; i < receiver_count; ++i) {
        struct vfs_mount *receiver = receivers[i];
        struct vfs_dentry *translated_mountpoint;
        struct vfs_mount *clone;

        translated_mountpoint = vfs_translate_dentry_between_mounts(
            parent->mnt_root, mountpoint, receiver);
        if (!translated_mountpoint) {
            ret = -ENOENT;
            goto rollback;
        }

        if (vfs_mount_find_propagated_child(receiver, translated_mountpoint,
                                            child)) {
            vfs_dput(translated_mountpoint);
            continue;
        }

        clone = vfs_clone_mount_tree_internal(child, true);
        if (!clone) {
            vfs_dput(translated_mountpoint);
            ret = -ENOMEM;
            goto rollback;
        }

        targets[target_count++] = (struct vfs_propagation_target){
            .receiver = receiver,
            .mountpoint = translated_mountpoint,
            .clone = clone,
            .depth = vfs_mount_depth_from_group(receiver, parent->mnt_group_id),
        };
        if (targets[target_count - 1].depth == SIZE_MAX) {
            ret = -EINVAL;
            goto rollback;
        }
    }

    for (i = 0; i < target_count; ++i) {
        size_t j;

        for (j = i + 1; j < target_count; ++j) {
            if (targets[j].depth < targets[i].depth) {
                struct vfs_propagation_target tmp = targets[i];

                targets[i] = targets[j];
                targets[j] = tmp;
            }
        }
    }

    for (i = 0; i < target_count; ++i) {
        ret = vfs_configure_propagated_mount(
            targets[i].clone, targets[i].receiver, child, parent->mnt_group_id,
            groups, &group_count, receiver_count);
        if (ret < 0)
            goto rollback;

        ret =
            vfs_mount_attach_locked(targets[i].receiver, targets[i].mountpoint,
                                    targets[i].clone, false);
        if (ret < 0)
            goto rollback;
        targets[i].attached = true;
    }

    goto out;

rollback:
    if (targets) {
        for (i = 0; i < target_count; ++i) {
            if (targets[i].attached && targets[i].clone) {
                vfs_put_mount_tree_locked(targets[i].clone);
                targets[i].clone = NULL;
                targets[i].attached = false;
            }
        }
    }

out:
    if (targets) {
        for (i = 0; i < target_count; ++i) {
            if (targets[i].mountpoint)
                vfs_dput(targets[i].mountpoint);
            if (!targets[i].attached && targets[i].clone)
                vfs_put_mount_tree_locked(targets[i].clone);
        }
    }
    free(groups);
    free(targets);
    vfs_mount_array_put(receivers, receiver_count);
    return ret;
}

static void vfs_propagate_detach_locked(struct vfs_mount *mnt) {
    struct vfs_mount **receivers = NULL;
    size_t receiver_count = 0;
    size_t i;

    if (!mnt || !mnt->mnt_parent || mnt->mnt_parent == mnt ||
        !mnt->mnt_mountpoint) {
        return;
    }
    if (mnt->mnt_parent->mnt_propagation != VFS_MNT_PROP_SHARED ||
        mnt->mnt_parent->mnt_group_id == 0) {
        return;
    }
    if (vfs_mount_collect_propagation_targets(
            mnt->mnt_parent, mnt->mnt_parent->mnt_group_id, &receivers,
            &receiver_count) < 0) {
        return;
    }

    for (i = 0; i < receiver_count; ++i) {
        struct vfs_dentry *translated_mountpoint;
        struct vfs_mount *propagated;

        translated_mountpoint = vfs_translate_dentry_between_mounts(
            mnt->mnt_parent->mnt_root, mnt->mnt_mountpoint, receivers[i]);
        if (!translated_mountpoint)
            continue;

        propagated = vfs_mount_find_propagated_child(
            receivers[i], translated_mountpoint, mnt);
        if (propagated)
            vfs_put_mount_tree_locked(propagated);
        vfs_dput(translated_mountpoint);
    }

    vfs_mount_array_put(receivers, receiver_count);
}

static void vfs_put_mount_tree_locked(struct vfs_mount *root) {
    while (root && !llist_empty(&root->mnt_mounts)) {
        struct vfs_mount *child =
            list_entry(root->mnt_mounts.next, struct vfs_mount, mnt_child);

        vfs_put_mount_tree_locked(child);
    }

    if (!root)
        return;
    if (root->mnt_mountpoint)
        vfs_mount_detach_locked(root, false);
    vfs_mntput(root);
}

static int vfs_mount_attach_tree(struct vfs_mount *parent,
                                 struct vfs_dentry *mountpoint,
                                 struct vfs_mount *child) {
    if (!mountpoint || !child)
        return -EINVAL;
    if (child->mnt_mountpoint || child->mnt_stack_prev ||
        child->mnt_stack_next || !llist_empty(&child->mnt_child)) {
        return -EBUSY;
    }

    child->mnt_parent = parent ? vfs_mntget(parent) : child;
    child->mnt_mountpoint = vfs_dget(mountpoint);
    child->mnt_stack_prev = mountpoint->d_mounted;
    child->mnt_stack_next = NULL;
    if (child->mnt_stack_prev)
        child->mnt_stack_prev->mnt_stack_next = child;
    mountpoint->d_mounted = child;
    mountpoint->d_flags |= VFS_DENTRY_MOUNTPOINT;
    if (parent)
        llist_append(&parent->mnt_mounts, &child->mnt_child);

    return 0;
}

void vfs_mntput(struct vfs_mount *mnt) {
    struct vfs_mount *parent;
    struct vfs_dentry *mountpoint;
    struct vfs_mount *below;
    struct vfs_mount *above;

    if (!mnt)
        return;
    if (!vfs_ref_put(&mnt->mnt_ref))
        return;

    parent =
        (mnt->mnt_parent && mnt->mnt_parent != mnt) ? mnt->mnt_parent : NULL;
    mountpoint = mnt->mnt_mountpoint;
    below = mnt->mnt_stack_prev;
    above = mnt->mnt_stack_next;

    if (above)
        above->mnt_stack_prev = below;
    if (below)
        below->mnt_stack_next = above;
    if (mountpoint && mountpoint->d_mounted == mnt)
        mountpoint->d_mounted = below;
    if (mountpoint && !mountpoint->d_mounted)
        mountpoint->d_flags &= ~VFS_DENTRY_MOUNTPOINT;

    if (!llist_empty(&mnt->mnt_child))
        llist_delete(&mnt->mnt_child);
    if (!llist_empty(&mnt->mnt_sb_link))
        llist_delete(&mnt->mnt_sb_link);
    if (mountpoint)
        vfs_dput(mountpoint);
    if (mnt->mnt_root)
        vfs_dput(mnt->mnt_root);
    if (mnt->mnt_sb)
        vfs_put_super(mnt->mnt_sb);
    mnt->mnt_parent = NULL;
    mnt->mnt_master = NULL;
    mnt->mnt_stack_prev = NULL;
    mnt->mnt_stack_next = NULL;
    mnt->mnt_mountpoint = NULL;
    mnt->mnt_root = NULL;
    mnt->mnt_sb = NULL;
    mnt->mnt_clone_source_id = 0;
    mnt->mnt_propagation_source_id = 0;
    free(mnt);
    if (parent)
        vfs_mntput(parent);
}

static int vfs_mount_attach_locked(struct vfs_mount *parent,
                                   struct vfs_dentry *mountpoint,
                                   struct vfs_mount *child, bool propagate) {
    uint8_t old_propagation;
    unsigned int old_group_id;
    struct vfs_mount *old_master;
    int ret;

    if (child->mnt_mountpoint || child->mnt_stack_prev ||
        child->mnt_stack_next || !llist_empty(&child->mnt_child)) {
        return -EBUSY;
    }

    child->mnt_parent = parent ? vfs_mntget(parent) : child;
    child->mnt_mountpoint = vfs_dget(mountpoint);
    child->mnt_stack_prev = mountpoint->d_mounted;
    child->mnt_stack_next = NULL;
    if (child->mnt_stack_prev)
        child->mnt_stack_prev->mnt_stack_next = child;
    mountpoint->d_mounted = child;
    mountpoint->d_flags |= VFS_DENTRY_MOUNTPOINT;
    if (parent)
        llist_append(&parent->mnt_mounts, &child->mnt_child);

    old_propagation = child->mnt_propagation;
    old_group_id = child->mnt_group_id;
    old_master = child->mnt_master;
    ret = vfs_prepare_mount_for_destination(parent, child);
    if (ret < 0)
        goto rollback_attach;

    if (propagate)
        ret = vfs_propagate_attach_locked(parent, mountpoint, child);
    if (ret < 0) {
    rollback_attach:
        if (!llist_empty(&child->mnt_child))
            llist_delete(&child->mnt_child);
        if (mountpoint->d_mounted == child)
            mountpoint->d_mounted = child->mnt_stack_prev;
        if (child->mnt_stack_prev)
            child->mnt_stack_prev->mnt_stack_next = NULL;
        if (!mountpoint->d_mounted)
            mountpoint->d_flags &= ~VFS_DENTRY_MOUNTPOINT;
        vfs_dput(child->mnt_mountpoint);
        if (child->mnt_parent && child->mnt_parent != child)
            vfs_mntput(child->mnt_parent);
        child->mnt_parent = child;
        child->mnt_mountpoint = NULL;
        child->mnt_stack_prev = NULL;
        child->mnt_stack_next = NULL;
        child->mnt_propagation = old_propagation;
        child->mnt_group_id = old_group_id;
        child->mnt_master = old_master;
        return ret;
    }

    vfs_mount_mark_changed();
    if (!vfs_init_mnt_ns.root)
        vfs_init_mnt_ns.root = child;
    return 0;
}

int vfs_mount_attach(struct vfs_mount *parent, struct vfs_dentry *mountpoint,
                     struct vfs_mount *child) {
    int ret;

    if (!mountpoint || !child)
        return -EINVAL;

    wait_mutex_lock(&vfs_mount_lock);
    ret = vfs_mount_attach_locked(parent, mountpoint, child, true);
    wait_mutex_unlock(&vfs_mount_lock);
    return ret;
}

static void vfs_mount_detach_locked(struct vfs_mount *mnt, bool propagate) {
    struct vfs_dentry *mountpoint;
    struct vfs_mount *parent;
    struct vfs_mount *below;
    struct vfs_mount *above;

    if (!mnt)
        return;
    mountpoint = mnt->mnt_mountpoint;
    parent =
        (mnt->mnt_parent && mnt->mnt_parent != mnt) ? mnt->mnt_parent : NULL;
    below = mnt->mnt_stack_prev;
    above = mnt->mnt_stack_next;

    if (propagate)
        vfs_propagate_detach_locked(mnt);

    if (above)
        above->mnt_stack_prev = below;
    if (below)
        below->mnt_stack_next = above;

    if (mountpoint && mountpoint->d_mounted == mnt)
        mountpoint->d_mounted = below;
    if (mountpoint && !mountpoint->d_mounted)
        mountpoint->d_flags &= ~VFS_DENTRY_MOUNTPOINT;

    mnt->mnt_parent = mnt;
    mnt->mnt_mountpoint = NULL;
    mnt->mnt_stack_prev = NULL;
    mnt->mnt_stack_next = NULL;

    if (!llist_empty(&mnt->mnt_child))
        llist_delete(&mnt->mnt_child);

    vfs_mount_mark_changed();
    if (mountpoint)
        vfs_dput(mountpoint);
    if (parent)
        vfs_mntput(parent);
}

void vfs_mount_detach(struct vfs_mount *mnt) {
    if (!mnt)
        return;

    wait_mutex_lock(&vfs_mount_lock);
    vfs_mount_detach_locked(mnt, true);
    wait_mutex_unlock(&vfs_mount_lock);
}

struct vfs_mount *vfs_path_mount(const struct vfs_path *path) {
    struct vfs_mount *root_mnt;
    struct vfs_mount *mnt;

    if (!path)
        return NULL;

    root_mnt = vfs_active_namespace_root_mount();
    mnt = (path->mnt && path->dentry)
              ? vfs_child_mount_at(path->mnt, path->dentry)
              : NULL;
    if (mnt)
        return mnt;

    if (path->mnt && path->dentry == path->mnt->mnt_root)
        return vfs_mntget(path->mnt);

    if (path->mnt && path->mnt != root_mnt)
        return vfs_mntget(path->mnt);
    return NULL;
}

#define VFS_MOUNT_TRANSLATE_MAX_DEPTH 256

struct vfs_dentry *
vfs_translate_dentry_between_mounts(const struct vfs_dentry *src_root,
                                    const struct vfs_dentry *src_dentry,
                                    struct vfs_mount *dst_mnt) {
    const struct vfs_dentry *chain[VFS_MOUNT_TRANSLATE_MAX_DEPTH];
    const struct vfs_dentry *cursor;
    struct vfs_path translated_path = {0};
    struct vfs_dentry *translated;
    size_t depth = 0;

    if (!src_root || !src_dentry || !dst_mnt || !dst_mnt->mnt_root)
        return NULL;
    if (src_dentry == src_root)
        return vfs_dget(dst_mnt->mnt_root);

    for (cursor = src_dentry; cursor && cursor != src_root;
         cursor = cursor->d_parent) {
        if (depth >= VFS_MOUNT_TRANSLATE_MAX_DEPTH || !cursor->d_parent ||
            cursor == cursor->d_parent) {
            return NULL;
        }
        chain[depth++] = cursor;
    }

    if (cursor != src_root)
        return NULL;

    if (!vfs_path_set(&translated_path, dst_mnt, dst_mnt->mnt_root))
        return NULL;

    while (depth > 0) {
        struct vfs_dentry *next;
        bool has_remaining = depth > 1;
        struct vfs_path next_path = {0};

        next = vfs_lookup_component(&translated_path,
                                    chain[--depth]->d_name.name, 0);

        if (IS_ERR(next) || !next || !next->d_inode) {
            if (next && !IS_ERR(next))
                vfs_dput(next);
            vfs_path_put(&translated_path);
            return NULL;
        }

        if (!vfs_path_set(&next_path, translated_path.mnt, next)) {
            vfs_dput(next);
            vfs_path_put(&translated_path);
            return NULL;
        }
        vfs_path_move(&translated_path, &next_path);
        vfs_dput(next);

        if (has_remaining)
            vfs_follow_mount(&translated_path);
    }

    translated = vfs_dget(translated_path.dentry);
    vfs_path_put(&translated_path);
    return translated;
}

struct vfs_mount *vfs_translate_mount_between_roots(struct vfs_mount *old_root,
                                                    struct vfs_mount *new_root,
                                                    struct vfs_mount *old_mnt) {
    struct vfs_mount *chain[VFS_MOUNT_TRANSLATE_MAX_DEPTH];
    struct vfs_mount *cursor;
    struct vfs_mount *translated;
    size_t depth = 0;

    if (!old_root || !new_root || !old_mnt)
        return NULL;
    if (old_mnt == old_root)
        return vfs_mntget(new_root);

    for (cursor = old_mnt; cursor && cursor != old_root;
         cursor = cursor->mnt_parent) {
        if (depth >= VFS_MOUNT_TRANSLATE_MAX_DEPTH || !cursor->mnt_parent ||
            cursor->mnt_parent == cursor || !cursor->mnt_mountpoint) {
            return NULL;
        }
        chain[depth++] = cursor;
    }

    if (cursor != old_root)
        return NULL;

    translated = vfs_mntget(new_root);
    if (!translated)
        return NULL;

    while (depth > 0) {
        struct vfs_mount *next =
            vfs_find_mount_child_by_source(translated, chain[--depth]);

        vfs_mntput(translated);
        if (!next)
            return NULL;
        translated = next;
    }

    return translated;
}

int vfs_translate_path_between_roots(const struct vfs_path *old_root,
                                     const struct vfs_path *old_path,
                                     struct vfs_mount *new_root,
                                     struct vfs_path *new_path) {
    struct vfs_mount *translated_mnt;
    struct vfs_dentry *translated_dentry;

    if (!old_root || !old_root->mnt || !old_root->dentry || !old_path ||
        !old_path->mnt || !old_path->dentry || !new_root || !new_path) {
        return -EINVAL;
    }

    translated_mnt = vfs_translate_mount_between_roots(old_root->mnt, new_root,
                                                       old_path->mnt);
    if (!translated_mnt)
        return -ENOENT;

    if (old_path->dentry == old_path->mnt->mnt_root) {
        translated_dentry = vfs_dget(translated_mnt->mnt_root);
    } else {
        translated_dentry = vfs_translate_dentry_between_mounts(
            old_path->mnt->mnt_root, old_path->dentry, translated_mnt);
    }
    if (!translated_dentry) {
        vfs_mntput(translated_mnt);
        return -ENOENT;
    }

    memset(new_path, 0, sizeof(*new_path));
    new_path->mnt = translated_mnt;
    new_path->dentry = translated_dentry;
    return 0;
}

static struct vfs_mount *vfs_clone_single_mount(struct vfs_mount *src) {
    struct vfs_mount *dst;

    if (!src)
        return NULL;

    dst = vfs_mount_alloc(src->mnt_sb, src->mnt_flags);
    if (!dst)
        return NULL;

    if (src->mnt_root && dst->mnt_root != src->mnt_root) {
        if (dst->mnt_root)
            vfs_dput(dst->mnt_root);
        dst->mnt_root = vfs_dget(src->mnt_root);
    }

    vfs_mount_copy_propagation(dst, src);
    return dst;
}

static struct vfs_mount *vfs_bind_single_mount(struct vfs_mount *src,
                                               struct vfs_dentry *root) {
    struct vfs_mount *dst;

    if (!src || !root || !root->d_inode)
        return NULL;

    dst = vfs_mount_alloc(src->mnt_sb, src->mnt_flags);
    if (!dst)
        return NULL;

    if (dst->mnt_root)
        vfs_dput(dst->mnt_root);
    dst->mnt_root = vfs_dget(root);
    vfs_mount_copy_propagation(dst, src);
    return dst;
}

static bool vfs_dentry_is_descendant(struct vfs_dentry *root,
                                     struct vfs_dentry *child) {
    struct vfs_dentry *cursor;

    if (!root || !child)
        return false;

    for (cursor = child; cursor; cursor = cursor->d_parent) {
        if (cursor == root)
            return true;
        if (!cursor->d_parent || cursor == cursor->d_parent)
            break;
    }

    return false;
}

static bool vfs_mount_is_same_or_descendant(const struct vfs_mount *root,
                                            const struct vfs_mount *mnt) {
    const struct vfs_mount *cursor;

    if (!root || !mnt)
        return false;

    for (cursor = mnt; cursor; cursor = cursor->mnt_parent) {
        if (cursor == root)
            return true;
        if (!cursor->mnt_parent || cursor == cursor->mnt_parent)
            break;
    }

    return false;
}

static bool vfs_mount_tree_contains_unbindable(const struct vfs_mount *root) {
    const struct vfs_mount *child;
    const struct vfs_mount *tmp;

    if (!root)
        return false;
    if (root->mnt_propagation == VFS_MNT_PROP_UNBINDABLE)
        return true;

    llist_for_each(child, tmp, &root->mnt_mounts, mnt_child) {
        if (vfs_mount_tree_contains_unbindable(child))
            return true;
    }

    return false;
}

static int vfs_clone_bind_mount_children(struct vfs_mount *src_base,
                                         struct vfs_dentry *src_root,
                                         struct vfs_mount *src_parent,
                                         struct vfs_mount *dst_parent,
                                         struct vfs_mount_clone_pair *pairs,
                                         size_t pair_capacity,
                                         size_t *pair_count) {
    struct vfs_mount *src_child;
    struct vfs_mount *tmp;
    struct vfs_mount **extra_children = NULL;
    size_t extra_count = 0;
    size_t i;
    int ret;

    if (!src_base || !src_root || !src_parent || !dst_parent)
        return -EINVAL;

    llist_for_each(src_child, tmp, &src_parent->mnt_mounts, mnt_child) {
        struct vfs_mount *dst_child;
        struct vfs_dentry *dst_mountpoint;
        struct vfs_dentry *translation_root;
        int ret;

        if (!src_child->mnt_mountpoint)
            continue;
        if (src_parent == src_base &&
            !vfs_dentry_is_descendant(src_root, src_child->mnt_mountpoint)) {
            continue;
        }

        dst_child = vfs_bind_single_mount(src_child, src_child->mnt_root);
        if (!dst_child)
            return -ENOMEM;

        translation_root =
            src_parent == src_base ? src_root : src_parent->mnt_root;
        dst_mountpoint = vfs_translate_dentry_between_mounts(
            translation_root, src_child->mnt_mountpoint, dst_parent);
        if (!dst_mountpoint) {
            vfs_mntput(dst_child);
            return -ENOENT;
        }

        ret = vfs_mount_attach_tree(dst_parent, dst_mountpoint, dst_child);
        vfs_dput(dst_mountpoint);
        if (ret < 0) {
            vfs_mntput(dst_child);
            return ret;
        }

        ret = vfs_mount_clone_pair_add(pairs, pair_capacity, pair_count,
                                       src_child, dst_child);
        if (ret < 0)
            return ret;

        ret = vfs_clone_bind_mount_children(src_base, src_root, src_child,
                                            dst_child, pairs, pair_capacity,
                                            pair_count);
        if (ret < 0)
            return ret;
    }

    ret = vfs_mount_collect_missing_direct_children(src_parent, &extra_children,
                                                    &extra_count);
    if (ret < 0)
        return ret;

    for (i = 0; i < extra_count; ++i) {
        struct vfs_mount *dst_child;
        struct vfs_dentry *dst_mountpoint;
        struct vfs_dentry *translation_root;
        int ret;

        src_child = extra_children[i];
        if (!src_child || !src_child->mnt_mountpoint)
            continue;
        if (src_parent == src_base &&
            !vfs_dentry_is_descendant(src_root, src_child->mnt_mountpoint)) {
            continue;
        }

        dst_child = vfs_bind_single_mount(src_child, src_child->mnt_root);
        if (!dst_child) {
            free(extra_children);
            return -ENOMEM;
        }

        translation_root =
            src_parent == src_base ? src_root : src_parent->mnt_root;
        dst_mountpoint = vfs_translate_dentry_between_mounts(
            translation_root, src_child->mnt_mountpoint, dst_parent);
        if (!dst_mountpoint) {
            vfs_mntput(dst_child);
            free(extra_children);
            return -ENOENT;
        }

        ret = vfs_mount_attach_tree(dst_parent, dst_mountpoint, dst_child);
        vfs_dput(dst_mountpoint);
        if (ret < 0) {
            vfs_mntput(dst_child);
            free(extra_children);
            return ret;
        }

        ret = vfs_mount_clone_pair_add(pairs, pair_capacity, pair_count,
                                       src_child, dst_child);
        if (ret < 0) {
            free(extra_children);
            return ret;
        }

        ret = vfs_clone_bind_mount_children(src_base, src_root, src_child,
                                            dst_child, pairs, pair_capacity,
                                            pair_count);
        if (ret < 0) {
            free(extra_children);
            return ret;
        }
    }

    free(extra_children);
    return 0;
}

int vfs_create_bind_mount_err(const struct vfs_path *from, bool recursive,
                              struct vfs_mount **out) {
    struct vfs_mount *src_mnt;
    struct vfs_mount *clone;
    struct vfs_mount_clone_pair *pairs = NULL;
    size_t pair_capacity = 0;
    size_t pair_count = 0;
    int ret;

    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!from || !from->mnt || !from->dentry || !from->dentry->d_inode)
        return -EINVAL;

    src_mnt = vfs_path_mount(from);
    if (!src_mnt)
        src_mnt = vfs_mntget(from->mnt);
    if (!src_mnt)
        return -EINVAL;

    clone = vfs_bind_single_mount(src_mnt, from->dentry);
    if (!clone) {
        vfs_mntput(src_mnt);
        return -ENOMEM;
    }

    clone->mnt_parent = clone;
    clone->mnt_mountpoint = NULL;
    clone->mnt_stack_prev = NULL;
    clone->mnt_stack_next = NULL;

    pair_capacity = vfs_count_complete_mount_tree_nodes(src_mnt);
    pairs = calloc(pair_capacity ? pair_capacity : 1, sizeof(*pairs));
    if (!pairs) {
        vfs_mntput(src_mnt);
        vfs_put_mount_tree(clone);
        return -ENOMEM;
    }
    if (vfs_mount_clone_pair_add(pairs, pair_capacity ? pair_capacity : 1,
                                 &pair_count, src_mnt, clone) < 0) {
        vfs_mntput(src_mnt);
        vfs_put_mount_tree(clone);
        free(pairs);
        return -EOVERFLOW;
    }

    if (recursive) {
        ret = vfs_clone_bind_mount_children(
            src_mnt, from->dentry, src_mnt, clone, pairs,
            pair_capacity ? pair_capacity : 1, &pair_count);
        if (ret < 0) {
            vfs_mntput(src_mnt);
            vfs_put_mount_tree(clone);
            free(pairs);
            return ret;
        }
    }

    vfs_remap_cloned_tree_metadata(pairs, pair_count, false);
    vfs_mntput(src_mnt);
    free(pairs);
    *out = clone;
    return 0;
}

struct vfs_mount *vfs_create_bind_mount(const struct vfs_path *from,
                                        bool recursive) {
    struct vfs_mount *mnt = NULL;

    if (vfs_create_bind_mount_err(from, recursive, &mnt) < 0)
        return NULL;
    return mnt;
}

static int vfs_clone_mount_children(struct vfs_mount *src_parent,
                                    struct vfs_mount *dst_parent,
                                    struct vfs_mount_clone_pair *pairs,
                                    size_t pair_capacity, size_t *pair_count) {
    struct vfs_mount *src_child, *tmp;
    struct vfs_mount **extra_children = NULL;
    size_t extra_count = 0;
    size_t i;
    int ret;

    llist_for_each(src_child, tmp, &src_parent->mnt_mounts, mnt_child) {
        struct vfs_mount *dst_child = vfs_clone_single_mount(src_child);
        struct vfs_dentry *dst_mountpoint;

        if (!dst_child)
            return -ENOMEM;

        dst_mountpoint = vfs_translate_dentry_between_mounts(
            src_parent->mnt_root, src_child->mnt_mountpoint, dst_parent);
        if (!dst_mountpoint) {
            vfs_mntput(dst_child);
            return -ENOENT;
        }

        ret = vfs_mount_attach_tree(dst_parent, dst_mountpoint, dst_child);
        vfs_dput(dst_mountpoint);
        if (ret < 0) {
            vfs_mntput(dst_child);
            return ret;
        }

        ret = vfs_mount_clone_pair_add(pairs, pair_capacity, pair_count,
                                       src_child, dst_child);
        if (ret < 0)
            return ret;

        ret = vfs_clone_mount_children(src_child, dst_child, pairs,
                                       pair_capacity, pair_count);
        if (ret < 0)
            return ret;
    }

    ret = vfs_mount_collect_missing_direct_children(src_parent, &extra_children,
                                                    &extra_count);
    if (ret < 0)
        return ret;

    for (i = 0; i < extra_count; ++i) {
        struct vfs_mount *dst_child;
        struct vfs_dentry *dst_mountpoint;

        src_child = extra_children[i];
        if (!src_child->mnt_mountpoint)
            continue;

        dst_child = vfs_clone_single_mount(src_child);
        if (!dst_child) {
            free(extra_children);
            return -ENOMEM;
        }

        dst_mountpoint = vfs_translate_dentry_between_mounts(
            src_parent->mnt_root, src_child->mnt_mountpoint, dst_parent);
        if (!dst_mountpoint) {
            vfs_mntput(dst_child);
            free(extra_children);
            return -ENOENT;
        }

        ret = vfs_mount_attach_tree(dst_parent, dst_mountpoint, dst_child);
        vfs_dput(dst_mountpoint);
        if (ret < 0) {
            vfs_mntput(dst_child);
            free(extra_children);
            return ret;
        }

        ret = vfs_mount_clone_pair_add(pairs, pair_capacity, pair_count,
                                       src_child, dst_child);
        if (ret < 0) {
            free(extra_children);
            return ret;
        }

        ret = vfs_clone_mount_children(src_child, dst_child, pairs,
                                       pair_capacity, pair_count);
        if (ret < 0) {
            free(extra_children);
            return ret;
        }
    }

    free(extra_children);

    return 0;
}

static struct vfs_mount *vfs_clone_mount_tree_internal(struct vfs_mount *root,
                                                       bool mark_source_id) {
    struct vfs_mount *clone;
    struct vfs_mount_clone_pair *pairs = NULL;
    size_t pair_capacity = 0;
    size_t pair_count = 0;
    int ret;

    if (!root)
        return NULL;

    clone = vfs_clone_single_mount(root);
    if (!clone)
        return NULL;

    clone->mnt_parent = clone;
    clone->mnt_mountpoint = NULL;
    clone->mnt_stack_prev = NULL;
    clone->mnt_stack_next = NULL;

    pair_capacity = vfs_count_complete_mount_tree_nodes(root);
    pairs = calloc(pair_capacity ? pair_capacity : 1, sizeof(*pairs));
    if (!pairs) {
        vfs_put_mount_tree_locked(clone);
        return NULL;
    }
    ret = vfs_mount_clone_pair_add(pairs, pair_capacity ? pair_capacity : 1,
                                   &pair_count, root, clone);
    if (ret < 0) {
        free(pairs);
        vfs_put_mount_tree_locked(clone);
        return NULL;
    }

    ret = vfs_clone_mount_children(
        root, clone, pairs, pair_capacity ? pair_capacity : 1, &pair_count);
    if (ret < 0) {
        free(pairs);
        vfs_put_mount_tree_locked(clone);
        return NULL;
    }

    vfs_remap_cloned_tree_metadata(pairs, pair_count, mark_source_id);
    free(pairs);
    return clone;
}

struct vfs_mount *vfs_clone_mount_tree(struct vfs_mount *root) {
    return vfs_clone_mount_tree_internal(root, false);
}

struct vfs_mount *vfs_clone_visible_mount_tree(const struct vfs_path *from) {
    return vfs_create_bind_mount(from, true);
}

void vfs_put_mount_tree(struct vfs_mount *root) {
    struct vfs_mount *child, *tmp;

    if (!root)
        return;

    llist_for_each(child, tmp, &root->mnt_mounts, mnt_child) {
        vfs_put_mount_tree(child);
    }

    if (root->mnt_mountpoint)
        vfs_mount_detach(root);
    vfs_mntput(root);
}

int vfs_pivot_root_mounts(struct vfs_mount *old_root,
                          struct vfs_mount *new_root,
                          const struct vfs_path *put_old) {
    struct vfs_mount *new_root_parent;
    struct vfs_dentry *new_root_mountpoint;
    int ret;

    if (!old_root || !new_root || !put_old || !put_old->mnt ||
        !put_old->dentry) {
        return -EINVAL;
    }

    if (old_root == new_root)
        return -EBUSY;
    if (!vfs_mount_is_same_or_descendant(new_root, put_old->mnt))
        return -EINVAL;
    if (new_root->mnt_parent == new_root || !new_root->mnt_mountpoint)
        return -EINVAL;

    new_root_parent = vfs_mntget(new_root->mnt_parent);
    new_root_mountpoint = vfs_dget(new_root->mnt_mountpoint);
    if (!new_root_parent || !new_root_mountpoint) {
        if (new_root_parent)
            vfs_mntput(new_root_parent);
        if (new_root_mountpoint)
            vfs_dput(new_root_mountpoint);
        return -ENOMEM;
    }

    wait_mutex_lock(&vfs_mount_lock);

    vfs_mount_detach_locked(new_root, false);
    ret =
        vfs_mount_attach_locked(put_old->mnt, put_old->dentry, old_root, false);
    if (ret < 0) {
        (void)vfs_mount_attach_locked(new_root_parent, new_root_mountpoint,
                                      new_root, false);
        wait_mutex_unlock(&vfs_mount_lock);
        vfs_mntput(new_root_parent);
        vfs_dput(new_root_mountpoint);
        return ret;
    }

    wait_mutex_unlock(&vfs_mount_lock);
    vfs_mntput(new_root_parent);
    vfs_dput(new_root_mountpoint);
    return 0;
}

int vfs_reconfigure_mount(struct vfs_mount *mnt, const struct vfs_path *to_path,
                          bool detached) {
    struct vfs_mount *old_parent = NULL;
    struct vfs_dentry *old_mountpoint = NULL;
    struct vfs_path old_root = {0};
    struct vfs_mount *active_root = NULL;
    bool src_is_dir;
    bool dst_is_dir;
    bool replacing_namespace_root = false;
    int ret;

    if (!mnt || !to_path || !to_path->mnt || !to_path->dentry ||
        !to_path->dentry->d_inode) {
        return -EINVAL;
    }

    if (!mnt->mnt_root || !mnt->mnt_root->d_inode)
        return -EINVAL;

    if (vfs_mount_is_same_or_descendant(mnt, to_path->mnt))
        return -ELOOP;
    if (!detached && mnt->mnt_parent && mnt->mnt_parent != mnt &&
        vfs_mount_is_shared(mnt->mnt_parent)) {
        return -EINVAL;
    }
    if (vfs_mount_is_shared(to_path->mnt) &&
        vfs_mount_tree_contains_unbindable(mnt)) {
        return -EINVAL;
    }

    src_is_dir = S_ISDIR(mnt->mnt_root->d_inode->i_mode);
    dst_is_dir = S_ISDIR(to_path->dentry->d_inode->i_mode);
    if (src_is_dir != dst_is_dir)
        return -ENOTDIR;

    if (!detached && mnt->mnt_parent == to_path->mnt &&
        mnt->mnt_mountpoint == to_path->dentry &&
        to_path->dentry->d_mounted == mnt) {
        return 0;
    }

    active_root = vfs_active_namespace_root_mount();
    replacing_namespace_root = !detached && active_root == to_path->mnt &&
                               to_path->dentry == to_path->mnt->mnt_root;

    if (!vfs_path_copy(&old_root, to_path))
        return -ENOENT;

    wait_mutex_lock(&vfs_mount_lock);

    if (!detached) {
        old_parent =
            mnt->mnt_parent != mnt ? vfs_mntget(mnt->mnt_parent) : NULL;
        old_mountpoint =
            mnt->mnt_mountpoint ? vfs_dget(mnt->mnt_mountpoint) : NULL;
        vfs_mount_detach_locked(mnt, false);
    }

    ret = vfs_mount_attach_locked(to_path->mnt, to_path->dentry, mnt, detached);
    if (ret < 0) {
        if (!detached && old_mountpoint)
            (void)vfs_mount_attach_locked(old_parent, old_mountpoint, mnt,
                                          false);
        wait_mutex_unlock(&vfs_mount_lock);
        goto out;
    }

    wait_mutex_unlock(&vfs_mount_lock);

    vfs_rebind_task_root_paths(&old_root, mnt);
    if (replacing_namespace_root)
        vfs_rebind_namespace_root(&old_root, mnt);

out:
    if (old_parent)
        vfs_mntput(old_parent);
    if (old_mountpoint)
        vfs_dput(old_mountpoint);
    vfs_path_put(&old_root);
    return ret;
}

int vfs_mount_set_propagation(struct vfs_mount *mnt, unsigned long flags,
                              bool recursive) {
    enum vfs_mount_propagation propagation;

    if (!mnt)
        return -EINVAL;

    switch (flags) {
    case MS_SHARED:
        propagation = VFS_MNT_PROP_SHARED;
        break;
    case MS_PRIVATE:
        propagation = VFS_MNT_PROP_PRIVATE;
        break;
    case MS_SLAVE:
        propagation = VFS_MNT_PROP_SLAVE;
        break;
    case MS_UNBINDABLE:
        propagation = VFS_MNT_PROP_UNBINDABLE;
        break;
    default:
        return -EINVAL;
    }

    wait_mutex_lock(&vfs_mount_lock);
    if (recursive)
        vfs_mount_apply_propagation_tree(mnt, propagation);
    else
        vfs_mount_apply_propagation(mnt, propagation);
    vfs_mount_mark_changed();
    wait_mutex_unlock(&vfs_mount_lock);
    return 0;
}

bool vfs_mount_is_shared(const struct vfs_mount *mnt) {
    return mnt && mnt->mnt_propagation == VFS_MNT_PROP_SHARED &&
           mnt->mnt_group_id != 0;
}

unsigned int vfs_mount_peer_group_id(const struct vfs_mount *mnt) {
    return mnt ? mnt->mnt_group_id : 0;
}

unsigned int vfs_mount_master_group_id(const struct vfs_mount *mnt) {
    if (!mnt || !mnt->mnt_master)
        return 0;
    return mnt->mnt_master->mnt_group_id;
}
