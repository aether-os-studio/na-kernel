/*
 * DRM ioctl implementation
 *
 * This file contains the implementation of DRM ioctl handling functions.
 * It separates the ioctl implementation from the core driver framework.
 */

#include <drivers/drm/drm.h>
#include <drivers/drm/drmfs.h>
#include <drivers/drm/drm_ioctl.h>
#include <drivers/drm/drm_core.h>
#include <drivers/logger.h>
#include <fs/fs_syscall.h>
#include <fs/dev.h>
#include <fs/proc.h>
#include <fs/vfs/vfs.h>
#include <mm/mm.h>
#include <task/task.h>
#include <init/callbacks.h>

static ssize_t drm_copy_to_user_ptr(uint64_t user_ptr, const void *src,
                                    size_t size) {
    if (!user_ptr || size == 0) {
        return 0;
    }

    if (copy_to_user((void *)(uintptr_t)user_ptr, src, size)) {
        return -EFAULT;
    }

    return 0;
}

#define DRM_MAX_PRIME_EXPORTS 256
#define DRM_MAX_DUMB_EXPORTS 512
#define DRM_MAX_USER_BLOBS 256
#define DRM_USER_BLOB_MAX_SIZE (64 * 1024)
#define DRM_BLOB_ID_USER_BASE 0x30000000U
#define DRM_BLOB_ID_USER_LAST 0x3fffffffU

#define DMA_BUF_BASE 'b'
#define DMA_BUF_NAME_LEN 32

#define DMA_BUF_SYNC_READ (1U << 0)
#define DMA_BUF_SYNC_WRITE (2U << 0)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_END (1U << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK (DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)

struct dma_buf_sync {
    uint64_t flags;
};

struct dma_buf_export_sync_file {
    uint32_t flags;
    int32_t fd;
};

struct dma_buf_import_sync_file {
    uint32_t flags;
    int32_t fd;
};

struct sync_merge_data {
    char name[32];
    int32_t fd2;
    int32_t fence;
    uint32_t flags;
    uint32_t pad;
};

struct sync_fence_info {
    char obj_name[32];
    char driver_name[32];
    int32_t status;
    uint32_t flags;
    uint64_t timestamp_ns;
};

struct sync_file_info {
    char name[32];
    int32_t status;
    uint32_t flags;
    uint32_t num_fences;
    uint32_t pad;
    uint64_t sync_fence_info;
};

struct sync_set_deadline {
    uint64_t deadline_ns;
    uint64_t pad;
};

#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#define DMA_BUF_IOCTL_SET_NAME _IOW(DMA_BUF_BASE, 1, char[DMA_BUF_NAME_LEN])
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE                                         \
    _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE                                         \
    _IOW(DMA_BUF_BASE, 3, struct dma_buf_import_sync_file)

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
#define SYNC_IOC_SET_DEADLINE _IOW(SYNC_IOC_MAGIC, 5, struct sync_set_deadline)

typedef struct drm_prime_export_entry {
    bool used;
    drm_device_t *dev;
    uint64_t inode;
    uint32_t handle;
} drm_prime_export_entry_t;

static drm_prime_export_entry_t drm_prime_exports[DRM_MAX_PRIME_EXPORTS];
static spinlock_t drm_prime_exports_lock = SPIN_INIT;

static const struct vfs_file_operations drmfdfs_dir_file_ops;
static const struct vfs_file_operations drmfdfs_prime_file_ops;
static const struct vfs_file_operations drmfdfs_sync_file_ops;
static const struct vfs_file_operations drmfdfs_drm_file_ops;

typedef struct drm_syncfd_ctx drm_syncfd_ctx_t;
static drm_syncfd_ctx_t *drm_syncfd_create_immediate_ctx(const char *name,
                                                         bool signaled);
static drm_syncfd_ctx_t *drm_syncfd_create_syncobj_ctx(drm_device_t *dev,
                                                       uint64_t owner_pid,
                                                       uint32_t handle,
                                                       uint64_t point,
                                                       const char *name);
static ssize_t drm_syncfd_create_fd(drm_syncfd_ctx_t *ctx, uint32_t flags);
static void drm_syncfd_ctx_put(drm_syncfd_ctx_t *ctx);
static bool drm_syncobj_is_signaled(drm_device_t *dev, uint64_t owner_pid,
                                    uint32_t handle, uint64_t point);

typedef struct drm_prime_fd_ctx {
    vfs_node_t *node;
    drm_device_t *dev;
    uint32_t handle;
    uint64_t phys;
    uint64_t size;
    char name[DMA_BUF_NAME_LEN];
} drm_prime_fd_ctx_t;

static inline drm_file_t *drm_file_from_data(void *data) {
    drm_file_t *file = (drm_file_t *)data;
    if (file && file->magic == DRM_FILE_MAGIC && file->dev) {
        return file;
    }
    return NULL;
}

static inline drm_file_t *drm_file_from_vfs_file(fd_t *fd) {
    drm_file_t *file = (drm_file_t *)device_file_private(fd);
    if (!file && fd)
        file = (drm_file_t *)fd->private_data;
    if (file && file->magic == DRM_FILE_MAGIC && file->dev) {
        return file;
    }
    return NULL;
}

static inline drm_file_t *drm_current_file(void *data, fd_t *fd) {
    drm_file_t *file = drm_file_from_vfs_file(fd);
    if (file) {
        return file;
    }
    return drm_file_from_data(data);
}

static inline void *drm_effective_ioctl_data(void *data, fd_t *fd) {
    drm_file_t *file = drm_current_file(data, fd);
    return file ? (void *)file : data;
}

static loff_t drmfdfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int drmfdfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!file)
        return -EINVAL;
    file->private_data = inode ? inode->i_private : NULL;
    return 0;
}

static int drmfdfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (!file)
        return 0;
    file->private_data = NULL;
    return 0;
}

static drm_prime_fd_ctx_t *drm_primefd_file_ctx(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (drm_prime_fd_ctx_t *)file->private_data;
    if (!drmfs_owns_file(file)) {
        return NULL;
    }
    return (drm_prime_fd_ctx_t *)file->f_inode->i_private;
}

static ssize_t drm_primefd_read(struct vfs_file *fd, void *buf, size_t len,
                                loff_t *ppos) {
    uint64_t offset = ppos ? (uint64_t)*ppos : fd_get_offset(fd);
    drm_prime_fd_ctx_t *ctx = drm_primefd_file_ctx(fd);
    if (!ctx || !buf || ctx->phys == 0 || offset >= ctx->size) {
        return 0;
    }

    uint64_t copy_len = MIN(len, ctx->size - offset);
    memcpy(buf, (void *)(uintptr_t)phys_to_virt(ctx->phys + offset), copy_len);
    if (ppos)
        *ppos += (loff_t)copy_len;
    return (ssize_t)copy_len;
}

static ssize_t drm_primefd_write(struct vfs_file *fd, const void *buf,
                                 size_t len, loff_t *ppos) {
    uint64_t offset = ppos ? (uint64_t)*ppos : fd_get_offset(fd);
    drm_prime_fd_ctx_t *ctx = drm_primefd_file_ctx(fd);
    if (!ctx || !buf || ctx->phys == 0 || offset >= ctx->size) {
        return 0;
    }

    uint64_t copy_len = MIN(len, ctx->size - offset);
    memcpy((void *)(uintptr_t)phys_to_virt(ctx->phys + offset), buf, copy_len);
    if (ppos)
        *ppos += (loff_t)copy_len;
    return (ssize_t)copy_len;
}

static int drm_primefd_release(struct vfs_inode *inode, struct vfs_file *file) {
    drm_prime_fd_ctx_t *ctx = drm_primefd_file_ctx(file);
    if (!ctx)
        return 0;
    if (ctx->dev && ctx->dev->op && ctx->dev->op->driver_ioctl &&
        ctx->handle != 0) {
        struct drm_gem_close close = {.handle = ctx->handle};
        ssize_t ret = ctx->dev->op->driver_ioctl(ctx->dev, DRM_IOCTL_GEM_CLOSE,
                                                 &close, false, NULL);
        (void)ret;
    }
    if (file)
        file->private_data = NULL;
    if (inode && inode->i_private == ctx)
        inode->i_private = NULL;
    free(ctx);
    return 0;
}

static int drm_primefd_resize(struct vfs_inode *node, uint64_t size) {
    drm_prime_fd_ctx_t *ctx =
        node ? (drm_prime_fd_ctx_t *)node->i_private : NULL;
    if (!ctx) {
        return -EINVAL;
    }

    ctx->size = MIN(size, ctx->size);
    if (ctx->node) {
        ctx->node->i_size = ctx->size;
    }

    return 0;
}

static long drm_primefs_ioctl(struct vfs_file *fd, unsigned long cmd,
                              unsigned long arg) {
    drm_prime_fd_ctx_t *ctx = drm_primefd_file_ctx(fd);
    if (!ctx) {
        return -EBADF;
    }

    uint32_t ioctl_cmd = (uint32_t)(cmd & 0xffffffffU);
    switch (ioctl_cmd) {
    case DMA_BUF_IOCTL_SYNC: {
        struct dma_buf_sync sync = {0};
        if (!arg ||
            copy_from_user(&sync, (void *)(uintptr_t)arg, sizeof(sync))) {
            return -EFAULT;
        }

        if (sync.flags & ~DMA_BUF_SYNC_VALID_FLAGS_MASK) {
            return -EINVAL;
        }

        return 0;
    }
    case DMA_BUF_IOCTL_SET_NAME: {
        char name[DMA_BUF_NAME_LEN] = {0};
        if (!arg ||
            copy_from_user(name, (void *)(uintptr_t)arg, sizeof(name))) {
            return -EFAULT;
        }

        memcpy(ctx->name, name, sizeof(ctx->name));
        ctx->name[DMA_BUF_NAME_LEN - 1] = '\0';
        return 0;
    }
    case DMA_BUF_IOCTL_EXPORT_SYNC_FILE: {
        struct dma_buf_export_sync_file export_sync = {0};
        if (!arg || copy_from_user(&export_sync, (void *)(uintptr_t)arg,
                                   sizeof(export_sync))) {
            return -EFAULT;
        }

        if ((export_sync.flags & ~DMA_BUF_SYNC_RW) ||
            !(export_sync.flags & DMA_BUF_SYNC_RW)) {
            return -EINVAL;
        }

        drm_syncfd_ctx_t *ctx =
            drm_syncfd_create_immediate_ctx("dma-buf", true);
        if (!ctx) {
            return -ENOMEM;
        }

        ssize_t sync_fd = drm_syncfd_create_fd(ctx, DRM_CLOEXEC);
        drm_syncfd_ctx_put(ctx);
        if (sync_fd < 0) {
            return sync_fd;
        }

        export_sync.fd = (int32_t)sync_fd;
        if (copy_to_user((void *)(uintptr_t)arg, &export_sync,
                         sizeof(export_sync))) {
            sys_close((uint64_t)sync_fd);
            return -EFAULT;
        }

        return 0;
    }
    case DMA_BUF_IOCTL_IMPORT_SYNC_FILE: {
        struct dma_buf_import_sync_file import_sync = {0};
        if (!arg || copy_from_user(&import_sync, (void *)(uintptr_t)arg,
                                   sizeof(import_sync))) {
            return -EFAULT;
        }

        if ((import_sync.flags & ~DMA_BUF_SYNC_RW) ||
            !(import_sync.flags & DMA_BUF_SYNC_RW)) {
            return -EINVAL;
        }

        if (import_sync.fd < 0 || import_sync.fd >= MAX_FD_NUM) {
            return -EBADF;
        }

        int ret = -EBADF;
        fd_t *fd_obj = task_get_file(current_task, import_sync.fd);
        if (fd_obj) {
            ret = 0;
            vfs_file_put(fd_obj);
        }
        return ret;
    }
    default:
        return -ENOTTY;
    }
}

static void *drm_primefd_mmap(struct vfs_file *file, void *addr, size_t offset,
                              size_t size, size_t prot, uint64_t flags) {
    (void)size;
    (void)prot;
    (void)flags;
    drm_prime_fd_ctx_t *ctx = drm_primefd_file_ctx(file);
    if (!ctx || !addr || ctx->phys == 0 || offset >= ctx->size ||
        !current_task || !current_task->mm) {
        return (void *)(int64_t)-EINVAL;
    }

    uint64_t map_len = MIN(size, ctx->size - offset);
    if (map_len == 0) {
        return addr;
    }

    uint64_t page_delta = offset & (PAGE_SIZE - 1);
    uint64_t map_phys = PADDING_DOWN(ctx->phys + offset, PAGE_SIZE);
    uint64_t map_addr = PADDING_DOWN((uint64_t)addr, PAGE_SIZE);
    uint64_t rounded_len = PADDING_UP(map_len + page_delta, PAGE_SIZE);

    if (map_page_range(task_mm_pgdir(current_task->mm), map_addr, map_phys,
                       rounded_len, PT_FLAG_R | PT_FLAG_W | PT_FLAG_U) != 0) {
        return (void *)(int64_t)-ENOMEM;
    }

    return addr;
}

static const struct vfs_file_operations drmfdfs_dir_file_ops = {
    .llseek = drmfdfs_llseek,
    .open = drmfdfs_open,
    .release = drmfdfs_release,
};

static const struct vfs_file_operations drmfdfs_prime_file_ops = {
    .llseek = drmfdfs_llseek,
    .read = drm_primefd_read,
    .write = drm_primefd_write,
    .mmap = drm_primefd_mmap,
    .unlocked_ioctl = drm_primefs_ioctl,
    .open = drmfdfs_open,
    .release = drm_primefd_release,
};

typedef enum drm_syncfd_kind {
    DRM_SYNCFD_KIND_IMMEDIATE = 0,
    DRM_SYNCFD_KIND_SYNCOBJ = 1,
    DRM_SYNCFD_KIND_MERGE = 2,
} drm_syncfd_kind_t;

struct drm_syncfd_ctx {
    vfs_node_t *node;
    spinlock_t lock;
    struct llist_header link;
    uint32_t refs;
    drm_syncfd_kind_t kind;
    bool signaled;
    int32_t status;
    uint64_t timestamp_ns;
    uint64_t deadline_ns;
    char name[32];
    union {
        struct {
            drm_device_t *dev;
            uint64_t owner_pid;
            uint32_t handle;
            uint64_t point;
        } syncobj;
        struct {
            struct drm_syncfd_ctx *left;
            struct drm_syncfd_ctx *right;
        } merge;
    } u;
};

static struct llist_header drm_syncfd_contexts;
static spinlock_t drm_syncfd_contexts_lock = SPIN_INIT;
static bool drm_syncfd_contexts_initialized = false;

static drm_syncfd_ctx_t *drm_syncfd_file_ctx(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (drm_syncfd_ctx_t *)file->private_data;
    if (!drmfs_owns_file(file)) {
        return NULL;
    }
    return (drm_syncfd_ctx_t *)file->f_inode->i_private;
}

static void drm_syncfd_ctx_get(drm_syncfd_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    __atomic_add_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL);
}

static void drm_syncfd_ctx_unlink(drm_syncfd_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    spin_lock(&drm_syncfd_contexts_lock);
    if (!llist_empty(&ctx->link)) {
        llist_delete(&ctx->link);
    }
    spin_unlock(&drm_syncfd_contexts_lock);
}

static void drm_syncfd_ctx_put(drm_syncfd_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (__atomic_sub_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }

    if (ctx->kind == DRM_SYNCFD_KIND_MERGE) {
        drm_syncfd_ctx_put(ctx->u.merge.left);
        drm_syncfd_ctx_put(ctx->u.merge.right);
    }
    if (ctx->node) {
        vfs_iput(ctx->node);
        ctx->node = NULL;
    }
    free(ctx);
}

static bool drm_syncfd_update_state(drm_syncfd_ctx_t *ctx);

static bool drm_syncfd_is_signaled(drm_syncfd_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }
    return drm_syncfd_update_state(ctx);
}

static bool drm_syncfd_update_state(drm_syncfd_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }

    spin_lock(&ctx->lock);
    if (ctx->signaled) {
        spin_unlock(&ctx->lock);
        return true;
    }
    drm_syncfd_kind_t kind = ctx->kind;
    drm_device_t *dev = NULL;
    uint64_t owner_pid = 0;
    uint32_t handle = 0;
    uint64_t point = 0;
    drm_syncfd_ctx_t *left = NULL;
    drm_syncfd_ctx_t *right = NULL;
    if (kind == DRM_SYNCFD_KIND_SYNCOBJ) {
        dev = ctx->u.syncobj.dev;
        owner_pid = ctx->u.syncobj.owner_pid;
        handle = ctx->u.syncobj.handle;
        point = ctx->u.syncobj.point;
    } else if (kind == DRM_SYNCFD_KIND_MERGE) {
        left = ctx->u.merge.left;
        right = ctx->u.merge.right;
    }
    spin_unlock(&ctx->lock);

    bool signaled = false;
    switch (kind) {
    case DRM_SYNCFD_KIND_IMMEDIATE:
        signaled = true;
        break;
    case DRM_SYNCFD_KIND_SYNCOBJ:
        signaled = drm_syncobj_is_signaled(dev, owner_pid, handle, point);
        break;
    case DRM_SYNCFD_KIND_MERGE:
        signaled =
            drm_syncfd_is_signaled(left) && drm_syncfd_is_signaled(right);
        break;
    default:
        break;
    }

    if (!signaled) {
        return false;
    }

    spin_lock(&ctx->lock);
    if (!ctx->signaled) {
        ctx->signaled = true;
        ctx->status = 1;
        ctx->timestamp_ns = nano_time();
    }
    spin_unlock(&ctx->lock);

    return true;
}

static long drm_syncfdfs_ioctl(struct vfs_file *fd, unsigned long cmd,
                               unsigned long arg) {
    drm_syncfd_ctx_t *ctx = drm_syncfd_file_ctx(fd);
    if (!ctx) {
        return -EBADF;
    }

    switch ((uint32_t)cmd) {
    case SYNC_IOC_SET_DEADLINE: {
        struct sync_set_deadline deadline = {0};
        if (!arg || copy_from_user(&deadline, (void *)(uintptr_t)arg,
                                   sizeof(deadline))) {
            return -EFAULT;
        }
        if (deadline.pad != 0) {
            return -EINVAL;
        }

        spin_lock(&ctx->lock);
        ctx->deadline_ns = deadline.deadline_ns;
        spin_unlock(&ctx->lock);
        return 0;
    }
    case SYNC_IOC_FILE_INFO: {
        struct sync_file_info info = {0};
        if (!arg ||
            copy_from_user(&info, (void *)(uintptr_t)arg, sizeof(info))) {
            return -EFAULT;
        }

        struct sync_fence_info fence = {0};
        drm_syncfd_update_state(ctx);

        spin_lock(&ctx->lock);
        memcpy(info.name, ctx->name, sizeof(info.name));
        info.status = ctx->status;
        info.flags = 0;
        uint32_t requested = info.num_fences;
        info.num_fences = 1;
        fence.status = ctx->status;
        fence.flags = 0;
        fence.timestamp_ns = ctx->timestamp_ns;
        memcpy(fence.obj_name, ctx->name, sizeof(fence.obj_name));
        memcpy(fence.driver_name, "naos", 5);
        spin_unlock(&ctx->lock);

        if (requested > 0 && info.sync_fence_info) {
            if (copy_to_user((void *)(uintptr_t)info.sync_fence_info, &fence,
                             sizeof(fence))) {
                return -EFAULT;
            }
        }

        if (copy_to_user((void *)(uintptr_t)arg, &info, sizeof(info))) {
            return -EFAULT;
        }
        return 0;
    }
    case SYNC_IOC_MERGE: {
        struct sync_merge_data merge = {0};
        if (!arg ||
            copy_from_user(&merge, (void *)(uintptr_t)arg, sizeof(merge))) {
            return -EFAULT;
        }
        if (merge.flags != 0 || merge.pad != 0) {
            return -EINVAL;
        }
        if (merge.fd2 < 0 || merge.fd2 >= MAX_FD_NUM) {
            return -EBADF;
        }

        drm_syncfd_ctx_t *other = NULL;
        fd_t *fd_obj = task_get_file(current_task, merge.fd2);
        drm_syncfd_ctx_t *ctx2 = drm_syncfd_file_ctx(fd_obj);
        if (ctx2) {
            other = ctx2;
            drm_syncfd_ctx_get(other);
        }
        vfs_file_put(fd_obj);
        if (!other) {
            return -EINVAL;
        }

        drm_syncfd_ctx_t *merged = calloc(1, sizeof(*merged));
        if (!merged) {
            drm_syncfd_ctx_put(other);
            return -ENOMEM;
        }
        spin_init(&merged->lock);
        llist_init_head(&merged->link);
        merged->refs = 1;
        merged->kind = DRM_SYNCFD_KIND_MERGE;
        merged->status = 0;
        merged->u.merge.left = ctx;
        merged->u.merge.right = other;
        drm_syncfd_ctx_get(ctx);
        if (merge.name[0]) {
            memcpy(merged->name, merge.name, sizeof(merged->name));
        } else {
            memcpy(merged->name, "naos-sync-merge", 16);
        }

        ssize_t new_fd = drm_syncfd_create_fd(merged, 0);
        drm_syncfd_ctx_put(merged);
        if (new_fd < 0) {
            return (int)new_fd;
        }

        merge.fence = (int32_t)new_fd;
        if (copy_to_user((void *)(uintptr_t)arg, &merge, sizeof(merge))) {
            sys_close((uint64_t)new_fd);
            return -EFAULT;
        }
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static __poll_t drm_syncfdfs_poll(struct vfs_file *file,
                                  struct vfs_poll_table *pt) {
    drm_syncfd_ctx_t *ctx = drm_syncfd_file_ctx(file);
    size_t events = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP;
    (void)pt;
    if (!ctx) {
        return EPOLLNVAL;
    }

    int revents = 0;
    if ((events & EPOLLIN) && drm_syncfd_is_signaled(ctx)) {
        revents |= EPOLLIN;
    }
    if (events & EPOLLOUT) {
        revents |= EPOLLOUT;
    }
    return revents;
}

static int drm_syncfdfs_release(struct vfs_inode *inode,
                                struct vfs_file *file) {
    drm_syncfd_ctx_t *ctx = drm_syncfd_file_ctx(file);
    vfs_node_t *node = NULL;
    (void)inode;
    if (!ctx) {
        return 0;
    }

    drm_syncfd_ctx_unlink(ctx);

    spin_lock(&ctx->lock);
    node = ctx->node;
    ctx->node = NULL;
    spin_unlock(&ctx->lock);
    vfs_iput(node);

    drm_syncfd_ctx_put(ctx);
    if (!drm_syncfd_contexts_initialized) {
        llist_init_head(&drm_syncfd_contexts);
        drm_syncfd_contexts_initialized = true;
    }
    return 0;
}

static const struct vfs_file_operations drmfdfs_sync_file_ops = {
    .llseek = drmfdfs_llseek,
    .unlocked_ioctl = drm_syncfdfs_ioctl,
    .poll = drm_syncfdfs_poll,
    .open = drmfdfs_open,
    .release = drm_syncfdfs_release,
};

static int drm_leasefd_release(struct vfs_inode *inode, struct vfs_file *file) {
    drm_file_t *drm_file = drm_file_from_vfs_file(file);
    drm_device_t *dev = drm_file ? drm_file->dev : NULL;
    (void)inode;

    if (!drm_file) {
        return 0;
    }

    if (dev) {
        spin_lock(&dev->lease_lock);
        for (uint32_t i = 0; i < DRM_MAX_LEASES_PER_DEVICE; i++) {
            if (dev->leases[i].active && dev->leases[i].file == drm_file) {
                dev->leases[i].active = false;
                dev->leases[i].file = NULL;
                break;
            }
        }
        spin_unlock(&dev->lease_lock);
    }

    drm_cancel_file_events(drm_file);

    if (drm_file->event_node) {
        vfs_iput(drm_file->event_node);
        drm_file->event_node = NULL;
    }

    file->private_data = NULL;
    memset(drm_file, 0, sizeof(*drm_file));
    free(drm_file);
    return 0;
}

static long drm_leasefd_ioctl(struct vfs_file *file, unsigned long cmd,
                              unsigned long arg) {
    drm_file_t *drm_file = drm_file_from_vfs_file(file);
    if (!drm_file) {
        return -EBADF;
    }
    return drm_ioctl(drm_file, (ssize_t)cmd, (ssize_t)arg, file);
}

static ssize_t drm_leasefd_read(struct vfs_file *file, void *buf, size_t count,
                                loff_t *ppos) {
    drm_file_t *drm_file = drm_file_from_vfs_file(file);
    (void)ppos;
    if (!drm_file) {
        return -EBADF;
    }
    return drm_read(drm_file, buf, 0, count, file);
}

static __poll_t drm_leasefd_poll(struct vfs_file *file,
                                 struct vfs_poll_table *pt) {
    drm_file_t *drm_file = drm_file_from_vfs_file(file);
    (void)pt;
    if (!drm_file) {
        return EPOLLNVAL;
    }
    return (__poll_t)drm_poll(drm_file, EPOLLIN | EPOLLOUT | EPOLLPRI, file);
}

static void *drm_leasefd_mmap(struct vfs_file *file, void *addr, size_t offset,
                              size_t size, size_t prot, uint64_t flags) {
    drm_file_t *drm_file = drm_file_from_vfs_file(file);
    (void)prot;
    (void)flags;
    if (!drm_file) {
        return (void *)(int64_t)-EBADF;
    }
    return drm_map(drm_file, addr, offset, size);
}

static const struct vfs_file_operations drmfdfs_drm_file_ops = {
    .llseek = drmfdfs_llseek,
    .read = drm_leasefd_read,
    .mmap = drm_leasefd_mmap,
    .unlocked_ioctl = drm_leasefd_ioctl,
    .poll = drm_leasefd_poll,
    .open = drmfdfs_open,
    .release = drm_leasefd_release,
};

static bool drm_mode_object_exists(drm_device_t *dev, uint32_t object_id) {
    if (!dev || object_id == 0) {
        return false;
    }

    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
        if (dev->resource_mgr.connectors[i] &&
            dev->resource_mgr.connectors[i]->id == object_id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < DRM_MAX_CRTCS_PER_DEVICE; i++) {
        if (dev->resource_mgr.crtcs[i] &&
            dev->resource_mgr.crtcs[i]->id == object_id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
        if (dev->resource_mgr.encoders[i] &&
            dev->resource_mgr.encoders[i]->id == object_id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < DRM_MAX_PLANES_PER_DEVICE; i++) {
        if (dev->resource_mgr.planes[i] &&
            dev->resource_mgr.planes[i]->id == object_id) {
            return true;
        }
    }

    return false;
}

static bool drm_file_has_object(drm_file_t *file, uint32_t object_id) {
    if (object_id == 0) {
        return false;
    }

    if (!file) {
        return true;
    }

    if (!file->lessee) {
        return true;
    }

    if (file->revoked) {
        return false;
    }

    for (uint32_t i = 0; i < file->object_count; i++) {
        if (file->object_ids[i] == object_id) {
            return true;
        }
    }

    return false;
}

static bool drm_file_has_encoder(drm_device_t *dev, drm_file_t *file,
                                 drm_encoder_t *encoder) {
    if (!encoder) {
        return false;
    }

    if (drm_file_has_object(file, encoder->id)) {
        return true;
    }

    if (!dev || !file || !file->lessee || file->revoked) {
        return false;
    }

    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
        drm_connector_t *connector = dev->resource_mgr.connectors[i];
        if (connector && connector->encoder_id == encoder->id &&
            drm_file_has_object(file, connector->id)) {
            return true;
        }
    }

    return false;
}

static bool drm_file_can_create_lease(drm_file_t *file) {
    return file && file->magic == DRM_FILE_MAGIC && !file->lessee &&
           file->type == DRM_MINOR_PRIMARY;
}

static uint32_t drm_file_lessor_id(drm_file_t *file) {
    if (!file || !file->lessee) {
        return 0;
    }
    return file->lessor_id;
}

static int drm_leasefd_create(drm_device_t *dev, drm_file_t *owner,
                              uint32_t *object_ids, uint32_t object_count,
                              uint32_t flags, uint32_t *out_lease_id) {
    drm_file_t *lease_file;
    struct vfs_file *file = NULL;
    struct vfs_inode *inode = NULL;
    uint32_t lease_id;
    int ret;
    int slot = -1;

    if (!dev || !owner || !object_ids || object_count == 0 ||
        object_count > DRM_MAX_LEASE_OBJECTS || !out_lease_id) {
        return -EINVAL;
    }

    lease_file = calloc(1, sizeof(*lease_file));
    if (!lease_file) {
        return -ENOMEM;
    }

    lease_file->magic = DRM_FILE_MAGIC;
    lease_file->dev = dev;
    lease_file->type = DRM_MINOR_PRIMARY;
    lease_file->lessee = true;
    lease_file->lessor_id = owner->lessee ? owner->lease_id : 0;
    lease_file->object_count = object_count;
    memcpy(lease_file->object_ids, object_ids,
           object_count * sizeof(object_ids[0]));

    spin_lock(&dev->lease_lock);
    for (uint32_t i = 0; i < DRM_MAX_LEASES_PER_DEVICE; i++) {
        if (!dev->leases[i].active) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock(&dev->lease_lock);
        free(lease_file);
        return -ENOSPC;
    }
    lease_id = dev->next_lease_id++;
    if (dev->next_lease_id == 0) {
        dev->next_lease_id = 1;
    }
    lease_file->lease_id = lease_id;
    dev->leases[slot].active = true;
    dev->leases[slot].file = lease_file;
    dev->leases[slot].lease_id = lease_id;
    dev->leases[slot].lessor_id = drm_file_lessor_id(owner);
    spin_unlock(&dev->lease_lock);

    ret = drmfs_create_file("drmlease", &drmfdfs_drm_file_ops, lease_file,
                            S_IFCHR | 0600, O_RDWR | (flags & O_NONBLOCK),
                            &file, &inode);
    if (ret < 0) {
        spin_lock(&dev->lease_lock);
        if (slot >= 0) {
            dev->leases[slot].active = false;
            dev->leases[slot].file = NULL;
        }
        spin_unlock(&dev->lease_lock);
        free(lease_file);
        return ret;
    }
    lease_file->event_node = vfs_igrab(file->f_inode);

    lease_file->lease_id = lease_id;
    (void)inode;

    ret = task_install_file(current_task, file,
                            (flags & DRM_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    if (ret < 0) {
        spin_lock(&dev->lease_lock);
        if (slot >= 0 && dev->leases[slot].file == lease_file) {
            dev->leases[slot].active = false;
            dev->leases[slot].file = NULL;
        }
        spin_unlock(&dev->lease_lock);
        return ret;
    }

    *out_lease_id = lease_id;
    return ret;
}

static ssize_t drm_syncfd_create_fd(drm_syncfd_ctx_t *ctx, uint32_t flags) {
    if (!ctx) {
        return -EINVAL;
    }

    spin_lock(&drm_syncfd_contexts_lock);
    if (!drm_syncfd_contexts_initialized) {
        llist_init_head(&drm_syncfd_contexts);
        drm_syncfd_contexts_initialized = true;
    }
    if (llist_empty(&ctx->link)) {
        llist_append(&drm_syncfd_contexts, &ctx->link);
    }
    spin_unlock(&drm_syncfd_contexts_lock);

    drm_syncfd_ctx_get(ctx);
    struct vfs_file *file = NULL;
    struct vfs_inode *inode = NULL;
    int ret = drmfs_create_file("drmsync", &drmfdfs_sync_file_ops, ctx,
                                S_IFCHR | 0600, O_RDWR, &file, &inode);
    if (ret < 0) {
        drm_syncfd_ctx_unlink(ctx);
        drm_syncfd_ctx_put(ctx);
        return ret;
    }
    ctx->node = inode;
    vfs_igrab(ctx->node);

    ret = task_install_file(current_task, file,
                            (flags & DRM_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    if (ret < 0) {
        drm_syncfd_ctx_unlink(ctx);
        return ret;
    }

    drm_syncfd_update_state(ctx);
    return ret;
}

static drm_syncfd_ctx_t *drm_syncfd_create_immediate_ctx(const char *name,
                                                         bool signaled) {
    drm_syncfd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    spin_init(&ctx->lock);
    llist_init_head(&ctx->link);
    ctx->refs = 1;
    ctx->kind = DRM_SYNCFD_KIND_IMMEDIATE;
    ctx->signaled = signaled;
    ctx->status = signaled ? 1 : 0;
    ctx->timestamp_ns = signaled ? nano_time() : 0;
    if (name && name[0]) {
        memcpy(ctx->name, name, MIN(sizeof(ctx->name), strlen(name)));
    } else {
        memcpy(ctx->name, "naos-sync", 10);
    }
    return ctx;
}

static drm_syncfd_ctx_t *drm_syncfd_create_syncobj_ctx(drm_device_t *dev,
                                                       uint64_t owner_pid,
                                                       uint32_t handle,
                                                       uint64_t point,
                                                       const char *name) {
    drm_syncfd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    spin_init(&ctx->lock);
    llist_init_head(&ctx->link);
    ctx->refs = 1;
    ctx->kind = DRM_SYNCFD_KIND_SYNCOBJ;
    ctx->status = 0;
    ctx->u.syncobj.dev = dev;
    ctx->u.syncobj.owner_pid = owner_pid;
    ctx->u.syncobj.handle = handle;
    ctx->u.syncobj.point = point ? point : 1;
    if (name && name[0]) {
        memcpy(ctx->name, name, MIN(sizeof(ctx->name), strlen(name)));
    } else {
        memcpy(ctx->name, "naos-syncobj", 13);
    }
    return ctx;
}

static drm_syncfd_ctx_t *drm_syncfd_ctx_from_fd(int fd) {
    drm_syncfd_ctx_t *ctx = NULL;
    if (fd < 0 || fd >= MAX_FD_NUM) {
        return NULL;
    }
    fd_t *fd_obj = task_get_file(current_task, fd);
    drm_syncfd_ctx_t *sync_ctx = drm_syncfd_file_ctx(fd_obj);
    if (sync_ctx) {
        ctx = sync_ctx;
        drm_syncfd_ctx_get(ctx);
    }
    vfs_file_put(fd_obj);
    return ctx;
}

static void drm_syncfd_notify_all(void) {
    spin_lock(&drm_syncfd_contexts_lock);
    drm_syncfd_ctx_t *ctx, *tmp;
    llist_for_each(ctx, tmp, &drm_syncfd_contexts, link) {
        drm_syncfd_update_state(ctx);
    }
    spin_unlock(&drm_syncfd_contexts_lock);
}

ssize_t drm_primefd_create(drm_device_t *dev, uint32_t handle, uint64_t phys,
                           uint64_t size, uint32_t flags) {
    drm_prime_fd_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        return -ENOMEM;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->dev = dev;
    ctx->handle = handle;
    ctx->phys = phys;
    ctx->size = size;

    struct vfs_file *file = NULL;
    struct vfs_inode *inode = NULL;
    int ret = drmfs_create_file("drmprime", &drmfdfs_prime_file_ops, ctx,
                                S_IFREG | 0600, O_RDWR, &file, &inode);
    if (ret < 0) {
        free(ctx);
        return ret;
    }

    inode->i_size = size;
    ctx->node = inode;

    ret = task_install_file(current_task, file,
                            (flags & DRM_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    if (ret < 0) {
        return ret;
    }

    return ret;
}

static ssize_t drm_prime_get_handle_phys(drm_device_t *dev, uint32_t handle,
                                         uint64_t *phys) {
    if (!dev || !dev->op || !phys) {
        return -ENOTTY;
    }

    if (dev->op->get_dumb_map) {
        uint64_t size = 0;
        return dev->op->get_dumb_map(dev, handle, phys, &size);
    }

    if (!dev->op->map_dumb) {
        return -ENOTTY;
    }

    struct drm_mode_map_dumb map = {0};
    map.handle = handle;

    int ret = dev->op->map_dumb(dev, &map, NULL);
    if (ret) {
        return ret;
    }

    *phys = map.offset;
    return 0;
}

typedef struct drm_dumb_export_entry {
    bool used;
    drm_device_t *dev;
    uint32_t handle;
    uint64_t size;
} drm_dumb_export_entry_t;

static drm_dumb_export_entry_t drm_dumb_exports[DRM_MAX_DUMB_EXPORTS];
static spinlock_t drm_dumb_exports_lock = SPIN_INIT;

typedef struct drm_user_blob_entry {
    bool used;
    drm_device_t *dev;
    uint32_t blob_id;
    uint32_t length;
    void *data;
} drm_user_blob_entry_t;

static drm_user_blob_entry_t drm_user_blobs[DRM_MAX_USER_BLOBS];
static spinlock_t drm_user_blobs_lock = SPIN_INIT;
static uint32_t drm_user_blob_next_id = DRM_BLOB_ID_USER_BASE + 1;

#define DRM_MAX_SYNCOBJS 1024

typedef struct drm_syncobj_entry {
    bool used;
    drm_device_t *dev;
    uint64_t owner_pid;
    uint32_t handle;
    uint64_t point; // 0 = unsignaled, >0 = signaled/timeline point
    struct vfs_file *eventfd_file;
} drm_syncobj_entry_t;

static drm_syncobj_entry_t drm_syncobjs[DRM_MAX_SYNCOBJS];
static spinlock_t drm_syncobjs_lock = SPIN_INIT;
static uint32_t drm_syncobj_next_handle = 1;

static inline uint64_t drm_syncobj_owner_pid(void) {
    if (!current_task) {
        return 0;
    }

    return current_task->tgid ? (uint64_t)current_task->tgid
                              : (uint64_t)current_task->pid;
}

static void drm_syncobj_prune_locked(void) {
    for (int i = 0; i < DRM_MAX_SYNCOBJS; i++) {
        if (!drm_syncobjs[i].used) {
            continue;
        }

        if (drm_syncobjs[i].owner_pid == 0 ||
            task_find_by_pid(drm_syncobjs[i].owner_pid) != NULL) {
            continue;
        }

        if (drm_syncobjs[i].eventfd_file)
            vfs_file_put(drm_syncobjs[i].eventfd_file);
        memset(&drm_syncobjs[i], 0, sizeof(drm_syncobjs[i]));
    }
}

static drm_syncobj_entry_t *drm_syncobj_lookup_locked(drm_device_t *dev,
                                                      uint64_t owner_pid,
                                                      uint32_t handle) {
    for (int i = 0; i < DRM_MAX_SYNCOBJS; i++) {
        if (!drm_syncobjs[i].used) {
            continue;
        }
        if (drm_syncobjs[i].dev != dev ||
            drm_syncobjs[i].owner_pid != owner_pid ||
            drm_syncobjs[i].handle != handle) {
            continue;
        }

        return &drm_syncobjs[i];
    }

    return NULL;
}

static bool drm_syncobj_is_signaled(drm_device_t *dev, uint64_t owner_pid,
                                    uint32_t handle, uint64_t point) {
    bool signaled = false;

    spin_lock(&drm_syncobjs_lock);
    drm_syncobj_entry_t *entry =
        drm_syncobj_lookup_locked(dev, owner_pid, handle);
    if (entry && entry->point >= (point ? point : 1)) {
        signaled = true;
    }
    spin_unlock(&drm_syncobjs_lock);

    return signaled;
}

static ssize_t drm_syncobj_alloc_handle_locked(uint32_t *out_handle) {
    if (!out_handle) {
        return -EINVAL;
    }

    uint32_t candidate = drm_syncobj_next_handle;
    for (uint32_t tries = 0; tries < UINT32_MAX; tries++) {
        if (candidate == 0) {
            candidate = 1;
        }

        bool exists = false;
        for (int i = 0; i < DRM_MAX_SYNCOBJS; i++) {
            if (drm_syncobjs[i].used && drm_syncobjs[i].handle == candidate) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            *out_handle = candidate;
            drm_syncobj_next_handle = candidate + 1;
            if (drm_syncobj_next_handle == 0) {
                drm_syncobj_next_handle = 1;
            }
            return 0;
        }

        candidate++;
    }

    return -ENOSPC;
}

static ssize_t drm_syncobj_create(drm_device_t *dev,
                                  struct drm_syncobj_create *create) {
    if (!dev || !create) {
        return -EINVAL;
    }

    if (create->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) {
        return -EINVAL;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    if (owner_pid == 0) {
        return -EINVAL;
    }

    spin_lock(&drm_syncobjs_lock);
    drm_syncobj_prune_locked();

    int free_slot = -1;
    for (int i = 0; i < DRM_MAX_SYNCOBJS; i++) {
        if (!drm_syncobjs[i].used) {
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0) {
        spin_unlock(&drm_syncobjs_lock);
        return -ENOSPC;
    }

    uint32_t handle = 0;
    ssize_t ret = drm_syncobj_alloc_handle_locked(&handle);
    if (ret < 0) {
        spin_unlock(&drm_syncobjs_lock);
        return ret;
    }

    drm_syncobjs[free_slot] = (drm_syncobj_entry_t){
        .used = true,
        .dev = dev,
        .owner_pid = owner_pid,
        .handle = handle,
        .point = (create->flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0,
        .eventfd_file = NULL,
    };

    create->handle = handle;
    spin_unlock(&drm_syncobjs_lock);
    return 0;
}

static ssize_t drm_syncobj_destroy(drm_device_t *dev,
                                   struct drm_syncobj_destroy *destroy) {
    if (!dev || !destroy || destroy->handle == 0) {
        return -EINVAL;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    spin_lock(&drm_syncobjs_lock);
    drm_syncobj_entry_t *entry =
        drm_syncobj_lookup_locked(dev, owner_pid, destroy->handle);
    if (!entry) {
        spin_unlock(&drm_syncobjs_lock);
        return -ENOENT;
    }

    if (entry->eventfd_file)
        vfs_file_put(entry->eventfd_file);
    memset(entry, 0, sizeof(*entry));
    spin_unlock(&drm_syncobjs_lock);
    return 0;
}

static ssize_t drm_syncobj_handle_to_fd(drm_device_t *dev,
                                        struct drm_syncobj_handle *h) {
    if (!dev || !h || h->handle == 0) {
        return -EINVAL;
    }

    uint32_t valid_flags =
        DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE | DRM_CLOEXEC;
    if (h->flags & ~valid_flags) {
        return -EINVAL;
    }

    if (!(h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) {
        return -ENOTTY;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    uint64_t point = 0;
    spin_lock(&drm_syncobjs_lock);
    drm_syncobj_entry_t *entry =
        drm_syncobj_lookup_locked(dev, owner_pid, h->handle);
    if (!entry) {
        spin_unlock(&drm_syncobjs_lock);
        return -ENOENT;
    }
    point = entry->point;
    spin_unlock(&drm_syncobjs_lock);

    drm_syncfd_ctx_t *ctx = drm_syncfd_create_syncobj_ctx(
        dev, owner_pid, h->handle, point ? point : 1, "drm-syncobj");
    if (!ctx) {
        return -ENOMEM;
    }
    ssize_t fd = drm_syncfd_create_fd(ctx, h->flags & DRM_CLOEXEC);
    drm_syncfd_ctx_put(ctx);
    if (fd < 0) {
        return fd;
    }

    h->fd = (int32_t)fd;
    return 0;
}

static ssize_t drm_syncobj_fd_to_handle(drm_device_t *dev,
                                        struct drm_syncobj_handle *h) {
    if (!dev || !h) {
        return -EINVAL;
    }

    if (h->flags & ~DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {
        return -EINVAL;
    }

    if (!(h->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)) {
        return -ENOTTY;
    }

    if (h->fd < 0 || h->fd >= MAX_FD_NUM) {
        return -EBADF;
    }

    uint64_t imported_point = 1;
    fd_t *fd_obj = task_get_file(current_task, h->fd);
    if (eventfd_is_file(fd_obj)) {
        eventfd_t *efd = eventfd_file_handle(fd_obj);
        if (efd) {
            imported_point =
                __atomic_load_n(&efd->count, __ATOMIC_ACQUIRE) ? 1 : 0;
        }
    } else {
        drm_syncfd_ctx_t *ctx = drm_syncfd_file_ctx(fd_obj);
        if (ctx) {
            imported_point = drm_syncfd_is_signaled(ctx) ? 1 : 0;
        }
    }
    vfs_file_put(fd_obj);

    struct drm_syncobj_create create = {
        .handle = 0,
        .flags = imported_point ? DRM_SYNCOBJ_CREATE_SIGNALED : 0,
    };

    ssize_t ret = drm_syncobj_create(dev, &create);
    if (ret < 0) {
        return ret;
    }

    h->handle = create.handle;
    return 0;
}

static ssize_t
drm_syncobj_wait_any_or_all(drm_device_t *dev, uint64_t owner_pid,
                            const uint32_t *handles, const uint64_t *points,
                            uint32_t count, bool wait_all,
                            int32_t *first_signaled, int64_t timeout_nsec) {
    if (!dev || !handles || count == 0) {
        return -EINVAL;
    }

    uint64_t deadline = 0;
    bool has_deadline = false;
    if (timeout_nsec >= 0) {
        deadline = (uint64_t)timeout_nsec;
        has_deadline = true;
    }

    while (true) {
        bool any_ok = false;
        bool all_ok = true;
        int32_t first_ok = -1;

        spin_lock(&drm_syncobjs_lock);
        for (uint32_t i = 0; i < count; i++) {
            uint64_t required = points ? points[i] : 1;
            drm_syncobj_entry_t *entry =
                drm_syncobj_lookup_locked(dev, owner_pid, handles[i]);
            if (!entry) {
                spin_unlock(&drm_syncobjs_lock);
                return -ENOENT;
            }

            bool ok = entry->point >= required;
            if (ok) {
                any_ok = true;
                if (first_ok < 0) {
                    first_ok = (int32_t)i;
                }
            } else {
                all_ok = false;
            }

            if (!wait_all && any_ok) {
                break;
            }
        }
        spin_unlock(&drm_syncobjs_lock);

        if (wait_all ? all_ok : any_ok) {
            if (!wait_all && first_signaled) {
                *first_signaled = first_ok < 0 ? 0 : first_ok;
            }
            return 0;
        }

        if (has_deadline && nano_time() >= deadline) {
            return -ETIME;
        }

        arch_enable_interrupt();
        arch_wait_for_interrupt();
        arch_disable_interrupt();
    }
}

static ssize_t drm_syncobj_wait(drm_device_t *dev, struct drm_syncobj_wait *w) {
    if (!dev || !w || w->count_handles == 0 || !w->handles) {
        return -EINVAL;
    }

    if (w->flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
        return -EINVAL;
    }

    uint32_t count = w->count_handles;
    if (count > DRM_MAX_SYNCOBJS) {
        return -EINVAL;
    }

    uint32_t *handles = malloc(sizeof(handles[0]) * count);
    if (!handles) {
        return -ENOMEM;
    }

    if (copy_from_user(handles, (void *)(uintptr_t)w->handles,
                       sizeof(handles[0]) * count)) {
        free(handles);
        return -EFAULT;
    }

    int32_t first_signaled = 0;
    ssize_t ret = drm_syncobj_wait_any_or_all(
        dev, drm_syncobj_owner_pid(), handles, NULL, count,
        !!(w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL), &first_signaled,
        w->timeout_nsec);
    free(handles);

    if (ret == 0 && !(w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
        w->first_signaled = (uint32_t)first_signaled;
    }

    return ret;
}

static ssize_t drm_syncobj_timeline_wait(drm_device_t *dev,
                                         struct drm_syncobj_timeline_wait *w) {
    if (!dev || !w || w->count_handles == 0 || !w->handles || !w->points) {
        return -EINVAL;
    }

    if (w->flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
        return -EINVAL;
    }

    uint32_t count = w->count_handles;
    if (count > DRM_MAX_SYNCOBJS) {
        return -EINVAL;
    }

    uint32_t *handles = malloc(sizeof(handles[0]) * count);
    uint64_t *points = malloc(sizeof(points[0]) * count);
    if (!handles || !points) {
        free(handles);
        free(points);
        return -ENOMEM;
    }

    if (copy_from_user(handles, (void *)(uintptr_t)w->handles,
                       sizeof(handles[0]) * count) ||
        copy_from_user(points, (void *)(uintptr_t)w->points,
                       sizeof(points[0]) * count)) {
        free(handles);
        free(points);
        return -EFAULT;
    }

    int32_t first_signaled = 0;
    ssize_t ret = drm_syncobj_wait_any_or_all(
        dev, drm_syncobj_owner_pid(), handles, points, count,
        !!(w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL), &first_signaled,
        w->timeout_nsec);
    free(handles);
    free(points);

    if (ret == 0 && !(w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
        w->first_signaled = (uint32_t)first_signaled;
    }

    return ret;
}

static ssize_t drm_syncobj_reset_or_signal(drm_device_t *dev,
                                           struct drm_syncobj_array *a,
                                           bool signal) {
    if (!dev || !a || a->count_handles == 0 || !a->handles) {
        return -EINVAL;
    }

    uint32_t count = a->count_handles;
    if (count > DRM_MAX_SYNCOBJS) {
        return -EINVAL;
    }

    uint32_t *handles = malloc(sizeof(handles[0]) * count);
    if (!handles) {
        return -ENOMEM;
    }
    if (copy_from_user(handles, (void *)(uintptr_t)a->handles,
                       sizeof(handles[0]) * count)) {
        free(handles);
        return -EFAULT;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    spin_lock(&drm_syncobjs_lock);
    for (uint32_t i = 0; i < count; i++) {
        drm_syncobj_entry_t *entry =
            drm_syncobj_lookup_locked(dev, owner_pid, handles[i]);
        if (!entry) {
            spin_unlock(&drm_syncobjs_lock);
            free(handles);
            return -ENOENT;
        }

        uint64_t old_point = entry->point;
        entry->point = signal ? (old_point ? old_point : 1) : 0;

        if (signal && entry->eventfd_file && entry->point != old_point) {
            uint64_t one = 1;
            vfs_write_file(entry->eventfd_file, &one, sizeof(one), NULL);
        }
    }
    spin_unlock(&drm_syncobjs_lock);

    free(handles);
    if (signal) {
        drm_syncfd_notify_all();
    }
    return 0;
}

static ssize_t
drm_syncobj_timeline_signal(drm_device_t *dev,
                            struct drm_syncobj_timeline_array *a) {
    if (!dev || !a || a->count_handles == 0 || !a->handles || !a->points) {
        return -EINVAL;
    }

    if (a->flags != 0) {
        return -EINVAL;
    }

    uint32_t count = a->count_handles;
    if (count > DRM_MAX_SYNCOBJS) {
        return -EINVAL;
    }

    uint32_t *handles = malloc(sizeof(handles[0]) * count);
    uint64_t *points = malloc(sizeof(points[0]) * count);
    if (!handles || !points) {
        free(handles);
        free(points);
        return -ENOMEM;
    }

    if (copy_from_user(handles, (void *)(uintptr_t)a->handles,
                       sizeof(handles[0]) * count) ||
        copy_from_user(points, (void *)(uintptr_t)a->points,
                       sizeof(points[0]) * count)) {
        free(handles);
        free(points);
        return -EFAULT;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    spin_lock(&drm_syncobjs_lock);
    for (uint32_t i = 0; i < count; i++) {
        drm_syncobj_entry_t *entry =
            drm_syncobj_lookup_locked(dev, owner_pid, handles[i]);
        if (!entry) {
            spin_unlock(&drm_syncobjs_lock);
            free(handles);
            free(points);
            return -ENOENT;
        }

        uint64_t old_point = entry->point;
        if (points[i] > entry->point) {
            entry->point = points[i];
        }

        if (entry->eventfd_file && entry->point != old_point) {
            uint64_t one = 1;
            vfs_write_file(entry->eventfd_file, &one, sizeof(one), NULL);
        }
    }
    spin_unlock(&drm_syncobjs_lock);

    free(handles);
    free(points);
    drm_syncfd_notify_all();
    return 0;
}

static ssize_t drm_syncobj_query(drm_device_t *dev,
                                 struct drm_syncobj_timeline_array *a) {
    if (!dev || !a || a->count_handles == 0 || !a->handles || !a->points) {
        return -EINVAL;
    }

    if (a->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
        return -EINVAL;
    }

    uint32_t count = a->count_handles;
    if (count > DRM_MAX_SYNCOBJS) {
        return -EINVAL;
    }

    uint32_t *handles = malloc(sizeof(handles[0]) * count);
    uint64_t *points = malloc(sizeof(points[0]) * count);
    if (!handles || !points) {
        free(handles);
        free(points);
        return -ENOMEM;
    }

    if (copy_from_user(handles, (void *)(uintptr_t)a->handles,
                       sizeof(handles[0]) * count)) {
        free(handles);
        free(points);
        return -EFAULT;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    spin_lock(&drm_syncobjs_lock);
    for (uint32_t i = 0; i < count; i++) {
        drm_syncobj_entry_t *entry =
            drm_syncobj_lookup_locked(dev, owner_pid, handles[i]);
        if (!entry) {
            spin_unlock(&drm_syncobjs_lock);
            free(handles);
            free(points);
            return -ENOENT;
        }

        points[i] = entry->point;
    }
    spin_unlock(&drm_syncobjs_lock);

    if (copy_to_user((void *)(uintptr_t)a->points, points,
                     sizeof(points[0]) * count)) {
        free(handles);
        free(points);
        return -EFAULT;
    }

    free(handles);
    free(points);
    return 0;
}

static ssize_t drm_syncobj_transfer(drm_device_t *dev,
                                    struct drm_syncobj_transfer *t) {
    if (!dev || !t || t->src_handle == 0 || t->dst_handle == 0) {
        return -EINVAL;
    }

    if (t->flags != 0) {
        return -EINVAL;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    spin_lock(&drm_syncobjs_lock);
    drm_syncobj_entry_t *src =
        drm_syncobj_lookup_locked(dev, owner_pid, t->src_handle);
    drm_syncobj_entry_t *dst =
        drm_syncobj_lookup_locked(dev, owner_pid, t->dst_handle);
    if (!src || !dst) {
        spin_unlock(&drm_syncobjs_lock);
        return -ENOENT;
    }

    if (src->point >= t->src_point) {
        uint64_t delta = src->point - t->src_point;
        uint64_t dst_point = t->dst_point + delta;
        uint64_t old_point = dst->point;
        if (dst_point > dst->point) {
            dst->point = dst_point;
        }

        if (dst->eventfd_file && dst->point != old_point) {
            uint64_t one = 1;
            vfs_write_file(dst->eventfd_file, &one, sizeof(one), NULL);
        }
    }

    spin_unlock(&drm_syncobjs_lock);
    return 0;
}

static ssize_t drm_syncobj_eventfd_ioctl(drm_device_t *dev,
                                         struct drm_syncobj_eventfd *e) {
    if (!dev || !e || e->handle == 0) {
        return -EINVAL;
    }

    if (e->flags != 0) {
        return -EINVAL;
    }

    uint64_t owner_pid = drm_syncobj_owner_pid();
    struct vfs_file *new_node = NULL;

    if (e->fd >= 0) {
        if (e->fd >= MAX_FD_NUM) {
            return -EBADF;
        }

        fd_t *fd_obj = task_get_file(current_task, e->fd);
        if (eventfd_is_file(fd_obj))
            new_node = vfs_file_get(fd_obj);
        vfs_file_put(fd_obj);

        if (!new_node) {
            return -EINVAL;
        }
    }

    spin_lock(&drm_syncobjs_lock);
    drm_syncobj_entry_t *entry =
        drm_syncobj_lookup_locked(dev, owner_pid, e->handle);
    if (!entry) {
        spin_unlock(&drm_syncobjs_lock);
        if (new_node) {
            vfs_file_put(new_node);
        }
        return -ENOENT;
    }

    if (entry->eventfd_file)
        vfs_file_put(entry->eventfd_file);
    entry->eventfd_file = new_node;

    if (entry->eventfd_file && entry->point > 0) {
        uint64_t one = 1;
        vfs_write_file(entry->eventfd_file, &one, sizeof(one), NULL);
    }

    spin_unlock(&drm_syncobjs_lock);
    return 0;
}

static ssize_t drm_prime_get_fd_inode(int fd, uint64_t *inode) {
    if (fd < 0 || fd >= MAX_FD_NUM || !inode) {
        return -EBADF;
    }

    int ret = -EBADF;
    fd_t *fd_obj = task_get_file(current_task, fd);
    if (fd_obj && fd_obj->node) {
        *inode = fd_obj->node->inode;
        ret = 0;
    }
    vfs_file_put(fd_obj);

    return ret;
}

static ssize_t drm_prime_store_export(drm_device_t *dev, uint64_t inode,
                                      uint32_t handle) {
    int free_slot = -1;

    spin_lock(&drm_prime_exports_lock);
    for (int i = 0; i < DRM_MAX_PRIME_EXPORTS; i++) {
        if (drm_prime_exports[i].used) {
            if (drm_prime_exports[i].dev == dev &&
                drm_prime_exports[i].inode == inode) {
                drm_prime_exports[i].handle = handle;
                spin_unlock(&drm_prime_exports_lock);
                return 0;
            }
            continue;
        }

        if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        spin_unlock(&drm_prime_exports_lock);
        return -ENOSPC;
    }

    drm_prime_exports[free_slot].used = true;
    drm_prime_exports[free_slot].dev = dev;
    drm_prime_exports[free_slot].inode = inode;
    drm_prime_exports[free_slot].handle = handle;
    spin_unlock(&drm_prime_exports_lock);

    return 0;
}

static ssize_t drm_prime_lookup_handle(drm_device_t *dev, uint64_t inode,
                                       uint32_t *handle) {
    spin_lock(&drm_prime_exports_lock);
    for (int i = 0; i < DRM_MAX_PRIME_EXPORTS; i++) {
        if (!drm_prime_exports[i].used) {
            continue;
        }
        if (drm_prime_exports[i].dev != dev ||
            drm_prime_exports[i].inode != inode) {
            continue;
        }

        *handle = drm_prime_exports[i].handle;
        spin_unlock(&drm_prime_exports_lock);
        return 0;
    }
    spin_unlock(&drm_prime_exports_lock);

    return -EBADF;
}

static void drm_dumb_store_size(drm_device_t *dev, uint32_t handle,
                                uint64_t size) {
    int free_slot = -1;

    spin_lock(&drm_dumb_exports_lock);
    for (int i = 0; i < DRM_MAX_DUMB_EXPORTS; i++) {
        if (drm_dumb_exports[i].used) {
            if (drm_dumb_exports[i].dev == dev &&
                drm_dumb_exports[i].handle == handle) {
                drm_dumb_exports[i].size = size;
                spin_unlock(&drm_dumb_exports_lock);
                return;
            }
            continue;
        }

        if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        drm_dumb_exports[free_slot].used = true;
        drm_dumb_exports[free_slot].dev = dev;
        drm_dumb_exports[free_slot].handle = handle;
        drm_dumb_exports[free_slot].size = size;
    }
    spin_unlock(&drm_dumb_exports_lock);
}

static uint64_t drm_dumb_get_size(drm_device_t *dev, uint32_t handle) {
    uint64_t size = 0;

    spin_lock(&drm_dumb_exports_lock);
    for (int i = 0; i < DRM_MAX_DUMB_EXPORTS; i++) {
        if (!drm_dumb_exports[i].used) {
            continue;
        }
        if (drm_dumb_exports[i].dev == dev &&
            drm_dumb_exports[i].handle == handle) {
            size = drm_dumb_exports[i].size;
            break;
        }
    }
    spin_unlock(&drm_dumb_exports_lock);

    if (size != 0) {
        return size;
    }

    for (uint32_t i = 0; i < DRM_MAX_FRAMEBUFFERS_PER_DEVICE; i++) {
        drm_framebuffer_t *fb = dev->resource_mgr.framebuffers[i];
        if (!fb || fb->handle != handle) {
            continue;
        }
        if (fb->pitch && fb->height) {
            return (uint64_t)fb->pitch * (uint64_t)fb->height;
        }
    }

    return 0;
}

static ssize_t drm_user_blob_find_index_locked(drm_device_t *dev,
                                               uint32_t blob_id) {
    for (int i = 0; i < DRM_MAX_USER_BLOBS; i++) {
        if (!drm_user_blobs[i].used) {
            continue;
        }
        if (drm_user_blobs[i].dev == dev &&
            drm_user_blobs[i].blob_id == blob_id) {
            return i;
        }
    }

    return -1;
}

static ssize_t drm_user_blob_generate_id_locked(uint32_t *blob_id) {
    uint32_t candidate = drm_user_blob_next_id;
    if (candidate <= DRM_BLOB_ID_USER_BASE ||
        candidate > DRM_BLOB_ID_USER_LAST) {
        candidate = DRM_BLOB_ID_USER_BASE + 1;
    }

    uint32_t id_space = DRM_BLOB_ID_USER_LAST - DRM_BLOB_ID_USER_BASE;
    for (uint32_t tries = 0; tries < id_space; tries++) {
        bool exists = false;
        for (int i = 0; i < DRM_MAX_USER_BLOBS; i++) {
            if (drm_user_blobs[i].used &&
                drm_user_blobs[i].blob_id == candidate) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            *blob_id = candidate;
            drm_user_blob_next_id = candidate + 1;
            if (drm_user_blob_next_id > DRM_BLOB_ID_USER_LAST) {
                drm_user_blob_next_id = DRM_BLOB_ID_USER_BASE + 1;
            }
            return 0;
        }

        candidate++;
        if (candidate > DRM_BLOB_ID_USER_LAST) {
            candidate = DRM_BLOB_ID_USER_BASE + 1;
        }
    }

    return -ENOSPC;
}

#define DRM_BLOB_ID_CRTC_MODE_BASE 0x10000000U
#define DRM_BLOB_ID_CONNECTOR_EDID_BASE 0x20000000U
#define DRM_BLOB_ID_PLANE_IN_FORMATS_BASE 0x28000000U

static uint32_t drm_crtc_mode_blob_id(uint32_t crtc_id) {
    return DRM_BLOB_ID_CRTC_MODE_BASE + crtc_id;
}

static uint32_t drm_connector_edid_blob_id(uint32_t connector_id) {
    return DRM_BLOB_ID_CONNECTOR_EDID_BASE + connector_id;
}

static uint32_t drm_plane_in_formats_blob_id(uint32_t plane_id) {
    return DRM_BLOB_ID_PLANE_IN_FORMATS_BASE + plane_id;
}

static bool drm_mode_blob_to_crtc_id(uint32_t blob_id, uint32_t *crtc_id) {
    if (blob_id <= DRM_BLOB_ID_CRTC_MODE_BASE ||
        blob_id >= DRM_BLOB_ID_CONNECTOR_EDID_BASE) {
        return false;
    }

    *crtc_id = blob_id - DRM_BLOB_ID_CRTC_MODE_BASE;
    return *crtc_id != 0;
}

static bool drm_blob_to_connector_edid_id(uint32_t blob_id,
                                          uint32_t *connector_id) {
    if (blob_id <= DRM_BLOB_ID_CONNECTOR_EDID_BASE ||
        blob_id >= DRM_BLOB_ID_PLANE_IN_FORMATS_BASE) {
        return false;
    }

    *connector_id = blob_id - DRM_BLOB_ID_CONNECTOR_EDID_BASE;
    return *connector_id != 0;
}

static bool drm_blob_to_plane_in_formats_id(uint32_t blob_id,
                                            uint32_t *plane_id) {
    if (blob_id <= DRM_BLOB_ID_PLANE_IN_FORMATS_BASE ||
        blob_id >= DRM_BLOB_ID_USER_BASE) {
        return false;
    }

    *plane_id = blob_id - DRM_BLOB_ID_PLANE_IN_FORMATS_BASE;
    return *plane_id != 0;
}

static void drm_fill_default_modeinfo(drm_device_t *dev,
                                      struct drm_mode_modeinfo *mode) {
    uint32_t width = 0, height = 0, bpp = 0;
    memset(mode, 0, sizeof(*mode));

    if (dev->op->get_display_info &&
        dev->op->get_display_info(dev, &width, &height, &bpp) == 0 &&
        width > 0 && height > 0) {
        mode->hdisplay = width;
        mode->hsync_start = width + 16;
        mode->hsync_end = width + 16 + 96;
        mode->htotal = width + 16 + 96 + 48;
        mode->vdisplay = height;
        mode->vsync_start = height + 10;
        mode->vsync_end = height + 10 + 2;
        mode->vtotal = height + 10 + 2 + 33;
        mode->vrefresh = HZ;
        mode->clock =
            (uint32_t)(((uint64_t)mode->htotal * mode->vtotal * mode->vrefresh +
                        500) /
                       1000);
        sprintf(mode->name, "%dx%d", width, height);
        return;
    }

    strcpy(mode->name, "unknown");
}

static void drm_fill_crtc_modeinfo(drm_device_t *dev, drm_crtc_t *crtc,
                                   struct drm_mode_modeinfo *mode) {
    if (crtc && crtc->mode_valid && crtc->mode.hdisplay > 0 &&
        crtc->mode.vdisplay > 0) {
        memcpy(mode, &crtc->mode, sizeof(*mode));
        return;
    }

    drm_fill_default_modeinfo(dev, mode);
}

static void drm_edid_set_descriptor_text(uint8_t *desc, uint8_t tag,
                                         const char *text) {
    memset(desc, 0, 18);
    desc[3] = tag;
    desc[4] = 0x00;

    size_t i = 0;
    for (; i < 13 && text[i]; i++) {
        desc[5 + i] = (uint8_t)text[i];
    }
    if (i < 13) {
        desc[5 + i++] = '\n';
    }
    for (; i < 13; i++) {
        desc[5 + i] = ' ';
    }
}

static void drm_edid_fill_dtd(uint8_t *dtd, uint32_t width, uint32_t height,
                              uint32_t mm_width, uint32_t mm_height,
                              uint32_t refresh_hz) {
    uint32_t hblank = 160;
    uint32_t vblank = 45;
    uint32_t hsync_offset = 48;
    uint32_t hsync_pulse = 32;
    uint32_t vsync_offset = 3;
    uint32_t vsync_pulse = 5;
    uint32_t htotal = width + hblank;
    uint32_t vtotal = height + vblank;
    uint32_t pixel_clock_10khz = (htotal * vtotal * refresh_hz) / 10000U;

    memset(dtd, 0, 18);
    dtd[0] = pixel_clock_10khz & 0xff;
    dtd[1] = (pixel_clock_10khz >> 8) & 0xff;
    dtd[2] = width & 0xff;
    dtd[3] = hblank & 0xff;
    dtd[4] = ((width >> 8) & 0xf) << 4 | ((hblank >> 8) & 0xf);
    dtd[5] = height & 0xff;
    dtd[6] = vblank & 0xff;
    dtd[7] = ((height >> 8) & 0xf) << 4 | ((vblank >> 8) & 0xf);
    dtd[8] = hsync_offset & 0xff;
    dtd[9] = hsync_pulse & 0xff;
    dtd[10] = ((vsync_offset & 0xf) << 4) | (vsync_pulse & 0xf);
    dtd[11] = ((hsync_offset >> 8) & 0x3) << 6 |
              ((hsync_pulse >> 8) & 0x3) << 4 |
              ((vsync_offset >> 4) & 0x3) << 2 | ((vsync_pulse >> 4) & 0x3);
    dtd[12] = mm_width & 0xff;
    dtd[13] = mm_height & 0xff;
    dtd[14] = ((mm_width >> 8) & 0xf) << 4 | ((mm_height >> 8) & 0xf);
    dtd[17] = 0x1a;
}

static void drm_edid_set_default_chromaticity(uint8_t edid[128]) {
    const uint16_t red_x = 655;   // 0.640
    const uint16_t red_y = 338;   // 0.330
    const uint16_t green_x = 307; // 0.300
    const uint16_t green_y = 614; // 0.600
    const uint16_t blue_x = 154;  // 0.150
    const uint16_t blue_y = 61;   // 0.060
    const uint16_t white_x = 320; // 0.313
    const uint16_t white_y = 337; // 0.329

    edid[25] = ((red_x & 0x3) << 6) | ((red_y & 0x3) << 4) |
               ((green_x & 0x3) << 2) | (green_y & 0x3);
    edid[26] = ((blue_x & 0x3) << 6) | ((blue_y & 0x3) << 4) |
               ((white_x & 0x3) << 2) | (white_y & 0x3);
    edid[27] = (uint8_t)(red_x >> 2);
    edid[28] = (uint8_t)(red_y >> 2);
    edid[29] = (uint8_t)(green_x >> 2);
    edid[30] = (uint8_t)(green_y >> 2);
    edid[31] = (uint8_t)(blue_x >> 2);
    edid[32] = (uint8_t)(blue_y >> 2);
    edid[33] = (uint8_t)(white_x >> 2);
    edid[34] = (uint8_t)(white_y >> 2);
}

static void drm_build_connector_edid(drm_device_t *dev, drm_connector_t *conn,
                                     uint8_t edid[128]) {
    uint32_t width = 1024;
    uint32_t height = 768;
    uint32_t refresh = 60;
    uint32_t mm_width = conn->mm_width;
    uint32_t mm_height = conn->mm_height;

    if (conn->modes && conn->count_modes > 0) {
        if (conn->modes[0].hdisplay > 0) {
            width = conn->modes[0].hdisplay;
        }
        if (conn->modes[0].vdisplay > 0) {
            height = conn->modes[0].vdisplay;
        }
        if (conn->modes[0].vrefresh > 0) {
            refresh = conn->modes[0].vrefresh;
        }
    } else if (dev->op->get_display_info) {
        uint32_t bpp = 0;
        if (dev->op->get_display_info(dev, &width, &height, &bpp) != 0) {
            width = 1024;
            height = 768;
        }
    }

    if (mm_width == 0) {
        mm_width = (width * 264U) / 1000U;
        if (mm_width == 0) {
            mm_width = 1;
        }
    }
    if (mm_height == 0) {
        mm_height = (height * 264U) / 1000U;
        if (mm_height == 0) {
            mm_height = 1;
        }
    }

    memset(edid, 0, 128);
    edid[0] = 0x00;
    edid[1] = 0xff;
    edid[2] = 0xff;
    edid[3] = 0xff;
    edid[4] = 0xff;
    edid[5] = 0xff;
    edid[6] = 0xff;
    edid[7] = 0x00;
    edid[8] = 0x38;
    edid[9] = 0x2f;
    edid[10] = 0x01;
    edid[11] = 0x00;
    edid[12] = 0x01;
    edid[13] = 0x00;
    edid[14] = 0x00;
    edid[15] = 0x00;
    edid[16] = 0x01;
    edid[17] = 34;
    edid[18] = 0x01;
    edid[19] = 0x04;
    edid[20] = 0x80;
    edid[21] = width & 0xff;
    edid[22] = height & 0xff;
    edid[23] = 0x78;
    edid[24] = 0x0a;
    drm_edid_set_default_chromaticity(edid);

    for (int i = 38; i < 54; i++) {
        edid[i] = 0x01;
    }

    drm_edid_fill_dtd(&edid[54], width, height, mm_width, mm_height, refresh);
    drm_edid_set_descriptor_text(&edid[72], 0xfc, "NAOS Virtual");
    drm_edid_set_descriptor_text(&edid[90], 0xff, "00000001");
    drm_edid_set_descriptor_text(&edid[108], 0xfe, "NAOS DRM");

    edid[126] = 0;

    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) {
        sum += edid[i];
    }
    edid[127] = (uint8_t)(0x100 - sum);
}

/**
 * drm_ioctl_version - Handle DRM_IOCTL_VERSION
 */
ssize_t drm_ioctl_version(drm_device_t *dev, void *arg) {
    struct drm_version *version = (struct drm_version *)arg;
    const char *name =
        (dev && dev->driver_name[0]) ? dev->driver_name : DRM_NAME;
    const char *date =
        (dev && dev->driver_date[0]) ? dev->driver_date : "20060810";
    const char *desc =
        (dev && dev->driver_desc[0]) ? dev->driver_desc : DRM_NAME;
    int version_major = 1;
    int version_minor = 0;
    int version_patchlevel = 0;

    if (!strcmp(name, "virtio_gpu")) {
        version_major = 0;
        version_minor = 0;
    }

    size_t user_name_len = version->name_len;
    size_t user_date_len = version->date_len;
    size_t user_desc_len = version->desc_len;

    version->version_major = version_major;
    version->version_minor = version_minor;
    version->version_patchlevel = version_patchlevel;
    version->name_len = strlen(name);
    if (version->name && user_name_len) {
        if (copy_to_user_str(version->name, name, user_name_len))
            return -EFAULT;
    }
    version->date_len = strlen(date);
    if (version->date && user_date_len) {
        if (copy_to_user_str(version->date, date, user_date_len))
            return -EFAULT;
    }
    version->desc_len = strlen(desc);
    if (version->desc && user_desc_len) {
        if (copy_to_user_str(version->desc, desc, user_desc_len))
            return -EFAULT;
    }
    return 0;
}

/**
 * drm_ioctl_get_cap - Handle DRM_IOCTL_GET_CAP
 */
ssize_t drm_ioctl_get_cap(drm_device_t *dev, void *arg) {
    struct drm_get_cap *cap = (struct drm_get_cap *)arg;

    if (dev && dev->op && dev->op->get_cap) {
        int ret = dev->op->get_cap(dev, cap);

        if (ret != -ENOTSUP)
            return ret;
    }

    switch (cap->capability) {
    case DRM_CAP_DUMB_BUFFER:
        cap->value = 1; // Support dumb buffer
        return 0;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        cap->value = 24;
        return 0;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
        cap->value = 1;
        return 0;
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        cap->value = 1;
        return 0;
    case DRM_CAP_CURSOR_WIDTH:
        cap->value = 24;
        return 0;
    case DRM_CAP_CURSOR_HEIGHT:
        cap->value = 24;
        return 0;
    case DRM_CAP_PRIME:
        cap->value = DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT;
        return 0;
    case DRM_CAP_ASYNC_PAGE_FLIP:
        cap->value = 0;
        return 0;
    case DRM_CAP_ADDFB2_MODIFIERS:
        cap->value = 0;
        return 0;
    case DRM_CAP_DUMB_PREFER_SHADOW:
        cap->value = (dev && !dev->render_node_registered) ? 1 : 0;
        return 0;
    case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP:
        cap->value = 0;
        return 0;
    case DRM_CAP_SYNCOBJ:
        cap->value = 1;
        return 0;
    case DRM_CAP_SYNCOBJ_TIMELINE:
        cap->value = 1;
        return 0;
    default:
        printk("drm: Unsupported capability %d\n", cap->capability);
        cap->value = 0;
        return 0;
    }
}

/**
 * drm_ioctl_gem_close - Handle DRM_IOCTL_GEM_CLOSE
 */
ssize_t drm_ioctl_gem_close(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_gem_close *close = (struct drm_gem_close *)arg;

    if (close->handle == 0) {
        return -EINVAL;
    }

    if (dev->op && dev->op->driver_ioctl) {
        ssize_t ret =
            dev->op->driver_ioctl(dev, DRM_IOCTL_GEM_CLOSE, arg, false, fd);
        if (ret != -ENOTTY) {
            return ret;
        }
    }

    return 0;
}

/**
 * drm_ioctl_prime_handle_to_fd - Handle DRM_IOCTL_PRIME_HANDLE_TO_FD
 */
ssize_t drm_ioctl_prime_handle_to_fd(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_prime_handle *prime = (struct drm_prime_handle *)arg;

    if (prime->flags & ~(DRM_CLOEXEC | DRM_RDWR)) {
        return -EINVAL;
    }

    if (dev->op && dev->op->driver_ioctl) {
        ssize_t ret = dev->op->driver_ioctl(dev, DRM_IOCTL_PRIME_HANDLE_TO_FD,
                                            arg, false, fd);
        if (ret != -ENOTTY) {
            return ret;
        }
    }

    uint64_t phys = 0;
    uint64_t size = drm_dumb_get_size(dev, prime->handle);
    if (size != 0) {
        int ret = drm_prime_get_handle_phys(dev, prime->handle, &phys);
        if (ret) {
            if (dev->op && dev->op->driver_ioctl) {
                struct drm_gem_close close = {.handle = prime->handle};
                dev->op->driver_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &close, false,
                                      NULL);
            }
            return ret;
        }
    } else if (dev && dev->op && dev->op->get_dumb_map) {
        int ret = dev->op->get_dumb_map(dev, prime->handle, &phys, &size);
        if (ret) {
            if (dev->op && dev->op->driver_ioctl) {
                struct drm_gem_close close = {.handle = prime->handle};
                dev->op->driver_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &close, false,
                                      NULL);
            }
            return ret;
        }
        if (size == 0) {
            if (dev->op && dev->op->driver_ioctl) {
                struct drm_gem_close close = {.handle = prime->handle};
                dev->op->driver_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &close, false,
                                      NULL);
            }
            return -ENOENT;
        }
    } else {
        if (dev->op && dev->op->driver_ioctl) {
            struct drm_gem_close close = {.handle = prime->handle};
            dev->op->driver_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &close, false,
                                  NULL);
        }
        return -ENOENT;
    }

    ssize_t fd_ret =
        drm_primefd_create(dev, prime->handle, phys, size, prime->flags);
    if (fd_ret < 0) {
        if (dev->op && dev->op->driver_ioctl) {
            struct drm_gem_close close = {.handle = prime->handle};
            dev->op->driver_ioctl(dev, DRM_IOCTL_GEM_CLOSE, &close, false,
                                  NULL);
        }
        return fd_ret;
    }

    prime->fd = (int)fd_ret;
    return 0;
}

/**
 * drm_ioctl_prime_fd_to_handle - Handle DRM_IOCTL_PRIME_FD_TO_HANDLE
 */
ssize_t drm_ioctl_prime_fd_to_handle(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_prime_handle *prime = (struct drm_prime_handle *)arg;

    if (prime->fd < 0 || prime->fd >= MAX_FD_NUM) {
        return -EBADF;
    }

    int direct_ret = -EBADF;
    fd_t *fd_obj = task_get_file(current_task, prime->fd);
    drm_prime_fd_ctx_t *ctx = drm_primefd_file_ctx(fd_obj);
    if (ctx && ctx->dev == dev && ctx->handle != 0) {
        prime->handle = ctx->handle;
        direct_ret = 0;
    }
    vfs_file_put(fd_obj);

    if (direct_ret == 0) {
        if (dev->op && dev->op->driver_ioctl) {
            ssize_t driver_ret = dev->op->driver_ioctl(
                dev, DRM_IOCTL_PRIME_FD_TO_HANDLE, arg, false, fd);
            if (driver_ret != -ENOTTY) {
                return driver_ret;
            }
        }
        return 0;
    }

    uint64_t inode = 0;
    int ret = drm_prime_get_fd_inode(prime->fd, &inode);
    if (ret) {
        return ret;
    }

    uint32_t handle = 0;
    ret = drm_prime_lookup_handle(dev, inode, &handle);
    if (ret) {
        return ret;
    }

    prime->handle = handle;

    if (dev->op && dev->op->driver_ioctl) {
        ssize_t driver_ret = dev->op->driver_ioctl(
            dev, DRM_IOCTL_PRIME_FD_TO_HANDLE, arg, false, fd);
        if (driver_ret != -ENOTTY) {
            return driver_ret;
        }
    }

    return 0;
}

/**
 * drm_ioctl_mode_getresources - Handle DRM_IOCTL_MODE_GETRESOURCES
 */
ssize_t drm_ioctl_mode_getresources(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;
    drm_file_t *drm_file = drm_file_from_vfs_file(fd);
    uint32_t fbs_cap = res->count_fbs;
    uint32_t crtcs_cap = res->count_crtcs;
    uint32_t connectors_cap = res->count_connectors;
    uint32_t encoders_cap = res->count_encoders;

    // Count available resources
    res->count_fbs = 0;
    res->count_crtcs = 0;
    res->count_connectors = 0;
    res->count_encoders = 0;

    // Count framebuffers
    for (uint32_t i = 0; i < DRM_MAX_FRAMEBUFFERS_PER_DEVICE; i++) {
        if (dev->resource_mgr.framebuffers[i]) {
            res->count_fbs++;
        }
    }

    // Count CRTCs
    for (uint32_t i = 0; i < DRM_MAX_CRTCS_PER_DEVICE; i++) {
        if (dev->resource_mgr.crtcs[i] &&
            drm_file_has_object(drm_file, dev->resource_mgr.crtcs[i]->id)) {
            res->count_crtcs++;
        }
    }

    // Count connectors
    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
        if (dev->resource_mgr.connectors[i] &&
            drm_file_has_object(drm_file,
                                dev->resource_mgr.connectors[i]->id)) {
            res->count_connectors++;
        }
    }

    // Count encoders
    for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
        if (dev->resource_mgr.encoders[i] &&
            drm_file_has_encoder(dev, drm_file,
                                 dev->resource_mgr.encoders[i])) {
            res->count_encoders++;
        }
    }

    uint32_t width, height, bpp;
    if (dev->op->get_display_info) {
        dev->op->get_display_info(dev, &width, &height, &bpp);
        res->min_width = width;
        res->min_height = height;
        res->max_width = width;
        res->max_height = height;
    } else {
        res->min_width = 0;
        res->min_height = 0;
        res->max_width = 0;
        res->max_height = 0;
    }

    // Fill encoder IDs if pointer provided
    if (res->encoder_id_ptr && encoders_cap > 0 && res->count_encoders > 0) {
        uint32_t copy_count = MIN(encoders_cap, res->count_encoders);
        uint32_t idx = 0;
        for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
            if (idx >= copy_count) {
                break;
            }
            if (dev->resource_mgr.encoders[i] &&
                drm_file_has_encoder(dev, drm_file,
                                     dev->resource_mgr.encoders[i])) {
                uint32_t encoder_id = dev->resource_mgr.encoders[i]->id;
                int ret = drm_copy_to_user_ptr(res->encoder_id_ptr +
                                                   idx * sizeof(uint32_t),
                                               &encoder_id, sizeof(encoder_id));
                if (ret) {
                    return ret;
                }
                idx++;
            }
        }
    }

    // Fill CRTC IDs if pointer provided
    if (res->crtc_id_ptr && crtcs_cap > 0 && res->count_crtcs > 0) {
        uint32_t copy_count = MIN(crtcs_cap, res->count_crtcs);
        uint32_t idx = 0;
        for (uint32_t i = 0; i < DRM_MAX_CRTCS_PER_DEVICE; i++) {
            if (idx >= copy_count) {
                break;
            }
            if (dev->resource_mgr.crtcs[i] &&
                drm_file_has_object(drm_file, dev->resource_mgr.crtcs[i]->id)) {
                uint32_t crtc_id = dev->resource_mgr.crtcs[i]->id;
                int ret = drm_copy_to_user_ptr(res->crtc_id_ptr +
                                                   idx * sizeof(uint32_t),
                                               &crtc_id, sizeof(crtc_id));
                if (ret) {
                    return ret;
                }
                idx++;
            }
        }
    }

    // Fill connector IDs if pointer provided
    if (res->connector_id_ptr && connectors_cap > 0 &&
        res->count_connectors > 0) {
        uint32_t copy_count = MIN(connectors_cap, res->count_connectors);
        uint32_t idx = 0;
        for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
            if (idx >= copy_count) {
                break;
            }
            if (dev->resource_mgr.connectors[i] &&
                drm_file_has_object(drm_file,
                                    dev->resource_mgr.connectors[i]->id)) {
                uint32_t connector_id = dev->resource_mgr.connectors[i]->id;
                int ret = drm_copy_to_user_ptr(
                    res->connector_id_ptr + idx * sizeof(uint32_t),
                    &connector_id, sizeof(connector_id));
                if (ret) {
                    return ret;
                }
                idx++;
            }
        }
    }

    // Fill framebuffer IDs if pointer provided
    if (res->fb_id_ptr && fbs_cap > 0 && res->count_fbs > 0) {
        uint32_t copy_count = MIN(fbs_cap, res->count_fbs);
        uint32_t idx = 0;
        for (uint32_t i = 0; i < DRM_MAX_FRAMEBUFFERS_PER_DEVICE; i++) {
            if (idx >= copy_count) {
                break;
            }
            if (dev->resource_mgr.framebuffers[i]) {
                uint32_t fb_id = dev->resource_mgr.framebuffers[i]->id;
                int ret = drm_copy_to_user_ptr(res->fb_id_ptr +
                                                   idx * sizeof(uint32_t),
                                               &fb_id, sizeof(fb_id));
                if (ret) {
                    return ret;
                }
                idx++;
            }
        }
    }

    return 0;
}

/**
 * drm_ioctl_mode_getcrtc - Handle DRM_IOCTL_MODE_GETCRTC
 */
ssize_t drm_ioctl_mode_getcrtc(drm_device_t *dev, void *arg) {
    struct drm_mode_crtc *crtc = (struct drm_mode_crtc *)arg;

    // Find the CRTC by ID
    drm_crtc_t *crtc_obj = drm_crtc_get(&dev->resource_mgr, crtc->crtc_id);
    if (!crtc_obj) {
        return -ENOENT;
    }

    struct drm_mode_modeinfo mode;
    drm_fill_crtc_modeinfo(dev, crtc_obj, &mode);

    crtc->gamma_size = 0;
    crtc->mode_valid = 1;
    memcpy(&crtc->mode, &mode, sizeof(struct drm_mode_modeinfo));
    crtc->fb_id = crtc_obj->fb_id;
    crtc->x = crtc_obj->x;
    crtc->y = crtc_obj->y;

    // Release reference
    drm_crtc_free(&dev->resource_mgr, crtc_obj->id);
    return 0;
}

/**
 * drm_ioctl_mode_getencoder - Handle DRM_IOCTL_MODE_GETENCODER
 */
ssize_t drm_ioctl_mode_getencoder(drm_device_t *dev, void *arg) {
    struct drm_mode_get_encoder *enc = (struct drm_mode_get_encoder *)arg;

    // Find the encoder by ID
    drm_encoder_t *encoder =
        drm_encoder_get(&dev->resource_mgr, enc->encoder_id);
    if (!encoder) {
        return -ENOENT;
    }

    enc->encoder_type = encoder->type;
    enc->crtc_id = encoder->crtc_id;
    enc->possible_crtcs = encoder->possible_crtcs;
    enc->possible_clones = encoder->possible_clones;

    // Release reference
    drm_encoder_free(&dev->resource_mgr, encoder->id);
    return 0;
}

/**
 * drm_ioctl_mode_create_dumb - Handle DRM_IOCTL_MODE_CREATE_DUMB
 */
ssize_t drm_ioctl_mode_create_dumb(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op->create_dumb) {
        return -ENOTTY;
    }
    struct drm_mode_create_dumb *create = (struct drm_mode_create_dumb *)arg;
    ssize_t ret = dev->op->create_dumb(dev, create, fd);
    if (ret == 0) {
        drm_dumb_store_size(dev, create->handle, create->size);
    }
    return ret;
}

/**
 * drm_ioctl_mode_map_dumb - Handle DRM_IOCTL_MODE_MAP_DUMB
 */
ssize_t drm_ioctl_mode_map_dumb(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op->map_dumb) {
        return -ENOTTY;
    }
    return dev->op->map_dumb(dev, (struct drm_mode_map_dumb *)arg, fd);
}

/**
 * drm_ioctl_mode_destroy_dumb - Handle DRM_IOCTL_MODE_DESTROY_DUMB
 */
ssize_t drm_ioctl_mode_destroy_dumb(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op->destroy_dumb) {
        return -ENOTTY;
    }

    struct drm_mode_destroy_dumb *destroy = (struct drm_mode_destroy_dumb *)arg;
    if (destroy->handle == 0) {
        return -EINVAL;
    }

    return dev->op->destroy_dumb(dev, destroy->handle, fd);
}

/**
 * drm_ioctl_mode_getconnector - Handle DRM_IOCTL_MODE_GETCONNECTOR
 */
ssize_t drm_ioctl_mode_getconnector(drm_device_t *dev, void *arg) {
    struct drm_mode_get_connector *conn = (struct drm_mode_get_connector *)arg;
    uint32_t modes_cap = conn->count_modes;
    uint32_t props_cap = conn->count_props;
    uint32_t encoders_cap = conn->count_encoders;

    // Find the connector by ID
    drm_connector_t *connector =
        drm_connector_get(&dev->resource_mgr, conn->connector_id);
    if (!connector) {
        return -ENOENT;
    }

    uint32_t mode_width = 0;
    uint32_t mode_height = 0;
    if (connector->modes && connector->count_modes > 0) {
        mode_width = connector->modes[0].hdisplay;
        mode_height = connector->modes[0].vdisplay;
    } else if (dev->op->get_display_info) {
        uint32_t bpp = 0;
        if (dev->op->get_display_info(dev, &mode_width, &mode_height, &bpp) !=
            0) {
            mode_width = 0;
            mode_height = 0;
        }
    }

    conn->encoder_id = connector->encoder_id;
    conn->connector_type = connector->type;
    conn->connector_type_id = 1;
    conn->connection = connector->connection;
    conn->count_modes = connector->count_modes;
    conn->count_props = 3;
    conn->count_encoders = connector->encoder_id ? 1 : 0;
    conn->subpixel = connector->subpixel;
    conn->mm_width = connector->mm_width;
    conn->mm_height = connector->mm_height;
    if (conn->mm_width == 0 && mode_width > 0) {
        conn->mm_width = (mode_width * 264UL) / 1000UL;
        if (conn->mm_width == 0) {
            conn->mm_width = 1;
        }
    }
    if (conn->mm_height == 0 && mode_height > 0) {
        conn->mm_height = (mode_height * 264UL) / 1000UL;
        if (conn->mm_height == 0) {
            conn->mm_height = 1;
        }
    }

    // Fill modes if pointer provided
    if (conn->modes_ptr && connector->modes && connector->count_modes > 0) {
        uint32_t copy_modes = MIN(modes_cap, connector->count_modes);
        int ret =
            drm_copy_to_user_ptr(conn->modes_ptr, connector->modes,
                                 copy_modes * sizeof(struct drm_mode_modeinfo));
        if (ret) {
            drm_connector_free(&dev->resource_mgr, connector->id);
            return ret;
        }
    }

    // Fill encoders if pointer provided
    if (conn->encoders_ptr && conn->count_encoders > 0) {
        uint32_t copy_encoders = MIN(encoders_cap, conn->count_encoders);
        if (copy_encoders == 0) {
            goto skip_encoders;
        }
        int ret =
            drm_copy_to_user_ptr(conn->encoders_ptr, &connector->encoder_id,
                                 sizeof(connector->encoder_id));
        if (ret) {
            drm_connector_free(&dev->resource_mgr, connector->id);
            return ret;
        }
    }
skip_encoders:

    // Fill properties if pointers provided
    if (conn->count_props > 0) {
        uint32_t prop_ids[3] = {DRM_CONNECTOR_DPMS_PROP_ID,
                                DRM_CONNECTOR_EDID_PROP_ID,
                                DRM_CONNECTOR_CRTC_ID_PROP_ID};
        uint64_t prop_values[3] = {DRM_MODE_DPMS_ON, 0, connector->crtc_id};
        prop_values[1] = drm_connector_edid_blob_id(connector->id);
        uint32_t copy_props = MIN(props_cap, conn->count_props);

        if (conn->props_ptr && copy_props > 0) {
            int ret = drm_copy_to_user_ptr(conn->props_ptr, prop_ids,
                                           copy_props * sizeof(uint32_t));
            if (ret) {
                drm_connector_free(&dev->resource_mgr, connector->id);
                return ret;
            }
        }

        if (conn->prop_values_ptr && copy_props > 0) {
            int ret = drm_copy_to_user_ptr(conn->prop_values_ptr, prop_values,
                                           copy_props * sizeof(uint64_t));
            if (ret) {
                drm_connector_free(&dev->resource_mgr, connector->id);
                return ret;
            }
        }
    }

    // Release reference
    drm_connector_free(&dev->resource_mgr, connector->id);
    return 0;
}

/**
 * drm_ioctl_mode_getfb - Handle DRM_IOCTL_MODE_GETFB
 */
ssize_t drm_ioctl_mode_getfb(drm_device_t *dev, void *arg) {
    struct drm_mode_fb_cmd *fb_cmd = (struct drm_mode_fb_cmd *)arg;

    // Find the framebuffer by ID
    drm_framebuffer_t *fb =
        drm_framebuffer_get(&dev->resource_mgr, fb_cmd->fb_id);
    if (!fb) {
        return -ENOENT;
    }

    fb_cmd->width = fb->width;
    fb_cmd->height = fb->height;
    fb_cmd->pitch = fb->pitch;
    fb_cmd->bpp = fb->bpp;
    fb_cmd->depth = fb->depth;
    fb_cmd->handle = fb->handle;

    // Release reference
    drm_framebuffer_free(&dev->resource_mgr, fb->id);
    return 0;
}

/**
 * drm_ioctl_mode_addfb - Handle DRM_IOCTL_MODE_ADDFB
 */
ssize_t drm_ioctl_mode_addfb(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op->add_fb) {
        return -ENOTTY;
    }
    return dev->op->add_fb(dev, (struct drm_mode_fb_cmd *)arg, fd);
}

/**
 * drm_ioctl_mode_addfb2 - Handle DRM_IOCTL_MODE_ADDFB2
 */
ssize_t drm_ioctl_mode_addfb2(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op->add_fb2) {
        return -ENOTTY;
    }
    return dev->op->add_fb2(dev, (struct drm_mode_fb_cmd2 *)arg, fd);
}

/**
 * drm_ioctl_mode_closefb - Handle DRM_IOCTL_MODE_CLOSEFB
 */
ssize_t drm_ioctl_mode_closefb(drm_device_t *dev, void *arg) {
    struct drm_mode_closefb *closefb = (struct drm_mode_closefb *)arg;

    if (!closefb || closefb->fb_id == 0) {
        return -EINVAL;
    }
    if (closefb->pad != 0) {
        return -EINVAL;
    }

    return drm_framebuffer_close(dev, closefb->fb_id);
}

static ssize_t drm_ioctl_mode_rmfb(drm_device_t *dev, void *arg) {
    if (!arg) {
        return -EINVAL;
    }

    uint32_t fb_id = *(uint32_t *)arg;
    if (fb_id == 0) {
        return -EINVAL;
    }

    return drm_framebuffer_remove(dev, fb_id);
}

/**
 * drm_ioctl_mode_setcrtc - Handle DRM_IOCTL_MODE_SETCRTC
 */
ssize_t drm_ioctl_mode_setcrtc(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_crtc *crtc_cmd = (struct drm_mode_crtc *)arg;

    // Find the CRTC by ID
    drm_crtc_t *crtc = drm_crtc_get(&dev->resource_mgr, crtc_cmd->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }

    // Update CRTC state
    uint32_t old_fb_id = crtc->fb_id;
    crtc->fb_id = crtc_cmd->fb_id;
    crtc->x = crtc_cmd->x;
    crtc->y = crtc_cmd->y;
    crtc->mode_valid = crtc_cmd->mode_valid;
    if (crtc_cmd->mode_valid) {
        memcpy(&crtc->mode, &crtc_cmd->mode, sizeof(struct drm_mode_modeinfo));
        crtc->w = crtc_cmd->mode.hdisplay;
        crtc->h = crtc_cmd->mode.vdisplay;
    }

    // Call driver to set CRTC if supported
    int ret = 0;
    if (dev->op->set_crtc) {
        ret = dev->op->set_crtc(dev, crtc_cmd, fd);
    }

    if (ret == 0 && old_fb_id != 0 && old_fb_id != crtc_cmd->fb_id) {
        drm_framebuffer_cleanup_closed(dev, old_fb_id);
    }

    // Release reference
    drm_crtc_free(&dev->resource_mgr, crtc->id);
    return ret;
}

/**
 * drm_ioctl_mode_getplaneresources - Handle DRM_IOCTL_MODE_GETPLANERESOURCES
 */
ssize_t drm_ioctl_mode_getplaneresources(drm_device_t *dev, void *arg,
                                         fd_t *fd) {
    struct drm_mode_get_plane_res *res = (struct drm_mode_get_plane_res *)arg;
    drm_file_t *drm_file = drm_file_from_vfs_file(fd);
    uint32_t planes_cap = res->count_planes;

    // Count available planes
    res->count_planes = 0;
    for (uint32_t i = 0; i < DRM_MAX_PLANES_PER_DEVICE; i++) {
        if (dev->resource_mgr.planes[i] &&
            drm_file_has_object(drm_file, dev->resource_mgr.planes[i]->id)) {
            res->count_planes++;
        }
    }

    // Fill plane IDs if pointer provided
    if (res->plane_id_ptr && planes_cap > 0 && res->count_planes > 0) {
        uint32_t copy_count = MIN(planes_cap, res->count_planes);
        uint32_t idx = 0;
        for (uint32_t i = 0; i < DRM_MAX_PLANES_PER_DEVICE; i++) {
            if (idx >= copy_count) {
                break;
            }
            if (dev->resource_mgr.planes[i] &&
                drm_file_has_object(drm_file,
                                    dev->resource_mgr.planes[i]->id)) {
                uint32_t plane_id = dev->resource_mgr.planes[i]->id;
                int ret = drm_copy_to_user_ptr(res->plane_id_ptr +
                                                   idx * sizeof(uint32_t),
                                               &plane_id, sizeof(plane_id));
                if (ret) {
                    return ret;
                }
                idx++;
            }
        }
    }

    return 0;
}

/**
 * drm_ioctl_mode_getplane - Handle DRM_IOCTL_MODE_GETPLANE
 */
ssize_t drm_ioctl_mode_getplane(drm_device_t *dev, void *arg) {
    struct drm_mode_get_plane *plane_cmd = (struct drm_mode_get_plane *)arg;
    uint32_t format_cap = plane_cmd->count_format_types;

    // Find the plane by ID
    drm_plane_t *plane = drm_plane_get(&dev->resource_mgr, plane_cmd->plane_id);
    if (!plane) {
        return -ENOENT;
    }

    plane_cmd->plane_id = plane->id;
    plane_cmd->crtc_id = plane->crtc_id;
    plane_cmd->fb_id = plane->fb_id;
    plane_cmd->possible_crtcs = plane->possible_crtcs;
    plane_cmd->gamma_size = plane->gamma_size;
    plane_cmd->count_format_types = plane->count_format_types;

    // Fill format types if pointer provided
    if (plane_cmd->format_type_ptr && format_cap > 0 &&
        plane->count_format_types > 0 && plane->format_types) {
        uint32_t copy_count = MIN(format_cap, plane->count_format_types);
        int ret = drm_copy_to_user_ptr(plane_cmd->format_type_ptr,
                                       plane->format_types,
                                       copy_count * sizeof(uint32_t));
        if (ret) {
            drm_plane_free(&dev->resource_mgr, plane->id);
            return ret;
        }
    }

    // Release reference
    drm_plane_free(&dev->resource_mgr, plane->id);
    return 0;
}

/**
 * drm_ioctl_mode_setplane - Handle DRM_IOCTL_MODE_SETPLANE
 */
ssize_t drm_ioctl_mode_setplane(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_set_plane *plane_cmd = (struct drm_mode_set_plane *)arg;

    // Find the plane by ID
    drm_plane_t *plane = drm_plane_get(&dev->resource_mgr, plane_cmd->plane_id);
    if (!plane) {
        return -ENOENT;
    }

    // Update plane state
    uint32_t old_fb_id = plane->fb_id;
    plane->crtc_id = plane_cmd->crtc_id;
    plane->fb_id = plane_cmd->fb_id;
    plane->src_x = plane_cmd->src_x;
    plane->src_y = plane_cmd->src_y;
    plane->src_w = plane_cmd->src_w;
    plane->src_h = plane_cmd->src_h;
    plane->crtc_x = plane_cmd->crtc_x;
    plane->crtc_y = plane_cmd->crtc_y;
    plane->crtc_w = plane_cmd->crtc_w;
    plane->crtc_h = plane_cmd->crtc_h;

    // Call driver to set plane (if supported)
    int ret = 0;
    if (dev->op->set_plane) {
        ret = dev->op->set_plane(dev, plane_cmd, fd);
    }

    if (ret == 0 && old_fb_id != 0 && old_fb_id != plane_cmd->fb_id) {
        drm_framebuffer_cleanup_closed(dev, old_fb_id);
    }

    // Release reference
    drm_plane_free(&dev->resource_mgr, plane->id);
    return ret;
}

/**
 * drm_ioctl_mode_getproperty - Handle DRM_IOCTL_MODE_GETPROPERTY
 */
ssize_t drm_ioctl_mode_getproperty(drm_device_t *dev, void *arg) {
    struct drm_mode_get_property *prop = (struct drm_mode_get_property *)arg;
    uint32_t values_cap = prop->count_values;
    uint32_t enum_blobs_cap = prop->count_enum_blobs;

    switch (prop->prop_id) {
    case DRM_PROPERTY_ID_FB_ID:
        prop->flags = DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "FB_ID", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 1;
        if (prop->values_ptr) {
            uint64_t values[1] = {DRM_MODE_OBJECT_FB};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_CRTC_ID:
    case DRM_CONNECTOR_CRTC_ID_PROP_ID:
        prop->flags = DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "CRTC_ID", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 1;
        if (prop->values_ptr) {
            uint64_t values[1] = {DRM_MODE_OBJECT_CRTC};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_CRTC_X:
    case DRM_PROPERTY_ID_CRTC_Y:
        prop->flags = DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC;
        if (prop->prop_id == DRM_PROPERTY_ID_CRTC_X)
            strncpy((char *)prop->name, "CRTC_X", DRM_PROP_NAME_LEN);
        else
            strncpy((char *)prop->name, "CRTC_Y", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {(uint64_t)(-(1LL << 31)),
                                  (uint64_t)((1LL << 31) - 1)};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_SRC_X:
    case DRM_PROPERTY_ID_SRC_Y:
    case DRM_PROPERTY_ID_SRC_W:
    case DRM_PROPERTY_ID_SRC_H:
        prop->flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        if (prop->prop_id == DRM_PROPERTY_ID_SRC_X) {
            strncpy((char *)prop->name, "SRC_X", DRM_PROP_NAME_LEN);
        } else if (prop->prop_id == DRM_PROPERTY_ID_SRC_Y) {
            strncpy((char *)prop->name, "SRC_Y", DRM_PROP_NAME_LEN);
        } else if (prop->prop_id == DRM_PROPERTY_ID_SRC_W) {
            strncpy((char *)prop->name, "SRC_W", DRM_PROP_NAME_LEN);
        } else {
            strncpy((char *)prop->name, "SRC_H", DRM_PROP_NAME_LEN);
        }
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {0, UINT32_MAX};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_HOTSPOT_X:
    case DRM_PROPERTY_ID_HOTSPOT_Y:
        prop->flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        if (prop->prop_id == DRM_PROPERTY_ID_HOTSPOT_X)
            strncpy((char *)prop->name, "HOTSPOT_X", DRM_PROP_NAME_LEN);
        else
            strncpy((char *)prop->name, "HOTSPOT_Y", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {0, INT32_MAX};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_CRTC_W:
    case DRM_PROPERTY_ID_CRTC_H:
        prop->flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        if (prop->prop_id == DRM_PROPERTY_ID_CRTC_W)
            strncpy((char *)prop->name, "CRTC_W", DRM_PROP_NAME_LEN);
        else
            strncpy((char *)prop->name, "CRTC_H", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {0, 8192};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_IN_FENCE_FD:
        prop->flags = DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "IN_FENCE_FD", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {(uint64_t)(-(1LL << 31)),
                                  (uint64_t)((1LL << 31) - 1)};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_FB_DAMAGE_CLIPS:
        prop->flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "FB_DAMAGE_CLIPS", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 0;
        return 0;

    case DRM_CRTC_OUT_FENCE_PTR_PROP_ID:
        if (!dev || !dev->op || !dev->op->driver_ioctl) {
            return -EINVAL;
        }
        prop->flags = DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "OUT_FENCE_PTR", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {0, INT64_MAX};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_PROPERTY_ID_PLANE_TYPE:
        prop->flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
        strncpy((char *)prop->name, "type", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 3;
        if (prop->enum_blob_ptr) {
            struct drm_mode_property_enum enums[3];
            memset(enums, 0, sizeof(enums));

            strncpy(enums[0].name, "Primary", DRM_PROP_NAME_LEN);
            enums[0].value = DRM_PLANE_TYPE_PRIMARY;
            strncpy(enums[1].name, "Overlay", DRM_PROP_NAME_LEN);
            enums[1].value = DRM_PLANE_TYPE_OVERLAY;
            strncpy(enums[2].name, "Cursor", DRM_PROP_NAME_LEN);
            enums[2].value = DRM_PLANE_TYPE_CURSOR;

            uint32_t copy_enums = MIN(enum_blobs_cap, prop->count_enum_blobs);
            int ret = drm_copy_to_user_ptr(
                prop->enum_blob_ptr, enums,
                copy_enums * sizeof(struct drm_mode_property_enum));
            if (ret) {
                return ret;
            }
        }
        prop->count_values = 0;
        return 0;

    case DRM_CRTC_MODE_ID_PROP_ID:
        prop->flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "MODE_ID", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 0;
        return 0;

    case DRM_CRTC_ACTIVE_PROP_ID:
        prop->flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        strncpy((char *)prop->name, "ACTIVE", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2] = {0, 1};
            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;

    case DRM_FB_WIDTH_PROP_ID:
    case DRM_FB_HEIGHT_PROP_ID:
    case DRM_FB_BPP_PROP_ID:
    case DRM_FB_DEPTH_PROP_ID: {
        prop->flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC;
        if (prop->prop_id == DRM_FB_WIDTH_PROP_ID)
            strncpy((char *)prop->name, "WIDTH", DRM_PROP_NAME_LEN);
        else if (prop->prop_id == DRM_FB_HEIGHT_PROP_ID)
            strncpy((char *)prop->name, "HEIGHT", DRM_PROP_NAME_LEN);
        else if (prop->prop_id == DRM_FB_BPP_PROP_ID)
            strncpy((char *)prop->name, "BPP", DRM_PROP_NAME_LEN);
        else
            strncpy((char *)prop->name, "DEPTH", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 2;
        if (prop->values_ptr) {
            uint64_t values[2];
            if (prop->prop_id == DRM_FB_BPP_PROP_ID ||
                prop->prop_id == DRM_FB_DEPTH_PROP_ID) {
                values[0] = 8;
                values[1] = 32;
            } else {
                values[0] = 1;
                values[1] = 8192;
            }

            uint32_t copy_values = MIN(values_cap, prop->count_values);
            int ret = drm_copy_to_user_ptr(prop->values_ptr, values,
                                           copy_values * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        return 0;
    }

    case DRM_CONNECTOR_DPMS_PROP_ID:
        prop->flags = DRM_MODE_PROP_ENUM;
        strncpy((char *)prop->name, "DPMS", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 4;
        if (prop->enum_blob_ptr) {
            struct drm_mode_property_enum enums[4];
            memset(enums, 0, sizeof(enums));

            strncpy(enums[0].name, "On", DRM_PROP_NAME_LEN);
            enums[0].value = DRM_MODE_DPMS_ON;
            strncpy(enums[1].name, "Standby", DRM_PROP_NAME_LEN);
            enums[1].value = DRM_MODE_DPMS_STANDBY;
            strncpy(enums[2].name, "Suspend", DRM_PROP_NAME_LEN);
            enums[2].value = DRM_MODE_DPMS_SUSPEND;
            strncpy(enums[3].name, "Off", DRM_PROP_NAME_LEN);
            enums[3].value = DRM_MODE_DPMS_OFF;

            uint32_t copy_enums = MIN(enum_blobs_cap, prop->count_enum_blobs);
            int ret = drm_copy_to_user_ptr(
                prop->enum_blob_ptr, enums,
                copy_enums * sizeof(struct drm_mode_property_enum));
            if (ret) {
                return ret;
            }
        }
        prop->count_values = 0;
        return 0;

    case DRM_CONNECTOR_EDID_PROP_ID:
        prop->flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE;
        strncpy((char *)prop->name, "EDID", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 0;
        return 0;

    case DRM_PROPERTY_ID_IN_FORMATS:
        prop->flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE;
        strncpy((char *)prop->name, "IN_FORMATS", DRM_PROP_NAME_LEN);
        prop->name[DRM_PROP_NAME_LEN - 1] = '\0';
        prop->count_enum_blobs = 0;
        prop->count_values = 0;
        return 0;

    default:
        printk("drm: Unsupported property ID: %d\n", prop->prop_id);
        return -EINVAL;
    }
}

/**
 * drm_ioctl_mode_createpropblob - Handle DRM_IOCTL_MODE_CREATEPROPBLOB
 */
ssize_t drm_ioctl_mode_createpropblob(drm_device_t *dev, void *arg) {
    struct drm_mode_create_blob *create_blob =
        (struct drm_mode_create_blob *)arg;

    if (!create_blob->data || create_blob->length == 0 ||
        create_blob->length > DRM_USER_BLOB_MAX_SIZE) {
        return -EINVAL;
    }

    void *blob_data = malloc(create_blob->length);
    if (!blob_data) {
        return -ENOMEM;
    }

    if (copy_from_user(blob_data, (void *)(uintptr_t)create_blob->data,
                       create_blob->length)) {
        free(blob_data);
        return -EFAULT;
    }

    int free_slot = -1;
    uint32_t blob_id = 0;

    spin_lock(&drm_user_blobs_lock);
    for (int i = 0; i < DRM_MAX_USER_BLOBS; i++) {
        if (!drm_user_blobs[i].used) {
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0) {
        spin_unlock(&drm_user_blobs_lock);
        free(blob_data);
        return -ENOSPC;
    }

    int ret = drm_user_blob_generate_id_locked(&blob_id);
    if (ret) {
        spin_unlock(&drm_user_blobs_lock);
        free(blob_data);
        return ret;
    }

    drm_user_blobs[free_slot].used = true;
    drm_user_blobs[free_slot].dev = dev;
    drm_user_blobs[free_slot].blob_id = blob_id;
    drm_user_blobs[free_slot].length = create_blob->length;
    drm_user_blobs[free_slot].data = blob_data;
    spin_unlock(&drm_user_blobs_lock);

    create_blob->blob_id = blob_id;
    return 0;
}

/**
 * drm_ioctl_mode_destroypropblob - Handle DRM_IOCTL_MODE_DESTROYPROPBLOB
 */
ssize_t drm_ioctl_mode_destroypropblob(drm_device_t *dev, void *arg) {
    struct drm_mode_destroy_blob *destroy_blob =
        (struct drm_mode_destroy_blob *)arg;

    if (destroy_blob->blob_id == 0) {
        return -EINVAL;
    }

    int idx = -1;
    void *blob_data = NULL;

    spin_lock(&drm_user_blobs_lock);
    idx = drm_user_blob_find_index_locked(dev, destroy_blob->blob_id);
    if (idx >= 0) {
        blob_data = drm_user_blobs[idx].data;
        memset(&drm_user_blobs[idx], 0, sizeof(drm_user_blobs[idx]));
        spin_unlock(&drm_user_blobs_lock);

        free(blob_data);
        return 0;
    }
    spin_unlock(&drm_user_blobs_lock);

    uint32_t reserved_obj_id = 0;
    if (destroy_blob->blob_id == DRM_BLOB_ID_PLANE_TYPE ||
        drm_mode_blob_to_crtc_id(destroy_blob->blob_id, &reserved_obj_id) ||
        drm_blob_to_plane_in_formats_id(destroy_blob->blob_id,
                                        &reserved_obj_id) ||
        drm_blob_to_connector_edid_id(destroy_blob->blob_id,
                                      &reserved_obj_id)) {
        return -EPERM;
    }

    return -ENOENT;
}

/**
 * drm_ioctl_mode_getpropblob - Handle DRM_IOCTL_MODE_GETPROPBLOB
 */
ssize_t drm_ioctl_mode_getpropblob(drm_device_t *dev, void *arg) {
    struct drm_mode_get_blob *blob = (struct drm_mode_get_blob *)arg;

    uint32_t crtc_id = 0;
    if (drm_mode_blob_to_crtc_id(blob->blob_id, &crtc_id)) {
        drm_crtc_t *crtc = drm_crtc_get(&dev->resource_mgr, crtc_id);
        if (!crtc) {
            return -ENOENT;
        }

        struct drm_mode_modeinfo mode;
        drm_fill_crtc_modeinfo(dev, crtc, &mode);
        drm_crtc_free(&dev->resource_mgr, crtc->id);

        size_t blob_len = sizeof(mode);
        size_t copy_len = MIN((size_t)blob->length, blob_len);

        blob->length = (uint32_t)blob_len;
        if (copy_len > 0 && blob->data) {
            int ret = drm_copy_to_user_ptr(blob->data, &mode, copy_len);
            if (ret) {
                return ret;
            }
        }
        return 0;
    }

    uint32_t connector_id = 0;
    if (drm_blob_to_connector_edid_id(blob->blob_id, &connector_id)) {
        drm_connector_t *connector =
            drm_connector_get(&dev->resource_mgr, connector_id);
        if (!connector) {
            return -ENOENT;
        }

        uint8_t edid[128];
        drm_build_connector_edid(dev, connector, edid);
        drm_connector_free(&dev->resource_mgr, connector->id);

        size_t blob_len = sizeof(edid);
        size_t copy_len = MIN((size_t)blob->length, blob_len);
        blob->length = (uint32_t)blob_len;

        if (copy_len > 0 && blob->data) {
            int ret = drm_copy_to_user_ptr(blob->data, edid, copy_len);
            if (ret) {
                return ret;
            }
        }
        return 0;
    }

    uint32_t plane_id = 0;
    if (drm_blob_to_plane_in_formats_id(blob->blob_id, &plane_id)) {
        drm_plane_t *plane = drm_plane_get(&dev->resource_mgr, plane_id);
        if (!plane) {
            return -ENOENT;
        }

        if (plane->count_format_types == 0 || !plane->format_types) {
            drm_plane_free(&dev->resource_mgr, plane->id);
            return -ENOENT;
        }

        uint32_t count_formats = plane->count_format_types;
        uint32_t count_modifiers = (count_formats + 63U) / 64U;
        size_t formats_len = (size_t)count_formats * sizeof(uint32_t);
        size_t modifiers_len =
            (size_t)count_modifiers * sizeof(struct drm_format_modifier);
        size_t blob_len = sizeof(struct drm_format_modifier_blob) +
                          formats_len + modifiers_len;

        uint8_t *blob_data = malloc(blob_len);
        if (!blob_data) {
            drm_plane_free(&dev->resource_mgr, plane->id);
            return -ENOMEM;
        }

        struct drm_format_modifier_blob *fmt_blob =
            (struct drm_format_modifier_blob *)blob_data;
        memset(fmt_blob, 0, sizeof(*fmt_blob));
        fmt_blob->version = FORMAT_BLOB_CURRENT;
        fmt_blob->count_formats = count_formats;
        fmt_blob->formats_offset = sizeof(struct drm_format_modifier_blob);
        fmt_blob->count_modifiers = count_modifiers;
        fmt_blob->modifiers_offset = fmt_blob->formats_offset + formats_len;

        uint32_t *formats = (uint32_t *)(blob_data + fmt_blob->formats_offset);
        memcpy(formats, plane->format_types, formats_len);

        struct drm_format_modifier *mods =
            (struct drm_format_modifier *)(blob_data +
                                           fmt_blob->modifiers_offset);
        for (uint32_t chunk = 0; chunk < count_modifiers; chunk++) {
            uint32_t offset = chunk * 64U;
            uint32_t remain = count_formats - offset;
            uint32_t bits = MIN(remain, 64U);

            mods[chunk].offset = offset;
            mods[chunk].pad = 0;
            mods[chunk].modifier = 0;
            mods[chunk].formats =
                (bits == 64U) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
        }

        drm_plane_free(&dev->resource_mgr, plane->id);

        size_t copy_len = MIN((size_t)blob->length, blob_len);
        blob->length = (uint32_t)blob_len;
        int ret = 0;
        if (copy_len > 0 && blob->data) {
            ret = drm_copy_to_user_ptr(blob->data, blob_data, copy_len);
        }
        free(blob_data);
        return ret;
    }

    size_t snapshot_cap =
        blob->data ? MIN((size_t)blob->length, (size_t)DRM_USER_BLOB_MAX_SIZE)
                   : 0;
    void *snapshot = snapshot_cap ? malloc(snapshot_cap) : NULL;
    if (snapshot_cap && !snapshot)
        return -ENOMEM;

    spin_lock(&drm_user_blobs_lock);
    int idx = drm_user_blob_find_index_locked(dev, blob->blob_id);
    if (idx >= 0) {
        size_t blob_len = drm_user_blobs[idx].length;
        size_t copy_len = MIN(snapshot_cap, blob_len);
        blob->length = (uint32_t)blob_len;
        if (copy_len > 0)
            memcpy(snapshot, drm_user_blobs[idx].data, copy_len);
        spin_unlock(&drm_user_blobs_lock);

        int ret = drm_copy_to_user_ptr(blob->data, snapshot, copy_len);
        free(snapshot);
        return ret;
    }
    spin_unlock(&drm_user_blobs_lock);
    free(snapshot);

    switch (blob->blob_id) {
    case DRM_BLOB_ID_PLANE_TYPE: {
        static const char plane_type_blob[] = "Primary";
        size_t blob_len = sizeof(plane_type_blob) - 1;
        size_t copy_len = MIN((size_t)blob->length, blob_len);

        blob->length = (uint32_t)blob_len;
        if (copy_len > 0 && blob->data) {
            int ret =
                drm_copy_to_user_ptr(blob->data, plane_type_blob, copy_len);
            if (ret) {
                return ret;
            }
        }
        break;
    }

    default:
        printk("drm: Invalid blob id %d\n", blob->blob_id);
        return -ENOENT;
    }

    return 0;
}

int drm_property_lookup_blob_data(drm_device_t *dev, uint32_t blob_id,
                                  const void **data, uint32_t *length) {
    if (!dev || !data || !length || blob_id == 0) {
        return -EINVAL;
    }

    spin_lock(&drm_user_blobs_lock);
    int idx = drm_user_blob_find_index_locked(dev, blob_id);
    if (idx >= 0) {
        *data = drm_user_blobs[idx].data;
        *length = drm_user_blobs[idx].length;
        spin_unlock(&drm_user_blobs_lock);
        return 0;
    }
    spin_unlock(&drm_user_blobs_lock);

    return -ENOENT;
}

int drm_property_get_modeinfo_from_blob(drm_device_t *dev, uint32_t blob_id,
                                        struct drm_mode_modeinfo *mode) {
    if (!dev || !mode || blob_id == 0) {
        return -EINVAL;
    }

    const void *blob_data = NULL;
    uint32_t blob_len = 0;
    int ret =
        drm_property_lookup_blob_data(dev, blob_id, &blob_data, &blob_len);
    if (ret == 0) {
        if (!blob_data || blob_len < sizeof(*mode)) {
            return -EINVAL;
        }

        memcpy(mode, blob_data, sizeof(*mode));
        return 0;
    }

    uint32_t crtc_id = 0;
    if (drm_mode_blob_to_crtc_id(blob_id, &crtc_id)) {
        drm_crtc_t *crtc = drm_crtc_get(&dev->resource_mgr, crtc_id);
        if (!crtc) {
            return -ENOENT;
        }

        drm_fill_crtc_modeinfo(dev, crtc, mode);
        drm_crtc_free(&dev->resource_mgr, crtc->id);
        return 0;
    }

    return ret;
}

static ssize_t drm_mode_resolve_obj_type(drm_device_t *dev, uint32_t obj_id,
                                         uint32_t *obj_type) {
    uint32_t resolved_type = DRM_MODE_OBJECT_ANY;

    for (int idx = 0; idx < DRM_MAX_CONNECTORS_PER_DEVICE; idx++) {
        if (!dev->resource_mgr.connectors[idx] ||
            dev->resource_mgr.connectors[idx]->id != obj_id) {
            continue;
        }

        resolved_type = DRM_MODE_OBJECT_CONNECTOR;
        break;
    }

    for (int idx = 0; idx < DRM_MAX_CRTCS_PER_DEVICE; idx++) {
        if (!dev->resource_mgr.crtcs[idx] ||
            dev->resource_mgr.crtcs[idx]->id != obj_id) {
            continue;
        }

        if (resolved_type != DRM_MODE_OBJECT_ANY &&
            resolved_type != DRM_MODE_OBJECT_CRTC) {
            return -EINVAL;
        }
        resolved_type = DRM_MODE_OBJECT_CRTC;
        break;
    }

    for (int idx = 0; idx < DRM_MAX_ENCODERS_PER_DEVICE; idx++) {
        if (!dev->resource_mgr.encoders[idx] ||
            dev->resource_mgr.encoders[idx]->id != obj_id) {
            continue;
        }

        if (resolved_type != DRM_MODE_OBJECT_ANY &&
            resolved_type != DRM_MODE_OBJECT_ENCODER) {
            return -EINVAL;
        }
        resolved_type = DRM_MODE_OBJECT_ENCODER;
        break;
    }

    for (int idx = 0; idx < DRM_MAX_FRAMEBUFFERS_PER_DEVICE; idx++) {
        if (!dev->resource_mgr.framebuffers[idx] ||
            dev->resource_mgr.framebuffers[idx]->id != obj_id) {
            continue;
        }

        if (resolved_type != DRM_MODE_OBJECT_ANY &&
            resolved_type != DRM_MODE_OBJECT_FB) {
            return -EINVAL;
        }
        resolved_type = DRM_MODE_OBJECT_FB;
        break;
    }

    for (int idx = 0; idx < DRM_MAX_PLANES_PER_DEVICE; idx++) {
        if (!dev->resource_mgr.planes[idx] ||
            dev->resource_mgr.planes[idx]->id != obj_id) {
            continue;
        }

        if (resolved_type != DRM_MODE_OBJECT_ANY &&
            resolved_type != DRM_MODE_OBJECT_PLANE) {
            return -EINVAL;
        }
        resolved_type = DRM_MODE_OBJECT_PLANE;
        break;
    }

    if (resolved_type == DRM_MODE_OBJECT_ANY) {
        return -ENOENT;
    }

    *obj_type = resolved_type;
    return 0;
}

/**
 * drm_ioctl_mode_obj_getproperties - Handle DRM_IOCTL_MODE_OBJ_GETPROPERTIES
 */
ssize_t drm_ioctl_mode_obj_getproperties(drm_device_t *dev, void *arg) {
    struct drm_mode_obj_get_properties *props =
        (struct drm_mode_obj_get_properties *)arg;
    uint32_t obj_type = props->obj_type;
    uint32_t props_cap = props->count_props;

    if (obj_type == DRM_MODE_OBJECT_ANY) {
        int ret = drm_mode_resolve_obj_type(dev, props->obj_id, &obj_type);
        if (ret == -ENOENT) {
            printk("drm: Unknown object ID: %d\n", props->obj_id);
        } else if (ret == -EINVAL) {
            printk("drm: Ambiguous object ID: %d\n", props->obj_id);
        }
        if (ret) {
            return ret;
        }
    }

    switch (obj_type) {
    case DRM_MODE_OBJECT_PLANE: {
        // 查找对应的 plane
        drm_plane_t *plane = NULL;
        for (int idx = 0; idx < DRM_MAX_PLANES_PER_DEVICE; idx++) {
            if (dev->resource_mgr.planes[idx] &&
                dev->resource_mgr.planes[idx]->id == props->obj_id) {
                plane = dev->resource_mgr.planes[idx];
                break;
            }
        }

        if (!plane) {
            return -ENOENT;
        }

        uint32_t prop_count =
            plane->plane_type == DRM_PLANE_TYPE_CURSOR ? 16 : 14;

        if (props->props_ptr) {
            uint32_t copy_props = MIN(props_cap, prop_count);
            uint32_t prop_ids[16] = {
                DRM_PROPERTY_ID_PLANE_TYPE,  DRM_PROPERTY_ID_IN_FORMATS,
                DRM_PROPERTY_ID_FB_ID,       DRM_PROPERTY_ID_CRTC_ID,
                DRM_PROPERTY_ID_SRC_X,       DRM_PROPERTY_ID_SRC_Y,
                DRM_PROPERTY_ID_SRC_W,       DRM_PROPERTY_ID_SRC_H,
                DRM_PROPERTY_ID_CRTC_X,      DRM_PROPERTY_ID_CRTC_Y,
                DRM_PROPERTY_ID_CRTC_W,      DRM_PROPERTY_ID_CRTC_H,
                DRM_PROPERTY_ID_IN_FENCE_FD, DRM_PROPERTY_ID_FB_DAMAGE_CLIPS,
                DRM_PROPERTY_ID_HOTSPOT_X,   DRM_PROPERTY_ID_HOTSPOT_Y,
            };
            props->count_props = prop_count;

            int ret = drm_copy_to_user_ptr(props->props_ptr, prop_ids,
                                           copy_props * sizeof(uint32_t));
            if (ret) {
                return ret;
            }
        }
        if (props->prop_values_ptr) {
            uint32_t copy_props;
            uint64_t prop_values[16];

            props->count_props = prop_count;
            copy_props = MIN(props_cap, prop_count);
            prop_values[0] = plane->plane_type;
            prop_values[1] = drm_plane_in_formats_blob_id(plane->id);
            prop_values[2] = plane->fb_id; // 当前关联的 framebuffer
            prop_values[3] = plane->crtc_id;
            prop_values[4] = plane->src_x;
            prop_values[5] = plane->src_y;
            prop_values[6] = plane->src_w;
            prop_values[7] = plane->src_h;
            prop_values[8] = (uint64_t)(int64_t)plane->crtc_x;
            prop_values[9] = (uint64_t)(int64_t)plane->crtc_y;
            prop_values[10] = plane->crtc_w;
            prop_values[11] = plane->crtc_h;

            if ((prop_values[6] == 0 || prop_values[7] == 0) &&
                plane->fb_id != 0) {
                drm_framebuffer_t *fb =
                    drm_framebuffer_get(&dev->resource_mgr, plane->fb_id);

                if (fb) {
                    if (prop_values[6] == 0)
                        prop_values[6] = ((uint64_t)fb->width) << 16;
                    if (prop_values[7] == 0)
                        prop_values[7] = ((uint64_t)fb->height) << 16;
                    drm_framebuffer_free(&dev->resource_mgr, fb->id);
                }
            }

            if ((prop_values[10] == 0 || prop_values[11] == 0) &&
                plane->crtc_id != 0) {
                drm_crtc_t *crtc =
                    drm_crtc_get(&dev->resource_mgr, plane->crtc_id);

                if (crtc) {
                    if (prop_values[10] == 0)
                        prop_values[10] = crtc->w;
                    if (prop_values[11] == 0)
                        prop_values[11] = crtc->h;
                    drm_crtc_free(&dev->resource_mgr, crtc->id);
                }
            }

            prop_values[12] = (uint64_t)-1;
            prop_values[13] = 0;
            prop_values[14] = (uint64_t)(int64_t)plane->hotspot_x;
            prop_values[15] = (uint64_t)(int64_t)plane->hotspot_y;

            int ret = drm_copy_to_user_ptr(props->prop_values_ptr, prop_values,
                                           copy_props * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        } else {
            props->count_props = prop_count;
        }

        break;
    }

    case DRM_MODE_OBJECT_CRTC: {
        drm_crtc_t *crtc = NULL;
        for (int idx = 0; idx < DRM_MAX_CRTCS_PER_DEVICE; idx++) {
            if (dev->resource_mgr.crtcs[idx] &&
                dev->resource_mgr.crtcs[idx]->id == props->obj_id) {
                crtc = dev->resource_mgr.crtcs[idx];
                break;
            }
        }

        if (!crtc) {
            return -ENOENT;
        }

        props->count_props = (dev && dev->op && dev->op->driver_ioctl) ? 3 : 2;

        if (props->props_ptr) {
            uint32_t copy_props = MIN(props_cap, props->count_props);
            uint32_t prop_ids[3] = {DRM_CRTC_ACTIVE_PROP_ID,
                                    DRM_CRTC_MODE_ID_PROP_ID,
                                    DRM_CRTC_OUT_FENCE_PTR_PROP_ID};
            int ret = drm_copy_to_user_ptr(props->props_ptr, prop_ids,
                                           copy_props * sizeof(uint32_t));
            if (ret) {
                return ret;
            }
        }
        if (props->prop_values_ptr) {
            uint32_t copy_props = MIN(props_cap, props->count_props);
            uint64_t prop_values[3] = {1, drm_crtc_mode_blob_id(crtc->id), 0};
            int ret = drm_copy_to_user_ptr(props->prop_values_ptr, prop_values,
                                           copy_props * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        break;
    }

    case DRM_MODE_OBJECT_FB: {
        drm_framebuffer_t *fb = NULL;
        for (int idx = 0; idx < DRM_MAX_FRAMEBUFFERS_PER_DEVICE; idx++) {
            if (dev->resource_mgr.framebuffers[idx] &&
                dev->resource_mgr.framebuffers[idx]->id == props->obj_id) {
                fb = dev->resource_mgr.framebuffers[idx];
                break;
            }
        }

        if (!fb) {
            return -ENOENT;
        }

        props->count_props = 4;

        if (props->props_ptr) {
            uint32_t copy_props = MIN(props_cap, props->count_props);
            uint32_t prop_ids[4] = {DRM_FB_WIDTH_PROP_ID, DRM_FB_HEIGHT_PROP_ID,
                                    DRM_FB_BPP_PROP_ID, DRM_FB_DEPTH_PROP_ID};
            int ret = drm_copy_to_user_ptr(props->props_ptr, prop_ids,
                                           copy_props * sizeof(uint32_t));
            if (ret) {
                return ret;
            }
        }
        if (props->prop_values_ptr) {
            uint32_t copy_props = MIN(props_cap, props->count_props);
            uint64_t prop_values[4] = {fb->width, fb->height, fb->bpp,
                                       fb->depth};
            int ret = drm_copy_to_user_ptr(props->prop_values_ptr, prop_values,
                                           copy_props * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }

        break;
    }

    case DRM_MODE_OBJECT_CONNECTOR: {
        drm_connector_t *connector = NULL;
        for (int idx = 0; idx < DRM_MAX_CONNECTORS_PER_DEVICE; idx++) {
            if (dev->resource_mgr.connectors[idx] &&
                dev->resource_mgr.connectors[idx]->id == props->obj_id) {
                connector = dev->resource_mgr.connectors[idx];
                break;
            }
        }

        if (!connector) {
            return -ENOENT;
        }

        props->count_props = 3;

        if (props->props_ptr) {
            uint32_t copy_props = MIN(props_cap, props->count_props);
            uint32_t prop_ids[3] = {DRM_CONNECTOR_DPMS_PROP_ID,
                                    DRM_CONNECTOR_EDID_PROP_ID,
                                    DRM_CONNECTOR_CRTC_ID_PROP_ID};
            int ret = drm_copy_to_user_ptr(props->props_ptr, prop_ids,
                                           copy_props * sizeof(uint32_t));
            if (ret) {
                return ret;
            }
        }
        if (props->prop_values_ptr) {
            uint32_t copy_props = MIN(props_cap, props->count_props);
            uint64_t prop_values[3] = {DRM_MODE_DPMS_ON, 0, connector->crtc_id};
            prop_values[1] = drm_connector_edid_blob_id(connector->id);
            int ret = drm_copy_to_user_ptr(props->prop_values_ptr, prop_values,
                                           copy_props * sizeof(uint64_t));
            if (ret) {
                return ret;
            }
        }
        break;
    }

    case DRM_MODE_OBJECT_ENCODER: {
        drm_encoder_t *encoder = NULL;
        for (int idx = 0; idx < DRM_MAX_ENCODERS_PER_DEVICE; idx++) {
            if (dev->resource_mgr.encoders[idx] &&
                dev->resource_mgr.encoders[idx]->id == props->obj_id) {
                encoder = dev->resource_mgr.encoders[idx];
                break;
            }
        }

        if (!encoder) {
            return -ENOENT;
        }

        props->count_props = 0;
        break;
    }

    default:
        printk("drm: Unsupported object type: %d\n", obj_type);
        return -EINVAL;
    }

    return 0;
}

/**
 * drm_ioctl_set_client_cap - Handle DRM_IOCTL_SET_CLIENT_CAP
 */
ssize_t drm_ioctl_set_client_cap(drm_device_t *dev, void *arg) {
    struct drm_set_client_cap *cap = (struct drm_set_client_cap *)arg;
    switch (cap->capability) {
    case DRM_CLIENT_CAP_ATOMIC:
        return 0;
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        return 0;
    case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
        return 0;
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
        return 0;
    default:
        printk("drm: Invalid client capability %d\n", cap->capability);
        return -EINVAL;
    }
}

/**
 * drm_ioctl_wait_vblank - Handle DRM_IOCTL_WAIT_VBLANK
 */
ssize_t drm_ioctl_wait_vblank(drm_device_t *dev, void *arg, fd_t *fd) {
    union drm_wait_vblank *vbl = (union drm_wait_vblank *)arg;

    if (!dev || !vbl)
        return -EINVAL;

    uint32_t type = vbl->request.type;
    uint32_t sequence = vbl->request.sequence;
    uint64_t user_data = vbl->request.signal;
    uint32_t allowed = _DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK |
                       _DRM_VBLANK_FLIP | _DRM_VBLANK_HIGH_CRTC_MASK;

    if (type & ~allowed)
        return -EINVAL;
    if (type & _DRM_VBLANK_SIGNAL)
        return -EINVAL;

    spin_lock(&dev->event_lock);
    uint64_t seq = dev->vblank_counter;
    spin_unlock(&dev->event_lock);

    uint64_t target_seq =
        (type & _DRM_VBLANK_RELATIVE) ? seq + sequence : sequence;

    if (target_seq <= seq && (type & _DRM_VBLANK_NEXTONMISS))
        target_seq = seq + 1;

    if (type & _DRM_VBLANK_EVENT)
        return drm_defer_event_at(dev, fd, DRM_EVENT_VBLANK, user_data,
                                  target_seq);

    while (true) {
        spin_lock(&dev->event_lock);
        seq = dev->vblank_counter;
        spin_unlock(&dev->event_lock);

        if (seq >= target_seq) {
            vbl->reply.sequence = seq;
            vbl->reply.tval_sec = nano_time() / 1000000000ULL;
            vbl->reply.tval_usec = (nano_time() % 1000000000ULL) / 1000ULL;
            return 0;
        }

        arch_enable_interrupt();
        arch_wait_for_interrupt();
        arch_disable_interrupt();
    }
}

/**
 * drm_ioctl_get_unique - Handle DRM_IOCTL_GET_UNIQUE
 */
ssize_t drm_ioctl_get_unique(drm_device_t *dev, void *arg) {
    struct drm_unique *u = (struct drm_unique *)arg;
    char unique[32];
    unique[0] = '\0';

    if (dev && dev->pci_dev) {
        sprintf(unique, "pci:%04x:%02x:%02x.%d", dev->pci_dev->segment,
                dev->pci_dev->bus, dev->pci_dev->slot, dev->pci_dev->func);
    }

    if (u->unique && copy_to_user_str((char *)(uintptr_t)u->unique, unique,
                                      u->unique_len ? u->unique_len : 1)) {
        return -EFAULT;
    }
    u->unique_len = strlen(unique);

    return 0;
}

/**
 * drm_ioctl_page_flip - Handle DRM_IOCTL_MODE_PAGE_FLIP
 */
ssize_t drm_ioctl_page_flip(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op->page_flip) {
        return -ENOTTY;
    }
    return dev->op->page_flip(dev, (struct drm_mode_crtc_page_flip *)arg, fd);
}

/**
 * drm_ioctl_cursor - Handle DRM_IOCTL_MODE_CURSOR
 */
ssize_t drm_ioctl_cursor(drm_device_t *dev, void *arg) {
    struct drm_mode_cursor *cmd = (struct drm_mode_cursor *)arg;
    if (!dev || !cmd) {
        return -EINVAL;
    }

    if (cmd->flags & ~(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE)) {
        return -EINVAL;
    }

    if (!dev->op || !dev->op->set_cursor) {
        return -ENOTSUP;
    }

    return dev->op->set_cursor(dev, cmd);
}

/**
 * drm_ioctl_cursor2 - Handle DRM_IOCTL_MODE_CURSOR2
 */
ssize_t drm_ioctl_cursor2(drm_device_t *dev, void *arg) {
    struct drm_mode_cursor2 *cmd = (struct drm_mode_cursor2 *)arg;
    if (!dev || !cmd) {
        return -EINVAL;
    }

    if (cmd->flags & ~(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE)) {
        return -EINVAL;
    }

    if (!dev->op || !dev->op->set_cursor) {
        return -ENOTSUP;
    }

    struct drm_mode_cursor legacy = {
        .flags = cmd->flags,
        .crtc_id = cmd->crtc_id,
        .x = cmd->x - cmd->hot_x,
        .y = cmd->y - cmd->hot_y,
        .width = cmd->width,
        .height = cmd->height,
        .handle = cmd->handle,
    };

    return dev->op->set_cursor(dev, &legacy);
}

/**
 * drm_ioctl_atomic - Handle DRM_IOCTL_MODE_ATOMIC
 */
ssize_t drm_ioctl_atomic(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_atomic *cmd = (struct drm_mode_atomic *)arg;
    if (!dev->op->atomic_commit) {
        return -ENOTTY;
    }

    return dev->op->atomic_commit(dev, cmd, fd);
}

/**
 * drm_ioctl_get_magic - Handle DRM_IOCTL_GET_MAGIC
 */
ssize_t drm_ioctl_get_magic(drm_device_t *dev, void *arg) {
    drm_auth_t *auth = (drm_auth_t *)arg;
    (void)dev;

    auth->magic = 0x12345678;
    return 0;
}

/**
 * drm_ioctl_auth_magic - Handle DRM_IOCTL_AUTH_MAGIC
 */
ssize_t drm_ioctl_auth_magic(drm_device_t *dev, void *arg) {
    drm_auth_t *auth = (drm_auth_t *)arg;
    if (auth->magic != 0x12345678)
        return -EINVAL;

    return 0;
}

/**
 * drm_ioctl_set_master - Handle DRM_IOCTL_SET_MASTER
 */
ssize_t drm_ioctl_set_master(drm_device_t *dev, void *arg) {
    (void)dev;
    (void)arg;
    return 0;
}

/**
 * drm_ioctl_drop_master - Handle DRM_IOCTL_DROP_MASTER
 */
ssize_t drm_ioctl_drop_master(drm_device_t *dev, void *arg) {
    (void)dev;
    (void)arg;
    return 0;
}

/**
 * drm_ioctl_gamma - Handle DRM_IOCTL_MODE_GETGAMMA/DRM_IOCTL_MODE_SETGAMMA
 */
ssize_t drm_ioctl_gamma(drm_device_t *dev, void *arg, ssize_t cmd) {
    (void)dev;
    (void)arg;
    (void)cmd;
    return 0;
}

/**
 * drm_ioctl_dirtyfb - Handle DRM_IOCTL_MODE_DIRTYFB
 */
ssize_t drm_ioctl_dirtyfb(drm_device_t *dev, void *arg, fd_t *fd) {
    if (!dev->op || !dev->op->dirty_fb) {
        return 0;
    }

    return dev->op->dirty_fb(dev, (struct drm_mode_fb_dirty_cmd *)arg, fd);
}

/**
 * drm_ioctl_mode_create_lease - Handle DRM_IOCTL_MODE_CREATE_LEASE
 */
ssize_t drm_ioctl_mode_create_lease(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_create_lease *cl = (struct drm_mode_create_lease *)arg;
    drm_file_t *owner = drm_file_from_vfs_file(fd);
    uint32_t *object_ids;
    int ret;

    if (!dev || !cl || !drm_file_can_create_lease(owner)) {
        return -EACCES;
    }

    if (cl->flags & ~(DRM_CLOEXEC | O_NONBLOCK)) {
        return -EINVAL;
    }

    if (cl->object_count == 0 || cl->object_count > DRM_MAX_LEASE_OBJECTS ||
        !cl->object_ids) {
        return -EINVAL;
    }

    object_ids = calloc(cl->object_count, sizeof(uint32_t));
    if (!object_ids) {
        return -ENOMEM;
    }

    if (copy_from_user(object_ids, (void *)(uintptr_t)cl->object_ids,
                       cl->object_count * sizeof(uint32_t))) {
        free(object_ids);
        return -EFAULT;
    }

    for (uint32_t i = 0; i < cl->object_count; i++) {
        if (!drm_mode_object_exists(dev, object_ids[i])) {
            free(object_ids);
            return -ENOENT;
        }
        for (uint32_t j = i + 1; j < cl->object_count; j++) {
            if (object_ids[i] == object_ids[j]) {
                free(object_ids);
                return -EINVAL;
            }
        }
    }

    ret = drm_leasefd_create(dev, owner, object_ids, cl->object_count,
                             cl->flags, &cl->lessee_id);
    free(object_ids);
    if (ret < 0) {
        return ret;
    }

    cl->fd = (uint32_t)ret;
    return 0;
}

/**
 * drm_ioctl_mode_list_lessees - Handle DRM_IOCTL_MODE_LIST_LESSEES
 */
ssize_t drm_ioctl_mode_list_lessees(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_list_lessees *l = (struct drm_mode_list_lessees *)arg;
    drm_file_t *file = drm_file_from_vfs_file(fd);
    uint64_t lessor_id = drm_file_lessor_id(file);
    uint32_t cap;
    uint32_t total = 0;
    uint32_t copy_count = 0;
    uint64_t ids[DRM_MAX_LEASES_PER_DEVICE];

    if (!dev || !l || !file || file->lessee) {
        return -EACCES;
    }

    cap = l->count_lessees;

    spin_lock(&dev->lease_lock);
    for (uint32_t i = 0; i < DRM_MAX_LEASES_PER_DEVICE; i++) {
        if (!dev->leases[i].active || dev->leases[i].lessor_id != lessor_id) {
            continue;
        }

        uint64_t id = dev->leases[i].lease_id;
        if (l->lessees_ptr && copy_count < cap)
            ids[copy_count++] = id;
        total++;
    }
    spin_unlock(&dev->lease_lock);

    if (l->lessees_ptr && copy_count > 0) {
        int ret = drm_copy_to_user_ptr(l->lessees_ptr, ids,
                                       copy_count * sizeof(ids[0]));
        if (ret)
            return ret;
    }

    l->count_lessees = total;
    return 0;
}

/**
 * drm_ioctl_mode_get_lease - Handle DRM_IOCTL_MODE_GET_LEASE
 */
ssize_t drm_ioctl_mode_get_lease(drm_device_t *dev, void *arg, fd_t *fd) {
    struct drm_mode_get_lease *gl = (struct drm_mode_get_lease *)arg;
    drm_file_t *file = drm_file_from_vfs_file(fd);
    uint32_t cap;
    uint32_t total;
    uint32_t copy_count;

    (void)dev;

    if (!gl || !file) {
        return -EACCES;
    }

    if (!file->lessee || file->revoked) {
        gl->count_objects = 0;
        return 0;
    }

    cap = gl->count_objects;
    total = file->object_count;
    copy_count = MIN(cap, total);

    if (gl->objects_ptr && copy_count > 0) {
        int ret = drm_copy_to_user_ptr(gl->objects_ptr, file->object_ids,
                                       copy_count * sizeof(uint32_t));
        if (ret) {
            return ret;
        }
    }

    gl->count_objects = total;
    return 0;
}

/**
 * drm_ioctl_mode_revoke_lease - Handle DRM_IOCTL_MODE_REVOKE_LEASE
 */
ssize_t drm_ioctl_mode_revoke_lease(drm_device_t *dev, void *arg) {
    struct drm_mode_revoke_lease *rl = (struct drm_mode_revoke_lease *)arg;

    if (!dev || !rl) {
        return -EINVAL;
    }

    spin_lock(&dev->lease_lock);
    for (uint32_t i = 0; i < DRM_MAX_LEASES_PER_DEVICE; i++) {
        drm_file_t *file = dev->leases[i].file;
        if (!dev->leases[i].active ||
            dev->leases[i].lease_id != rl->lessee_id || !file) {
            continue;
        }

        file->revoked = true;
        file->object_count = 0;
        dev->leases[i].active = false;
        dev->leases[i].file = NULL;
        spin_unlock(&dev->lease_lock);
        return 0;
    }
    spin_unlock(&dev->lease_lock);

    return -ENOENT;
}

static bool drm_ioctl_allow_on_render_node(drm_device_t *dev, uint32_t cmd) {
    uint32_t nr = _IOC_NR(cmd);

    if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
        return dev && dev->op && dev->op->driver_ioctl;
    }

    switch (cmd) {
    case DRM_IOCTL_VERSION:
    case DRM_IOCTL_GET_CAP:
    case DRM_IOCTL_GET_UNIQUE:
    case DRM_IOCTL_GEM_CLOSE:
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
    case DRM_IOCTL_MODE_CREATE_DUMB:
    case DRM_IOCTL_MODE_MAP_DUMB:
    case DRM_IOCTL_MODE_DESTROY_DUMB:
    case DRM_IOCTL_SET_CLIENT_CAP:
    case DRM_IOCTL_SET_VERSION:
    case DRM_IOCTL_SYNCOBJ_CREATE:
    case DRM_IOCTL_SYNCOBJ_DESTROY:
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
    case DRM_IOCTL_SYNCOBJ_WAIT:
    case DRM_IOCTL_SYNCOBJ_RESET:
    case DRM_IOCTL_SYNCOBJ_SIGNAL:
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
    case DRM_IOCTL_SYNCOBJ_QUERY:
    case DRM_IOCTL_SYNCOBJ_TRANSFER:
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
    case DRM_IOCTL_SYNCOBJ_EVENTFD:
        return true;
    default:
        return false;
    }
}

static int drm_ioctl_check_lease(drm_file_t *file, uint32_t cmd, void *arg) {
    if (!file || !file->lessee) {
        return 0;
    }

    switch (cmd) {
    case DRM_IOCTL_MODE_GETRESOURCES:
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
    case DRM_IOCTL_MODE_GET_LEASE:
    case DRM_IOCTL_VERSION:
    case DRM_IOCTL_GET_CAP:
    case DRM_IOCTL_GET_UNIQUE:
    case DRM_IOCTL_SET_CLIENT_CAP:
    case DRM_IOCTL_SET_VERSION:
    case DRM_IOCTL_GEM_CLOSE:
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
    case DRM_IOCTL_MODE_CREATE_DUMB:
    case DRM_IOCTL_MODE_MAP_DUMB:
    case DRM_IOCTL_MODE_DESTROY_DUMB:
    case DRM_IOCTL_MODE_ADDFB:
    case DRM_IOCTL_MODE_ADDFB2:
    case DRM_IOCTL_MODE_GETFB:
    case DRM_IOCTL_MODE_CLOSEFB:
    case DRM_IOCTL_MODE_RMFB:
    case DRM_IOCTL_MODE_CREATEPROPBLOB:
    case DRM_IOCTL_MODE_DESTROYPROPBLOB:
    case DRM_IOCTL_MODE_GETPROPBLOB:
    case DRM_IOCTL_MODE_GETPROPERTY:
        return 0;
    case DRM_IOCTL_MODE_SETPROPERTY:
        return drm_file_has_object(
                   file, ((struct drm_mode_connector_set_property *)arg)
                             ->connector_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return drm_file_has_object(
                   file, ((struct drm_mode_obj_get_properties *)arg)->obj_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
        return drm_file_has_object(
                   file, ((struct drm_mode_obj_set_property *)arg)->obj_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_GETCRTC:
        return drm_file_has_object(file, ((struct drm_mode_crtc *)arg)->crtc_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_SETCRTC:
        return drm_file_has_object(file, ((struct drm_mode_crtc *)arg)->crtc_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_GETENCODER:
        if (!file->dev) {
            return -ENOENT;
        }
        for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
            drm_encoder_t *encoder = file->dev->resource_mgr.encoders[i];
            if (encoder &&
                encoder->id ==
                    ((struct drm_mode_get_encoder *)arg)->encoder_id) {
                return drm_file_has_encoder(file->dev, file, encoder) ? 0
                                                                      : -ENOENT;
            }
        }
        return -ENOENT;
    case DRM_IOCTL_MODE_GETCONNECTOR:
        return drm_file_has_object(
                   file, ((struct drm_mode_get_connector *)arg)->connector_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_GETPLANE:
        return drm_file_has_object(file,
                                   ((struct drm_mode_get_plane *)arg)->plane_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_SETPLANE:
        return drm_file_has_object(file,
                                   ((struct drm_mode_set_plane *)arg)->plane_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return drm_file_has_object(
                   file, ((struct drm_mode_crtc_page_flip *)arg)->crtc_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_CURSOR:
        return drm_file_has_object(file,
                                   ((struct drm_mode_cursor *)arg)->crtc_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_CURSOR2:
        return drm_file_has_object(file,
                                   ((struct drm_mode_cursor2 *)arg)->crtc_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_MODE_GETGAMMA:
    case DRM_IOCTL_MODE_SETGAMMA:
        return drm_file_has_object(file,
                                   ((struct drm_mode_crtc_lut *)arg)->crtc_id)
                   ? 0
                   : -ENOENT;
    case DRM_IOCTL_WAIT_VBLANK:
        return file->revoked ? -EACCES : 0;
    case DRM_IOCTL_MODE_ATOMIC: {
        struct drm_mode_atomic *atomic = (struct drm_mode_atomic *)arg;
        if (file->revoked) {
            return -EACCES;
        }
        if (atomic->count_objs > DRM_MAX_LEASE_OBJECTS) {
            return -EINVAL;
        }
        uint32_t *objects = calloc(atomic->count_objs, sizeof(uint32_t));
        if (!objects) {
            return -ENOMEM;
        }
        if (atomic->count_objs > 0 &&
            (!atomic->objs_ptr ||
             copy_from_user(objects, (void *)(uintptr_t)atomic->objs_ptr,
                            atomic->count_objs * sizeof(uint32_t)))) {
            free(objects);
            return -EFAULT;
        }
        for (uint32_t i = 0; i < atomic->count_objs; i++) {
            if (!drm_file_has_object(file, objects[i])) {
                free(objects);
                return -ENOENT;
            }
        }
        free(objects);
        return 0;
    }
    case DRM_IOCTL_MODE_CREATE_LEASE:
    case DRM_IOCTL_MODE_LIST_LESSEES:
    case DRM_IOCTL_MODE_REVOKE_LEASE:
        return -EACCES;
    default:
        return file->revoked ? -EACCES : 0;
    }
}

/**
 * drm_ioctl - Main DRM ioctl handler
 */
ssize_t drm_ioctl(void *data, ssize_t cmd, ssize_t arg, fd_t *fd) {
    void *effective_data = drm_effective_ioctl_data(data, fd);
    drm_file_t *drm_file = drm_current_file(data, fd);
    drm_device_t *dev = drm_data_to_device(effective_data);
    if (!dev) {
        return -ENODEV;
    }

    uint32_t ioctl_cmd = (uint32_t)(cmd & 0xffffffff);
    if (drm_data_is_render_node(effective_data) &&
        !drm_ioctl_allow_on_render_node(dev, ioctl_cmd)) {
        return -EACCES;
    }

    uint32_t ioctl_dir = _IOC_DIR(ioctl_cmd);
    size_t ioctl_size = _IOC_SIZE(ioctl_cmd);
    void *ioarg = (void *)(uintptr_t)arg;
    void *karg = NULL;

    if (ioctl_size > 0) {
        karg = malloc(ioctl_size);
        if (!karg) {
            return -ENOMEM;
        }
        memset(karg, 0, ioctl_size);

        if (ioctl_dir & _IOC_WRITE) {
            if (!arg ||
                copy_from_user(karg, (void *)(uintptr_t)arg, ioctl_size)) {
                free(karg);
                return -EFAULT;
            }
        }

        ioarg = karg;
    }

    ssize_t lease_ret = drm_ioctl_check_lease(drm_file, ioctl_cmd, ioarg);
    if (lease_ret < 0) {
        if (karg) {
            free(karg);
        }
        return lease_ret;
    }

    ssize_t ret = -EINVAL;

    switch (ioctl_cmd) {
    case DRM_IOCTL_VERSION:
        ret = drm_ioctl_version(dev, ioarg);
        break;
    case DRM_IOCTL_GET_CAP:
        ret = drm_ioctl_get_cap(dev, ioarg);
        break;
    case DRM_IOCTL_GEM_CLOSE:
        ret = drm_ioctl_gem_close(dev, ioarg, fd);
        break;
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        ret = drm_ioctl_prime_handle_to_fd(dev, ioarg, fd);
        break;
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        ret = drm_ioctl_prime_fd_to_handle(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GETRESOURCES:
        ret = drm_ioctl_mode_getresources(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GETCRTC:
        ret = drm_ioctl_mode_getcrtc(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_GETENCODER:
        ret = drm_ioctl_mode_getencoder(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_CREATE_DUMB:
        ret = drm_ioctl_mode_create_dumb(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_MAP_DUMB:
        ret = drm_ioctl_mode_map_dumb(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_DESTROY_DUMB:
        ret = drm_ioctl_mode_destroy_dumb(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GETCONNECTOR:
        ret = drm_ioctl_mode_getconnector(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_GETFB:
        ret = drm_ioctl_mode_getfb(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_ADDFB:
        ret = drm_ioctl_mode_addfb(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_ADDFB2:
        ret = drm_ioctl_mode_addfb2(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_RMFB:
        ret = drm_ioctl_mode_rmfb(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_CLOSEFB:
        ret = drm_ioctl_mode_closefb(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_SETCRTC:
        ret = drm_ioctl_mode_setcrtc(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        ret = drm_ioctl_mode_getplaneresources(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GETPLANE:
        ret = drm_ioctl_mode_getplane(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_SETPLANE:
        ret = drm_ioctl_mode_setplane(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GETPROPERTY:
        ret = drm_ioctl_mode_getproperty(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_GETPROPBLOB:
        ret = drm_ioctl_mode_getpropblob(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_CREATEPROPBLOB:
        ret = drm_ioctl_mode_createpropblob(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_DESTROYPROPBLOB:
        ret = drm_ioctl_mode_destroypropblob(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_SETPROPERTY:
        ret = 0; // Not implemented
        break;
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        ret = drm_ioctl_mode_obj_getproperties(dev, ioarg);
        break;
    case DRM_IOCTL_SET_CLIENT_CAP:
        ret = drm_ioctl_set_client_cap(dev, ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_CREATE:
        ret = drm_syncobj_create(dev, (struct drm_syncobj_create *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_DESTROY:
        ret = drm_syncobj_destroy(dev, (struct drm_syncobj_destroy *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
        ret = drm_syncobj_handle_to_fd(dev, (struct drm_syncobj_handle *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
        ret = drm_syncobj_fd_to_handle(dev, (struct drm_syncobj_handle *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_WAIT:
        ret = drm_syncobj_wait(dev, (struct drm_syncobj_wait *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_RESET:
        ret = drm_syncobj_reset_or_signal(
            dev, (struct drm_syncobj_array *)ioarg, false);
        break;
    case DRM_IOCTL_SYNCOBJ_SIGNAL:
        ret = drm_syncobj_reset_or_signal(
            dev, (struct drm_syncobj_array *)ioarg, true);
        break;
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
        ret = drm_syncobj_timeline_wait(
            dev, (struct drm_syncobj_timeline_wait *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_QUERY:
        ret =
            drm_syncobj_query(dev, (struct drm_syncobj_timeline_array *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_TRANSFER:
        ret = drm_syncobj_transfer(dev, (struct drm_syncobj_transfer *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
        ret = drm_syncobj_timeline_signal(
            dev, (struct drm_syncobj_timeline_array *)ioarg);
        break;
    case DRM_IOCTL_SYNCOBJ_EVENTFD:
        ret =
            drm_syncobj_eventfd_ioctl(dev, (struct drm_syncobj_eventfd *)ioarg);
        break;
    case DRM_IOCTL_SET_MASTER:
        ret = drm_ioctl_set_master(dev, ioarg);
        break;
    case DRM_IOCTL_DROP_MASTER:
        ret = drm_ioctl_drop_master(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_GETGAMMA:
        ret = drm_ioctl_gamma(dev, ioarg, cmd);
        break;
    case DRM_IOCTL_MODE_SETGAMMA:
        ret = drm_ioctl_gamma(dev, ioarg, cmd);
        break;
    case DRM_IOCTL_MODE_DIRTYFB:
        ret = drm_ioctl_dirtyfb(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_PAGE_FLIP:
        ret = drm_ioctl_page_flip(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_CURSOR:
        ret = drm_ioctl_cursor(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_CURSOR2:
        ret = drm_ioctl_cursor2(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_ATOMIC:
        ret = drm_ioctl_atomic(dev, ioarg, fd);
        break;
    case DRM_IOCTL_WAIT_VBLANK:
        ret = drm_ioctl_wait_vblank(dev, ioarg, fd);
        break;
    case DRM_IOCTL_GET_UNIQUE:
        ret = drm_ioctl_get_unique(dev, ioarg);
        break;
    case DRM_IOCTL_MODE_LIST_LESSEES:
        ret = drm_ioctl_mode_list_lessees(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_CREATE_LEASE:
        ret = drm_ioctl_mode_create_lease(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_GET_LEASE:
        ret = drm_ioctl_mode_get_lease(dev, ioarg, fd);
        break;
    case DRM_IOCTL_MODE_REVOKE_LEASE:
        ret = drm_ioctl_mode_revoke_lease(dev, ioarg);
        break;
    case DRM_IOCTL_SET_VERSION:
        ret = 0; // Not implemented
        break;
    case DRM_IOCTL_GET_MAGIC:
        ret = drm_ioctl_get_magic(dev, ioarg);
        break;
    case DRM_IOCTL_AUTH_MAGIC:
        ret = drm_ioctl_auth_magic(dev, ioarg);
        break;
    default:
        if (dev->op && dev->op->driver_ioctl) {
            ret = dev->op->driver_ioctl(dev, ioctl_cmd, ioarg,
                                        drm_data_is_render_node(effective_data),
                                        fd);
        } else {
            printk("drm: Unsupported ioctl: cmd = %#010lx\n", cmd);
            ret = -EINVAL;
        }
        break;
    }

    if (ret >= 0 && karg && (ioctl_dir & _IOC_READ)) {
        if (!arg || copy_to_user((void *)(uintptr_t)arg, karg, ioctl_size)) {
            ret = -EFAULT;
        }
    }

    if (karg) {
        free(karg);
    }

    return ret;
}
