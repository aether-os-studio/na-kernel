#pragma once

#include <libs/klibc.h>
#include <fs/vfs/vfs.h>
#include <fs/fs_syscall.h>

#define PIPE_BUFF (512 * 1024)
#define PIPE_ATOMIC_MAX MIN(PIPE_BUFF, PAGE_SIZE)
#define PIPE_MAX_BUFFERS (PIPE_BUFF / PAGE_SIZE + 32)
#define PIPE_PAGE_CACHE_MAX 16

#define MAX_PIPES 32

struct task;
typedef struct task task_t;

struct spinlock;
typedef struct spinlock spinlock_t;

struct vfs_inode;
typedef struct vfs_inode vfs_node_t;

typedef struct pipe_info {
    uint32_t ptr;
    uint32_t head;
    uint32_t nr_buffers;
    uint32_t nr_cached_pages;
    uint64_t cached_pages[PIPE_PAGE_CACHE_MAX];

    int write_fds;
    int read_fds;
    uint32_t endpoint_refs;

    vfs_node_t *read_node;
    vfs_node_t *write_node;
    bool owns_node_refs;

    spinlock_t lock;
} pipe_info_t;

typedef struct pipe_buffer {
    uint64_t phys;
    uint16_t offset;
    uint16_t len;
    bool page_ref;
    bool can_merge;
} pipe_buffer_t;

typedef struct pipe_specific pipe_specific_t;
struct pipe_specific {
    bool read;
    bool write;
    /*
     * POLLHUP on a FIFO reader means that a writer which was visible to this
     * open file description has gone away.  A nonblocking reader opened
     * before the first writer must not report POLLHUP immediately.
     */
    bool seen_writer;
    pipe_info_t *info;
};

ssize_t pipefs_named_read(struct vfs_file *file, void *buf, size_t count,
                          loff_t *ppos);
ssize_t pipefs_named_write(struct vfs_file *file, const void *buf, size_t count,
                           loff_t *ppos);
__poll_t pipefs_named_poll(struct vfs_file *file, struct vfs_poll_table *pt);
int pipefs_named_open(struct vfs_inode *inode, struct vfs_file *file);
int pipefs_named_release(struct vfs_inode *inode, struct vfs_file *file);
void pipefs_named_evict_inode(struct vfs_inode *inode);
bool pipefs_is_pipe(struct vfs_file *file);
ssize_t pipefs_splice_to(struct vfs_file *in, struct vfs_file *out,
                         size_t count, bool nonblock);
ssize_t pipefs_splice_from_user(struct vfs_file *file, const struct iovec *iov,
                                size_t nr_segs, size_t count, bool nonblock);
