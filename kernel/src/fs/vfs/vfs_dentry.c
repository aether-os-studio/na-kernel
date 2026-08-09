#include "fs/vfs/vfs_internal.h"
#include "mm/mm.h"

struct vfs_dcache_bucket {
    spinlock_t lock;
    struct hlist_node *head;
};

static struct vfs_dcache_bucket vfs_dcache[VFS_DCACHE_BUCKETS];

static inline bool vfs_qstr_equal(const struct vfs_qstr *a,
                                  const struct vfs_qstr *b) {
    if (!a || !b)
        return false;
    if (a->len != b->len || a->hash != b->hash)
        return false;
    if (!a->name || !b->name)
        return false;
    return strncmp(a->name, b->name, a->len) == 0;
}

static inline struct vfs_dcache_bucket *
vfs_dcache_bucket_for(struct vfs_super_block *sb, struct vfs_dentry *parent,
                      const struct vfs_qstr *name) {
    uintptr_t seed =
        ((uintptr_t)sb >> 6) ^ ((uintptr_t)parent >> 4) ^ name->hash;
    return &vfs_dcache[seed & (VFS_DCACHE_BUCKETS - 1)];
}

void vfs_dcache_init(void) {
    unsigned int i;

    for (i = 0; i < VFS_DCACHE_BUCKETS; ++i) {
        spin_init(&vfs_dcache[i].lock);
        vfs_dcache[i].head = NULL;
    }
}

static void vfs_dentry_rehash(struct vfs_dentry *dentry) {
    struct vfs_dcache_bucket *bucket;

    if (!dentry || !dentry->d_parent || !dentry->d_sb)
        return;
    if (dentry->d_flags & VFS_DENTRY_HASHED)
        return;

    vfs_dget(dentry);
    bucket =
        vfs_dcache_bucket_for(dentry->d_sb, dentry->d_parent, &dentry->d_name);
    spin_lock(&bucket->lock);
    hlist_add(&bucket->head, &dentry->d_hash);
    dentry->d_flags |= VFS_DENTRY_HASHED;
    spin_unlock(&bucket->lock);
}

static void vfs_dentry_detach_alias(struct vfs_dentry *dentry) {
    struct vfs_inode *inode;

    if (!dentry || llist_empty(&dentry->d_alias))
        return;

    inode = dentry->d_inode;
    if (inode)
        spin_lock(&inode->i_lock);
    if (!llist_empty(&dentry->d_alias))
        llist_delete(&dentry->d_alias);
    if (inode)
        spin_unlock(&inode->i_lock);
}

void vfs_dentry_unhash(struct vfs_dentry *dentry) {
    struct vfs_dcache_bucket *bucket;
    bool had_cache_ref = false;

    if (!dentry || !(dentry->d_flags & VFS_DENTRY_HASHED))
        return;
    bucket =
        vfs_dcache_bucket_for(dentry->d_sb, dentry->d_parent, &dentry->d_name);
    spin_lock(&bucket->lock);
    if (dentry->d_flags & VFS_DENTRY_HASHED) {
        hlist_delete(&dentry->d_hash);
        dentry->d_flags &= ~VFS_DENTRY_HASHED;
        had_cache_ref = true;
    }
    spin_unlock(&bucket->lock);
    if (had_cache_ref)
        vfs_dput(dentry);
}

void vfs_dcache_shrink_super(struct vfs_super_block *sb) {
    unsigned int i;

    if (!sb)
        return;

    for (i = 0; i < VFS_DCACHE_BUCKETS; ++i) {
        struct vfs_dcache_bucket *bucket = &vfs_dcache[i];

        for (;;) {
            struct vfs_dentry *victim = NULL;
            struct hlist_node *node;

            spin_lock(&bucket->lock);
            for (node = bucket->head; node; node = node->next) {
                struct vfs_dentry *dentry =
                    container_of(node, struct vfs_dentry, d_hash);

                if (dentry->d_sb != sb)
                    continue;
                victim = vfs_dget(dentry);
                if (!victim)
                    continue;
                hlist_delete(&victim->d_hash);
                victim->d_flags &= ~VFS_DENTRY_HASHED;
                break;
            }
            spin_unlock(&bucket->lock);

            if (!victim)
                break;
            /* Drop the dcache reference removed above and our temporary
             * traversal reference. */
            vfs_dput(victim);
            vfs_dput(victim);
        }
    }
}

struct vfs_dentry *vfs_d_alloc(struct vfs_super_block *sb,
                               struct vfs_dentry *parent,
                               const struct vfs_qstr *name) {
    struct vfs_dentry *dentry = calloc(1, sizeof(*dentry));
    if (!dentry)
        return NULL;

    spin_init(&dentry->d_lock);
    spin_init(&dentry->d_children_lock);
    vfs_lockref_init(&dentry->d_lockref, 1);
    llist_init_head(&dentry->d_child);
    llist_init_head(&dentry->d_subdirs);
    llist_init_head(&dentry->d_alias);
    dentry->d_sb = sb;
    dentry->d_parent = parent ? vfs_dget(parent) : dentry;
    dentry->d_op = sb ? sb->s_d_op : NULL;

    if (name && name->name) {
        if (name->len > VFS_NAME_MAX) {
            if (parent)
                vfs_dput(dentry->d_parent);
            free(dentry);
            return NULL;
        }
        char *copy = malloc((size_t)name->len + 1);
        if (!copy) {
            if (parent)
                vfs_dput(dentry->d_parent);
            free(dentry);
            return NULL;
        }
        memcpy(copy, name->name, name->len);
        copy[name->len] = '\0';
        dentry->d_name.name = copy;
        dentry->d_name.len = name->len;
        dentry->d_name.hash = name->hash;
    } else {
        dentry->d_name.name = strdup("");
        dentry->d_name.len = 0;
        dentry->d_name.hash = 0;
        if (!dentry->d_name.name) {
            if (parent)
                vfs_dput(dentry->d_parent);
            free(dentry);
            return NULL;
        }
    }

    if (!parent)
        dentry->d_flags |= VFS_DENTRY_ROOT;
    return dentry;
}

struct vfs_dentry *vfs_dget(struct vfs_dentry *dentry) {
    if (!dentry)
        return NULL;
    if (!vfs_lockref_try_get(&dentry->d_lockref))
        return NULL;
    return dentry;
}

void vfs_dput(struct vfs_dentry *dentry) {
    struct vfs_dentry *parent;

    if (!dentry)
        return;
    if (!vfs_lockref_put(&dentry->d_lockref))
        return;

    if (dentry->d_op && dentry->d_op->d_release)
        dentry->d_op->d_release(dentry);

    if (dentry->d_parent && dentry->d_parent != dentry &&
        !llist_empty(&dentry->d_child)) {
        spin_lock(&dentry->d_parent->d_children_lock);
        if (!llist_empty(&dentry->d_child))
            llist_delete(&dentry->d_child);
        spin_unlock(&dentry->d_parent->d_children_lock);
    }

    if (dentry->d_flags & VFS_DENTRY_HASHED)
        vfs_dentry_unhash(dentry);

    if (dentry->d_inode) {
        vfs_dentry_detach_alias(dentry);
        vfs_iput(dentry->d_inode);
    }

    parent = dentry->d_parent;
    if (dentry->d_name.name)
        free((void *)dentry->d_name.name);
    free(dentry);

    if (parent && parent != dentry)
        vfs_dput(parent);
}

void vfs_d_add(struct vfs_dentry *parent, struct vfs_dentry *dentry) {
    if (!dentry)
        return;

    if (!dentry->d_parent && parent)
        dentry->d_parent = vfs_dget(parent);

    if (parent && llist_empty(&dentry->d_child)) {
        spin_lock(&parent->d_children_lock);
        if (llist_empty(&dentry->d_child))
            llist_append(&parent->d_subdirs, &dentry->d_child);
        spin_unlock(&parent->d_children_lock);
    }

    vfs_dentry_rehash(dentry);
}

void vfs_d_instantiate(struct vfs_dentry *dentry, struct vfs_inode *inode) {
    if (!dentry)
        return;
    if (dentry->d_inode) {
        vfs_dentry_detach_alias(dentry);
        vfs_iput(dentry->d_inode);
    }

    dentry->d_inode = vfs_igrab(inode);
    if (inode) {
        vfs_sync_inode_compat(inode);
        dentry->d_flags &= ~VFS_DENTRY_NEGATIVE;
        spin_lock(&inode->i_lock);
        if (llist_empty(&dentry->d_alias))
            llist_append(&inode->i_dentry_aliases, &dentry->d_alias);
        spin_unlock(&inode->i_lock);
    } else {
        dentry->d_flags |= VFS_DENTRY_NEGATIVE;
    }
    dentry->d_seq++;
}

struct vfs_dentry *vfs_d_lookup(struct vfs_dentry *parent,
                                const struct vfs_qstr *name) {
    struct vfs_dcache_bucket *bucket;
    struct hlist_node *node;

    if (!parent || !name)
        return NULL;

    struct vfs_qstr dot_name;
    vfs_qstr_make(&dot_name, ".");
    struct vfs_qstr dotdot_name;
    vfs_qstr_make(&dotdot_name, "..");
    if (vfs_qstr_equal(name, &dot_name)) {
        vfs_dget(parent);
        return parent;
    } else if (vfs_qstr_equal(name, &dotdot_name)) {
        struct vfs_dentry *parent_parent = parent->d_parent;
        if (!parent_parent)
            return NULL;
        vfs_dget(parent_parent);
        return parent_parent;
    }

    bucket = vfs_dcache_bucket_for(parent->d_sb, parent, name);
    spin_lock(&bucket->lock);
    for (node = bucket->head; node; node = node->next) {
        struct vfs_dentry *dentry =
            container_of(node, struct vfs_dentry, d_hash);
        if (dentry->d_parent != parent)
            continue;
        if (!vfs_qstr_equal(&dentry->d_name, name))
            continue;
        vfs_dget(dentry);
        spin_unlock(&bucket->lock);
        return dentry;
    }
    spin_unlock(&bucket->lock);
    return NULL;
}
