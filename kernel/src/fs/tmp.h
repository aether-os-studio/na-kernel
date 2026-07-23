#pragma once

#include <fs/vfs/vfs.h>
#include <fs/vfs/paged_store.h>

typedef struct tmpfs_fs_info {
    uint64_t next_ino;
    dev64_t dev;
    uid32_t root_uid;
    gid32_t root_gid;
    umode_t root_mode;
    spinlock_t lock;
} tmpfs_fs_info_t;

typedef struct tmpfs_dirent {
    struct llist_header node;
    char *name;
    struct vfs_inode *inode;
} tmpfs_dirent_t;

typedef struct tmpfs_xattr {
    struct llist_header node;
    char *name;
    void *value;
    size_t size;
} tmpfs_xattr_t;

typedef struct tmpfs_inode_info {
    struct vfs_inode vfs_inode;
    spinlock_t lock;
    paged_file_store_t store;
    char *link_target;
    struct llist_header children;
    struct llist_header xattrs;
} tmpfs_inode_info_t;

void tmpfs_init(void);
