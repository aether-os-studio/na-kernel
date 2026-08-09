#pragma once

#include "fs/vfs/vfs.h"

void vfs_dcache_init(void);
void vfs_mount_subsys_init(void);
void vfs_ops_init(void);

void vfs_sync_inode_compat(struct vfs_inode *inode);
void vfs_dentry_unhash(struct vfs_dentry *dentry);
void vfs_dcache_shrink_super(struct vfs_super_block *sb);

struct vfs_mount *vfs_active_namespace_root_mount(void);
struct vfs_mount *vfs_child_mount_at(struct vfs_mount *parent,
                                     struct vfs_dentry *mountpoint);
struct vfs_mount *
vfs_find_mount_child_by_source(struct vfs_mount *parent,
                               const struct vfs_mount *src_child);
struct vfs_mount *vfs_create_bind_mount(const struct vfs_path *from,
                                        bool recursive);
int vfs_create_bind_mount_err(const struct vfs_path *from, bool recursive,
                              struct vfs_mount **out);

int vfs_get_fs_start(int dfd, const char *name, unsigned int lookup_flags,
                     struct vfs_path *start, struct vfs_path *root);
int vfs_filename_lookup_from(const struct vfs_path *start,
                             const struct vfs_path *root, const char *name,
                             unsigned int lookup_flags, struct vfs_path *path);
int vfs_path_parent_lookup_from(const struct vfs_path *start,
                                const struct vfs_path *root, const char *name,
                                unsigned int lookup_flags,
                                struct vfs_path *parent, struct vfs_qstr *last,
                                unsigned int *type);
struct vfs_dentry *vfs_lookup_component(struct vfs_path *parent,
                                        const char *component,
                                        unsigned int flags);
void vfs_follow_mount(struct vfs_path *path);

static inline bool vfs_qstr_is_dot(const struct vfs_qstr *qstr) {
    return qstr && qstr->name && qstr->len == 1 && qstr->name[0] == '.';
}

static inline bool vfs_qstr_is_dotdot(const struct vfs_qstr *qstr) {
    return qstr && qstr->name && qstr->len == 2 && qstr->name[0] == '.' &&
           qstr->name[1] == '.';
}
