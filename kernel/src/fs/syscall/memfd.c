#include <fs/vfs/vfs.h>
#include <fs/proc.h>
#include <mm/cache.h>
#include <mm/mm_syscall.h>
#include <mm/page.h>
#include <mm/vma.h>
#include <task/task.h>
#include <init/callbacks.h>

#define MEMFDFS_MAGIC 0x6d656d66ULL

static struct vfs_file_system_type memfdfs_fs_type;
static const struct vfs_super_operations memfdfs_super_ops;
static const struct vfs_inode_operations memfdfs_inode_ops;
static const struct vfs_file_operations memfdfs_dir_file_ops;
static const struct vfs_file_operations memfdfs_file_ops;
static const struct vfs_address_space_operations memfdfs_aops;
static spinlock_t memfdfs_mount_lock;
static struct vfs_mount *memfdfs_internal_mnt;

struct memfd_ctx {
    vfs_node_t *node;
    char name[64];
    uint64_t *pages;
    size_t page_slots;
    uint64_t len;
    int flags;
    unsigned int seals;
    spinlock_t lock;
};

typedef struct memfdfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} memfdfs_info_t;

typedef struct memfdfs_inode_info {
    struct vfs_inode vfs_inode;
} memfdfs_inode_info_t;

static inline memfdfs_info_t *memfdfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (memfdfs_info_t *)sb->s_fs_info : NULL;
}

static inline struct memfd_ctx *memfd_file_handle(struct vfs_file *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (struct memfd_ctx *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (struct memfd_ctx *)file->f_inode->i_private;
}

static inline struct memfd_ctx *
memfd_mapping_handle(struct vfs_address_space *mapping) {
    if (!mapping || !mapping->host)
        return NULL;
    return (struct memfd_ctx *)mapping->host->i_private;
}

static struct vfs_inode *memfdfs_alloc_inode(struct vfs_super_block *sb) {
    memfdfs_inode_info_t *info = calloc(1, sizeof(*info));
    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void memfdfs_destroy_ctx(struct memfd_ctx *ctx) {
    if (!ctx)
        return;
    for (size_t i = 0; i < ctx->page_slots; i++) {
        if (ctx->pages[i])
            address_release(ctx->pages[i]);
    }
    free(ctx->pages);
    free(ctx);
}

static void memfdfs_destroy_inode(struct vfs_inode *inode) {
    if (!inode)
        return;
    if (inode->i_private) {
        memfdfs_destroy_ctx((struct memfd_ctx *)inode->i_private);
        inode->i_private = NULL;
    }
    free(container_of(inode, memfdfs_inode_info_t, vfs_inode));
}

static void memfdfs_evict_inode(struct vfs_inode *inode) {
    if (inode)
        page_cache_evict(&inode->i_mapping);
}

static int memfdfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int memfdfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    memfdfs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *root;

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
    sb->s_magic = MEMFDFS_MAGIC;
    sb->s_fs_info = fsi;
    sb->s_op = &memfdfs_super_ops;
    sb->s_type = &memfdfs_fs_type;

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
    inode->i_op = &memfdfs_inode_ops;
    inode->i_fop = &memfdfs_dir_file_ops;

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

static void memfdfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static int memfdfs_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                           uint32_t request_mask, unsigned int flags) {
    (void)request_mask;
    (void)flags;
    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static const struct vfs_super_operations memfdfs_super_ops = {
    .alloc_inode = memfdfs_alloc_inode,
    .evict_inode = memfdfs_evict_inode,
    .destroy_inode = memfdfs_destroy_inode,
    .put_super = memfdfs_put_super,
};

static struct vfs_file_system_type memfdfs_fs_type = {
    .name = "memfdfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = memfdfs_init_fs_context,
    .get_tree = memfdfs_get_tree,
};

static struct vfs_mount *memfdfs_get_internal_mount(void) {
    int ret;

    spin_lock(&memfdfs_mount_lock);
    if (!memfdfs_internal_mnt) {
        ret = vfs_kern_mount("memfdfs", 0, NULL, NULL, &memfdfs_internal_mnt);
        if (ret < 0)
            memfdfs_internal_mnt = NULL;
    }
    if (memfdfs_internal_mnt)
        vfs_mntget(memfdfs_internal_mnt);
    spin_unlock(&memfdfs_mount_lock);
    return memfdfs_internal_mnt;
}

static ino64_t memfdfs_next_ino(struct vfs_super_block *sb) {
    memfdfs_info_t *fsi = memfdfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static size_t memfd_pages_for_size(uint64_t size) {
    return size ? (size_t)(PADDING_UP(size, PAGE_SIZE) / PAGE_SIZE) : 0;
}

static int memfd_ensure_page_slots_locked(struct memfd_ctx *ctx, size_t slots) {
    if (!ctx || slots <= ctx->page_slots)
        return 0;

    size_t new_slots = ctx->page_slots ? ctx->page_slots : 1;
    while (new_slots < slots) {
        if (new_slots > SIZE_MAX / 2) {
            new_slots = slots;
            break;
        }
        new_slots *= 2;
    }
    if (new_slots < slots)
        new_slots = slots;

    uint64_t *new_pages = calloc(new_slots, sizeof(uint64_t));
    if (!new_pages)
        return -ENOMEM;

    if (ctx->pages && ctx->page_slots > 0) {
        memcpy(new_pages, ctx->pages, ctx->page_slots * sizeof(uint64_t));
        free(ctx->pages);
    }

    ctx->pages = new_pages;
    ctx->page_slots = new_slots;
    return 0;
}

static int memfd_resolve_page_locked(struct memfd_ctx *ctx, size_t page_idx,
                                     bool create, uint64_t *out_paddr) {
    if (!ctx || !out_paddr)
        return -EINVAL;

    *out_paddr = 0;
    if (page_idx >= ctx->page_slots) {
        if (!create)
            return 0;
        int ret = memfd_ensure_page_slots_locked(ctx, page_idx + 1);
        if (ret < 0)
            return ret;
    }

    uint64_t paddr = ctx->pages[page_idx];
    if (!paddr && create) {
        paddr = alloc_frames(1);
        if (!paddr)
            return -ENOMEM;
        memset((void *)phys_to_virt(paddr), 0, PAGE_SIZE);
        ctx->pages[page_idx] = paddr;
    }

    *out_paddr = paddr;
    return 0;
}

static int memfdfs_readpage(struct vfs_file *file,
                            struct vfs_address_space *mapping, uint64_t index,
                            void *page) {
    struct memfd_ctx *ctx = memfd_mapping_handle(mapping);
    uint64_t file_off;
    uint64_t paddr = 0;

    (void)file;
    if (!ctx || !page)
        return -EINVAL;
    if (index > UINT64_MAX / PAGE_SIZE)
        return -EFBIG;

    file_off = index * PAGE_SIZE;
    memset(page, 0, PAGE_SIZE);

    spin_lock(&ctx->lock);
    if (file_off >= ctx->len) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    (void)memfd_resolve_page_locked(ctx, (size_t)index, false, &paddr);
    if (paddr) {
        size_t copy_len = (size_t)MIN(PAGE_SIZE, ctx->len - file_off);
        memcpy(page, (const void *)phys_to_virt(paddr), copy_len);
    }
    spin_unlock(&ctx->lock);
    return 0;
}

static int memfdfs_writepage(struct vfs_file *file,
                             struct vfs_address_space *mapping, uint64_t index,
                             const void *page) {
    struct memfd_ctx *ctx = memfd_mapping_handle(mapping);
    uint64_t file_off;
    uint64_t paddr = 0;
    int ret;

    (void)file;
    if (!ctx || !page)
        return -EINVAL;
    if (index > UINT64_MAX / PAGE_SIZE)
        return -EFBIG;

    file_off = index * PAGE_SIZE;

    spin_lock(&ctx->lock);
    if (file_off >= ctx->len) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    ret = memfd_resolve_page_locked(ctx, (size_t)index, true, &paddr);
    if (ret < 0) {
        spin_unlock(&ctx->lock);
        return ret;
    }

    size_t copy_len = (size_t)MIN(PAGE_SIZE, ctx->len - file_off);
    memcpy((void *)phys_to_virt(paddr), page, copy_len);
    if (copy_len < PAGE_SIZE)
        memset((uint8_t *)phys_to_virt(paddr) + copy_len, 0,
               PAGE_SIZE - copy_len);
    spin_unlock(&ctx->lock);
    return 0;
}

static ssize_t memfdfs_read(struct vfs_file *file, void *buf, size_t count,
                            loff_t *ppos) {
    struct memfd_ctx *ctx = memfd_file_handle(file);

    if (!ctx || !ppos)
        return -EINVAL;
    return page_cache_read(file, buf, count, ppos);
}

static ssize_t memfdfs_write(struct vfs_file *file, const void *buf,
                             size_t count, loff_t *ppos) {
    struct memfd_ctx *ctx = memfd_file_handle(file);
    ssize_t ret;

    if (!ctx || !ppos)
        return -EINVAL;

    ret = page_cache_write(file, buf, count, ppos);
    if (ret < 0)
        return ret;

    spin_lock(&ctx->lock);
    if (file && file->f_inode)
        ctx->len = file->f_inode->i_size;
    if (ctx->node)
        ctx->node->i_size = ctx->len;
    spin_unlock(&ctx->lock);
    return ret;
}

static int memfdfs_resize(struct vfs_inode *node, uint64_t size) {
    struct memfd_ctx *ctx = node ? (struct memfd_ctx *)node->i_private : NULL;
    size_t old_pages, new_pages;
    uint64_t old_len;

    if (!ctx)
        return -EINVAL;

    spin_lock(&ctx->lock);
    old_len = ctx->len;
    if (size == old_len) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    old_pages = memfd_pages_for_size(old_len);
    new_pages = memfd_pages_for_size(size);

    if (size > old_len) {
        ctx->len = size;
        if (ctx->node)
            ctx->node->i_size = size;
        spin_unlock(&ctx->lock);
        return 0;
    }

    if ((size & (PAGE_SIZE - 1)) != 0) {
        size_t tail_page = (size_t)(size / PAGE_SIZE);
        uint64_t paddr = 0;
        if (memfd_resolve_page_locked(ctx, tail_page, false, &paddr) == 0 &&
            paddr) {
            /*
             * Shrinking into the middle of a page must zero the now-invisible
             * tail. If the file is grown again later, Linux-style behavior is
             * that the reopened range reads back as zero rather than leaking
             * the stale bytes that used to live past EOF. This showed up in
             * practice when userspace resized memfd-backed objects and expected
             * ordinary tmpfs semantics.
             */
            memset((uint8_t *)phys_to_virt(paddr) + (size % PAGE_SIZE), 0,
                   PAGE_SIZE - (size % PAGE_SIZE));
        }
    }

    ctx->len = size;
    if (ctx->node)
        ctx->node->i_size = size;
    spin_unlock(&ctx->lock);

    page_cache_truncate(&node->i_mapping, size);

    for (size_t i = new_pages; i < old_pages; i++) {
        uint64_t paddr = 0;

        spin_lock(&ctx->lock);
        if (i < ctx->page_slots) {
            paddr = ctx->pages[i];
            ctx->pages[i] = 0;
        }
        spin_unlock(&ctx->lock);

        if (paddr)
            address_release(paddr);
    }

    return 0;
}

static int memfdfs_setattr(struct vfs_dentry *dentry,
                           const struct vfs_kstat *stat) {
    struct vfs_inode *inode;
    int ret = 0;

    if (!dentry || !dentry->d_inode || !stat)
        return -EINVAL;

    inode = dentry->d_inode;
    if (stat->mode)
        inode->i_mode = (inode->i_mode & S_IFMT) | (stat->mode & 07777);
    inode->i_uid = stat->uid;
    inode->i_gid = stat->gid;

    if (!S_ISDIR(inode->i_mode) && stat->size != inode->i_size)
        ret = memfdfs_resize(inode, stat->size);

    inode->inode = inode->i_ino;
    return ret;
}

static int memfdfs_fsync(struct vfs_file *file, loff_t start, loff_t end,
                         int datasync) {
    if (!file || !file->f_inode)
        return -EINVAL;
    if (start < 0)
        start = 0;
    if (end < start)
        end = (loff_t)file->f_inode->i_size;
    return page_cache_writeback_range(&file->f_inode->i_mapping,
                                      (uint64_t)start, (uint64_t)end,
                                      datasync) < 0
               ? -EIO
               : 0;
}

static int memfdfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    file->private_data = inode->i_private;
    return 0;
}

static int memfdfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    if (file)
        file->private_data = NULL;
    return 0;
}

static void *memfdfs_mmap(struct vfs_file *file, void *addr, size_t offset,
                          size_t size, size_t prot, uint64_t flags) {
    if ((flags & MAP_TYPE) == MAP_PRIVATE)
        return general_map(file, (uint64_t)addr, size, prot, flags, offset);

    struct memfd_ctx *ctx = memfd_file_handle(file);
    if (!ctx)
        return (void *)(int64_t)-EINVAL;
    if (offset > SIZE_MAX - size)
        return (void *)(int64_t)-EINVAL;

    /*
     * Shared memfd mappings are faulted through the inode page cache. The page
     * cache page is the live shared page for fd I/O and mmap, so updates are
     * visible immediately instead of waiting for a later writeback.
     */
    return addr;
}

static loff_t memfdfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    loff_t pos;

    if (!file || !file->f_inode)
        return -EBADF;

    vfs_file_pos_lock(file);
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
        vfs_file_pos_unlock(file);
        return -EINVAL;
    }
    if (pos < 0) {
        vfs_file_pos_unlock(file);
        return -EINVAL;
    }
    file->f_pos = pos;
    vfs_file_pos_unlock(file);
    return pos;
}

static const struct vfs_inode_operations memfdfs_inode_ops = {
    .getattr = memfdfs_getattr,
    .setattr = memfdfs_setattr,
};

static const struct vfs_file_operations memfdfs_dir_file_ops = {
    .llseek = memfdfs_llseek,
    .open = memfdfs_open,
    .release = memfdfs_release,
};

static const struct vfs_file_operations memfdfs_file_ops = {
    .llseek = memfdfs_llseek,
    .read = memfdfs_read,
    .write = memfdfs_write,
    .mmap = memfdfs_mmap,
    .open = memfdfs_open,
    .release = memfdfs_release,
    .fsync = memfdfs_fsync,
};

static const struct vfs_address_space_operations memfdfs_aops = {
    .readpage = memfdfs_readpage,
    .writepage = memfdfs_writepage,
};

#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#define MFD_HUGETLB 0x0004U
#define MFD_NOEXEC_SEAL 0x0008U
#define MFD_EXEC 0x0010U

static int memfd_create_file(struct vfs_file **out_file, const char *name,
                             unsigned int flags, struct memfd_ctx **out_ctx) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr qname = {0};
    struct vfs_file *file;
    struct memfd_ctx *ctx;
    char label[80];

    if (!out_file)
        return -EINVAL;

    mnt = memfdfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    /*
     * The stored memfd name is descriptive only. Linux exposes it for procfs
     * style reporting and debugging, but it is not a pathname and does not
     * participate in lookup once the fd exists.
     */
    if (name)
        strncpy(ctx->name, name, sizeof(ctx->name) - 1);
    ctx->flags = (int)flags;
    spin_init(&ctx->lock);

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = memfdfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFREG | 0600;
    inode->i_nlink = 1;
    inode->i_op = &memfdfs_inode_ops;
    inode->i_fop = &memfdfs_file_ops;
    inode->i_mapping.a_ops = &memfdfs_aops;
    inode->i_private = ctx;
    ctx->node = inode;

    snprintf(label, sizeof(label), "memfd-%s-%llu",
             ctx->name[0] ? ctx->name : "anon",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&qname, label);
    dentry = vfs_d_alloc(sb, sb->s_root, &qname);
    if (!dentry) {
        inode->i_private = NULL;
        vfs_iput(inode);
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDWR);
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        free(ctx);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = ctx;
    *out_file = file;
    if (out_ctx)
        *out_ctx = ctx;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

uint64_t sys_memfd_create(const char *name, unsigned int flags) {
    struct vfs_file *file = NULL;
    int ret;
    char kname[64] = {0};

    if ((flags & MFD_HUGETLB) || (flags & MFD_NOEXEC_SEAL) ||
        (flags & MFD_EXEC))
        return -EINVAL;
    if (name && copy_from_user_str(kname, name, sizeof(kname)))
        return -EFAULT;

    ret = memfd_create_file(&file, kname, flags, NULL);
    if (ret < 0)
        return (uint64_t)ret;

    ret = task_install_file(current_task, file,
                            (flags & MFD_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return (uint64_t)ret;
}

void memfd_init() {
    spin_init(&memfdfs_mount_lock);
    vfs_register_filesystem(&memfdfs_fs_type);
}
