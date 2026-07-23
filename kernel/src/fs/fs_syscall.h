#pragma once

#include <libs/klibc.h>
#include <libs/termios.h>
#include <fs/vfs/vfs.h>
#include <fs/vfs/fcntl.h>
#include <task/task.h>

char *at_resolve_pathname(int dirfd, char *pathname);
char *at_resolve_pathname_fullpath(int dirfd, char *pathname);
int fsfd_mount_get_path(struct vfs_file *file, struct vfs_path *path);

struct iovec {
    uint8_t *iov_base;
    uint64_t len;
};

/*
 * These are the fs-independent mount-flags: up to 32 flags are supported
 *
 * Usage of these is restricted within the kernel to core mount(2) code and
 * callers of sys_mount() only.  Filesystems should be using the SB_*
 * equivalent instead.
 */
#define MS_RDONLY 1        /* Mount read-only */
#define MS_NOSUID 2        /* Ignore suid and sgid bits */
#define MS_NODEV 4         /* Disallow access to device special files */
#define MS_NOEXEC 8        /* Disallow program execution */
#define MS_SYNCHRONOUS 16  /* Writes are synced at once */
#define MS_REMOUNT 32      /* Alter flags of a mounted FS */
#define MS_MANDLOCK 64     /* Allow mandatory locks on an FS */
#define MS_DIRSYNC 128     /* Directory modifications are synchronous */
#define MS_NOSYMFOLLOW 256 /* Do not follow symlinks */
#define MS_NOATIME 1024    /* Do not update access times. */
#define MS_NODIRATIME 2048 /* Do not update directory access times */
#define MS_BIND 4096
#define MS_MOVE 8192
#define MS_REC 16384
#define MS_VERBOSE                                                             \
    32768 /* War is peace. Verbosity is silence.                               \
             MS_VERBOSE is deprecated. */
#define MS_SILENT 32768
#define MS_POSIXACL (1 << 16)    /* VFS does not apply the umask */
#define MS_UNBINDABLE (1 << 17)  /* change to unbindable */
#define MS_PRIVATE (1 << 18)     /* change to private */
#define MS_SLAVE (1 << 19)       /* change to slave */
#define MS_SHARED (1 << 20)      /* change to shared */
#define MS_RELATIME (1 << 21)    /* Update atime relative to mtime/ctime. */
#define MS_KERNMOUNT (1 << 22)   /* this is a kern_mount call */
#define MS_I_VERSION (1 << 23)   /* Update inode I_version field */
#define MS_STRICTATIME (1 << 24) /* Always perform atime updates */
#define MS_LAZYTIME (1 << 25)    /* Update the on-disk [acm]times lazily */

/* These sb flags are internal to the kernel */
#define MS_SUBMOUNT (1 << 26)
#define MS_NOREMOTELOCK (1 << 27)
#define MS_NOSEC (1 << 28)
#define MS_BORN (1 << 29)
#define MS_ACTIVE (1 << 30)
#define MS_NOUSER (1 << 31)

#define CLOSE_RANGE_UNSHARE (1U << 1)
#define CLOSE_RANGE_CLOEXEC (1U << 2)

#define PIDFD_NONBLOCK O_NONBLOCK

#ifndef USRQUOTA
#define USRQUOTA 0
#endif

#ifndef SUBCMDMASK
#define SUBCMDMASK 0x00ff
#endif

#ifndef SUBCMDSHIFT
#define SUBCMDSHIFT 8
#endif

#ifndef QCMD
#define QCMD(cmd, type) (((cmd) << SUBCMDSHIFT) | ((type) & SUBCMDMASK))
#endif

#ifndef Q_GETQUOTA
#define Q_GETQUOTA 0x800007
#endif

#ifndef QIF_BLIMITS
#define QIF_BLIMITS (1U << 0)
#endif

struct dqblk {
    uint64_t dqb_bhardlimit;
    uint64_t dqb_bsoftlimit;
    uint64_t dqb_curspace;
    uint64_t dqb_ihardlimit;
    uint64_t dqb_isoftlimit;
    uint64_t dqb_curinodes;
    uint64_t dqb_btime;
    uint64_t dqb_itime;
    uint32_t dqb_valid;
};

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_SETOWN 8
#define F_GETOWN 9
#define F_SETSIG 10
#define F_GETSIG 11

#define F_DUPFD_CLOEXEC 1030
#define F_LINUX_SPECIFIC_BASE 1024

#define F_SETLEASE (F_LINUX_SPECIFIC_BASE + 0)
#define F_GETLEASE (F_LINUX_SPECIFIC_BASE + 1)

#define F_DUPFD_QUERY (F_LINUX_SPECIFIC_BASE + 3)

#define F_SETPIPE_SZ (F_LINUX_SPECIFIC_BASE + 7)
#define F_GETPIPE_SZ (F_LINUX_SPECIFIC_BASE + 8)

#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)

#define F_GET_RW_HINT (F_LINUX_SPECIFIC_BASE + 11)
#define F_SET_RW_HINT (F_LINUX_SPECIFIC_BASE + 12)
#define F_GET_FILE_RW_HINT (F_LINUX_SPECIFIC_BASE + 13)
#define F_SET_FILE_RW_HINT (F_LINUX_SPECIFIC_BASE + 14)

#define F_SEAL_SEAL 0x0001   /* 防止后续seal操作 */
#define F_SEAL_SHRINK 0x0002 /* 禁止缩小文件 */
#define F_SEAL_GROW 0x0004   /* 禁止增大文件 */
#define F_SEAL_WRITE 0x0008  /* 禁止写操作 */

#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38

struct statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

#define STATX_TYPE 0x00000001U
#define STATX_MODE 0x00000002U
#define STATX_NLINK 0x00000004U
#define STATX_UID 0x00000008U
#define STATX_GID 0x00000010U
#define STATX_ATIME 0x00000020U
#define STATX_MTIME 0x00000040U
#define STATX_CTIME 0x00000080U
#define STATX_INO 0x00000100U
#define STATX_SIZE 0x00000200U
#define STATX_BLOCKS 0x00000400U
#define STATX_BASIC_STATS 0x000007ffU
#define STATX_BTIME 0x00000800U
#define STATX_MNT_ID 0x00001000U
#define STATX_DIOALIGN 0x00002000U
#define STATX_MNT_ID_UNIQUE 0x00004000U
#define STATX_SUBVOL 0x00008000U
#define STATX_WRITE_ATOMIC 0x00010000U
#define STATX_DIO_READ_ALIGN 0x00020000U

#define STATX_ATTR_COMPRESSED 0x00000004
#define STATX_ATTR_IMMUTABLE 0x00000010
#define STATX_ATTR_APPEND 0x00000020
#define STATX_ATTR_NODUMP 0x00000040
#define STATX_ATTR_ENCRYPTED 0x00000800
#define STATX_ATTR_AUTOMOUNT 0x00001000
#define STATX_ATTR_MOUNT_ROOT 0x00002000
#define STATX_ATTR_VERITY 0x00100000
#define STATX_ATTR_DAX 0x00200000
#define STATX_ATTR_WRITE_ATOMIC 0x00400000

struct statx {
    /* 0x00 */
    uint32_t stx_mask;       /* What results were written [uncond] */
    uint32_t stx_blksize;    /* Preferred general I/O size [uncond] */
    uint64_t stx_attributes; /* Flags conveying information about the file
                                [uncond] */
    /* 0x10 */
    uint32_t stx_nlink; /* Number of hard links */
    uint32_t stx_uid;   /* User ID of owner */
    uint32_t stx_gid;   /* Group ID of owner */
    uint16_t stx_mode;  /* File mode */
    uint16_t __spare0[1];
    /* 0x20 */
    uint64_t stx_ino;             /* Inode number */
    uint64_t stx_size;            /* File size */
    uint64_t stx_blocks;          /* Number of 512-byte blocks allocated */
    uint64_t stx_attributes_mask; /* Mask to show what's supported in
                                     stx_attributes */
    /* 0x40 */
    struct statx_timestamp stx_atime; /* Last access time */
    struct statx_timestamp stx_btime; /* File creation time */
    struct statx_timestamp stx_ctime; /* Last attribute change time */
    struct statx_timestamp stx_mtime; /* Last data modification time */
    /* 0x80 */
    uint32_t stx_rdev_major; /* Device ID of special file [if bdev/cdev] */
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major; /* ID of device containing file [uncond] */
    uint32_t stx_dev_minor;
    /* 0x90 */
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;    /* Memory buffer alignment for direct I/O */
    uint32_t stx_dio_offset_align; /* File offset alignment for direct I/O */
    /* 0xa0 */
    uint64_t __spare3[12]; /* Spare space for future expansion */
                           /* 0x100 */
};

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9
#define RLIMIT_LOCKS 10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE 13
#define RLIMIT_RTPRIO 14
#define RLIMIT_RTTIME 15
#define RLIMIT_NLIMITS 16

#define FD_SETSIZE 1024

typedef unsigned long fd_mask;

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / 8 / sizeof(long)];
} fd_set;

typedef struct {
    sigset_t *ss;
    size_t ss_len;
} weird_pselect6_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
}
#ifdef __x86_64__
__attribute__((__packed__))
#endif
;

typedef struct epoll_watch {
    struct llist_header node;
    struct llist_header file_node;
    struct llist_header ready_node;
    struct vfs_file *file;
    struct epoll *owner;
    wait_queue_entry_t wait;
    bool wait_armed;
    uint32_t events;
    uint32_t last_ready_events;
    uint64_t collect_seq;
    uint64_t data;
    bool edge_triggered;
    bool one_shot;
    bool disabled;
    bool ready_queued;
} epoll_watch_t;

typedef struct epoll {
    struct llist_header watches;
    struct llist_header ready_watches;
    spinlock_t lock;
    spinlock_t ready_lock;
    uint64_t collect_seq;
    struct vfs_inode *inode;
} epoll_t;

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

struct timerfd_timespec {
    long long tv_sec;
    long tv_nsec;
};

struct itimerspec {
    struct timerfd_timespec it_interval;
    struct timerfd_timespec it_value;
};

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME (1 << 0)
#endif

#define EFD_CLOEXEC 02000000
#define EFD_NONBLOCK 04000
#define EFD_SEMAPHORE 00000001

#define TFD_CLOEXEC O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK

typedef struct eventfd {
    vfs_node_t *node;
    spinlock_t lock;
    uint64_t count;
    int flags;
} eventfd_t;

struct vfs_file;
eventfd_t *eventfd_file_handle(struct vfs_file *file);
int eventfd_is_file(struct vfs_file *file);
int eventfd_create_file(struct vfs_file **out_file, uint64_t initial_val,
                        unsigned int flags, eventfd_t **out_efd);

#define SIGNALFD_IOC_MASK 0x53010008

typedef struct {
    kernel_timer_t timer;
    uint64_t count;
    int flags;
    vfs_node_t *node;
    spinlock_t lock;
    rb_node_t timeout_node;
    bool timeout_queued;
} timerfd_t;

#define TFD_TIMER_ABSTIME (1 << 0)
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)

typedef struct {
    int val[2];
} __kernel_fsid_t;

struct statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    __kernel_fsid_t f_fsid;
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
};

enum fsconfig_command {
    FSCONFIG_SET_FLAG = 0, /* Set parameter, supplying no value */
#define FSCONFIG_SET_FLAG FSCONFIG_SET_FLAG
    FSCONFIG_SET_STRING = 1, /* Set parameter, supplying a string value */
#define FSCONFIG_SET_STRING FSCONFIG_SET_STRING
    FSCONFIG_SET_BINARY = 2, /* Set parameter, supplying a binary blob value */
#define FSCONFIG_SET_BINARY FSCONFIG_SET_BINARY
    FSCONFIG_SET_PATH = 3, /* Set parameter, supplying an object by path */
#define FSCONFIG_SET_PATH FSCONFIG_SET_PATH
    FSCONFIG_SET_PATH_EMPTY =
        4, /* Set parameter, supplying an object by (empty) path */
#define FSCONFIG_SET_PATH_EMPTY FSCONFIG_SET_PATH_EMPTY
    FSCONFIG_SET_FD = 5, /* Set parameter, supplying an object by fd */
#define FSCONFIG_SET_FD FSCONFIG_SET_FD
    FSCONFIG_CMD_CREATE = 6, /* Invoke superblock creation */
#define FSCONFIG_CMD_CREATE FSCONFIG_CMD_CREATE
    FSCONFIG_CMD_RECONFIGURE = 7, /* Invoke superblock reconfiguration */
#define FSCONFIG_CMD_RECONFIGURE FSCONFIG_CMD_RECONFIGURE
    FSCONFIG_CMD_CREATE_EXCL =
        8, /* Create new superblock, fail if reusing existing superblock */
#define FSCONFIG_CMD_CREATE_EXCL FSCONFIG_CMD_CREATE_EXCL
};

/* fsopen/fspick flags */
#define FSOPEN_CLOEXEC 0x00000001
#define FSPICK_CLOEXEC 0x00000001
#define FSPICK_SYMLINK_NOFOLLOW 0x00000002
#define FSPICK_NO_AUTOMOUNT 0x00000004
#define FSPICK_EMPTY_PATH 0x00000008

/* FSMOUNT flags */
#define FSMOUNT_CLOEXEC 0x00000001

/* open_tree flags */
#define OPEN_TREE_CLONE 0x00000001
#define OPEN_TREE_CLOEXEC O_CLOEXEC

/* Mount attributes for fsmount */
#define MOUNT_ATTR_RDONLY 0x00000001
#define MOUNT_ATTR_NOSUID 0x00000002
#define MOUNT_ATTR_NODEV 0x00000004
#define MOUNT_ATTR_NOEXEC 0x00000008
#define MOUNT_ATTR_RELATIME 0x00000000
#define MOUNT_ATTR_NOATIME 0x00000010
#define MOUNT_ATTR_STRICTATIME 0x00000020
#define MOUNT_ATTR_NODIRATIME 0x00000080
#define MOUNT_ATTR__ATIME 0x00000070
#define MOUNT_ATTR_IDMAP 0x00100000
#define MOUNT_ATTR_NOSYMFOLLOW 0x00200000
#define MOUNT_ATTR_SIZE_VER0 32

typedef struct mount_attr {
    uint64_t attr_set;
    uint64_t attr_clr;
    uint64_t propagation;
    uint64_t userns_fd;
} mount_attr_t;

/* move_mount flags */
#define MOVE_MOUNT_F_SYMLINKS 0x00000001
#define MOVE_MOUNT_F_AUTOMOUNTS 0x00000002
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#define MOVE_MOUNT_T_SYMLINKS 0x00000010
#define MOVE_MOUNT_T_AUTOMOUNTS 0x00000020
#define MOVE_MOUNT_T_EMPTY_PATH 0x00000040
#define MOVE_MOUNT_SET_GROUP 0x00000100
#define MOVE_MOUNT_BENEATH 0x00000200

struct sysinfo {
    int64_t uptime;     /* Seconds since boot */
    uint64_t loads[3];  /* 1, 5, and 15 minute load averages */
    uint64_t totalram;  /* Total usable main memory size */
    uint64_t freeram;   /* Available memory size */
    uint64_t sharedram; /* Amount of shared memory */
    uint64_t bufferram; /* Memory used by buffers */
    uint64_t totalswap; /* Total swap space size */
    uint64_t freeswap;  /* swap space still available */
    uint16_t procs;     /* Number of current processes */
    uint16_t pad;       /* Explicit padding for m68k */
    uint64_t totalhigh; /* Total high memory size */
    uint64_t freehigh;  /* Available high memory size */
    uint32_t mem_unit;  /* Memory unit size in bytes */
    char _f[20 - 2 * sizeof(uint64_t) -
            sizeof(uint32_t)]; /* Padding: libc5 uses this.. */
};

/**
 * Linux contract: mount a filesystem or perform bind/remount operations.
 * Current kernel: routes to the local VFS mount API and supports the flags
 * that are translated by generic.c.
 * Notes: mount() is overloaded on Linux. The same entry point covers fresh
 * mounts, bind mounts, remounts, and propagation-like changes, so callers often
 * leave some parameters unused depending on the flag mix.
 * Gaps: only filesystems and mount flags implemented in the in-kernel VFS are
 * available.
 */
uint64_t sys_mount(char *dev_name, char *dir_name, char *type, uint64_t flags,
                   void *data);
uint64_t sys_mount_setattr(int dfd, const char *path, uint64_t flags,
                           const mount_attr_t *attr, size_t size);
/**
 * Linux contract: unmount a mountpoint with umount2(2) semantics.
 * Current kernel: copies the path from userspace and delegates to
 * vfs_do_umount().
 * Gaps: only the unmount flags understood by the local VFS namespace layer are
 * supported.
 */
uint64_t sys_umount2(const char *target, uint64_t flags);
/**
 * Linux contract: atomically replace the process root mount tree.
 * Current kernel: validates the mount topology and calls
 * vfs_pivot_root_mounts() plus task mount-namespace rebinding.
 * Gaps: behavior is limited to the namespace and propagation features currently
 * implemented by this VFS.
 */
uint64_t sys_pivot_root(const char *new_root, const char *put_old);
/**
 * Linux contract: query or modify quota state for a mounted filesystem.
 * Current kernel: supports only the quota operations implemented by the local
 * superblock quota hook.
 * Gaps: unsupported commands fail according to the backing filesystem support;
 * there is no Linux-complete quota subsystem yet.
 */
uint64_t sys_quotactl(uint32_t cmd, const char *special, uint32_t id,
                      struct dqblk *addr);

/**
 * Linux contract: create or truncate a file with O_CREAT|O_WRONLY|O_TRUNC.
 * Current kernel: translates directly into openat on AT_FDCWD.
 * Notes: creat() is basically a fixed open() recipe kept for ABI reasons.
 * Callers that expect read access afterwards usually need an extra reopen or a
 * different entry point.
 * Gaps: O_TMPFILE is explicitly rejected in the generic open helper.
 */
uint64_t sys_creat(const char *path, uint64_t mode);
/**
 * Linux contract: open a path relative to the caller's cwd.
 * Current kernel: implemented on top of vfs_sys_openat().
 * Gaps: support is bounded by the open flags and resolve behavior implemented
 * by the local VFS.
 */
uint64_t sys_open(const char *name, uint64_t flags, uint64_t mode);
/**
 * Linux contract: open a path relative to dirfd.
 * Current kernel: implemented on top of vfs_sys_openat().
 * Gaps: support is bounded by the open flags and resolve behavior implemented
 * by the local VFS.
 */
uint64_t sys_openat(uint64_t dirfd, const char *name, uint64_t flags,
                    uint64_t mode);
uint64_t sys_getxattr(const char *path, const char *name, void *value,
                      uint64_t size);
uint64_t sys_lgetxattr(const char *path, const char *name, void *value,
                       uint64_t size);
uint64_t sys_fgetxattr(int fd, const char *name, void *value, uint64_t size);
uint64_t sys_setxattr(const char *path, const char *name, const void *value,
                      uint64_t size, int flags);
uint64_t sys_lsetxattr(const char *path, const char *name, const void *value,
                       uint64_t size, int flags);
uint64_t sys_fsetxattr(int fd, const char *name, const void *value,
                       uint64_t size, int flags);
uint64_t sys_listxattr(const char *path, char *list, uint64_t size);
uint64_t sys_llistxattr(const char *path, char *list, uint64_t size);
uint64_t sys_flistxattr(int fd, char *list, uint64_t size);
uint64_t sys_removexattr(const char *path, const char *name);
uint64_t sys_lremovexattr(const char *path, const char *name);
uint64_t sys_fremovexattr(int fd, const char *name);

/**
 * Linux contract: open a path using openat2(2) resolution semantics.
 * Current kernel: consumes the supplied vfs_open_how structure through the
 * local VFS open path.
 * Notes: userspace may pass a larger structure size to probe for newer kernel
 * support, so size validation and unknown-field handling are part of the ABI,
 * not just input hygiene.
 * Gaps: only resolve flags implemented by the in-kernel VFS are honored.
 */
uint64_t sys_openat2(uint64_t dirfd, const char *name,
                     const struct vfs_open_how *how, uint64_t size);
/**
 * Linux contract: convert a path to an exportable file handle.
 * Current kernel: exposes an in-memory handle format understood by
 * sys_open_by_handle_at().
 * Gaps: the handle format is not stable across reboot/remount like Linux export
 * handles.
 */
uint64_t sys_name_to_handle_at(int dfd, const char *name,
                               struct file_handle *handle, int *mnt_id,
                               int flag);
/**
 * Linux contract: reopen an object from a file handle and mount ID.
 * Current kernel: understands only the handle format produced by
 * sys_name_to_handle_at().
 */
uint64_t sys_open_by_handle_at(int mountdirfd, struct file_handle *handle,
                               int flags);
/**
 * Linux contract: create an inotify instance.
 * Current kernel: backs the instance with notifyfs.
 * Gaps: semantics are limited to the watch and event types implemented by the
 * in-kernel notifyfs backend.
 */
uint64_t sys_inotify_init();
/**
 * Linux contract: create an inotify instance with cloexec/nonblock flags.
 * Current kernel: backs the instance with notifyfs.
 * Gaps: only IN_CLOEXEC and IN_NONBLOCK are accepted here.
 */
uint64_t sys_inotify_init1(uint64_t flags);
/**
 * Linux contract: add an inotify watch on a resolved path.
 * Current kernel: resolves the path through the VFS and registers it with
 * notifyfs.
 * Gaps: Linux watch mask coverage depends on what notifyfs currently emits.
 */
uint64_t sys_inotify_add_watch(uint64_t notifyfd, const char *path,
                               uint64_t mask);
/**
 * Linux contract: remove a previously registered inotify watch.
 * Current kernel: delegates to the notifyfs watch table.
 */
uint64_t sys_inotify_rm_watch(uint64_t watchfd, uint64_t wd);

/**
 * Linux contract: flush file data and metadata as required by fsync(2).
 * Current kernel: delegates to vfs_fsync_file().
 * Gaps: persistence semantics depend entirely on the backing filesystem
 * implementation.
 */
uint64_t sys_fsync(uint64_t fd);
uint64_t sys_syncfs(int fd);
uint64_t sys_fdatasync(uint64_t fd);
uint64_t sys_readahead(int fd, uint64_t offset, uint64_t count);
/**
 * Linux contract: close a file descriptor and release the underlying file.
 * Current kernel: removes the fd table entry first, then runs close callbacks
 * and vfs_close_file().
 */
uint64_t sys_close(uint64_t fd);
/**
 * Linux contract: close or mark cloexec on an fd interval.
 * Current kernel: supports CLOSE_RANGE_UNSHARE and CLOSE_RANGE_CLOEXEC.
 * Gaps: flags outside those two Linux bits are rejected.
 */
uint64_t sys_close_range(uint64_t fd, uint64_t maxfd, uint64_t flags);
/**
 * Linux contract: copy bytes between two file descriptors without a userspace
 * bounce buffer.
 * Current kernel: reads into a temporary kernel buffer and writes back out.
 * Gaps: Linux copy offload and full sparse-file behavior are not implemented.
 */
uint64_t sys_copy_file_range(uint64_t fd_in, int *offset_in, uint64_t fd_out,
                             int *offset_out, uint64_t len, uint64_t flags);
/**
 * Linux contract: read from a file descriptor into a user buffer.
 * Current kernel: rejects directory inodes with -EISDIR and otherwise
 * delegates to vfs_read_file().
 * Notes: a zero-length request still goes through the normal file path instead
 * of being short-circuited in the syscall wrapper.
 */
uint64_t sys_read(uint64_t fd, void *buf, uint64_t len);
/**
 * Linux contract: write to a file descriptor from a user buffer.
 * Current kernel: rejects directory inodes with -EISDIR and otherwise
 * delegates to vfs_write_file().
 * Notes: len == 0 is still forwarded to the underlying file object for the
 * same ABI reason as sys_read().
 */
uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len);
/**
 * Linux contract: splice data from an input file into an output file.
 * Current kernel: emulates sendfile(2) with buffered VFS read/write calls.
 */
uint64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, int *offset_ptr,
                      size_t count);
uint64_t sys_splice(uint64_t fd_in, int64_t *off_in, uint64_t fd_out,
                    int64_t *off_out, size_t len, uint64_t flags);
uint64_t sys_vmsplice(uint64_t fd, const struct iovec *iov, uint64_t nr_segs,
                      uint64_t flags);
/**
 * Linux contract: reposition the file offset.
 * Current kernel: supports SEEK_SET/CUR/END and simplified SEEK_DATA/HOLE.
 * Gaps: SEEK_HOLE always returns i_size and SEEK_DATA returns the supplied
 * offset, so sparse-file probing is only a Linux-compatibility approximation.
 */
uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence);
/**
 * Linux contract: issue an ioctl to the file descriptor backend.
 * Current kernel: handles a few generic fd commands locally and forwards the
 * rest to vfs_ioctl_file().
 * Gaps: unsupported backend commands collapse to -ENOTTY.
 */
uint64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg);
/**
 * Linux contract: vectored read using an iovec array.
 * Current kernel: validates all iovecs, then performs sequential VFS reads
 * until a short read or error occurs.
 * Notes: zero-length iovec entries are skipped for copying purposes, but the
 * call may still collapse to a real zero-byte read.
 */
uint64_t sys_readv(uint64_t fd, struct iovec *iovec, uint64_t count);
/**
 * Linux contract: vectored write using an iovec array.
 * Current kernel: validates all iovecs, then performs sequential VFS writes
 * until a short write or error occurs.
 * Notes: as with readv, empty iovecs are tolerated and do not by themselves
 * turn the call into an error.
 */
uint64_t sys_writev(uint64_t fd, struct iovec *iovec, uint64_t count);
/**
 * Linux contract: vectored positioned read without changing the file offset.
 * Raw Linux syscall ABI passes the 64-bit offset split into low/high words.
 */
uint64_t sys_preadv(uint64_t fd, struct iovec *iovec, uint64_t count,
                    uint64_t pos_l, uint64_t pos_h);
/**
 * Linux contract: vectored positioned write without changing the file offset,
 * except Linux O_APPEND/RWF_APPEND semantics still append to EOF.
 */
uint64_t sys_pwritev(uint64_t fd, struct iovec *iovec, uint64_t count,
                     uint64_t pos_l, uint64_t pos_h);
/**
 * Linux contract: preadv with RWF_* flags. Unsupported flag bits fail with
 * -EOPNOTSUPP.
 */
uint64_t sys_preadv2(uint64_t fd, struct iovec *iovec, uint64_t count,
                     uint64_t pos_l, uint64_t pos_h, uint64_t flags);
/**
 * Linux contract: pwritev with RWF_* flags. Unsupported flag bits fail with
 * -EOPNOTSUPP.
 */
uint64_t sys_pwritev2(uint64_t fd, struct iovec *iovec, uint64_t count,
                      uint64_t pos_l, uint64_t pos_h, uint64_t flags);

/**
 * Linux contract: read directory entries in legacy getdents layout.
 */
uint64_t sys_getdents(uint64_t fd, uint64_t buf, uint64_t size);
/**
 * Linux contract: read directory entries in linux_dirent64 layout.
 */
uint64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t size);
/**
 * Linux contract: change the caller's current working directory.
 */
uint64_t sys_chdir(const char *dirname);
/**
 * Linux contract: change the caller's root directory.
 */
uint64_t sys_chroot(const char *dname);
/**
 * Linux contract: stringify the caller's current working directory.
 */
uint64_t sys_getcwd(char *cwd, uint64_t size);

/**
 * Linux contract: duplicate a file descriptor to the lowest available slot.
 * Current kernel: duplicates the open file description and copies only the fd
 * flags explicitly requested by the dup helper.
 */
uint64_t sys_dup(uint64_t fd);
/**
 * Linux contract: duplicate a file descriptor to an exact target number.
 * Current kernel: closes any existing target fd before installing the new
 * reference, like dup2(2).
 */
uint64_t sys_dup2(uint64_t fd, uint64_t newfd);
/**
 * Linux contract: duplicate a file descriptor with explicit flags.
 * Current kernel: accepts only O_CLOEXEC in flags.
 */
uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags);

#define PIDFS_IOCTL_MAGIC 0xFF

#define PIDFD_GET_CGROUP_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 1)
#define PIDFD_GET_IPC_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 2)
#define PIDFD_GET_MNT_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 3)
#define PIDFD_GET_NET_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 4)
#define PIDFD_GET_PID_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 5)
#define PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 6)
#define PIDFD_GET_TIME_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 7)
#define PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 8)
#define PIDFD_GET_USER_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 9)
#define PIDFD_GET_UTS_NAMESPACE _IO(PIDFS_IOCTL_MAGIC, 10)

#define PIDFD_INFO_PID (1UL << 0)
#define PIDFD_INFO_CREDS (1UL << 1)
#define PIDFD_INFO_CGROUPID (1UL << 2)
#define PIDFD_INFO_EXIT (1UL << 3)
#define PIDFD_INFO_COREDUMP (1UL << 4)
#define PIDFD_INFO_SUPPORTED_MASK (1UL << 5)
#define PIDFD_INFO_COREDUMP_SIGNAL (1UL << 6)
#define PIDFD_INFO_COREDUMP_CODE (1UL << 7)

#define PIDFD_INFO_SIZE_VER0 64
#define PIDFD_INFO_SIZE_VER1 72
#define PIDFD_INFO_SIZE_VER2 80
#define PIDFD_INFO_SIZE_VER3 88

#define PIDFD_COREDUMPED (1U << 0)
#define PIDFD_COREDUMP_SKIP (1U << 1)
#define PIDFD_COREDUMP_USER (1U << 2)
#define PIDFD_COREDUMP_ROOT (1U << 3)

typedef struct pidfd_info {
    uint64_t mask;
    uint64_t cgroupid;
    uint32_t pid;
    uint32_t tgid;
    uint32_t ppid;
    uint32_t ruid;
    uint32_t rgid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t fsuid;
    uint32_t fsgid;
    int32_t exit_code;
    uint32_t coredump_mask;
    uint32_t coredump_signal;
    uint32_t coredump_code;
    uint32_t coredump_pad;
    uint64_t supported_mask;
} pidfd_info_t;

#define PIDFD_GET_INFO _IOWR(PIDFS_IOCTL_MAGIC, 11, struct pidfd_info)

/**
 * Linux contract: open a pidfd referring to a live task.
 * Current kernel: creates a pollable pidfd that becomes readable when the task
 * exits.
 * Notes: pidfds are usually used to avoid pid reuse races. Callers expect the
 * fd handle to stay tied to one concrete task lifetime rather than to a numeric
 * pid slot.
 */
uint64_t sys_pidfd_open(int pid, uint64_t flags);
/**
 * Linux contract: deliver a signal through a pidfd.
 * Current kernel: supports sig==0 probes, SI_USER sends, and optional user
 * siginfo copied verbatim from userspace.
 * Gaps: Linux pidfd_send_signal flags are not implemented; non-zero flags are
 * rejected.
 */
uint64_t sys_pidfd_send_signal(uint64_t pidfd, int sig, siginfo_t *info,
                               uint64_t flags);
uint64_t pidfd_create_for_pid(uint64_t pid, uint64_t flags, bool cloexec);
int pidfd_get_pid_from_fd(uint64_t fd, uint64_t *pid_out);
void pidfd_on_task_exit(task_t *task);

/**
 * Linux contract: multiplex fd control commands such as dup, fd flags, status
 * flags, and locks.
 * Current kernel: implements the commands wired in generic.c.
 * Notes: fcntl() is one of those interfaces where unsupported commands matter.
 * Userspace libraries often probe feature support by trying a command and
 * inspecting the exact error code.
 * Gaps: many Linux fcntl commands are still missing or simplified; for example
 * pipe sizing and file seals currently behave as compatibility stubs.
 */
uint64_t sys_fcntl(uint64_t fd, uint64_t command, uint64_t arg);
/**
 * Linux contract: create a pipe pair with optional flags.
 * Current kernel: depends on the local pipefs backend for actual pipe object
 * creation and flag handling.
 * Notes: callers usually assume pipe ends are ordered as read end first, write
 * end second, and that O_NONBLOCK/O_CLOEXEC apply immediately to the returned
 * descriptors.
 */
uint64_t sys_pipe(int fd[2], uint64_t flags);
/**
 * Linux contract: stat a path relative to cwd.
 */
uint64_t sys_stat(const char *fd, struct stat *buf);
/**
 * Linux contract: stat an open file descriptor.
 */
uint64_t sys_fstat(uint64_t fd, struct stat *buf);
/**
 * Linux contract: stat a path relative to dirfd with AT_* flags.
 * Notes: this entry point is widely used for metadata-only probes, often with
 * AT_SYMLINK_NOFOLLOW or AT_EMPTY_PATH, so path resolution flags are part of
 * the behavior rather than an optional extra.
 */
uint64_t sys_newfstatat(uint64_t dirfd, const char *pathname, struct stat *buf,
                        uint64_t flags);

/**
 * Linux contract: statx(2) with explicit mask and flags.
 * Current kernel: fills fields supported by the local VFS stat path.
 * Gaps: unsupported Linux statx mask bits are not backed by additional kernel
 * metadata and therefore return only the synthesized subset.
 */
uint64_t sys_statx(uint64_t dirfd, const char *pathname, uint64_t flags,
                   uint64_t mask, struct statx *buf);

/**
 * Linux contract: set a resource limit for the current task.
 * Current kernel: forwards to prlimit64(pid=0).
 */
uint64_t sys_set_rlimit(uint64_t resource, const struct rlimit *lim);
/**
 * Linux contract: query a resource limit for the current task.
 * Current kernel: reads the calling task's cached rlimit array.
 */
uint64_t sys_get_rlimit(uint64_t resource, struct rlimit *lim);
/**
 * Linux contract: get/set limits for the selected process.
 * Current kernel: supports the current task and same-thread-group targets.
 * Gaps: cross-process reads/writes outside the caller's thread group currently
 * fail with -ENOSYS rather than Linux permission-checked behavior.
 */
uint64_t sys_prlimit64(uint64_t pid, int resource,
                       const struct rlimit *new_rlim, struct rlimit *old_rlim);

/**
 * Linux contract: wait for fd readiness using poll(2).
 * Current kernel: uses the VFS poll-wait infrastructure.
 * Notes: poll() reports readiness, not guaranteed progress. Callers still need
 * to handle short I/O, EAGAIN, and races with peer shutdown after wakeup.
 */
size_t sys_poll(struct pollfd *fds, int nfds, uint64_t timeout);
/**
 * Linux contract: poll(2) with an optional signal mask and timespec timeout.
 * Notes: ppoll() is commonly used to atomically swap a temporary signal mask
 * around the sleep window. Callers expect that to be tighter than composing
 * sigprocmask() with poll() in userspace.
 */
uint64_t sys_ppoll(struct pollfd *fds, uint64_t nfds,
                   const struct timespec *timeout_ts, const sigset_t *sigmask,
                   size_t sigsetsize);

/**
 * Linux contract: check effective access permissions for a path.
 * Current kernel: uses the inode permission hook when present and otherwise
 * succeeds.
 * Gaps: Linux fsuid/euid distinction and richer permission fallback logic are
 * not fully modeled.
 */
size_t sys_access(char *filename, int mode);
/**
 * Linux contract: access(2) relative to dirfd.
 * Current kernel: resolves the path relative to dirfd and calls the inode
 * permission hook when available.
 * Gaps: an empty pathname currently succeeds directly instead of reproducing
 * the full Linux faccessat semantics.
 */
uint64_t sys_faccessat(uint64_t dirfd, const char *pathname, uint64_t mode);
/**
 * Linux contract: faccessat2(2) with Linux AT_* flags.
 * Current kernel: supports AT_EMPTY_PATH and AT_SYMLINK_NOFOLLOW handling in
 * the local path resolver.
 * Gaps: Linux AT_EACCESS behavior is not implemented.
 */
uint64_t sys_faccessat2(uint64_t dirfd, const char *pathname, uint64_t mode,
                        uint64_t flags);
/**
 * Linux contract: wait for readiness using select(2).
 * Notes: select() keeps the historical in/out fd-set ABI, so the input sets
 * are overwritten with the ready subset. Callers that need the originals must
 * preserve them in userspace.
 */
size_t sys_select(int nfds, uint8_t *read, uint8_t *write, uint8_t *except,
                  struct timeval *timeout);
/**
 * Linux contract: select(2) with pselect6 signal-mask ABI.
 * Notes: like ppoll(), this is usually chosen for its signal-mask atomicity
 * rather than because fd_set is a pleasant API.
 */
uint64_t sys_pselect6(uint64_t nfds, fd_set *readfds, fd_set *writefds,
                      fd_set *exceptfds, struct timespec *timeout,
                      weird_pselect6_t *weirdPselect6);

/**
 * Linux contract: create a hard link.
 */
uint64_t sys_link(const char *old, const char *new);
/**
 * Linux contract: read the target of a symbolic link.
 * Current kernel: resolves the path with LOOKUP_NOFOLLOW and calls the inode's
 * get_link method.
 */
uint64_t sys_readlink(char *path, char *buf, uint64_t size);
/**
 * Linux contract: readlink(2) relative to dirfd.
 * Current kernel: resolves the path relative to dirfd and calls the inode's
 * get_link method.
 */
uint64_t sys_readlinkat(int dfd, char *path, char *buf, uint64_t size);

uint32_t poll_to_epoll_comp(uint32_t poll_events);
uint32_t epoll_to_poll_comp(uint32_t epoll_events);

/**
 * Linux contract: create an epoll instance.
 * Current kernel: allocates an epoll-backed file object through the local
 * epoll implementation.
 */
uint64_t sys_epoll_create(int size);
/**
 * Linux contract: wait for epoll events.
 * Current kernel: depends on the local epoll watch bookkeeping and poll
 * notification paths.
 * Notes: readiness returned from epoll is level- or edge-triggered according
 * to the watch state, so userspace often relies on subtle rearm and drain
 * patterns here.
 */
uint64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                        int timeout);
/**
 * Linux contract: add, modify, or delete an epoll watch.
 * Current kernel: supports the watch lifecycle implemented by
 * fs/syscall/epoll.c.
 * Notes: an epoll watch attaches to the underlying file object plus user data,
 * not merely to an integer fd slot, so dup/close behavior tends to surprise
 * people the first time they debug it.
 */
uint64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
/**
 * Linux contract: epoll_wait(2) with a temporary signal mask.
 */
uint64_t sys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                         int timeout, sigset_t *sigmask, size_t sigsetsize);
/**
 * Linux contract: epoll_pwait2(2) with timespec timeout resolution.
 */
uint64_t sys_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                          struct timespec *timeout, sigset_t *sigmask,
                          size_t sigsetsize);
/**
 * Linux contract: create an epoll instance with flags.
 */
uint64_t sys_epoll_create1(int flags);

/**
 * Linux contract: create an eventfd with flags.
 * Current kernel: supports EFD_CLOEXEC, EFD_NONBLOCK, and EFD_SEMAPHORE.
 */
uint64_t sys_eventfd2(uint64_t initial_val, uint64_t flags);
/**
 * Linux contract: create an eventfd without extra flags.
 * Current kernel: simple wrapper around sys_eventfd2(..., 0).
 */
uint64_t sys_eventfd(uint64_t arg1);

/**
 * Linux contract: create or reconfigure a signalfd.
 * Current kernel: supports creating a new signalfd or updating an existing one
 * when `ufd >= 0`.
 * Notes: signalfd only observes signals that are blocked from normal delivery
 * to the thread or process in the expected Linux style, so mask handling is a
 * real part of the interface, not just configuration metadata.
 * Gaps: behavior is limited to the local signal queue model and mask handling
 * implemented by signalfd.c.
 */
uint64_t sys_signalfd4(int ufd, const sigset_t *mask, size_t sizemask,
                       int flags);
/**
 * Linux contract: legacy signalfd entry point.
 * Current kernel: simple wrapper around sys_signalfd4(..., 0).
 */
uint64_t sys_signalfd(int ufd, const sigset_t *mask, size_t sizemask);

/**
 * Linux contract: apply BSD-style advisory locks with flock(2).
 * Current kernel: tracks exactly one BSD flock owner per inode via
 * inode->flock_lock.
 * Gaps: the Linux interaction model between flock and POSIX byte-range locks is
 * not fully reproduced.
 */
uint64_t sys_flock(int fd, uint64_t cmd);

/**
 * Linux contract: set the process umask and return the old mask.
 */
uint64_t sys_umask(uint64_t mask);

/**
 * Linux contract: create a directory relative to cwd.
 */
uint64_t sys_mkdir(const char *name, uint64_t mode);
/**
 * Linux contract: create a directory relative to dirfd.
 */
uint64_t sys_mkdirat(int dfd, const char *name, uint64_t mode);

/**
 * Linux contract: create a hard link.
 */
uint64_t sys_link(const char *name, const char *target_name);
/**
 * Linux contract: create a symbolic link.
 * Current kernel: delegates to vfs_symlinkat() and stores the supplied target
 * string verbatim.
 * Notes: the target path is not resolved at symlink creation time. Callers are
 * writing raw link text, which may be relative and may intentionally point at
 * something nonexistent for now.
 */
uint64_t sys_symlink(const char *name, const char *target_name);
/**
 * Linux contract: create a hard link relative to directory fds.
 */
uint64_t sys_linkat(uint64_t olddirfd, const char *oldpath_user,
                    uint64_t newdirfd, const char *newpath_user, int flags);
/**
 * Linux contract: create a symbolic link relative to dirfd.
 */
uint64_t sys_symlinkat(const char *name_user, int dfd, const char *new_user);
/**
 * Linux contract: create a filesystem object node.
 */
uint64_t sys_mknod(const char *name, uint16_t umode, int dev);
/**
 * Linux contract: create a filesystem object node relative to dirfd.
 */
uint64_t sys_mknodat(uint64_t fd, const char *path_user, uint16_t umode,
                     int dev);

/**
 * Linux contract: change mode bits on a path.
 */
uint64_t sys_chmod(const char *name, uint16_t mode);
/**
 * Linux contract: change mode bits on an open file descriptor.
 */
uint64_t sys_fchmod(int fd, uint16_t mode);
/**
 * Linux contract: change mode bits relative to dirfd.
 */
uint64_t sys_fchmodat(int dfd, const char *name, uint16_t mode);
/**
 * Linux contract: fchmodat2(2) with flags.
 */
uint64_t sys_fchmodat2(int dfd, const char *name, uint16_t mode, int flags);

/**
 * Linux contract: change ownership on a path.
 * Gaps: Linux capability and id-mapping permission checks are not fully
 * implemented.
 * Current kernel: mutates ownership through generic_setattr_path().
 */
uint64_t sys_chown(const char *filename, uint64_t uid, uint64_t gid);
/**
 * Linux contract: change ownership on an open file descriptor.
 */
uint64_t sys_fchown(int fd, uint64_t uid, uint64_t gid);
/**
 * Linux contract: change ownership relative to dirfd.
 */
uint64_t sys_fchownat(int dfd, const char *filename, uint64_t uid, uint64_t gid,
                      int flags);

/**
 * Linux contract: rename a path.
 */
uint64_t sys_rename(const char *old, const char *new);
/**
 * Linux contract: rename relative to explicit directory fds.
 */
uint64_t sys_renameat(uint64_t oldfd, const char *old, uint64_t newfd,
                      const char *new);
/**
 * Linux contract: renameat2(2) with Linux rename flags.
 * Current kernel: supports the flags implemented by vfs_renameat2().
 * Notes: rename is widely used as the publication step for atomic file
 * replacement. That means overwrite, exchange, and no-replace semantics matter
 * a lot more than the name suggests.
 * Gaps: only the rename flag subset implemented by the local VFS is available.
 */
uint64_t sys_renameat2(uint64_t oldfd, const char *old, uint64_t newfd,
                       const char *new, uint64_t flags);

/**
 * Linux contract: change cwd to the directory referenced by fd.
 */
uint64_t sys_fchdir(uint64_t fd);

/**
 * Linux contract: remove an empty directory.
 */
uint64_t sys_rmdir(const char *name);
/**
 * Linux contract: unlink a non-directory path.
 */
uint64_t sys_unlink(const char *name);
/**
 * Linux contract: unlink relative to dirfd with AT_REMOVEDIR support.
 * Notes: the meaning of the target changes with flags. AT_REMOVEDIR makes this
 * the directory-removal path, so callers often funnel both unlink and rmdir
 * behavior through this one entry point.
 */
uint64_t sys_unlinkat(uint64_t dirfd, const char *name, uint64_t flags);

/**
 * Linux contract: create a timerfd.
 * Current kernel: supports only CLOCK_REALTIME and CLOCK_MONOTONIC plus
 * TFD_NONBLOCK/TFD_CLOEXEC.
 * Notes: timerfd is usually consumed through poll/epoll loops, so its readable
 * state and counter semantics matter at least as much as the timer source.
 */
uint64_t sys_timerfd_create(int clockid, int flags);
/**
 * Linux contract: arm or disarm a timerfd.
 * Current kernel: supports relative and absolute arming plus reading the old
 * timer state.
 * Gaps: TFD_TIMER_CANCEL_ON_SET is accepted at the API boundary but only the
 * behavior implemented by timerfd.c is available.
 */
uint64_t sys_timerfd_settime(int fd, int flags,
                             const struct itimerspec *new_value,
                             struct itimerspec *old_v);
void timerfd_check_wakeup(void);
void timerfd_softirq(void);

/**
 * Linux contract: create an anonymous in-memory file.
 * Current kernel: creates a memfd-backed file and installs it in the caller's
 * fd table.
 * Notes: memfd is commonly used for shared memory objects, executable image
 * staging, and file-seal workflows. Callers therefore tend to care about both
 * ordinary file I/O and fcntl()-driven behavior on the returned fd.
 * Gaps: MFD_HUGETLB, MFD_NOEXEC_SEAL, and MFD_EXEC are rejected.
 */
uint64_t sys_memfd_create(const char *name, unsigned int flags);

/**
 * Linux contract: create a detached fs context with the new mount API.
 * Current kernel: supports the fs context features implemented by the local
 * VFS.
 */
uint64_t sys_fsopen(const char *fsname, unsigned int flags);
uint64_t sys_fspick(int dfd, const char *path, unsigned int flags);
/**
 * Linux contract: open a mount tree or detached mount fd.
 * Current kernel: supports OPEN_TREE_CLONE, AT_EMPTY_PATH,
 * AT_SYMLINK_NOFOLLOW, OPEN_TREE_CLOEXEC, and recursive clone through
 * AT_RECURSIVE.
 * Gaps: semantics are limited to bind-mount cloning and the mountfd model
 * implemented by generic.c/mountfd.c.
 */
uint64_t sys_open_tree(int dfd, const char *pathname, unsigned int flags);
/**
 * Linux contract: report filesystem statistics for a path.
 * Current kernel: resolves the path then fills a Linux-like statfs structure
 * from the backing superblock.
 */
uint64_t sys_statfs(const char *fsname, struct statfs *buf);
/**
 * Linux contract: report filesystem statistics for an fd.
 * Current kernel: uses the fd's current path and superblock to fill statfs.
 */
uint64_t sys_fstatfs(int fd, struct statfs *buf);
/**
 * Linux contract: push parameters into an fsopen-created context.
 * Current kernel: supports only commands and keys understood by the local mount
 * implementation.
 * Notes: fsconfig() is stateful. Callers usually build a mount in multiple
 * steps and expect some errors to depend on whether the context has already
 * advanced too far.
 * Gaps: unsupported command/key combinations fail with -EOPNOTSUPP or are
 * silently limited to the small option vocabulary in fsfd.c.
 */
uint64_t sys_fsconfig(int fd, uint32_t cmd, const char *key, const void *value,
                      int aux);
/**
 * Linux contract: materialize a mount object from an fs context.
 * Current kernel: requires a context in the CREATED state and returns a mount
 * file descriptor backed by mountfd.c.
 * Gaps: only the mount attributes understood by attr_flags_to_ms_flags() are
 * applied.
 */
uint64_t sys_fsmount(int fd, uint32_t flags, uint32_t attr_flags);
/**
 * Linux contract: move or attach mount objects via the new mount API.
 * Current kernel: supports mount-handle sources and path-based sources through
 * the mountfd/VFS helpers.
 * Notes: callers use this to compose mount trees without exposing half-built
 * state to the live namespace, so source-vs-target resolution mistakes here are
 * usually quite visible.
 * Gaps: only the MOVE_MOUNT flag combinations implemented by fsfd.c are
 * available.
 */
uint64_t sys_move_mount(int from_dfd, const char *from_pathname_user,
                        int to_dfd, const char *to_pathname_user,
                        uint32_t flags);

/**
 * Linux contract: resize a file by path.
 */
uint64_t sys_truncate(const char *path, uint64_t length);
/**
 * Linux contract: resize a file by descriptor.
 */
uint64_t sys_ftruncate(int fd, uint64_t length);
/**
 * Linux contract: preallocate or punch space in a file.
 * Current kernel: supports the modes implemented by the underlying VFS helper.
 * Notes: userspace often calls fallocate() to avoid ENOSPC at write time or to
 * punch holes in sparse files, so mode handling is a real semantic switch here,
 * not a cosmetic extension field.
 * Gaps: generic.c currently accepts only mode == 0 and maps everything else to
 * -EOPNOTSUPP.
 */
uint64_t sys_fallocate(int fd, int mode, uint64_t offset, uint64_t len);

/**
 * Linux contract: provide file access advice.
 * Current kernel: validates the fd and returns success as a compatibility
 * no-op.
 */
uint64_t sys_fadvise64(int fd, uint64_t offset, uint64_t len, int advice);

/**
 * Linux contract: update path timestamps with nanosecond resolution.
 * Current kernel: supports UTIME_NOW, UTIME_OMIT, AT_EMPTY_PATH,
 * AT_SYMLINK_NOFOLLOW, and NULL pathname fd targeting.
 * Notes: UTIME_NOW and UTIME_OMIT intentionally mix explicit and implicit
 * timestamp updates in one call, so callers may pass partially meaningful input
 * arrays instead of a pair of ordinary concrete timestamps.
 * Notes: timestamp changes are passed through the filesystem setattr hook so
 * persistent filesystems can update their on-disk inode metadata.
 */
uint64_t sys_utimensat(int dfd, const char *pathname, struct timespec *utimes,
                       int flags);
/**
 * Linux contract: legacy futimesat(2) timestamp update entry point.
 * Current kernel: converts timeval input to utimensat-compatible nanosecond
 * timestamps and follows the same path/fd resolution rules.
 */
uint64_t sys_futimesat(int dfd, const char *pathname, struct timeval *utimes);

/**
 * Linux contract: positional read that does not update the shared file offset.
 */
uint64_t sys_pread64(int fd, void *buf, size_t count, uint64_t offset);

/**
 * Linux contract: positional write that does not update the shared file offset.
 */
uint64_t sys_pwrite64(int fd, const void *buf, size_t count, uint64_t offset);

/**
 * Linux contract: report global uptime, load, and memory statistics.
 * Current kernel: reports uptime and task count, but several memory/load fields
 * are synthesized or left zero.
 * Gaps: load averages, freeram, swap, and other detailed Linux accounting are
 * not implemented yet.
 */
uint64_t sys_sysinfo(struct sysinfo *info);

extern void fs_syscall_init();
