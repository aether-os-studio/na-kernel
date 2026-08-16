#include <boot/boot.h>
#include <drivers/tty.h>
#include <drivers/logger.h>
#include <libs/klibc.h>
#include <mm/cache.h>
#include <mm/hhdm.h>
#include <mm/mm.h>
#include <mm/page_table_flags.h>
#include <mm/mm_syscall.h>
#include <mod/rust/api.h>

int na_firmware_request(const char *name, void **data, size_t *size) {
    static const char prefix[] = "/lib/firmware/";
    struct vfs_open_how how = {.flags = O_RDONLY};
    struct vfs_file *file = NULL;
    char *path = NULL;
    void *buffer = NULL;
    loff_t position = 0;
    size_t name_length;
    size_t path_length;
    ssize_t result;
    int status;

    if (!name || !data || !size || !name[0] || name[0] == '/')
        return -EINVAL;
    for (const char *component = name; *component; component++) {
        if (component != name && component[-1] != '/')
            continue;
        if (component[0] == '.' && component[1] == '.' &&
            (component[2] == '\0' || component[2] == '/'))
            return -EINVAL;
    }
    *data = NULL;
    *size = 0;
    name_length = strlen(name);
    if (name_length > SIZE_MAX - sizeof(prefix))
        return -EINVAL;
    path_length = (sizeof(prefix) - 1) + name_length;
    path = malloc(path_length + 1);
    if (!path)
        return -ENOMEM;
    memcpy(path, prefix, sizeof(prefix) - 1);
    memcpy(path + sizeof(prefix) - 1, name, name_length + 1);

    status = vfs_openat(AT_FDCWD, path, &how, &file, true);
    free(path);
    if (status < 0)
        return status;
    if (!file->f_inode || !S_ISREG(file->f_inode->i_mode)) {
        vfs_file_put(file);
        return -EINVAL;
    }
    if (file->f_inode->i_size > SIZE_MAX) {
        vfs_file_put(file);
        return -EOVERFLOW;
    }

    *size = (size_t)file->f_inode->i_size;
    if (*size) {
        buffer = malloc(*size);
        if (!buffer) {
            vfs_file_put(file);
            return -ENOMEM;
        }
    }
    result = vfs_read_kernel_file(file, buffer, *size, &position);
    vfs_file_put(file);
    if (result < 0 || (size_t)result != *size) {
        free(buffer);
        *size = 0;
        return result < 0 ? (int)result : -EIO;
    }
    *data = buffer;
    return 0;
}

void na_log(const char *message) {
    if (message)
        printk("%s", message);
}

int na_boot_get_framebuffer(na_boot_framebuffer_t *out) {
    boot_framebuffer_t *fb;

    if (!out)
        return -EINVAL;

    fb = boot_get_framebuffer();
    if (!fb)
        return -ENODEV;

    memset(out, 0, sizeof(*out));
    out->physical_address = virt_to_phys((void *)fb->address);
    out->width = fb->width;
    out->height = fb->height;
    out->bpp = fb->bpp;
    out->pitch = fb->pitch;
    out->red_mask_size = fb->red_mask_size;
    out->red_mask_shift = fb->red_mask_shift;
    out->green_mask_size = fb->green_mask_size;
    out->green_mask_shift = fb->green_mask_shift;
    out->blue_mask_size = fb->blue_mask_size;
    out->blue_mask_shift = fb->blue_mask_shift;
    return 0;
}

int na_tty_rebind_framebuffer(const na_boot_framebuffer_t *framebuffer) {
    struct tty_graphics_ graphics;
    boot_framebuffer_t *boot_framebuffer;
    uint64_t byte_size;
    void *address;

    if (!framebuffer || !framebuffer->physical_address || !framebuffer->width ||
        !framebuffer->height || framebuffer->bpp != 32 ||
        framebuffer->pitch < framebuffer->width * 4 ||
        framebuffer->pitch > UINT64_MAX / framebuffer->height)
        return -EINVAL;

    byte_size = framebuffer->pitch * framebuffer->height;
    if (byte_size > SIZE_MAX)
        return -EOVERFLOW;
    address = na_mmio_map(framebuffer->physical_address, (size_t)byte_size);
    if (!address)
        return -ENOMEM;

    boot_framebuffer = boot_get_framebuffer();
    if (!boot_framebuffer)
        return -ENODEV;

    memset(&graphics, 0, sizeof(graphics));
    graphics.address = address;
    graphics.width = framebuffer->width;
    graphics.height = framebuffer->height;
    graphics.bpp = (uint16_t)framebuffer->bpp;
    graphics.pitch = framebuffer->pitch;
    graphics.red_mask_size = framebuffer->red_mask_size;
    graphics.red_mask_shift = framebuffer->red_mask_shift;
    graphics.green_mask_size = framebuffer->green_mask_size;
    graphics.green_mask_shift = framebuffer->green_mask_shift;
    graphics.blue_mask_size = framebuffer->blue_mask_size;
    graphics.blue_mask_shift = framebuffer->blue_mask_shift;

    int ret = tty_rebind_framebuffer(&graphics);
    if (ret < 0)
        return ret;

    *boot_framebuffer = (boot_framebuffer_t){
        .address = (uintptr_t)address,
        .width = framebuffer->width,
        .height = framebuffer->height,
        .bpp = framebuffer->bpp,
        .pitch = framebuffer->pitch,
        .red_mask_size = framebuffer->red_mask_size,
        .red_mask_shift = framebuffer->red_mask_shift,
        .green_mask_size = framebuffer->green_mask_size,
        .green_mask_shift = framebuffer->green_mask_shift,
        .blue_mask_size = framebuffer->blue_mask_size,
        .blue_mask_shift = framebuffer->blue_mask_shift,
    };
    return 0;
}

uint64_t na_monotonic_time_ns(void) { return nano_time(); }

void na_delay_us(uint64_t microseconds) {
    uint64_t now = nano_time();
    uint64_t delay_ns =
        microseconds > UINT64_MAX / 1000 ? UINT64_MAX : microseconds * 1000;
    uint64_t deadline =
        now > UINT64_MAX - delay_ns ? UINT64_MAX : now + delay_ns;

    if (!now) {
        for (uint64_t i = 0; i < microseconds * 64; i++)
            arch_pause();
        return;
    }
    while (nano_time() < deadline)
        arch_pause();
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
