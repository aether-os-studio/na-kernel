#include <arch/arch.h>
#include <task/task.h>
#include <task/signal.h>
#include <fs/pipe.h>
#include <fs/vfs/vfs.h>
#include <init/callbacks.h>
#include <mm/mm.h>
#include <mm/page.h>

#define PIPEFS_MAGIC 0x50495045ULL

typedef struct pipefs_fs_info {
    spinlock_t lock;
    ino64_t next_ino;
} pipefs_fs_info_t;

typedef struct pipefs_inode_info {
    struct vfs_inode vfs_inode;
} pipefs_inode_info_t;

static struct vfs_file_system_type pipefs_fs_type;
static const struct vfs_super_operations pipefs_super_ops;
static const struct vfs_file_operations pipefs_dir_file_ops;
static const struct vfs_file_operations pipefs_file_ops;
static spinlock_t pipefs_mount_lock;
static struct vfs_mount *pipefs_internal_mnt;

int pipefs_id = 0;

static inline pipefs_fs_info_t *pipefs_sb_info(struct vfs_super_block *sb) {
    return sb ? (pipefs_fs_info_t *)sb->s_fs_info : NULL;
}

static inline pipe_specific_t *pipefs_spec_from_inode(struct vfs_inode *inode) {
    return inode ? (pipe_specific_t *)inode->i_private : NULL;
}

static inline pipe_specific_t *pipefs_spec_from_file(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (pipe_specific_t *)file->private_data;
    return pipefs_spec_from_inode(file->f_inode);
}

static pipe_info_t *pipefs_named_info(struct vfs_inode *inode) {
    return inode ? (pipe_info_t *)inode->i_private : NULL;
}

static pipe_info_t *pipefs_alloc_info(void) {
    pipe_info_t *info =
        calloc(1, sizeof(*info) + sizeof(pipe_buffer_t) * PIPE_MAX_BUFFERS);

    if (!info)
        return NULL;
    spin_init(&info->lock);
    return info;
}

static void pipefs_notify_node(vfs_node_t *node, uint32_t events) {
    if (node && events)
        vfs_poll_notify_inode(node, events);
}

static void pipefs_update_size_locked(pipe_info_t *pipe,
                                      struct vfs_file *file) {
    if (!pipe)
        return;
    if (file && file->f_inode)
        file->f_inode->i_size = pipe->ptr;
    if (pipe->write_node)
        pipe->write_node->i_size = pipe->ptr;
    if (pipe->read_node)
        pipe->read_node->i_size = pipe->ptr;
}

static pipe_buffer_t *pipefs_buffers(pipe_info_t *pipe) {
    return pipe ? (pipe_buffer_t *)(pipe + 1) : NULL;
}

static uint32_t pipefs_buffer_index(pipe_info_t *pipe, uint32_t off) {
    return (pipe->head + off) % PIPE_MAX_BUFFERS;
}

static pipe_buffer_t *pipefs_front_buffer(pipe_info_t *pipe) {
    if (!pipe || !pipe->nr_buffers)
        return NULL;
    return &pipefs_buffers(pipe)[pipe->head];
}

static pipe_buffer_t *pipefs_back_buffer(pipe_info_t *pipe) {
    if (!pipe || !pipe->nr_buffers)
        return NULL;
    pipe_buffer_t *buffers = pipefs_buffers(pipe);
    return &buffers[pipefs_buffer_index(pipe, pipe->nr_buffers - 1)];
}

static pipe_buffer_t *pipefs_push_buffer_locked(pipe_info_t *pipe) {
    if (!pipe || pipe->nr_buffers >= PIPE_MAX_BUFFERS)
        return NULL;
    pipe_buffer_t *buf =
        &pipefs_buffers(pipe)[pipefs_buffer_index(pipe, pipe->nr_buffers++)];
    memset(buf, 0, sizeof(*buf));
    return buf;
}

static uint64_t pipefs_alloc_owned_page_locked(pipe_info_t *pipe) {
    if (pipe && pipe->nr_cached_pages)
        return pipe->cached_pages[--pipe->nr_cached_pages];
    return alloc_frames(1);
}

static void pipefs_release_owned_page_locked(pipe_info_t *pipe, uint64_t phys) {
    if (!phys)
        return;
    if (pipe && pipe->nr_cached_pages < PIPE_PAGE_CACHE_MAX) {
        pipe->cached_pages[pipe->nr_cached_pages++] = phys;
        return;
    }
    free_frames(phys, 1);
}

static void pipefs_drop_buffer_locked(pipe_info_t *pipe, pipe_buffer_t *buf) {
    if (!buf)
        return;
    if (buf->page_ref)
        address_release(buf->phys);
    else if (buf->phys)
        pipefs_release_owned_page_locked(pipe, buf->phys);
    memset(buf, 0, sizeof(*buf));
}

static void pipefs_pop_front_locked(pipe_info_t *pipe) {
    if (!pipe || !pipe->nr_buffers)
        return;
    pipefs_drop_buffer_locked(pipe, pipefs_front_buffer(pipe));
    pipe->head = (pipe->head + 1) % PIPE_MAX_BUFFERS;
    pipe->nr_buffers--;
    if (!pipe->nr_buffers)
        pipe->head = 0;
}

static void pipefs_discard_locked(pipe_info_t *pipe) {
    while (pipe && pipe->nr_buffers)
        pipefs_pop_front_locked(pipe);
    if (pipe)
        pipe->ptr = 0;
}

static void pipefs_buffer_consume_locked(pipe_info_t *pipe, pipe_buffer_t *buf,
                                         size_t len) {
    if (!pipe || !buf || !len)
        return;
    if (len >= buf->len) {
        pipe->ptr -= buf->len;
        pipefs_pop_front_locked(pipe);
        return;
    }
    buf->offset += (uint16_t)len;
    buf->len -= (uint16_t)len;
    pipe->ptr -= (uint32_t)len;
}

static size_t pipefs_copy_out_locked(pipe_info_t *pipe, void *addr,
                                     size_t len) {
    size_t copied = 0;

    while (copied < len && pipe->nr_buffers) {
        pipe_buffer_t *buf = pipefs_front_buffer(pipe);
        size_t chunk = MIN(len - copied, (size_t)buf->len);

        memcpy((char *)addr + copied,
               (const void *)((uintptr_t)phys_to_virt(buf->phys) + buf->offset),
               chunk);
        copied += chunk;
        pipefs_buffer_consume_locked(pipe, buf, chunk);
    }

    return copied;
}

static ssize_t pipefs_append_page_locked(pipe_info_t *pipe, uint64_t phys,
                                         size_t offset, size_t len,
                                         bool page_ref) {
    if (!pipe || !phys || !len || offset >= PAGE_SIZE ||
        len > PAGE_SIZE - offset)
        return -EINVAL;
    if (pipe->ptr + len > PIPE_BUFF)
        return 0;

    pipe_buffer_t *buf = pipefs_push_buffer_locked(pipe);
    if (!buf)
        return 0;
    buf->phys = phys;
    buf->offset = (uint16_t)offset;
    buf->len = (uint16_t)len;
    buf->page_ref = page_ref;
    buf->can_merge = !page_ref && offset == 0 && len < PAGE_SIZE;

    pipe->ptr += (uint32_t)len;
    return (ssize_t)len;
}

static size_t pipefs_ring_write_locked(pipe_info_t *pipe, const void *addr,
                                       size_t len) {
    size_t copied = 0;

    while (copied < len) {
        pipe_buffer_t *tail = pipefs_back_buffer(pipe);

        if (tail && tail->can_merge) {
            size_t tail_end = (size_t)tail->offset + tail->len;
            size_t chunk = MIN(len - copied, (size_t)PAGE_SIZE - tail_end);

            if (chunk) {
                void *tail_addr =
                    (void *)((uintptr_t)phys_to_virt(tail->phys) + tail_end);
                memcpy(tail_addr, (const char *)addr + copied, chunk);
                tail->len += (uint16_t)chunk;
                tail->can_merge = (tail_end + chunk) < PAGE_SIZE;
                pipe->ptr += (uint32_t)chunk;
                copied += chunk;
                continue;
            }
            tail->can_merge = false;
        }

        uint64_t phys = pipefs_alloc_owned_page_locked(pipe);
        size_t chunk = MIN(len - copied, (size_t)PAGE_SIZE);

        if (!phys)
            break;
        memcpy((void *)phys_to_virt(phys), (const char *)addr + copied, chunk);
        if (pipefs_append_page_locked(pipe, phys, 0, chunk, false) <= 0) {
            pipefs_release_owned_page_locked(pipe, phys);
            break;
        }
        copied += chunk;
    }
    return copied;
}

static bool pipefs_move_one_locked(pipe_info_t *dst, pipe_info_t *src,
                                   size_t max_len, size_t *moved) {
    pipe_buffer_t *in;
    pipe_buffer_t *out;
    size_t chunk;

    if (moved)
        *moved = 0;
    if (!dst || !src || !src->nr_buffers || dst->nr_buffers >= PIPE_MAX_BUFFERS)
        return false;
    if (dst->ptr >= PIPE_BUFF)
        return false;

    in = pipefs_front_buffer(src);
    chunk = MIN(max_len, (size_t)in->len);
    chunk = MIN(chunk, (size_t)PIPE_BUFF - dst->ptr);
    if (!chunk)
        return false;

    out = pipefs_push_buffer_locked(dst);
    if (!out)
        return false;

    *out = *in;
    out->len = (uint16_t)chunk;
    if (!address_ref(out->phys)) {
        memset(out, 0, sizeof(*out));
        dst->nr_buffers--;
        return false;
    }
    out->page_ref = true;
    out->can_merge = false;
    in->page_ref = true;
    in->can_merge = false;

    dst->ptr += (uint32_t)chunk;
    pipefs_buffer_consume_locked(src, in, chunk);
    if (moved)
        *moved = chunk;
    return true;
}

static void pipefs_free_info(pipe_info_t *pipe) {
    if (!pipe)
        return;
    pipefs_discard_locked(pipe);
    while (pipe->nr_cached_pages)
        free_frames(pipe->cached_pages[--pipe->nr_cached_pages], 1);
    free(pipe);
}

static pipe_info_t *pipefs_named_ensure_info(struct vfs_inode *inode) {
    pipe_info_t *pipe;
    pipe_info_t *new_pipe;
    pipe_info_t *expected = NULL;

    if (!inode)
        return NULL;

    pipe = pipefs_named_info(inode);
    if (pipe)
        return pipe;

    new_pipe = pipefs_alloc_info();
    if (!new_pipe)
        return NULL;

    new_pipe->read_node = inode;
    new_pipe->write_node = inode;

    if (!__atomic_compare_exchange_n((pipe_info_t **)&inode->i_private,
                                     &expected, new_pipe, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pipefs_free_info(new_pipe);
        pipe = expected;
    } else {
        pipe = new_pipe;
    }

    if (pipe) {
        pipe->read_node = inode;
        pipe->write_node = inode;
    }
    return pipe;
}

static struct vfs_inode *pipefs_alloc_inode(struct vfs_super_block *sb) {
    pipefs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void pipefs_destroy_inode(struct vfs_inode *inode) {
    if (!inode)
        return;
    free(container_of(inode, pipefs_inode_info_t, vfs_inode));
}

static void pipefs_evict_inode(struct vfs_inode *inode) {
    pipe_specific_t *spec = pipefs_spec_from_inode(inode);
    pipe_info_t *pipe;
    bool free_pipe = false;

    if (!spec)
        return;

    pipe = spec->info;
    if (pipe) {
        spin_lock(&pipe->lock);
        if (spec->write) {
            if (pipe->write_node == inode)
                pipe->write_node = NULL;
        } else {
            if (pipe->read_node == inode)
                pipe->read_node = NULL;
        }
        if (!pipe->read_node && !pipe->write_node)
            free_pipe = true;
        spin_unlock(&pipe->lock);
    }

    free(spec);
    inode->i_private = NULL;

    if (free_pipe && pipe)
        pipefs_free_info(pipe);
}

static int pipefs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int pipefs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    pipefs_fs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *root;

    if (!fc)
        return -EINVAL;

    sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!sb)
        return -ENOMEM;

    fsi = calloc(1, sizeof(*fsi));
    if (!fsi) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    spin_init(&fsi->lock);
    fsi->next_ino = 1;
    sb->s_magic = PIPEFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &pipefs_super_ops;
    sb->s_type = &pipefs_fs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(fsi);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &pipefs_dir_file_ops;

    root = vfs_d_alloc(sb, NULL, NULL);
    if (!root) {
        vfs_iput(inode);
        free(fsi);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root, inode);
    sb->s_root = root;
    fc->sb = sb;
    return 0;
}

static void pipefs_put_super(struct vfs_super_block *sb) {
    if (!sb)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations pipefs_super_ops = {
    .alloc_inode = pipefs_alloc_inode,
    .destroy_inode = pipefs_destroy_inode,
    .evict_inode = pipefs_evict_inode,
    .put_super = pipefs_put_super,
};

static struct vfs_file_system_type pipefs_fs_type = {
    .name = "pipefs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = pipefs_init_fs_context,
    .get_tree = pipefs_get_tree,
};

static struct vfs_mount *pipefs_get_internal_mount(void) {
    int ret;

    spin_lock(&pipefs_mount_lock);
    if (!pipefs_internal_mnt) {
        ret = vfs_kern_mount("pipefs", 0, NULL, NULL, &pipefs_internal_mnt);
        if (ret < 0)
            pipefs_internal_mnt = NULL;
    }
    if (pipefs_internal_mnt)
        vfs_mntget(pipefs_internal_mnt);
    spin_unlock(&pipefs_mount_lock);
    return pipefs_internal_mnt;
}

static ino64_t pipefs_next_ino(struct vfs_super_block *sb) {
    pipefs_fs_info_t *fsi = pipefs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static loff_t pipefs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    loff_t pos;

    if (!file || !file->f_inode)
        return -EBADF;

    spin_lock(&file->f_pos_lock);
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
        spin_unlock(&file->f_pos_lock);
        return -EINVAL;
    }
    if (pos < 0) {
        spin_unlock(&file->f_pos_lock);
        return -EINVAL;
    }
    file->f_pos = pos;
    spin_unlock(&file->f_pos_lock);
    return pos;
}

static __poll_t pipefs_poll_mask_locked(pipe_specific_t *spec,
                                        pipe_info_t *pipe) {
    __poll_t out = 0;

    if (!spec || !pipe)
        return EPOLLNVAL;

    if (spec->read) {
        if (pipe->write_fds > 0)
            spec->seen_writer = true;
        if (pipe->ptr > 0)
            out |= EPOLLIN | EPOLLRDNORM;
        if (pipe->write_fds == 0 && spec->seen_writer)
            out |= EPOLLHUP;
    }

    if (spec->write) {
        if (pipe->read_fds == 0) {
            out |= EPOLLERR;
        } else if (pipe->ptr < PIPE_BUFF) {
            out |= EPOLLOUT | EPOLLWRNORM;
        }
    }

    return out;
}

static void pipefs_account_open_locked(pipe_specific_t *spec) {
    if (!spec || !spec->info)
        return;

    if (spec->write)
        spec->info->write_fds++;
    if (spec->read)
        spec->info->read_fds++;
}

static ssize_t pipe_read_inner(struct vfs_file *file, void *addr, size_t size,
                               bool allow_wait) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);
    pipe_info_t *pipe;

    if (!spec || !spec->info)
        return -EINVAL;
    if (!spec->read)
        return -EBADF;
    pipe = spec->info;

    while (true) {
        spin_lock(&pipe->lock);

        if (pipe->ptr > 0) {
            size_t to_read = MIN(size, pipe->ptr);
            to_read = pipefs_copy_out_locked(pipe, addr, to_read);
            pipefs_update_size_locked(pipe, file);
            vfs_node_t *write_node = pipe->write_node;
            if (write_node)
                vfs_igrab(write_node);
            spin_unlock(&pipe->lock);

            pipefs_notify_node(write_node, EPOLLOUT | EPOLLWRNORM);
            if (write_node)
                vfs_iput(write_node);
            return (ssize_t)to_read;
        }

        if (pipe->write_fds == 0) {
            spin_unlock(&pipe->lock);
            return 0;
        }

        spin_unlock(&pipe->lock);

        if (!allow_wait)
            return 0;
        if (file->f_flags & O_NONBLOCK)
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(
            file, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLNVAL);
        if (reason < 0)
            return reason;
    }
}

static ssize_t pipefs_read(struct vfs_file *file, void *buf, size_t count,
                           loff_t *ppos) {
    char *out = (char *)buf;
    size_t readn = 0;

    (void)ppos;
    if (!count)
        return 0;

    while (readn < count) {
        ssize_t ret =
            pipe_read_inner(file, out + readn, count - readn, readn == 0);
        if (ret < 0)
            return readn ? (ssize_t)readn : ret;
        if (ret == 0)
            break;
        readn += (size_t)ret;
    }

    return (ssize_t)readn;
}

static ssize_t pipe_write_inner(struct vfs_file *file, const void *addr,
                                size_t size, bool atomic, bool allow_wait) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);
    pipe_info_t *pipe;

    if (!spec || !spec->info)
        return -EINVAL;
    if (!spec->write)
        return -EBADF;
    pipe = spec->info;

    while (true) {
        spin_lock(&pipe->lock);

        if (pipe->read_fds == 0) {
            spin_unlock(&pipe->lock);
            task_commit_signal(current_task, SIGPIPE, NULL);
            return -EPIPE;
        }

        size_t available = PIPE_BUFF - pipe->ptr;
        if (available > 0 && (!atomic || available >= size)) {
            bool was_empty = pipe->ptr == 0;
            size_t to_write = atomic ? size : MIN(size, available);
            to_write = pipefs_ring_write_locked(pipe, addr, to_write);
            if (!to_write) {
                spin_unlock(&pipe->lock);
                return -ENOMEM;
            }
            pipefs_update_size_locked(pipe, file);
            vfs_node_t *read_node = pipe->read_node;
            if (was_empty && read_node)
                vfs_igrab(read_node);
            spin_unlock(&pipe->lock);

            if (was_empty)
                pipefs_notify_node(read_node, EPOLLIN | EPOLLRDNORM);
            if (was_empty && read_node)
                vfs_iput(read_node);
            return (ssize_t)to_write;
        }

        spin_unlock(&pipe->lock);

        if (!allow_wait)
            return 0;
        if (file->f_flags & O_NONBLOCK)
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(
            file, EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLNVAL);
        if (reason < 0)
            return reason;
    }
}

static ssize_t pipefs_write(struct vfs_file *file, const void *buf,
                            size_t count, loff_t *ppos) {
    const char *data = (const char *)buf;
    size_t written = 0;
    bool atomic = count <= PIPE_ATOMIC_MAX;

    (void)ppos;
    while (written < count) {
        ssize_t ret = pipe_write_inner(file, data + written, count - written,
                                       atomic, true);
        if (ret < 0)
            return written ? (ssize_t)written : ret;
        if (ret == 0)
            break;
        written += (size_t)ret;
    }

    return (ssize_t)written;
}

static long pipefs_ioctl(struct vfs_file *file, unsigned long cmd,
                         unsigned long arg) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);

    if (!spec || !spec->info)
        return -EINVAL;

    switch (cmd) {
    case FIONREAD: {
        int available;

        spin_lock(&spec->info->lock);
        available = (int)spec->info->ptr;
        spin_unlock(&spec->info->lock);

        if (copy_to_user((void *)arg, &available, sizeof(available)))
            return -EFAULT;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static __poll_t pipefs_poll(struct vfs_file *file, struct vfs_poll_table *pt) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);
    pipe_info_t *pipe;
    __poll_t out = 0;

    (void)pt;
    if (!spec || !spec->info)
        return EPOLLNVAL;
    pipe = spec->info;

    spin_lock(&pipe->lock);
    out = pipefs_poll_mask_locked(spec, pipe);
    spin_unlock(&pipe->lock);

    return out;
}

static int pipefs_open(struct vfs_inode *inode, struct vfs_file *file) {
    pipe_specific_t *spec = pipefs_spec_from_inode(inode);
    pipe_info_t *pipe;

    if (!inode || !file || !spec || !spec->info)
        return -EINVAL;

    pipe = spec->info;
    spin_lock(&pipe->lock);
    pipefs_account_open_locked(spec);
    spin_unlock(&pipe->lock);

    file->f_op = inode->i_fop;
    file->private_data = spec;
    return 0;
}

static int pipefs_release(struct vfs_inode *inode, struct vfs_file *file) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);
    pipe_info_t *pipe;
    vfs_node_t *drop_read_node = NULL;
    vfs_node_t *drop_write_node = NULL;
    bool free_spec = false;

    (void)inode;
    if (!spec || !spec->info)
        return 0;

    pipe = spec->info;
    spin_lock(&pipe->lock);
    if (spec->write) {
        if (pipe->write_fds > 0) {
            pipe->write_fds--;
        }
        if (pipe->owns_node_refs && pipe->write_fds == 0 && pipe->write_node) {
            drop_write_node = pipe->write_node;
            pipe->write_node = NULL;
        }
    }
    if (spec->read) {
        if (pipe->read_fds > 0) {
            pipe->read_fds--;
        }
        if (pipe->owns_node_refs && pipe->read_fds == 0 && pipe->read_node) {
            drop_read_node = pipe->read_node;
            pipe->read_node = NULL;
        }
    }
    vfs_node_t *read_node = pipe->read_node;
    vfs_node_t *write_node = pipe->write_node;
    if (read_node)
        vfs_igrab(read_node);
    if (write_node && write_node != read_node)
        vfs_igrab(write_node);
    spin_unlock(&pipe->lock);

    pipefs_notify_node(read_node, EPOLLIN | EPOLLRDNORM | EPOLLHUP | EPOLLERR);
    pipefs_notify_node(write_node,
                       EPOLLOUT | EPOLLWRNORM | EPOLLHUP | EPOLLERR);
    if (read_node)
        vfs_iput(read_node);
    if (write_node && write_node != read_node)
        vfs_iput(write_node);
    if (drop_read_node)
        vfs_iput(drop_read_node);
    if (drop_write_node && drop_write_node != drop_read_node)
        vfs_iput(drop_write_node);

    free_spec = file->private_data && file->private_data != inode->i_private;
    file->private_data = NULL;
    if (free_spec)
        free(spec);
    return 0;
}

static const struct vfs_file_operations pipefs_dir_file_ops = {
    .llseek = pipefs_llseek,
    .open = pipefs_open,
    .release = pipefs_release,
};

static const struct vfs_file_operations pipefs_file_ops = {
    .llseek = pipefs_llseek,
    .read = pipefs_read,
    .write = pipefs_write,
    .unlocked_ioctl = pipefs_ioctl,
    .poll = pipefs_poll,
    .open = pipefs_open,
    .release = pipefs_release,
};

static int pipefs_create_endpoint(struct vfs_file **out_file, pipe_info_t *pipe,
                                  bool write_end, unsigned int open_flags) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    pipe_specific_t *spec;
    char namebuf[32];

    if (!out_file || !pipe)
        return -EINVAL;

    mnt = pipefs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    spec = calloc(1, sizeof(*spec));
    if (!spec) {
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = pipefs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFIFO | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &pipefs_file_ops;
    inode->i_private = spec;

    spec->read = !write_end;
    spec->write = write_end;
    spec->info = pipe;

    snprintf(namebuf, sizeof(namebuf), "pipe-%llu-%c",
             (unsigned long long)inode->i_ino, write_end ? 'w' : 'r');
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        free(spec);
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          open_flags);
    if (!file) {
        vfs_dput(dentry);
        free(spec);
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = spec;
    file->f_mode |= VFS_FMODE_NO_POS_LOCK;
    spin_lock(&pipe->lock);
    pipe->owns_node_refs = true;
    if (write_end)
        pipe->write_node = vfs_igrab(inode);
    else
        pipe->read_node = vfs_igrab(inode);
    spin_unlock(&pipe->lock);

    *out_file = file;
    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

void pipefs_init(void) {
    spin_init(&pipefs_mount_lock);
    vfs_register_filesystem(&pipefs_fs_type);
}

ssize_t pipefs_named_read(struct vfs_file *file, void *buf, size_t count,
                          loff_t *ppos) {
    return pipefs_read(file, buf, count, ppos);
}

ssize_t pipefs_named_write(struct vfs_file *file, const void *buf, size_t count,
                           loff_t *ppos) {
    return pipefs_write(file, buf, count, ppos);
}

__poll_t pipefs_named_poll(struct vfs_file *file, struct vfs_poll_table *pt) {
    return pipefs_poll(file, pt);
}

int pipefs_named_open(struct vfs_inode *inode, struct vfs_file *file) {
    pipe_info_t *pipe;
    pipe_specific_t *spec;
    unsigned int accmode;

    if (!inode || !file || !S_ISFIFO(inode->i_mode))
        return -EINVAL;

    pipe = pipefs_named_ensure_info(inode);
    if (!pipe)
        return -ENOMEM;

    accmode = file->f_flags & O_ACCMODE_FLAGS;
    spec = calloc(1, sizeof(*spec));
    if (!spec)
        return -ENOMEM;

    spec->read = accmode != O_WRONLY;
    spec->write = accmode != O_RDONLY;
    spec->info = pipe;

    spin_lock(&pipe->lock);
    /*
     * Account the endpoint before waiting.  This lets two blocking opens on
     * opposite sides rendezvous, as required by fifo(7).
     */
    pipefs_account_open_locked(spec);
    if (spec->read && pipe->write_fds > 0)
        spec->seen_writer = true;
    pipe->read_node = inode;
    pipe->write_node = inode;
    spin_unlock(&pipe->lock);

    file->private_data = spec;
    file->f_mode |= VFS_FMODE_NO_POS_LOCK;

    /* Opening both ends never blocks. */
    if (spec->read && spec->write) {
        pipefs_notify_node(inode,
                           EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM);
        return 0;
    }

    if (spec->write && (file->f_flags & O_NONBLOCK)) {
        bool have_reader;

        spin_lock(&pipe->lock);
        have_reader = pipe->read_fds > 0;
        spin_unlock(&pipe->lock);
        if (!have_reader) {
            pipefs_named_release(inode, file);
            return -ENXIO;
        }
    }

    pipefs_notify_node(inode, EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM);

    if (!(file->f_flags & O_NONBLOCK)) {
        while (true) {
            vfs_poll_wait_table_t table;
            bool have_peer;
            int reason;

            vfs_poll_wait_table_init(&table, current_task);
            vfs_poll_wait(file, &table.pt);
            reason = vfs_poll_wait_table_error(&table);
            if (reason) {
                vfs_poll_wait_table_cleanup(&table);
                pipefs_named_release(inode, file);
                return reason;
            }

            spin_lock(&pipe->lock);
            have_peer = spec->read ? pipe->write_fds > 0 : pipe->read_fds > 0;
            if (spec->read && pipe->write_fds > 0)
                spec->seen_writer = true;
            spin_unlock(&pipe->lock);
            if (have_peer) {
                vfs_poll_wait_table_cleanup(&table);
                break;
            }
            if (task_signal_has_deliverable(current_task)) {
                vfs_poll_wait_table_cleanup(&table);
                pipefs_named_release(inode, file);
                return -EINTR;
            }
            if (vfs_poll_wait_table_seq_changed(&table)) {
                vfs_poll_wait_table_cleanup(&table);
                continue;
            }

            reason = task_block(current_task, TASK_BLOCKING, -1, "fifo_open");
            vfs_poll_wait_table_cleanup(&table);
            if (reason < 0 ||
                (reason != EOK && task_signal_has_deliverable(current_task))) {
                pipefs_named_release(inode, file);
                return reason < 0 ? reason : -EINTR;
            }
        }
    }

    return 0;
}

int pipefs_named_release(struct vfs_inode *inode, struct vfs_file *file) {
    return pipefs_release(inode, file);
}

void pipefs_named_evict_inode(struct vfs_inode *inode) {
    pipe_info_t *pipe;

    if (!inode || !S_ISFIFO(inode->i_mode))
        return;

    pipe = pipefs_named_info(inode);
    if (!pipe)
        return;

    pipe->read_node = NULL;
    pipe->write_node = NULL;
    pipefs_free_info(pipe);
    inode->i_private = NULL;
}

bool pipefs_is_pipe(struct vfs_file *file) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);
    return spec && spec->info;
}

static int pipefs_wait_readable(struct vfs_file *file, bool nonblock) {
    if (nonblock || (file->f_flags & O_NONBLOCK))
        return -EWOULDBLOCK;
    return vfs_poll_wait_interruptible(file, EPOLLIN | EPOLLERR | EPOLLHUP |
                                                 EPOLLNVAL);
}

static int pipefs_wait_writable(struct vfs_file *file, bool nonblock) {
    if (nonblock || (file->f_flags & O_NONBLOCK))
        return -EWOULDBLOCK;
    return vfs_poll_wait_interruptible(file, EPOLLOUT | EPOLLERR | EPOLLHUP |
                                                 EPOLLNVAL);
}

static ssize_t pipefs_splice_pipe_to_pipe(struct vfs_file *in,
                                          struct vfs_file *out, size_t count,
                                          bool nonblock) {
    pipe_specific_t *ispec = pipefs_spec_from_file(in);
    pipe_specific_t *ospec = pipefs_spec_from_file(out);
    pipe_info_t *ipipe;
    pipe_info_t *opipe;
    size_t moved_total = 0;

    if (!ispec || !ospec || !ispec->read || !ospec->write)
        return -EBADF;
    ipipe = ispec->info;
    opipe = ospec->info;
    if (!ipipe || !opipe || ipipe == opipe)
        return -EINVAL;

    while (moved_total < count) {
        bool was_empty;
        size_t moved = 0;
        vfs_node_t *read_node = NULL;
        vfs_node_t *write_node = NULL;

        if ((uintptr_t)ipipe < (uintptr_t)opipe) {
            spin_lock(&ipipe->lock);
            spin_lock(&opipe->lock);
        } else {
            spin_lock(&opipe->lock);
            spin_lock(&ipipe->lock);
        }

        if (opipe->read_fds == 0) {
            spin_unlock(&ipipe->lock);
            spin_unlock(&opipe->lock);
            task_commit_signal(current_task, SIGPIPE, NULL);
            return moved_total ? (ssize_t)moved_total : -EPIPE;
        }
        if (ipipe->ptr == 0) {
            bool closed = ipipe->write_fds == 0;
            spin_unlock(&ipipe->lock);
            spin_unlock(&opipe->lock);
            if (closed)
                break;
            if (moved_total)
                break;
            int ret = pipefs_wait_readable(in, nonblock);
            if (ret < 0)
                return ret;
            continue;
        }
        if (opipe->ptr >= PIPE_BUFF || opipe->nr_buffers >= PIPE_MAX_BUFFERS) {
            spin_unlock(&ipipe->lock);
            spin_unlock(&opipe->lock);
            if (moved_total)
                break;
            int ret = pipefs_wait_writable(out, nonblock);
            if (ret < 0)
                return ret;
            continue;
        }

        was_empty = opipe->ptr == 0;
        if (!pipefs_move_one_locked(opipe, ipipe, count - moved_total,
                                    &moved)) {
            spin_unlock(&ipipe->lock);
            spin_unlock(&opipe->lock);
            if (moved_total)
                break;
            return -ENOMEM;
        }
        pipefs_update_size_locked(ipipe, in);
        pipefs_update_size_locked(opipe, out);
        if (ipipe->write_node)
            write_node = vfs_igrab(ipipe->write_node);
        if (was_empty && opipe->read_node)
            read_node = vfs_igrab(opipe->read_node);
        spin_unlock(&ipipe->lock);
        spin_unlock(&opipe->lock);

        if (write_node) {
            pipefs_notify_node(write_node, EPOLLOUT | EPOLLWRNORM);
            vfs_iput(write_node);
        }
        if (read_node) {
            pipefs_notify_node(read_node, EPOLLIN | EPOLLRDNORM);
            vfs_iput(read_node);
        }
        moved_total += moved;
    }

    return (ssize_t)moved_total;
}

ssize_t pipefs_splice_to(struct vfs_file *in, struct vfs_file *out,
                         size_t count, bool nonblock) {
    pipe_specific_t *spec = pipefs_spec_from_file(in);
    pipe_info_t *pipe;
    size_t moved_total = 0;

    if (!spec || !spec->read || !spec->info)
        return -EBADF;
    if (pipefs_is_pipe(out))
        return pipefs_splice_pipe_to_pipe(in, out, count, nonblock);
    pipe = spec->info;

    while (moved_total < count) {
        pipe_buffer_t local;
        size_t chunk;
        ssize_t wr;
        vfs_node_t *write_node = NULL;

        spin_lock(&pipe->lock);
        if (pipe->ptr == 0) {
            bool closed = pipe->write_fds == 0;
            spin_unlock(&pipe->lock);
            if (closed || moved_total)
                break;
            int ret = pipefs_wait_readable(in, nonblock);
            if (ret < 0)
                return ret;
            continue;
        }

        local = *pipefs_front_buffer(pipe);
        chunk = MIN(count - moved_total, (size_t)local.len);
        local.len = (uint16_t)chunk;
        if (!address_ref(local.phys)) {
            spin_unlock(&pipe->lock);
            return moved_total ? (ssize_t)moved_total : -EFAULT;
        }
        local.page_ref = true;
        spin_unlock(&pipe->lock);

        wr = vfs_write_kernel_file(
            out,
            (const void *)((uintptr_t)phys_to_virt(local.phys) + local.offset),
            chunk, NULL);
        address_release(local.phys);
        if (wr <= 0) {
            if (wr < 0)
                return moved_total ? (ssize_t)moved_total : wr;
            break;
        }

        spin_lock(&pipe->lock);
        pipefs_buffer_consume_locked(pipe, pipefs_front_buffer(pipe),
                                     (size_t)wr);
        pipefs_update_size_locked(pipe, in);
        if (pipe->write_node)
            write_node = vfs_igrab(pipe->write_node);
        spin_unlock(&pipe->lock);

        if (write_node) {
            pipefs_notify_node(write_node, EPOLLOUT | EPOLLWRNORM);
            vfs_iput(write_node);
        }
        moved_total += (size_t)wr;
        if ((size_t)wr < chunk)
            break;
    }

    return (ssize_t)moved_total;
}

ssize_t pipefs_splice_from_user(struct vfs_file *file, const struct iovec *iov,
                                size_t nr_segs, size_t count, bool nonblock) {
    pipe_specific_t *spec = pipefs_spec_from_file(file);
    pipe_info_t *pipe;
    size_t done = 0;
    size_t iov_off = 0;

    if (!spec || !spec->write || !spec->info)
        return -EBADF;
    pipe = spec->info;

    for (size_t i = 0; i < nr_segs && done < count; i++) {
        uintptr_t base = (uintptr_t)iov[i].iov_base;
        size_t len = MIN((size_t)iov[i].len, count - done);
        iov_off = 0;

        while (iov_off < len) {
            uintptr_t uaddr = base + iov_off;
            uint64_t *pgdir = get_current_page_dir(true);
            uint64_t pa = user_translate_or_fault(pgdir, uaddr, false);
            uint64_t page_phys;
            size_t page_off;
            size_t chunk;
            vfs_node_t *read_node = NULL;
            bool was_empty;

            if (!pa)
                return done ? (ssize_t)done : -EFAULT;

            page_phys = PADDING_DOWN(pa, PAGE_SIZE);
            page_off = pa - page_phys;
            chunk = MIN(len - iov_off, PAGE_SIZE - page_off);

            spin_lock(&pipe->lock);
            if (pipe->read_fds == 0) {
                spin_unlock(&pipe->lock);
                task_commit_signal(current_task, SIGPIPE, NULL);
                return done ? (ssize_t)done : -EPIPE;
            }
            if (pipe->ptr >= PIPE_BUFF ||
                pipe->nr_buffers >= PIPE_MAX_BUFFERS) {
                spin_unlock(&pipe->lock);
                if (done)
                    return (ssize_t)done;
                int ret = pipefs_wait_writable(file, nonblock);
                if (ret < 0)
                    return ret;
                continue;
            }

            chunk = MIN(chunk, (size_t)PIPE_BUFF - pipe->ptr);
            was_empty = pipe->ptr == 0;
            if (!address_ref(page_phys)) {
                spin_unlock(&pipe->lock);
                return done ? (ssize_t)done : -EFAULT;
            }
            ssize_t ret = pipefs_append_page_locked(pipe, page_phys, page_off,
                                                    chunk, true);
            if (ret <= 0) {
                address_release(page_phys);
                spin_unlock(&pipe->lock);
                if (done)
                    return (ssize_t)done;
                if (ret < 0)
                    return ret;
                ret = pipefs_wait_writable(file, nonblock);
                if (ret < 0)
                    return ret;
                continue;
            }
            pipefs_update_size_locked(pipe, file);
            if (was_empty && pipe->read_node)
                read_node = vfs_igrab(pipe->read_node);
            spin_unlock(&pipe->lock);

            if (read_node) {
                pipefs_notify_node(read_node, EPOLLIN | EPOLLRDNORM);
                vfs_iput(read_node);
            }
            done += (size_t)ret;
            iov_off += (size_t)ret;
        }
    }

    return (ssize_t)done;
}

uint64_t sys_pipe(int pipefd[2], uint64_t flags) {
    struct vfs_file *read_file = NULL;
    struct vfs_file *write_file = NULL;
    pipe_info_t *info;
    int i1, i2;
    int ret;

    if (!pipefd)
        return -EFAULT;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;

    info = pipefs_alloc_info();
    if (!info)
        return -ENOMEM;

    ret = pipefs_create_endpoint(&read_file, info, false,
                                 O_RDONLY | (flags & O_NONBLOCK));
    if (ret < 0)
        goto out_free_pipe;

    ret = pipefs_create_endpoint(&write_file, info, true,
                                 O_WRONLY | (flags & O_NONBLOCK));
    if (ret < 0)
        goto out_close_read;

    i1 = task_install_file(current_task, read_file,
                           (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    if (i1 < 0) {
        ret = i1;
        goto out_close_both;
    }
    spin_lock(&info->lock);
    info->read_fds++;
    spin_unlock(&info->lock);
    vfs_file_put(read_file);
    read_file = NULL;

    i2 = task_install_file(current_task, write_file,
                           (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, i1 + 1);
    if (i2 < 0) {
        task_close_file_descriptor(current_task, i1);
        ret = i2;
        goto out_close_both;
    }
    spin_lock(&info->lock);
    info->write_fds++;
    spin_unlock(&info->lock);
    vfs_file_put(write_file);
    write_file = NULL;

    int kpipefd[2] = {i1, i2};
    if (copy_to_user(pipefd, kpipefd, sizeof(kpipefd))) {
        task_close_file_descriptor(current_task, i1);
        task_close_file_descriptor(current_task, i2);
        return -EFAULT;
    }

    return 0;

out_close_both:
    if (write_file)
        vfs_close_file(write_file);
out_close_read:
    if (read_file)
        vfs_close_file(read_file);
    return ret;

out_free_pipe:
    pipefs_free_info(info);
    return ret;
}
