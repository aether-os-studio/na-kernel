#pragma once

#include <fs/vfs/fcntl.h>
#include <fs/vfs/utils.h>
#include <libs/klibc.h>
#include <libs/llist.h>
#include <libs/rbtree.h>
#include <task/wait.h>

#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

#ifndef F_RDLCK
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#endif

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9
#define CLOCK_SGI_CYCLE 10
#define CLOCK_TAI 11
#endif

#define VFS_PATH_MAX 4096
#define VFS_NAME_MAX 255
#define VFS_MAX_SYMLINKS 8
#define VFS_DCACHE_HASH_BITS 12
#define VFS_DCACHE_BUCKETS (1U << VFS_DCACHE_HASH_BITS)
#define VFS_FILESYSTEM_MAX 128

#define S_IFMT 00170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000

#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

#define VFS_MAY_EXEC 1
#define VFS_MAY_WRITE 2
#define VFS_MAY_READ 4

#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(ptr) ((long)(intptr_t)(ptr))
#define IS_ERR(ptr) ((uintptr_t)(void *)(ptr) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(ptr) (!(ptr) || IS_ERR(ptr))
#define ERR_CAST(ptr) ((void *)(ptr))

typedef uint16_t umode_t;
typedef uint64_t ino64_t;
typedef uint64_t dev64_t;
typedef uint32_t uid32_t;
typedef uint32_t gid32_t;
typedef int64_t loff_t;
typedef uint32_t __poll_t;

struct task;
struct task_mm_info;

typedef enum file_type {
    file_none = 0x0001UL,
    file_dir = 0x0002UL,
    file_symlink = 0x0004UL,
    file_block = 0x0008UL,
    file_stream = 0x0010UL,
    file_socket = 0x0020UL,
    file_epoll = 0x0040UL,
    file_fifo = 0x0080UL,
} file_type_t;

typedef struct flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t __pad;
} flock_t;

typedef struct vfs_bsd_lock {
    spinlock_t spin;
    volatile uint64_t l_type;
    volatile uintptr_t owner;
} vfs_bsd_lock_t;

typedef struct vfs_file_lock {
    struct llist_header node;
    uint64_t start;
    uint64_t end;
    uintptr_t owner;
    int32_t pid;
    int16_t type;
    bool ofd;
} vfs_file_lock_t;

typedef struct vfs_file_lock_waiter {
    struct llist_header node;
    uintptr_t owner;
    uintptr_t blocking_owner;
    struct task *task;
    bool ofd;
} vfs_file_lock_waiter_t;

struct vfs_qstr {
    const char *name;
    uint32_t len;
    uint32_t hash;
};

typedef struct vfs_ref {
    volatile int refs;
} vfs_ref_t;

typedef struct vfs_lockref {
    spinlock_t lock;
    volatile int count;
} vfs_lockref_t;

struct vfs_timespec64 {
    int64_t sec;
    uint32_t nsec;
    uint32_t pad;
};

struct vfs_kstat {
    uint64_t mask;
    uint64_t attributes;
    uint64_t attributes_mask;
    ino64_t ino;
    dev64_t dev;
    dev64_t rdev;
    umode_t mode;
    uid32_t uid;
    gid32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t blocks;
    uint32_t blksize;
    struct vfs_timespec64 atime;
    struct vfs_timespec64 btime;
    struct vfs_timespec64 ctime;
    struct vfs_timespec64 mtime;
    uint64_t mnt_id;
};

struct vfs_open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

struct vfs_address_space;
struct vfs_dentry;
struct vfs_file;
struct vfs_fs_context;
struct vfs_inode;
struct vfs_mount;
struct vfs_nameidata;
struct vfs_path;
struct vfs_poll_table;
struct vfs_super_block;
struct vfs_file_system_type;

enum vfs_lookup_flags {
    LOOKUP_FOLLOW = 1U << 0,
    LOOKUP_DIRECTORY = 1U << 1,
    LOOKUP_AUTOMOUNT = 1U << 2,
    LOOKUP_PARENT = 1U << 3,
    LOOKUP_CREATE = 1U << 4,
    LOOKUP_EXCL = 1U << 5,
    LOOKUP_RENAME_TARGET = 1U << 6,
    LOOKUP_NOFOLLOW = 1U << 7,
    LOOKUP_NO_SYMLINKS = 1U << 8,
    LOOKUP_BENEATH = 1U << 9,
    LOOKUP_IN_ROOT = 1U << 10,
    LOOKUP_EMPTY = 1U << 11,
    LOOKUP_RCU = 1U << 12,
    LOOKUP_CACHED = 1U << 13,
    LOOKUP_NO_LAST_MOUNT = 1U << 14,
    LOOKUP_NO_XDEV = 1U << 15,
};

enum vfs_resolve_flags {
    RESOLVE_NO_XDEV = 0x01,
    RESOLVE_NO_MAGICLINKS = 0x02,
    RESOLVE_NO_SYMLINKS = 0x04,
    RESOLVE_BENEATH = 0x08,
    RESOLVE_IN_ROOT = 0x10,
    RESOLVE_CACHED = 0x20,
};

enum vfs_dentry_flags {
    VFS_DENTRY_ROOT = 1UL << 0,
    VFS_DENTRY_HASHED = 1UL << 1,
    VFS_DENTRY_MOUNTPOINT = 1UL << 2,
    VFS_DENTRY_NEGATIVE = 1UL << 3,
    VFS_DENTRY_OP_REVALIDATE = 1UL << 4,
    VFS_DENTRY_DISCONNECTED = 1UL << 5,
};

enum vfs_inode_state {
    VFS_I_NEW = 1UL << 0,
    VFS_I_DIRTY_SYNC = 1UL << 1,
    VFS_I_DIRTY_DATASYNC = 1UL << 2,
    VFS_I_DIRTY_TIME = 1UL << 3,
    VFS_I_FREEING = 1UL << 4,
    VFS_I_WILL_FREE = 1UL << 5,
    VFS_I_LINKABLE = 1UL << 6,
};

enum vfs_mount_flags {
    VFS_MNT_READONLY = 1UL << 0,
    VFS_MNT_NOSUID = 1UL << 1,
    VFS_MNT_NODEV = 1UL << 2,
    VFS_MNT_NOEXEC = 1UL << 3,
    VFS_MNT_NOSYMFOLLOW = 1UL << 4,
    VFS_MNT_LOCKED = 1UL << 5,
};

enum vfs_mount_propagation {
    VFS_MNT_PROP_PRIVATE = 0,
    VFS_MNT_PROP_SHARED,
    VFS_MNT_PROP_SLAVE,
    VFS_MNT_PROP_UNBINDABLE,
};

enum vfs_filesystem_flags {
    VFS_FS_REQUIRES_DEV = 1UL << 0,
    VFS_FS_BINARY_MOUNTDATA = 1UL << 1,
    VFS_FS_USERNS_MOUNT = 1UL << 2,
    VFS_FS_HAS_SUBTYPE = 1UL << 3,
    VFS_FS_VIRTUAL = 1UL << 4,
};

enum vfs_rename_flags {
    VFS_RENAME_NOREPLACE = 1U << 0,
    VFS_RENAME_EXCHANGE = 1U << 1,
    VFS_RENAME_WHITEOUT = 1U << 2,
};

struct vfs_path {
    struct vfs_mount *mnt;
    struct vfs_dentry *dentry;
};

struct vfs_dir_context {
    loff_t pos;
    int (*actor)(struct vfs_dir_context *ctx, const char *name, int namelen,
                 loff_t pos, uint64_t ino, unsigned type);
    void *private;
};

struct vfs_poll_table {
    void (*queue_proc)(struct vfs_file *file, struct vfs_poll_table *pt);
    void *private_data;
    uint32_t events;
};

typedef struct vfs_poll_wait_entry {
    struct llist_header node;
    struct vfs_inode *inode;
    uint64_t seq;
    wait_queue_entry_t wait;
} vfs_poll_wait_entry_t;

typedef struct vfs_poll_wait_table {
    struct vfs_poll_table pt;
    task_t *task;
    struct llist_header entries;
    int error;
} vfs_poll_wait_table_t;

/**
 * Page-cache style operations for filesystems that back inodes with pageable
 * storage.
 */
struct vfs_address_space_operations {
    int (*readpage)(struct vfs_file *file, struct vfs_address_space *mapping,
                    uint64_t index, void *page);
    int (*writepage)(struct vfs_file *file, struct vfs_address_space *mapping,
                     uint64_t index, const void *page);
    void (*invalidatepage)(struct vfs_address_space *mapping, uint64_t index);
};

/**
 * Per-inode address-space state used by the VFS cache and mmap paths.
 */
struct vfs_address_space {
    struct vfs_inode *host;
    const struct vfs_address_space_operations *a_ops;
    spinlock_t lock;
    rb_root_t pages;
    uint64_t cached_pages;
    uint64_t dirty_pages;
    uint64_t mmap_pages;
    uint64_t readahead_window;
    uint64_t readahead_last_index;
};

/**
 * Superblock callbacks supplied by a filesystem implementation.
 */
struct vfs_super_operations {
    struct vfs_inode *(*alloc_inode)(struct vfs_super_block *sb);
    void (*destroy_inode)(struct vfs_inode *inode);
    void (*dirty_inode)(struct vfs_inode *inode, int flags);
    void (*evict_inode)(struct vfs_inode *inode);
    void (*put_super)(struct vfs_super_block *sb);
    int (*sync_fs)(struct vfs_super_block *sb, int wait);
    int (*freeze_fs)(struct vfs_super_block *sb);
    int (*thaw_fs)(struct vfs_super_block *sb);
    int (*statfs)(struct vfs_path *path, void *buf);
    int (*get_quota)(struct vfs_super_block *sb, unsigned int type, uint32_t id,
                     uint64_t *bhardlimit, uint64_t *bsoftlimit,
                     uint32_t *valid);
};

/**
 * Dentry-level callbacks used during lookup, hashing, and teardown.
 */
struct vfs_dentry_operations {
    int (*d_revalidate)(struct vfs_dentry *dentry, unsigned int flags);
    int (*d_hash)(const struct vfs_qstr *name, struct vfs_qstr *out);
    int (*d_compare)(const struct vfs_dentry *dentry, const struct vfs_qstr *a,
                     const struct vfs_qstr *b);
    void (*d_release)(struct vfs_dentry *dentry);
};

struct vfs_rename_ctx {
    struct vfs_inode *old_dir;
    struct vfs_dentry *old_dentry;
    struct vfs_inode *new_dir;
    struct vfs_dentry *new_dentry;
    unsigned int flags;
};

/**
 * Inode callbacks that implement filesystem namespace and metadata behavior.
 */
struct vfs_inode_operations {
    struct vfs_dentry *(*lookup)(struct vfs_inode *dir,
                                 struct vfs_dentry *dentry, unsigned int flags);
    int (*create)(struct vfs_inode *dir, struct vfs_dentry *dentry,
                  umode_t mode, bool excl);
    int (*link)(struct vfs_dentry *old_dentry, struct vfs_inode *dir,
                struct vfs_dentry *new_dentry);
    int (*unlink)(struct vfs_inode *dir, struct vfs_dentry *dentry);
    int (*symlink)(struct vfs_inode *dir, struct vfs_dentry *dentry,
                   const char *target);
    int (*mkdir)(struct vfs_inode *dir, struct vfs_dentry *dentry,
                 umode_t mode);
    int (*rmdir)(struct vfs_inode *dir, struct vfs_dentry *dentry);
    int (*mknod)(struct vfs_inode *dir, struct vfs_dentry *dentry, umode_t mode,
                 dev64_t dev);
    int (*rename)(struct vfs_rename_ctx *ctx);
    const char *(*get_link)(struct vfs_dentry *dentry, struct vfs_inode *inode,
                            struct vfs_nameidata *nd);
    void (*put_link)(struct vfs_inode *inode, const char *link);
    int (*permission)(struct vfs_inode *inode, int mask);
    int (*getattr)(const struct vfs_path *path, struct vfs_kstat *stat,
                   uint32_t request_mask, unsigned int flags);
    int (*setattr)(struct vfs_dentry *dentry, const struct vfs_kstat *stat);
    int (*atomic_open)(struct vfs_inode *dir, struct vfs_dentry *dentry,
                       struct vfs_file *file, unsigned int open_flags,
                       umode_t mode);
    int (*tmpfile)(struct vfs_inode *dir, struct vfs_file *file, umode_t mode);
    ssize_t (*getxattr)(struct vfs_inode *inode, const char *name, void *value,
                        size_t size);
    ssize_t (*listxattr)(struct vfs_inode *inode, char *list, size_t size);
    int (*setxattr)(struct vfs_inode *inode, const char *name,
                    const void *value, size_t size, int flags);
    int (*removexattr)(struct vfs_inode *inode, const char *name);
};

/**
 * File callbacks that implement opened-file operations after lookup/open.
 */
struct vfs_file_operations {
    loff_t (*llseek)(struct vfs_file *file, loff_t offset, int whence);
    ssize_t (*read)(struct vfs_file *file, void *buf, size_t count,
                    loff_t *ppos);
    ssize_t (*write)(struct vfs_file *file, const void *buf, size_t count,
                     loff_t *ppos);
    int (*iterate_shared)(struct vfs_file *file, struct vfs_dir_context *ctx);
    long (*unlocked_ioctl)(struct vfs_file *file, unsigned long cmd,
                           unsigned long arg);
    __poll_t (*poll)(struct vfs_file *file, struct vfs_poll_table *pt);
    void *(*mmap)(struct vfs_file *file, void *addr, size_t offset, size_t size,
                  size_t prot, uint64_t flags);
    int (*open)(struct vfs_inode *inode, struct vfs_file *file);
    int (*flush)(struct vfs_file *file);
    int (*release)(struct vfs_inode *inode, struct vfs_file *file);
    int (*fsync)(struct vfs_file *file, loff_t start, loff_t end, int datasync);
    size_t (*show_fdinfo)(struct vfs_file *file, char *buf, size_t size);
};

/**
 * Mounted superblock state shared by all dentries/inodes on one filesystem
 * instance.
 */
struct vfs_super_block {
    const struct vfs_super_operations *s_op;
    const struct vfs_dentry_operations *s_d_op;
    struct vfs_file_system_type *s_type;
    void *s_fs_info;
    dev64_t s_dev;
    uint64_t s_magic;
    unsigned long s_flags;
    unsigned long s_iflags;
    struct vfs_dentry *s_root;
    spinlock_t s_inode_lock;
    struct llist_header s_inodes;
    spinlock_t s_mount_lock;
    struct llist_header s_mounts;
    vfs_ref_t s_ref;
    volatile uint64_t s_seq;
};

/**
 * VFS inode object. Filesystems embed this in their private inode type and fill
 * i_op/i_fop plus inode metadata before the inode becomes visible.
 */
struct vfs_inode {
    struct vfs_super_block *i_sb;
    const struct vfs_inode_operations *i_op;
    const struct vfs_file_operations *i_fop;
    struct vfs_address_space i_mapping;
    void *i_private;
    ino64_t i_ino;
    umode_t i_mode;
    uid32_t i_uid;
    gid32_t i_gid;
    dev64_t i_rdev;
    ino64_t inode;
    uint32_t i_nlink;
    uint32_t rw_hint;
    uint64_t i_size;
    uint64_t i_blocks;
    uint32_t i_blkbits;
    struct vfs_timespec64 i_atime;
    struct vfs_timespec64 i_btime;
    struct vfs_timespec64 i_ctime;
    struct vfs_timespec64 i_mtime;
    unsigned long i_state;
    uint64_t i_version;
    spinlock_t i_rwsem;
    spinlock_t i_lock;
    vfs_bsd_lock_t flock_lock;
    spinlock_t file_locks_lock;
    struct llist_header file_locks;
    struct llist_header file_lock_waiters;
    volatile int i_write_count;
    volatile int i_exec_count;
    struct llist_header i_dentry_aliases;
    struct llist_header i_sb_list;
    vfs_ref_t i_ref;
    wait_queue_head_t poll_wait;
    uint64_t poll_seq;
};

#define VFS_FMODE_WRITE_ACCESS (1U << 0)
#define VFS_FMODE_EXEC_ACCESS (1U << 1)
#define VFS_FMODE_NO_POS_LOCK (1U << 2)
#define VFS_FMODE_KERNEL_IO (1U << 3)

/**
 * Directory cache entry. Dentries name filesystem objects and may exist without
 * an attached inode for negative cache entries.
 */
struct vfs_dentry {
    struct vfs_qstr d_name;
    struct vfs_dentry *d_parent;
    struct vfs_inode *d_inode;
    struct vfs_super_block *d_sb;
    const struct vfs_dentry_operations *d_op;
    void *d_fsdata;
    unsigned long d_flags;
    uint64_t d_seq;
    struct vfs_mount *d_mounted;
    vfs_lockref_t d_lockref;
    spinlock_t d_lock;
    spinlock_t d_children_lock;
    struct hlist_node d_hash;
    struct llist_header d_child;
    struct llist_header d_subdirs;
    struct llist_header d_alias;
};

/**
 * Mounted view of a superblock, including namespace propagation metadata.
 */
struct vfs_mount {
    struct vfs_mount *mnt_parent;
    struct vfs_mount *mnt_master;
    struct vfs_mount *mnt_stack_prev;
    struct vfs_mount *mnt_stack_next;
    struct vfs_dentry *mnt_mountpoint;
    struct vfs_dentry *mnt_root;
    struct vfs_super_block *mnt_sb;
    unsigned long mnt_flags;
    unsigned int mnt_group_id;
    uint8_t mnt_propagation;
    unsigned int mnt_id;
    unsigned int mnt_clone_source_id;
    unsigned int mnt_propagation_source_id;
    vfs_ref_t mnt_ref;
    spinlock_t mnt_lock;
    struct llist_header mnt_sb_link;
    struct llist_header mnt_child;
    struct llist_header mnt_mounts;
};

/**
 * Open file description tracked by the kernel and referenced by one or more fd
 * table entries.
 */
struct vfs_file {
    const struct vfs_file_operations *f_op;
    struct vfs_path f_path;
    struct vfs_inode *f_inode;
    void *private_data;
    unsigned int f_mode;
    unsigned int f_flags;
    loff_t f_pos;
    wait_mutex_t f_pos_lock;
    spinlock_t f_lock;
    vfs_ref_t f_ref;
    volatile int f_fd_refs;
    spinlock_t epoll_watches_lock;
    struct llist_header epoll_watches;
    struct vfs_inode *node;
};

typedef struct vfs_inode vfs_node_t;
typedef struct vfs_file fd_t;

struct vfs_nameidata {
    struct vfs_path path;
    struct vfs_path root;
    struct vfs_qstr last;
    unsigned int flags;
    unsigned int depth;
    uint64_t rename_seq;
    uint64_t mount_seq;
};

struct vfs_fs_context {
    struct vfs_file_system_type *fs_type;
    unsigned long sb_flags;
    unsigned long mnt_flags;
    const char *source;
    const void *data;
    void *fs_private;
    struct vfs_super_block *sb;
};

/**
 * Filesystem type descriptor registered with the VFS.
 */
struct vfs_file_system_type {
    const char *name;
    unsigned long fs_flags;
    int (*init_fs_context)(struct vfs_fs_context *fc);
    int (*get_tree)(struct vfs_fs_context *fc);
    void (*kill_sb)(struct vfs_super_block *sb);
    struct llist_header fs_list;
};

struct vfs_process_fs {
    struct vfs_path root;
    struct vfs_path pwd;
    spinlock_t lock;
    uint64_t seq;
};

struct vfs_mount_namespace {
    struct vfs_mount *root;
    spinlock_t lock;
    uint64_t seq;
};

extern struct vfs_mount_namespace vfs_init_mnt_ns;
extern struct vfs_path vfs_root_path;

uint64_t vfs_mount_seq_read(void);

static inline void vfs_ref_init(vfs_ref_t *ref, int value) {
    if (!ref)
        return;
    __atomic_store_n(&ref->refs, value, __ATOMIC_RELEASE);
}

static inline int vfs_ref_read(const vfs_ref_t *ref) {
    if (!ref)
        return 0;
    return __atomic_load_n(&ref->refs, __ATOMIC_ACQUIRE);
}

static inline int vfs_ref_get(vfs_ref_t *ref) {
    if (!ref)
        return 0;
    return __atomic_add_fetch(&ref->refs, 1, __ATOMIC_ACQ_REL);
}

static inline bool vfs_ref_try_get(vfs_ref_t *ref) {
    int old;

    if (!ref)
        return false;
    old = __atomic_load_n(&ref->refs, __ATOMIC_ACQUIRE);
    while (old > 0) {
        int new = old + 1;
        if (__atomic_compare_exchange_n(&ref->refs, &old, new, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

static inline bool vfs_ref_put(vfs_ref_t *ref) {
    int old;

    if (!ref)
        return false;
    old = __atomic_load_n(&ref->refs, __ATOMIC_ACQUIRE);
    while (old > 0) {
        int new = old - 1;
        if (__atomic_compare_exchange_n(&ref->refs, &old, new, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return new == 0;
    }
    return false;
}

static inline void vfs_file_fd_ref_get(struct vfs_file *file) {
    if (!file)
        return;
    __atomic_add_fetch(&file->f_fd_refs, 1, __ATOMIC_ACQ_REL);
}

static inline void vfs_file_fd_ref_put(struct vfs_file *file) {
    if (!file)
        return;
    __atomic_sub_fetch(&file->f_fd_refs, 1, __ATOMIC_ACQ_REL);
}

static inline int vfs_file_fd_ref_read(const struct vfs_file *file) {
    if (!file)
        return 0;
    return __atomic_load_n(&file->f_fd_refs, __ATOMIC_ACQUIRE);
}

static inline void vfs_lockref_init(vfs_lockref_t *lockref, int value) {
    if (!lockref)
        return;
    spin_init(&lockref->lock);
    __atomic_store_n(&lockref->count, value, __ATOMIC_RELEASE);
}

static inline int vfs_lockref_get(vfs_lockref_t *lockref) {
    if (!lockref)
        return 0;
    return __atomic_add_fetch(&lockref->count, 1, __ATOMIC_ACQ_REL);
}

static inline bool vfs_lockref_try_get(vfs_lockref_t *lockref) {
    int old;

    if (!lockref)
        return false;
    old = __atomic_load_n(&lockref->count, __ATOMIC_ACQUIRE);
    while (old > 0) {
        int new = old + 1;
        if (__atomic_compare_exchange_n(&lockref->count, &old, new, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

static inline bool vfs_lockref_put(vfs_lockref_t *lockref) {
    int old;

    if (!lockref)
        return false;
    old = __atomic_load_n(&lockref->count, __ATOMIC_ACQUIRE);
    while (old > 0) {
        int new = old - 1;
        if (__atomic_compare_exchange_n(&lockref->count, &old, new, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return new == 0;
    }
    return false;
}

static inline uint64_t fd_get_offset(const fd_t *fd) {
    if (!fd)
        return 0;
    return (uint64_t)__atomic_load_n(&fd->f_pos, __ATOMIC_ACQUIRE);
}

static inline void fd_set_offset(fd_t *fd, uint64_t offset) {
    if (!fd)
        return;
    __atomic_store_n(&fd->f_pos, (loff_t)offset, __ATOMIC_RELEASE);
}

static inline uint64_t fd_add_offset(fd_t *fd, int64_t delta) {
    if (!fd)
        return 0;
    return (uint64_t)__atomic_add_fetch(&fd->f_pos, delta, __ATOMIC_ACQ_REL);
}

static inline uint64_t fd_get_flags(const fd_t *fd) {
    if (!fd)
        return 0;
    return __atomic_load_n(&fd->f_flags, __ATOMIC_ACQUIRE);
}

static inline void fd_set_flags(fd_t *fd, uint64_t flags) {
    if (!fd)
        return;
    __atomic_store_n(&fd->f_flags, (unsigned int)flags, __ATOMIC_RELEASE);
}

void vfs_qstr_make(struct vfs_qstr *qstr, const char *name);
void vfs_qstr_dup(struct vfs_qstr *qstr, const char *name);
void vfs_qstr_destroy(struct vfs_qstr *qstr);

int vfs_init(void);

/**
 * Register a filesystem type so it can be mounted by name.
 */
int vfs_register_filesystem(struct vfs_file_system_type *fs);
/**
 * Remove a filesystem type from the global registry.
 */
void vfs_unregister_filesystem(struct vfs_file_system_type *fs);
/**
 * Look up a registered filesystem type by name.
 */
struct vfs_file_system_type *vfs_get_fs_type(const char *name);

/**
 * Allocate and initialize a new superblock instance for a filesystem type.
 */
struct vfs_super_block *vfs_alloc_super(struct vfs_file_system_type *type,
                                        unsigned long sb_flags);
bool vfs_get_super(struct vfs_super_block *sb);
void vfs_put_super(struct vfs_super_block *sb);

/**
 * Allocate a filesystem-specific inode through sb->s_op->alloc_inode when
 * available, otherwise allocate a plain VFS inode.
 */
struct vfs_inode *vfs_alloc_inode(struct vfs_super_block *sb);
struct vfs_inode *vfs_igrab(struct vfs_inode *inode);
void vfs_iput(struct vfs_inode *inode);
int vfs_inode_get_write_access(struct vfs_inode *inode);
void vfs_inode_put_write_access(struct vfs_inode *inode);
int vfs_file_get_exec_access(struct vfs_file *file);
void vfs_inode_init_owner(struct vfs_inode *inode, struct vfs_inode *dir,
                          umode_t mode);
uid32_t vfs_current_fsuid(void);
gid32_t vfs_current_fsgid(void);
void vfs_init_new_inode_owner(struct vfs_inode *dir, umode_t *mode,
                              uid32_t *uid, gid32_t *gid);
int vfs_inode_permission(struct vfs_inode *inode, int mask);

/**
 * Allocate a dentry under the given parent and copy the supplied name into it.
 */
struct vfs_dentry *vfs_d_alloc(struct vfs_super_block *sb,
                               struct vfs_dentry *parent,
                               const struct vfs_qstr *name);
struct vfs_dentry *vfs_dget(struct vfs_dentry *dentry);
void vfs_dput(struct vfs_dentry *dentry);
void vfs_d_add(struct vfs_dentry *parent, struct vfs_dentry *dentry);
void vfs_d_instantiate(struct vfs_dentry *dentry, struct vfs_inode *inode);
struct vfs_dentry *vfs_d_lookup(struct vfs_dentry *parent,
                                const struct vfs_qstr *name);

/**
 * Allocate a mount object for an already-created superblock.
 */
struct vfs_mount *vfs_mount_alloc(struct vfs_super_block *sb,
                                  unsigned long mnt_flags);
struct vfs_mount *vfs_mntget(struct vfs_mount *mnt);
void vfs_mntput(struct vfs_mount *mnt);
int vfs_mount_attach(struct vfs_mount *parent, struct vfs_dentry *mountpoint,
                     struct vfs_mount *child);
void vfs_mount_detach(struct vfs_mount *mnt);
struct vfs_mount *vfs_create_bind_mount(const struct vfs_path *from,
                                        bool recursive);
int vfs_create_bind_mount_err(const struct vfs_path *from, bool recursive,
                              struct vfs_mount **out);
struct vfs_mount *vfs_clone_mount_tree(struct vfs_mount *root);
struct vfs_mount *vfs_clone_visible_mount_tree(const struct vfs_path *from);
void vfs_put_mount_tree(struct vfs_mount *root);
struct vfs_mount *vfs_path_mount(const struct vfs_path *path);
struct vfs_dentry *
vfs_translate_dentry_between_mounts(const struct vfs_dentry *src_root,
                                    const struct vfs_dentry *src_dentry,
                                    struct vfs_mount *dst_mnt);
struct vfs_mount *vfs_translate_mount_between_roots(struct vfs_mount *old_root,
                                                    struct vfs_mount *new_root,
                                                    struct vfs_mount *old_mnt);
int vfs_translate_path_between_roots(const struct vfs_path *old_root,
                                     const struct vfs_path *old_path,
                                     struct vfs_mount *new_root,
                                     struct vfs_path *new_path);
int vfs_pivot_root_mounts(struct vfs_mount *old_root,
                          struct vfs_mount *new_root,
                          const struct vfs_path *put_old);
int vfs_reconfigure_mount(struct vfs_mount *mnt, const struct vfs_path *to_path,
                          bool detached);
int vfs_mount_set_propagation(struct vfs_mount *mnt, unsigned long flags,
                              bool recursive);
bool vfs_mount_is_shared(const struct vfs_mount *mnt);
unsigned int vfs_mount_peer_group_id(const struct vfs_mount *mnt);
unsigned int vfs_mount_master_group_id(const struct vfs_mount *mnt);

bool vfs_path_get(struct vfs_path *path);
void vfs_path_put(struct vfs_path *path);
void vfs_path_init(struct vfs_path *path);
bool vfs_path_set(struct vfs_path *path, struct vfs_mount *mnt,
                  struct vfs_dentry *dentry);
bool vfs_path_copy(struct vfs_path *dst, const struct vfs_path *src);
bool vfs_path_update(struct vfs_path *dst, const struct vfs_path *src);
void vfs_path_move(struct vfs_path *dst, struct vfs_path *src);
bool vfs_path_equal(const struct vfs_path *a, const struct vfs_path *b);

/**
 * Allocate an open file description for the resolved path.
 * Notes: this creates a new open file description, not just a borrowed alias
 * to an existing fd slot. Callers own one reference on success and need to
 * release it with vfs_file_put() or pass ownership into a higher-level helper.
 */
struct vfs_file *vfs_alloc_file(const struct vfs_path *path,
                                unsigned int open_flags);
/**
 * Acquire an extra reference to an open file description.
 * Notes: this is the normal helper when the same struct vfs_file must survive
 * across another layer's lifetime boundary. Do not use it as a substitute for
 * fd-table lookups; it only manages object lifetime.
 */
struct vfs_file *vfs_file_get(struct vfs_file *file);
/**
 * Drop one reference to an open file description and run final cleanup on the
 * last put.
 * Notes: the final put can trigger release callbacks and inode/path teardown,
 * so callers must not touch `file` again after handing it to vfs_file_put().
 */
void vfs_file_put(struct vfs_file *file);
int mountfd_get_path(struct vfs_file *file, struct vfs_path *path);
void mountfd_mark_attached(struct vfs_file *file);

void vfs_fill_generic_kstat(const struct vfs_path *path,
                            struct vfs_kstat *stat);
char *vfs_path_to_string(const struct vfs_path *path,
                         const struct vfs_path *root);
bool vfs_path_is_ancestor(const struct vfs_path *ancestor,
                          const struct vfs_path *path);

/**
 * Resolve a pathname into a referenced VFS path using Linux-style lookup
 * flags.
 * Notes: successful lookup returns a referenced path that the caller must drop
 * with vfs_path_put(). This helper resolves names only; it does not open the
 * object or pin a file description.
 */
int vfs_filename_lookup(int dfd, const char *name, unsigned int lookup_flags,
                        struct vfs_path *path);
/**
 * Resolve the parent path plus final pathname component.
 * Notes: this is the helper for create/unlink/rename-style operations where
 * callers need both the parent directory and the final qstr. The returned
 * parent path is referenced and must be released by the caller.
 */
int vfs_path_parent_lookup(int dfd, const char *name, unsigned int lookup_flags,
                           struct vfs_path *parent, struct vfs_qstr *last,
                           unsigned int *type);
int vfs_open_from(struct vfs_path *start, const char *name,
                  const struct vfs_open_how *how, struct vfs_file **out,
                  bool kernel);

/**
 * Open a path relative to dfd and return a referenced open file description.
 * Notes: ownership of the returned file object goes to the caller on success.
 * `kernel` changes pathname-buffer assumptions; it is not a generic
 * permission-bypass flag and should not be used casually by internal callers.
 */
int vfs_openat(int dfd, const char *name, const struct vfs_open_how *how,
               struct vfs_file **out, bool kernel);
int vfs_close_file(struct vfs_file *file);
int vfs_close_file_for_task(struct vfs_file *file, struct task *task);
ssize_t vfs_read_file(struct vfs_file *file, void *buf, size_t count,
                      loff_t *ppos);
ssize_t vfs_write_file(struct vfs_file *file, const void *buf, size_t count,
                       loff_t *ppos);
ssize_t vfs_read_kernel_file(struct vfs_file *file, void *buf, size_t count,
                             loff_t *ppos);
ssize_t vfs_write_kernel_file(struct vfs_file *file, const void *buf,
                              size_t count, loff_t *ppos);
void vfs_file_pos_lock(struct vfs_file *file);
void vfs_file_pos_unlock(struct vfs_file *file);
loff_t vfs_llseek_file(struct vfs_file *file, loff_t offset, int whence);
int vfs_iterate_dir(struct vfs_file *file, struct vfs_dir_context *ctx);
long vfs_ioctl_file(struct vfs_file *file, unsigned long cmd,
                    unsigned long arg);
int vfs_fsync_file(struct vfs_file *file);
int vfs_fsync_file_range(struct vfs_file *file, loff_t start, loff_t end,
                         int datasync);
int vfs_truncate_path(const struct vfs_path *path, uint64_t size);
int vfs_poll(struct vfs_file *file, uint32_t events);
int vfs_poll_with_table(struct vfs_file *file, uint32_t events,
                        struct vfs_poll_table *pt);
void vfs_poll_wait(struct vfs_file *file, struct vfs_poll_table *pt);
void vfs_poll_wait_table_init(vfs_poll_wait_table_t *table, task_t *task);
void vfs_poll_wait_table_cleanup(vfs_poll_wait_table_t *table);
int vfs_poll_wait_table_error(vfs_poll_wait_table_t *table);
bool vfs_poll_wait_table_seq_changed(vfs_poll_wait_table_t *table);
void vfs_poll_notify_inode(struct vfs_inode *inode, uint32_t events);
void vfs_poll_notify_file(struct vfs_file *file, uint32_t events);
int vfs_poll_wait_interruptible(struct vfs_file *file, uint32_t events);

int vfs_mkdirat(int dfd, const char *pathname, umode_t mode, bool kernel);
int vfs_mknodat(int dfd, const char *pathname, umode_t mode, dev64_t dev,
                bool kernel);
int vfs_unlinkat(int dfd, const char *pathname, int flags, bool kernel);
int vfs_linkat(int olddfd, const char *oldname, int newdfd, const char *newname,
               int flags, bool kernel);
int vfs_symlinkat(const char *target, int newdfd, const char *newname,
                  bool kernel);
int vfs_renameat2(int olddfd, const char *oldname, int newdfd,
                  const char *newname, unsigned int flags, bool kernel);
int vfs_statx(int dfd, const char *pathname, int flags, uint32_t mask,
              struct vfs_kstat *stat);
ssize_t vfs_getxattr(struct vfs_inode *inode, const char *name, void *value,
                     size_t size);
ssize_t vfs_listxattr(struct vfs_inode *inode, char *list, size_t size);
int vfs_setxattr(struct vfs_inode *inode, const char *name, const void *value,
                 size_t size, int flags);
int vfs_removexattr(struct vfs_inode *inode, const char *name);

/**
 * Internal mount helper used by kernel subsystems to mount a filesystem by
 * name.
 * Notes: this returns a mount object, not a path in the caller's namespace.
 * It is the right helper for internal pseudo-filesystems and bootstrap mounts,
 * but namespace-visible attachment still needs a separate mount-tree step.
 */
int vfs_kern_mount(const char *fs_name, unsigned long mnt_flags,
                   const char *source, void *data, struct vfs_mount **out);
int vfs_do_mount(int dfd, const char *pathname, const char *fs_name,
                 unsigned long mnt_flags, const char *source, void *data);
int vfs_do_bind_mount(int from_dfd, const char *from_pathname, int to_dfd,
                      const char *to_pathname, bool recursive);
int vfs_do_remount(int dfd, const char *pathname, unsigned long mnt_flags);
int vfs_do_move_mount(int from_dfd, const char *from_pathname, int to_dfd,
                      const char *to_pathname);
int vfs_do_umount(int dfd, const char *pathname, int flags);
int vfs_mount_collect_missing_direct_children(struct vfs_mount *src_parent,
                                              struct vfs_mount ***out,
                                              size_t *out_count);

/**
 * Syscall-facing helpers that translate fd numbers into VFS file operations.
 * Notes: these are convenience shims for syscall glue. Internal kernel code
 * that already holds a struct vfs_file should usually call the file-oriented
 * helpers above instead of round-tripping through integer fd semantics.
 */
int vfs_sys_openat(int dfd, const char *pathname,
                   const struct vfs_open_how *how);
int vfs_sys_close(int fd);
ssize_t vfs_sys_read(int fd, void *buf, size_t count);
ssize_t vfs_sys_pread64(int fd, void *buf, size_t count, loff_t pos);
ssize_t vfs_sys_write(int fd, const void *buf, size_t count);
ssize_t vfs_sys_pwrite64(int fd, const void *buf, size_t count, loff_t pos);
