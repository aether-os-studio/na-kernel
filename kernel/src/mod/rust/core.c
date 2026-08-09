#include <boot/boot.h>
#include <drivers/logger.h>
#include <libs/klibc.h>
#include <mm/cache.h>
#include <mm/hhdm.h>
#include <mm/mm.h>
#include <mm/page_table_flags.h>
#include <mm/mm_syscall.h>
#include <mod/rust/api.h>

void na_log(const char *message) {
    if (message)
        serial_fprintk("%s", message);
}

void *na_mmio_map(uint64_t physical_address, size_t size) {
    if (!size || physical_address > UINT64_MAX - size)
        return NULL;

    uint64_t physical_end = physical_address + size;
    uint64_t map_start = PADDING_DOWN(physical_address, PAGE_SIZE);
    uint64_t map_end = PADDING_UP(physical_end, PAGE_SIZE);
    if (map_end < physical_end)
        return NULL;

    uint64_t virtual_start = (uint64_t)phys_to_virt(map_start);
    uint64_t flags = PT_FLAG_R | PT_FLAG_W | PT_FLAG_UNCACHEABLE;
    if (map_page_range(get_kernel_page_dir(), virtual_start, map_start,
                       map_end - map_start, flags) != 0)
        return NULL;

    return phys_to_virt(physical_address);
}

int na_vfs_resolve_device(const struct vfs_fs_context *context,
                          uint64_t *device) {
    struct vfs_path path = {0};
    uint64_t resolved;
    int ret;

    if (!context || !device)
        return -EINVAL;

    resolved = (uint64_t)(uintptr_t)context->data;
    if (resolved) {
        *device = resolved;
        return 0;
    }
    if (!context->source || !context->source[0])
        return -EINVAL;

    ret = vfs_filename_lookup(AT_FDCWD, context->source, LOOKUP_FOLLOW, &path);
    if (ret < 0)
        return ret;
    if (!path.dentry || !path.dentry->d_inode) {
        vfs_path_put(&path);
        return -ENOENT;
    }

    resolved = path.dentry->d_inode->i_rdev ? path.dentry->d_inode->i_rdev
                                            : path.dentry->d_inode->i_sb->s_dev;
    vfs_path_put(&path);
    if (!resolved)
        return -EINVAL;
    *device = resolved;
    return 0;
}

void *na_vfs_general_map(struct vfs_file *file, void *address, size_t offset,
                         size_t size, size_t protection, uint64_t flags) {
    return general_map(file, (uint64_t)address, size, protection, flags,
                       offset);
}

ssize_t na_vfs_file_cached_read(struct vfs_file *file, void *buffer,
                                size_t count, int64_t *position) {
    return page_cache_read(file, buffer, count, position);
}

ssize_t na_vfs_file_cached_write(struct vfs_file *file, const void *buffer,
                                 size_t count, int64_t *position) {
    return page_cache_write(file, buffer, count, position);
}

int na_vfs_file_install_anonymous_inode(struct vfs_file *file,
                                        struct vfs_inode *inode) {
    struct vfs_qstr name = {.name = "", .len = 0, .hash = 0};
    struct vfs_dentry *dentry;
    struct vfs_inode *new_inode;
    struct vfs_path old_path;
    struct vfs_path new_path;

    if (!file || !inode || !file->f_path.dentry || !inode->i_sb ||
        file->f_path.dentry->d_sb != inode->i_sb)
        return -EINVAL;

    dentry = vfs_d_alloc(inode->i_sb, file->f_path.dentry, &name);
    if (!dentry)
        return -ENOMEM;
    vfs_d_instantiate(dentry, inode);
    if (!vfs_path_set(&new_path, file->f_path.mnt, dentry)) {
        vfs_dput(dentry);
        return -ENOENT;
    }
    new_inode = vfs_igrab(inode);
    if (!new_inode) {
        vfs_path_put(&new_path);
        vfs_dput(dentry);
        return -ENOENT;
    }
    if (file->f_mode & VFS_FMODE_WRITE_ACCESS) {
        int ret = vfs_inode_get_write_access(new_inode);
        if (ret < 0) {
            vfs_iput(new_inode);
            vfs_path_put(&new_path);
            vfs_dput(dentry);
            return ret;
        }
    }

    old_path = file->f_path;
    file->f_path = new_path;
    vfs_path_put(&old_path);
    if (file->f_inode)
        vfs_iput(file->f_inode);
    file->f_inode = new_inode;
    file->node = new_inode;
    file->f_op = new_inode->i_fop;
    vfs_dput(dentry);
    return 0;
}

int na_vfs_mapping_writeback(struct vfs_inode *inode, uint64_t end) {
    if (!inode)
        return -EINVAL;
    return page_cache_writeback_range(&inode->i_mapping, 0, end, false);
}

int na_vfs_mapping_writeback_range(struct vfs_inode *inode, uint64_t start,
                                   uint64_t end, bool datasync) {
    if (!inode)
        return -EINVAL;
    return page_cache_writeback_range(&inode->i_mapping, start, end, datasync);
}

void na_vfs_mapping_truncate(struct vfs_inode *inode, uint64_t size) {
    if (inode)
        page_cache_truncate(&inode->i_mapping, size);
}

void na_vfs_init_new_inode_owner(struct vfs_inode *parent, uint16_t *mode,
                                 uint32_t *uid, uint32_t *gid) {
    if (!mode || !uid || !gid)
        return;
    vfs_init_new_inode_owner(parent, mode, uid, gid);
}

uint64_t na_vfs_realtime_seconds(void) {
    uint64_t now = boot_get_boottime() * 1000000000ULL + nano_time();

    return now / 1000000000ULL;
}

uint32_t na_vfs_current_fsuid(void) { return vfs_current_fsuid(); }

uint32_t na_vfs_current_fsgid(void) { return vfs_current_fsgid(); }

int64_t na_vfs_device_open(uint64_t device, struct vfs_file *file) {
    return device_open(device, file);
}

int64_t na_vfs_device_close(uint64_t device, struct vfs_file *file) {
    return device_close(device, file);
}

int64_t na_vfs_device_read(uint64_t device, struct vfs_file *file, void *buffer,
                           uint64_t offset, size_t size) {
    return device_read(device, buffer, offset, size, file);
}

int64_t na_vfs_device_write(uint64_t device, struct vfs_file *file,
                            const void *buffer, uint64_t offset, size_t size) {
    return device_write(device, (void *)buffer, offset, size, file);
}

int64_t na_vfs_device_ioctl(uint64_t device, struct vfs_file *file,
                            uint64_t command, uint64_t argument) {
    return device_ioctl(device, (int)command, (void *)(uintptr_t)argument,
                        file);
}

int64_t na_vfs_device_poll(uint64_t device, struct vfs_file *file,
                           uint32_t events) {
    return device_poll(device, (int)events, file);
}

void *na_vfs_device_map(uint64_t device, struct vfs_file *file, void *address,
                        size_t offset, size_t size, size_t protection) {
    return device_map(device, address, offset, size, protection, file);
}
