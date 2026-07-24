#include <drivers/drm/drm.h>
#include <drivers/drm/drm_core.h>
#include <drivers/drm/drm_ioctl.h>
#include <drivers/logger.h>
#include <fs/fs_syscall.h>
#include <fs/dev.h>
#include <fs/sys.h>
#include <arch/arch.h>
#include <mm/mm.h>
#include <mm/page_table.h>
#include <libs/klibc.h>

#define DRM_DUMB_MMAP_OFFSET_BASE 0x100000000ULL
#define DRM_DUMB_MMAP_OFFSET_STRIDE 0x100000000ULL

#define DRM_MAX_EVENT_NODE_BINDINGS 16
#define DRM_MAX_TRACKED_DEVICES 16

typedef struct drm_event_node_binding {
    drm_device_t *dev;
    vfs_node_t *node;
} drm_event_node_binding_t;

static drm_event_node_binding_t
    drm_event_node_bindings[DRM_MAX_EVENT_NODE_BINDINGS];
static spinlock_t drm_event_node_bindings_lock = SPIN_INIT;
static drm_device_t *drm_devices[DRM_MAX_TRACKED_DEVICES];
static spinlock_t drm_devices_lock = SPIN_INIT;
static drm_file_t *drm_current_file(void *data, fd_t *fd) {
    drm_file_t *file = (drm_file_t *)device_file_private(fd);

    if (!file && fd)
        file = (drm_file_t *)fd->private_data;

    if (file && file->magic == DRM_FILE_MAGIC && file->dev)
        return file;

    file = (drm_file_t *)data;
    if (file && file->magic == DRM_FILE_MAGIC && file->dev)
        return file;

    return NULL;
}

static bool drm_queue_ready_event_locked(drm_file_t *file, uint32_t type,
                                         uint64_t user_data,
                                         uint64_t timestamp_ns,
                                         uint64_t sequence) {
    uint32_t slot = 0;

    if (!file)
        return false;

    if (file->drm_event_count == DRM_MAX_EVENTS_COUNT) {
        file->drm_event_head =
            (file->drm_event_head + 1) % DRM_MAX_EVENTS_COUNT;
        file->drm_event_count--;
    }

    slot =
        (file->drm_event_head + file->drm_event_count) % DRM_MAX_EVENTS_COUNT;
    file->drm_events[slot].type = type;
    file->drm_events[slot].user_data = user_data;
    file->drm_events[slot].timestamp.tv_sec = timestamp_ns / 1000000000ULL;
    file->drm_events[slot].timestamp.tv_nsec = timestamp_ns % 1000000000ULL;
    file->drm_events[slot].sequence = sequence;
    file->drm_events[slot].file = file;
    file->drm_event_count++;

    return true;
}

static vfs_node_t *drm_file_event_node_get_locked(drm_file_t *file) {
    return file && file->event_node ? vfs_igrab(file->event_node) : NULL;
}

static void drm_notify_file_event_node(vfs_node_t *node) {
    if (!node)
        return;

    vfs_poll_notify_inode(node, EPOLLIN | EPOLLRDNORM);
    vfs_iput(node);
}

static void drm_device_track(drm_device_t *dev) {
    if (!dev)
        return;

    spin_lock(&drm_devices_lock);
    for (uint32_t i = 0; i < DRM_MAX_TRACKED_DEVICES; i++) {
        if (!drm_devices[i]) {
            drm_devices[i] = dev;
            break;
        }
    }
    spin_unlock(&drm_devices_lock);
}

static void drm_device_untrack(drm_device_t *dev) {
    if (!dev)
        return;

    spin_lock(&drm_devices_lock);
    for (uint32_t i = 0; i < DRM_MAX_TRACKED_DEVICES; i++) {
        if (drm_devices[i] == dev) {
            drm_devices[i] = NULL;
            break;
        }
    }
    spin_unlock(&drm_devices_lock);
}

static vfs_node_t *drm_event_node_get(drm_device_t *dev) {
    if (!dev) {
        return NULL;
    }

    vfs_node_t *node = NULL;

    spin_lock(&drm_event_node_bindings_lock);
    for (uint32_t i = 0; i < DRM_MAX_EVENT_NODE_BINDINGS; i++) {
        if (drm_event_node_bindings[i].dev == dev &&
            drm_event_node_bindings[i].node) {
            node = drm_event_node_bindings[i].node;
            vfs_igrab(node);
            break;
        }
    }
    spin_unlock(&drm_event_node_bindings_lock);

    return node;
}

static void drm_notify_event_node_flags(drm_device_t *dev, uint32_t events) {
    vfs_node_t *event_node = drm_event_node_get(dev);
    if (event_node) {
        vfs_poll_notify_inode(event_node, events);
        vfs_iput(event_node);
    }
}

static vfs_node_t *drm_lookup_inode(const char *path) {
    struct vfs_path p = {0};
    vfs_node_t *inode = NULL;

    if (!path)
        return NULL;
    if (vfs_filename_lookup(AT_FDCWD, path, LOOKUP_FOLLOW, &p) < 0)
        return NULL;
    if (p.dentry && p.dentry->d_inode)
        inode = vfs_igrab(p.dentry->d_inode);
    vfs_path_put(&p);
    return inode;
}

static void drm_event_node_register(drm_device_t *dev,
                                    const char *card_dev_name) {
    if (!dev || !card_dev_name) {
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", card_dev_name);

    vfs_node_t *node = drm_lookup_inode(path);
    if (!node) {
        return;
    }

    int free_slot = -1;
    vfs_node_t *old_node = NULL;

    spin_lock(&drm_event_node_bindings_lock);
    for (uint32_t i = 0; i < DRM_MAX_EVENT_NODE_BINDINGS; i++) {
        if (drm_event_node_bindings[i].dev == dev) {
            old_node = drm_event_node_bindings[i].node;
            drm_event_node_bindings[i].node = node;
            node = NULL;
            break;
        }

        if (free_slot < 0 && drm_event_node_bindings[i].dev == NULL) {
            free_slot = (int)i;
        }
    }

    if (node && free_slot >= 0) {
        drm_event_node_bindings[free_slot].dev = dev;
        drm_event_node_bindings[free_slot].node = node;
        node = NULL;
    }
    spin_unlock(&drm_event_node_bindings_lock);

    if (old_node) {
        vfs_iput(old_node);
    }
}

static void drm_event_node_unregister(drm_device_t *dev) {
    if (!dev) {
        return;
    }

    vfs_node_t *node = NULL;

    spin_lock(&drm_event_node_bindings_lock);
    for (uint32_t i = 0; i < DRM_MAX_EVENT_NODE_BINDINGS; i++) {
        if (drm_event_node_bindings[i].dev == dev) {
            node = drm_event_node_bindings[i].node;
            drm_event_node_bindings[i].dev = NULL;
            drm_event_node_bindings[i].node = NULL;
            break;
        }
    }
    spin_unlock(&drm_event_node_bindings_lock);

    if (node) {
        vfs_iput(node);
    }
}

/**
 * drm_read - Read from DRM device
 * @data: DRM device pointer
 * @buf: User buffer
 * @offset: Offset in file
 * @len: Length to read
 * @fd: Open DRM file
 *
 * Handles reading of DRM events (vblank, flip complete, etc.)
 */
ssize_t drm_read(void *data, void *buf, uint64_t offset, uint64_t len,
                 fd_t *fd) {
    (void)offset;

    drm_file_t *drm_file = drm_current_file(data, fd);
    drm_device_t *dev = drm_file ? drm_file->dev : NULL;
    if (!drm_file || !dev)
        return -EBADF;

    if (drm_file->type == DRM_MINOR_RENDER) {
        return -EINVAL;
    }

    struct k_drm_event event;
    bool have_event = false;
    memset(&event, 0, sizeof(event));

    while (!have_event) {
        arch_enable_interrupt();

        spin_lock(&dev->event_lock);
        if (drm_file->drm_event_count != 0) {
            event = drm_file->drm_events[drm_file->drm_event_head];
            drm_file->drm_event_head =
                (drm_file->drm_event_head + 1) % DRM_MAX_EVENTS_COUNT;
            drm_file->drm_event_count--;
            have_event = true;
        }
        spin_unlock(&dev->event_lock);

        if (have_event)
            break;

        if (fd_get_flags(fd) & O_NONBLOCK)
            return -EWOULDBLOCK;

        const uint32_t want = EPOLLIN | EPOLLERR | EPOLLHUP;
        int reason = vfs_poll_wait_interruptible(fd, want);
        if (reason < 0)
            return reason;
    }

    struct drm_event_vblank vbl = {
        .base.type = event.type,
        .base.length = sizeof(vbl),
        .user_data = event.user_data,
        .tv_sec = (uint32_t)event.timestamp.tv_sec,
        .tv_usec = (uint32_t)(event.timestamp.tv_nsec / 1000),
        .sequence = (uint32_t)event.sequence,
        .crtc_id =
            dev->resource_mgr.crtcs[0] ? dev->resource_mgr.crtcs[0]->id : 0,
    };

    ssize_t ret = 0;

    if (len >= sizeof(vbl)) {
        memcpy(buf, &vbl, sizeof(vbl));
        ret = sizeof(vbl);
    } else {
        ret = -EINVAL;
    }

    return ret;
}

/**
 * drm_poll - Poll DRM device for events
 * @data: DRM device pointer
 * @event: Poll events to check
 *
 * Returns events that are ready
 */
ssize_t drm_poll(void *data, size_t event, fd_t *fd) {
    drm_file_t *drm_file = drm_current_file(data, fd);
    drm_device_t *dev = drm_file ? drm_file->dev : NULL;
    if (!drm_file || !dev)
        return -EBADF;

    if (drm_file->type == DRM_MINOR_RENDER) {
        return 0;
    }

    ssize_t revent = 0;
    bool has_event = false;
    bool has_hotplug = false;

    if (!(event & (EPOLLIN | EPOLLPRI))) {
        return 0;
    }

    spin_lock(&dev->event_lock);
    if (event & EPOLLIN) {
        has_event = (drm_file->drm_event_count != 0);
    }
    if (event & EPOLLPRI) {
        has_hotplug = dev->hotplug_pending;
        if (has_hotplug) {
            dev->hotplug_pending = false;
        }
    }
    spin_unlock(&dev->event_lock);

    if (has_event) {
        revent |= EPOLLIN;
    }
    if (has_hotplug) {
        revent |= EPOLLPRI;
    }

    return revent;
}

/**
 * drm_map - Map DRM buffer to user space
 * @data: DRM device pointer
 * @addr: User address to map to
 * @offset: Offset in buffer
 * @len: Length to map
 *
 * Maps a DRM buffer (typically a dumb buffer) to user space
 */
void *drm_map(void *data, void *addr, uint64_t offset, uint64_t len) {
    drm_device_t *dev = drm_data_to_device(data);
    if (!dev) {
        return (void *)-ENODEV;
    }

    uint64_t user_addr = (uint64_t)addr;
    uint64_t kernel_base = get_physical_memory_offset();

    if (dev->op && dev->op->mmap) {
        int ret = dev->op->mmap(dev, user_addr, offset, len);
        if (ret == 0) {
            return addr;
        }
        if (ret != -ENOSYS) {
            return (void *)(int64_t)ret;
        }
    }

    if (dev->op && dev->op->get_dumb_map &&
        offset >= DRM_DUMB_MMAP_OFFSET_BASE) {
        uint64_t map_offset = offset - DRM_DUMB_MMAP_OFFSET_BASE;
        uint32_t handle = (uint32_t)(map_offset / DRM_DUMB_MMAP_OFFSET_STRIDE);
        uint64_t buffer_offset = map_offset % DRM_DUMB_MMAP_OFFSET_STRIDE;
        uint64_t phys = 0;
        uint64_t size = 0;

        if (handle == 0 || !current_task || !current_task->mm) {
            return (void *)-EINVAL;
        }

        int ret = dev->op->get_dumb_map(dev, handle, &phys, &size);
        if (ret != 0) {
            return (void *)(int64_t)ret;
        }
        if (buffer_offset >= size || len > size - buffer_offset) {
            return (void *)-EINVAL;
        }

        uint64_t page_delta = buffer_offset & (PAGE_SIZE - 1);
        uint64_t map_phys = PADDING_DOWN(phys + buffer_offset, PAGE_SIZE);
        uint64_t map_addr = PADDING_DOWN((uint64_t)addr, PAGE_SIZE);
        uint64_t map_len = PADDING_UP(len + page_delta, PAGE_SIZE);

        if (map_page_range(
                (uint64_t *)phys_to_virt(current_task->mm->page_table_addr),
                map_addr, map_phys, map_len,
                PT_FLAG_R | PT_FLAG_W | PT_FLAG_U) != 0) {
            return (void *)-ENOMEM;
        }

        return addr;
    }

    if (offset < PAGE_SIZE) {
        return (void *)-EINVAL;
    }

    map_page_range((uint64_t *)phys_to_virt(current_task->mm->page_table_addr),
                   (uint64_t)addr, offset, len,
                   PT_FLAG_R | PT_FLAG_W | PT_FLAG_U);

    return addr;
}

/**
 * drm_device_init - Initialize a DRM device
 * @dev: DRM device to initialize
 * @data: Driver private data
 * @op: Driver operations
 *
 * Initializes the basic fields of a DRM device
 */
static void drm_device_init(drm_device_t *dev, void *data,
                            drm_device_op_t *op) {
    memset(dev, 0, sizeof(drm_device_t));
    // ID initialize after device registed
    dev->data = data;
    dev->op = op;
    dev->pci_dev = NULL;
    drm_device_set_driver_info(dev, DRM_NAME, "20060810", "NaOS DRM");
    spin_init(&dev->event_lock);
    spin_init(&dev->lease_lock);
    dev->next_lease_id = 1;
    dev->vblank_period_ns = 1000000000ULL / HZ;
    dev->next_vblank_ns = nano_time() + dev->vblank_period_ns;

    // Initialize resource manager
    drm_resource_manager_init(&dev->resource_mgr);
}

ssize_t drm_open(void *data, void *arg) {
    drm_file_node_t *node = drm_data_to_file_node(data);
    struct vfs_file *file = (struct vfs_file *)arg;
    drm_file_t *drm_file;

    if (!node || !file) {
        return -EINVAL;
    }

    drm_file = calloc(1, sizeof(*drm_file));
    if (!drm_file) {
        return -ENOMEM;
    }

    drm_file->magic = DRM_FILE_MAGIC;
    drm_file->dev = node->dev;
    drm_file->type = node->type;
    drm_file->event_node = vfs_igrab(file->f_inode);
    if (node->dev && node->dev->op && node->dev->op->open) {
        int ret = node->dev->op->open(node->dev, drm_file);
        if (ret != 0) {
            if (drm_file->event_node)
                vfs_iput(drm_file->event_node);
            memset(drm_file, 0, sizeof(*drm_file));
            free(drm_file);
            return ret;
        }
    }
    if (device_file_set_private(file, drm_file) < 0) {
        if (drm_file->event_node)
            vfs_iput(drm_file->event_node);
        if (node->dev && node->dev->op && node->dev->op->close)
            node->dev->op->close(node->dev, drm_file);
        memset(drm_file, 0, sizeof(*drm_file));
        free(drm_file);
        return -EBADF;
    }
    return 0;
}

void drm_cancel_file_events(drm_file_t *file) {
    drm_device_t *dev = file ? file->dev : NULL;

    if (!dev)
        return;

    spin_lock(&dev->event_lock);
    for (uint32_t i = 0; i < dev->pending_event_count; i++) {
        uint32_t slot = (dev->pending_event_head + i) % DRM_MAX_EVENTS_COUNT;
        if (dev->pending_events[slot].file == file)
            dev->pending_events[slot].file = NULL;
    }
    file->drm_event_head = 0;
    file->drm_event_count = 0;
    spin_unlock(&dev->event_lock);
}

ssize_t drm_close(void *data, void *arg) {
    struct vfs_file *file = (struct vfs_file *)arg;
    drm_file_t *drm_file = (drm_file_t *)device_file_private(file);
    drm_device_t *dev = drm_file ? drm_file->dev : drm_data_to_device(data);

    if (!drm_file || drm_file->magic != DRM_FILE_MAGIC) {
        return 0;
    }

    if (dev && drm_file->lessee) {
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

    if (dev && dev->op && dev->op->close) {
        dev->op->close(dev, drm_file);
    }

    drm_cancel_file_events(drm_file);

    if (drm_file->event_node) {
        vfs_iput(drm_file->event_node);
        drm_file->event_node = NULL;
    }

    device_file_set_private(file, NULL);
    memset(drm_file, 0, sizeof(*drm_file));
    free(drm_file);
    return 0;
}

void drm_device_set_driver_info(drm_device_t *dev, const char *name,
                                const char *date, const char *desc) {
    if (!dev) {
        return;
    }

    if (!name || !name[0]) {
        name = DRM_NAME;
    }
    if (!date || !date[0]) {
        date = "20060810";
    }
    if (!desc || !desc[0]) {
        desc = "NaOS DRM";
    }

    strncpy(dev->driver_name, name, sizeof(dev->driver_name) - 1);
    strncpy(dev->driver_date, date, sizeof(dev->driver_date) - 1);
    strncpy(dev->driver_desc, desc, sizeof(dev->driver_desc) - 1);
}

/**
 * drm_device_setup_sysfs - Setup sysfs entries for DRM device
 * @major: DRM device major
 * @card_minor: Primary node minor
 * @render_minor: Render node minor
 * @has_render_node: True if render node is registered
 * @pci_dev: PCI device (can be NULL for non-PCI devices)
 * @card_dev_name: Primary device node name (e.g., "dri/card0")
 * @render_dev_name: Render device node name (e.g., "dri/renderD128")
 *
 * Creates sysfs entries for the DRM device
 */
void drm_register_sysfs_nodes(int major, int card_minor, int render_minor,
                              bool has_render_node, pci_device_t *pci_dev,
                              const char *driver_name,
                              const char *card_dev_name,
                              const char *render_dev_name) {
    if (!pci_dev) {
        return;
    }

    char pci_device_path[128];
    if (pci_dev->device && pci_dev->device->sysfs_path &&
        pci_dev->device->sysfs_path[0]) {
        snprintf(pci_device_path, sizeof(pci_device_path), "%s",
                 pci_dev->device->sysfs_path);
    } else {
        sprintf(pci_device_path, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x",
                pci_dev->segment, pci_dev->bus, pci_dev->slot, pci_dev->func);
    }
    vfs_node_t *pci_device_dir = sysfs_ensure_dir(pci_device_path);
    if (!pci_device_dir) {
        printk("drm: Failed to open PCI sysfs node %s\n", pci_device_path);
        return;
    }

    if (!driver_name || !driver_name[0]) {
        driver_name = DRM_NAME;
    }

    char driver_root[128];
    snprintf(driver_root, sizeof(driver_root), "/sys/bus/pci/drivers/%s",
             driver_name);
    sysfs_ensure_dir(driver_root);

    char driver_link_path[192];
    snprintf(driver_link_path, sizeof(driver_link_path), "%s/driver",
             pci_device_path);
    vfs_node_t *driver_link =
        sysfs_ensure_symlink(driver_link_path, driver_root);

    char reverse_link_path[192];
    snprintf(reverse_link_path, sizeof(reverse_link_path),
             "%s/%04x:%02x:%02x.%01x", driver_root, pci_dev->segment,
             pci_dev->bus, pci_dev->slot, pci_dev->func);
    vfs_node_t *reverse_link =
        sysfs_ensure_symlink(reverse_link_path, pci_device_path);

    char pci_uevent_path[192];
    snprintf(pci_uevent_path, sizeof(pci_uevent_path), "%s/uevent",
             pci_device_path);
    vfs_node_t *pci_uevent = sysfs_ensure_file(pci_uevent_path);
    if (pci_uevent) {
        char uevent_content[384];
        snprintf(uevent_content, sizeof(uevent_content),
                 "DRIVER=%s\nPCI_CLASS=%06x\nPCI_ID=%04x:%04x\n"
                 "PCI_SUBSYS_ID=%04x:%04x\nPCI_SLOT_NAME=%04x:%02x:%02x.%01x\n",
                 driver_name, pci_dev->class_code, pci_dev->vendor_id,
                 pci_dev->device_id, pci_dev->subsystem_vendor_id,
                 pci_dev->subsystem_device_id, pci_dev->segment, pci_dev->bus,
                 pci_dev->slot, pci_dev->func);
        sysfs_write_node(pci_uevent, uevent_content, strlen(uevent_content), 0);
    }

    vfs_node_t *drm_dir = sysfs_child_append(pci_device_dir, "drm", true);

    char content[128];

    vfs_node_t *version = sysfs_child_append(drm_dir, "version", false);
    sprintf(content, "drm 1.1.0 20060810");
    sysfs_write_node(version, content, strlen(content), 0);

    char card_node_name[16];
    sprintf(card_node_name, "card%d", card_minor);
    char card_path[256];
    sprintf(card_path, "%s/drm/%s", pci_device_path, card_node_name);
    char card_uevent[96];
    snprintf(card_uevent, sizeof(card_uevent),
             "SUBSYSTEM=drm\nDEVTYPE=drm_minor\nMINOR=%d\n", card_minor);
    vfs_node_t *card_node = sysfs_regist_dev(
        'c', major, card_minor, card_path, card_dev_name, card_uevent,
        "/sys/class/drm", "/sys/class/drm", card_node_name, pci_device_path);

    if (has_render_node) {
        char render_node_name[16];
        sprintf(render_node_name, "renderD%d", render_minor);
        char render_path[256];
        sprintf(render_path, "%s/drm/%s", pci_device_path, render_node_name);
        char render_uevent[96];
        snprintf(render_uevent, sizeof(render_uevent),
                 "SUBSYSTEM=drm\nDEVTYPE=drm_minor\nMINOR=%d\n", render_minor);
        vfs_node_t *render_node = sysfs_regist_dev(
            'c', major, render_minor, render_path, render_dev_name,
            render_uevent, "/sys/class/drm", "/sys/class/drm", render_node_name,
            pci_device_path);

        vfs_iput(render_node);
    }

    if (pci_uevent)
        vfs_iput(pci_uevent);
    if (driver_link)
        vfs_iput(driver_link);
    if (reverse_link)
        vfs_iput(reverse_link);
    vfs_iput(pci_device_dir);
    vfs_iput(drm_dir);
    vfs_iput(version);
    vfs_iput(card_node);
}

static int drm_id = 0;

static void drm_import_resource_id(drm_device_t *dev, uint32_t obj_id) {
    if (!dev || obj_id == 0) {
        return;
    }
    if (obj_id >= dev->resource_mgr.next_object_id) {
        dev->resource_mgr.next_object_id = obj_id + 1;
    }
}

/**
 * drm_register_device - Register a DRM device with the system
 * @data: Driver private data
 * @op: Driver operations
 * @name: Base name for the device
 * @pci_dev: PCI device (optional, can be NULL for non-PCI devices)
 *
 * Registers a new DRM device and returns the device structure.
 * The caller is responsible for freeing the device when it's no longer needed.
 */
drm_device_t *drm_register_device_with_info(void *data, drm_device_op_t *op,
                                            const char *node_name,
                                            pci_device_t *pci_dev,
                                            const char *driver_name,
                                            const char *driver_date,
                                            const char *driver_desc) {
    char card_dev_name[32];
    char render_dev_name[32];
    uint32_t card_minor = (uint32_t)drm_id;
    uint32_t render_minor = 128U + (uint32_t)drm_id;

    sprintf(card_dev_name, "%s%d", node_name, drm_id);
    sprintf(render_dev_name, "dri/renderD%d", render_minor);

    // Allocate and initialize DRM device
    drm_device_t *dev = malloc(sizeof(drm_device_t));
    if (!dev) {
        printk("drm: Failed to allocate DRM device\n");
        return NULL;
    }

    drm_device_init(dev, data, op);
    drm_device_set_driver_info(dev, driver_name, driver_date, driver_desc);
    dev->pci_dev = pci_dev;
    dev->primary_minor = card_minor;
    dev->render_minor = render_minor;

    dev->primary_node.magic = DRM_FILE_NODE_MAGIC;
    dev->primary_node.type = DRM_MINOR_PRIMARY;
    dev->primary_node.minor = card_minor;
    dev->primary_node.dev = dev;

    uint64_t card_dev_nr = device_install_with_minor(
        DEV_CHAR, DEV_GPU, &dev->primary_node, card_dev_name, 0, drm_open,
        drm_close, drm_ioctl, drm_poll, drm_read, NULL, drm_map, card_minor);
    if (card_dev_nr == 0) {
        printk("drm: Failed to register primary node %s\n", card_dev_name);
        free(dev);
        return NULL;
    }

    dev->id = card_minor;

    dev->render_node.magic = DRM_FILE_NODE_MAGIC;
    dev->render_node.type = DRM_MINOR_RENDER;
    dev->render_node.minor = render_minor;
    dev->render_node.dev = dev;

    uint64_t render_dev_nr = 0;
    if (dev->op && dev->op->supports_render_node) {
        render_dev_nr = device_install_with_minor(
            DEV_CHAR, DEV_GPU, &dev->render_node, render_dev_name, 0, drm_open,
            drm_close, drm_ioctl, drm_poll, drm_read, NULL, drm_map,
            render_minor);
    }
    if (render_dev_nr == 0) {
        if (dev->op && dev->op->supports_render_node) {
            printk("drm: Failed to register render node %s\n", render_dev_name);
        }
        memset(&dev->render_node, 0, sizeof(dev->render_node));
        dev->render_minor = 0;
        dev->render_node_registered = false;
    } else {
        dev->render_node_registered = true;
    }

    drm_event_node_register(dev, card_dev_name);
    drm_device_track(dev);

    // Populate hardware resources if driver supports it
    if (dev->op->get_connectors) {
        drm_connector_t *connectors[DRM_MAX_CONNECTORS_PER_DEVICE];
        memset(connectors, 0, sizeof(connectors));
        uint32_t connector_count = 0;
        if (dev->op->get_connectors(dev, connectors, &connector_count) == 0) {
            for (uint32_t i = 0;
                 i < connector_count && i < DRM_MAX_CONNECTORS_PER_DEVICE;
                 i++) {
                if (connectors[i]) {
                    uint32_t slot = drm_find_free_slot(
                        (void **)dev->resource_mgr.connectors,
                        DRM_MAX_CONNECTORS_PER_DEVICE);
                    if (slot != (uint32_t)-1) {
                        dev->resource_mgr.connectors[slot] = connectors[i];
                        if (connectors[i]->id == 0) {
                            connectors[i]->id =
                                dev->resource_mgr.next_object_id++;
                        }
                        drm_import_resource_id(dev, connectors[i]->id);
                    }
                }
            }
        }
    }

    if (dev->op->get_crtcs) {
        drm_crtc_t *crtcs[DRM_MAX_CRTCS_PER_DEVICE];
        memset(crtcs, 0, sizeof(crtcs));
        uint32_t crtc_count = 0;
        if (dev->op->get_crtcs(dev, crtcs, &crtc_count) == 0) {
            for (uint32_t i = 0; i < crtc_count && i < DRM_MAX_CRTCS_PER_DEVICE;
                 i++) {
                if (crtcs[i]) {
                    uint32_t slot =
                        drm_find_free_slot((void **)dev->resource_mgr.crtcs,
                                           DRM_MAX_CRTCS_PER_DEVICE);
                    if (slot != (uint32_t)-1) {
                        dev->resource_mgr.crtcs[slot] = crtcs[i];
                        if (crtcs[i]->id == 0) {
                            crtcs[i]->id = dev->resource_mgr.next_object_id++;
                        }
                        drm_import_resource_id(dev, crtcs[i]->id);
                    }
                }
            }
        }
    }

    if (dev->op->get_encoders) {
        drm_encoder_t *encoders[DRM_MAX_ENCODERS_PER_DEVICE];
        memset(encoders, 0, sizeof(encoders));
        uint32_t encoder_count = 0;
        if (dev->op->get_encoders(dev, encoders, &encoder_count) == 0) {
            for (uint32_t i = 0;
                 i < encoder_count && i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
                if (encoders[i]) {
                    uint32_t slot =
                        drm_find_free_slot((void **)dev->resource_mgr.encoders,
                                           DRM_MAX_ENCODERS_PER_DEVICE);
                    if (slot != (uint32_t)-1) {
                        dev->resource_mgr.encoders[slot] = encoders[i];
                        if (encoders[i]->id == 0) {
                            encoders[i]->id =
                                dev->resource_mgr.next_object_id++;
                        }
                        drm_import_resource_id(dev, encoders[i]->id);
                    }
                }
            }
        }
    }

    if (dev->op->get_planes) {
        drm_plane_t *planes[DRM_MAX_PLANES_PER_DEVICE];
        memset(planes, 0, sizeof(planes));
        uint32_t plane_count = 0;
        if (dev->op->get_planes(dev, planes, &plane_count) == 0) {
            for (uint32_t i = 0;
                 i < plane_count && i < DRM_MAX_PLANES_PER_DEVICE; i++) {
                if (planes[i]) {
                    uint32_t slot =
                        drm_find_free_slot((void **)dev->resource_mgr.planes,
                                           DRM_MAX_PLANES_PER_DEVICE);
                    if (slot != (uint32_t)-1) {
                        dev->resource_mgr.planes[slot] = planes[i];
                        if (planes[i]->id == 0) {
                            planes[i]->id = dev->resource_mgr.next_object_id++;
                        }
                        drm_import_resource_id(dev, planes[i]->id);
                    }
                }
            }
        }
    }

    // If no hardware resources were found, create default ones
    if (!dev->resource_mgr.connectors[0]) {
        drm_connector_t *connector = drm_connector_alloc(
            &dev->resource_mgr, DRM_MODE_CONNECTOR_VIRTUAL, NULL);
        if (connector) {
            connector->connection = DRM_MODE_CONNECTED;
            connector->count_modes = 1;
            connector->modes = malloc(sizeof(struct drm_mode_modeinfo));
            if (connector->modes) {
                uint32_t width, height, bpp;
                dev->op->get_display_info(dev, &width, &height, &bpp);

                struct drm_mode_modeinfo mode = {
                    .clock = width * HZ,
                    .hdisplay = width,
                    .hsync_start = width + 16,
                    .hsync_end = width + 16 + 96,
                    .htotal = width + 16 + 96 + 48,
                    .vdisplay = height,
                    .vsync_start = height + 10,
                    .vsync_end = height + 10 + 2,
                    .vtotal = height + 10 + 2 + 33,
                    .vrefresh = HZ,
                };
                sprintf(mode.name, "%dx%d", width, height);
                memcpy(connector->modes, &mode,
                       sizeof(struct drm_mode_modeinfo));
            }
        }
    }

    if (!dev->resource_mgr.crtcs[0]) {
        drm_crtc_t *crtc = drm_crtc_alloc(&dev->resource_mgr, NULL);
        // CRTC will be configured when used
    }

    if (!dev->resource_mgr.encoders[0]) {
        drm_encoder_t *encoder = drm_encoder_alloc(
            &dev->resource_mgr, DRM_MODE_ENCODER_VIRTUAL, NULL);
        if (encoder && dev->resource_mgr.connectors[0] &&
            dev->resource_mgr.crtcs[0]) {
            encoder->possible_crtcs = 1;
            encoder->crtc_id = dev->resource_mgr.crtcs[0]->id;
            dev->resource_mgr.connectors[0]->encoder_id = encoder->id;
        }
    }

    if (dev->resource_mgr.connectors[0] && dev->resource_mgr.crtcs[0]) {
        drm_connector_t *connector = dev->resource_mgr.connectors[0];
        drm_crtc_t *crtc = dev->resource_mgr.crtcs[0];

        connector->crtc_id = crtc->id;
        if (!crtc->mode_valid && connector->modes && connector->count_modes) {
            crtc->mode = connector->modes[0];
            crtc->mode_valid = 1;
            crtc->w = connector->modes[0].hdisplay;
            crtc->h = connector->modes[0].vdisplay;
        }
    }

    drm_framebuffer_t *framebuffer =
        drm_framebuffer_alloc(&dev->resource_mgr, NULL);
    uint32_t width, height, bpp;
    dev->op->get_display_info(dev, &width, &height, &bpp);
    framebuffer->width = width;
    framebuffer->height = height;
    framebuffer->bpp = bpp;
    framebuffer->pitch = width * sizeof(uint32_t);
    framebuffer->depth = 24;

    // Setup sysfs entries
    drm_register_sysfs_nodes((card_dev_nr >> 8) & 0xFF, dev->primary_minor,
                             dev->render_minor, dev->render_node_registered,
                             pci_dev, dev->driver_name, card_dev_name,
                             render_dev_name);

    drm_id++;

    return dev;
}

drm_device_t *drm_register_device(void *data, drm_device_op_t *op,
                                  const char *name, pci_device_t *pci_dev) {
    return drm_register_device_with_info(data, op, name, pci_dev, DRM_NAME,
                                         "20060810", "NaOS DRM");
}

/**
 * drm_regist_pci_dev - Register a PCI-based DRM device (legacy API)
 * @data: Driver private data
 * @op: Driver operations
 * @pci_dev: PCI device
 *
 * This is the legacy API for backwards compatibility.
 * New drivers should use drm_register_device() instead.
 */
drm_device_t *drm_regist_pci_dev(void *data, drm_device_op_t *op,
                                 pci_device_t *pci_dev) {
    return drm_register_device(data, op, "dri/card", pci_dev);
}

/**
 * drm_unregister_device - Unregister a DRM device
 * @dev: DRM device to unregister
 *
 * Unregisters a DRM device and frees its resources.
 * The device must not be used after this call.
 */
void drm_unregister_device(drm_device_t *dev) {
    if (!dev)
        return;

    drm_device_untrack(dev);
    drm_event_node_unregister(dev);

    // Clean up resource manager
    drm_resource_manager_cleanup(&dev->resource_mgr);

    free(dev);
}

/**
 * drm_init_after_pci_sysfs - Initialize fallback DRM device
 *
 * Initializes a plain framebuffer DRM device if no GPU is found.
 * This provides basic display functionality even without hardware acceleration.
 */
void drm_init_after_pci_sysfs() {
    struct vfs_path path = {0};

    if (vfs_filename_lookup(AT_FDCWD, "/dev/dri/card0", LOOKUP_FOLLOW, &path) <
        0) {
        printk("Cannot find GPU device, using framebuffer.\n");
        extern void drm_plainfb_init(void);
        drm_plainfb_init();
        return;
    }
    vfs_path_put(&path);
}

/**
 * drm_post_event - Post an event to the DRM device
 * @dev: DRM device
 * @type: Event type (DRM_EVENT_VBLANK, DRM_EVENT_FLIP_COMPLETE, etc.)
 * @user_data: User data to include with the event
 *
 * Posts an event to the DRM device's event queue.
 * Returns 0 on success, drops oldest event when the queue is full.
 */
int drm_post_event(drm_device_t *dev, fd_t *fd, uint32_t type,
                   uint64_t user_data) {
    drm_file_t *file = drm_current_file(NULL, fd);
    vfs_node_t *event_node = NULL;
    uint64_t now = nano_time();
    uint64_t sequence = 0;

    if (!dev || !file || file->dev != dev)
        return -EBADF;

    spin_lock(&dev->event_lock);

    if (type == DRM_EVENT_VBLANK) {
        dev->vblank_counter++;
    }
    sequence = dev->vblank_counter;
    if (drm_queue_ready_event_locked(file, type, user_data, now, sequence))
        event_node = drm_file_event_node_get_locked(file);

    spin_unlock(&dev->event_lock);
    drm_notify_file_event_node(event_node);

    return 0;
}

int drm_defer_event(drm_device_t *dev, fd_t *fd, uint32_t type,
                    uint64_t user_data) {
    uint64_t target_sequence;

    if (!dev)
        return -EBADF;

    spin_lock(&dev->event_lock);
    target_sequence = dev->vblank_counter + 1;
    spin_unlock(&dev->event_lock);

    return drm_defer_event_at(dev, fd, type, user_data, target_sequence);
}

int drm_defer_event_at(drm_device_t *dev, fd_t *fd, uint32_t type,
                       uint64_t user_data, uint64_t target_sequence) {
    drm_file_t *file = drm_current_file(NULL, fd);
    uint32_t slot = 0;

    if (!dev || !file || file->dev != dev)
        return -EBADF;

    spin_lock(&dev->event_lock);

    if (!dev->vblank_period_ns || target_sequence <= dev->vblank_counter) {
        uint64_t now = nano_time();
        vfs_node_t *event_node = NULL;
        if (drm_queue_ready_event_locked(file, type, user_data, now,
                                         dev->vblank_counter))
            event_node = drm_file_event_node_get_locked(file);
        spin_unlock(&dev->event_lock);
        drm_notify_file_event_node(event_node);
        return 0;
    }

    if (dev->pending_event_count == DRM_MAX_EVENTS_COUNT) {
        dev->pending_event_head =
            (dev->pending_event_head + 1) % DRM_MAX_EVENTS_COUNT;
        dev->pending_event_count--;
    }

    slot = (dev->pending_event_head + dev->pending_event_count) %
           DRM_MAX_EVENTS_COUNT;
    dev->pending_events[slot].type = type;
    dev->pending_events[slot].user_data = user_data;
    dev->pending_events[slot].target_sequence = target_sequence;
    dev->pending_events[slot].file = file;
    dev->pending_event_count++;

    spin_unlock(&dev->event_lock);

    return 0;
}

int drm_notify_hotplug(drm_device_t *dev) {
    if (!dev) {
        return -ENODEV;
    }

    spin_lock(&dev->event_lock);
    dev->hotplug_pending = true;
    spin_unlock(&dev->event_lock);

    drm_notify_event_node_flags(dev, EPOLLPRI);
    return 0;
}

void drm_handle_vblank_tick(void) {
    drm_device_t *devices[DRM_MAX_TRACKED_DEVICES] = {0};
    uint64_t now = nano_time();

    spin_lock(&drm_devices_lock);
    memcpy(devices, drm_devices, sizeof(devices));
    spin_unlock(&drm_devices_lock);

    for (uint32_t i = 0; i < DRM_MAX_TRACKED_DEVICES; i++) {
        drm_device_t *dev = devices[i];
        vfs_node_t *notify_nodes[DRM_MAX_EVENTS_COUNT] = {0};
        uint32_t notify_count = 0;

        if (!dev)
            continue;

        if (!dev->vblank_period_ns) {
            continue;
        }

        spin_lock(&dev->event_lock);

        if (dev->next_vblank_ns == 0)
            dev->next_vblank_ns = now + dev->vblank_period_ns;

        if (now < dev->next_vblank_ns) {
            spin_unlock(&dev->event_lock);
            continue;
        }

        uint64_t periods =
            ((now - dev->next_vblank_ns) / dev->vblank_period_ns) + 1;
        dev->vblank_counter += periods;
        dev->next_vblank_ns += periods * dev->vblank_period_ns;

        uint32_t pending_count = dev->pending_event_count;
        for (uint32_t j = 0; j < pending_count; j++) {
            struct k_drm_event pending =
                dev->pending_events[dev->pending_event_head];
            dev->pending_events[dev->pending_event_head].file = NULL;
            dev->pending_event_head =
                (dev->pending_event_head + 1) % DRM_MAX_EVENTS_COUNT;
            dev->pending_event_count--;

            if (pending.file && pending.target_sequence > dev->vblank_counter) {
                uint32_t slot =
                    (dev->pending_event_head + dev->pending_event_count) %
                    DRM_MAX_EVENTS_COUNT;
                dev->pending_events[slot] = pending;
                dev->pending_event_count++;
                continue;
            }

            if (pending.file) {
                if (drm_queue_ready_event_locked(pending.file, pending.type,
                                                 pending.user_data, now,
                                                 dev->vblank_counter)) {
                    vfs_node_t *node =
                        drm_file_event_node_get_locked(pending.file);
                    if (node)
                        notify_nodes[notify_count++] = node;
                }
            }
        }

        spin_unlock(&dev->event_lock);

        for (uint32_t j = 0; j < notify_count; j++)
            drm_notify_file_event_node(notify_nodes[j]);
    }
}
