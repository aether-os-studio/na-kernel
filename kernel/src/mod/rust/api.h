#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libs/errno.h>
#include <fs/fs_syscall.h>
#include <fs/vfs/vfs.h>
#include <dev/device.h>

enum { NA_VFS_FS_REQUIRES_DEV = VFS_FS_REQUIRES_DEV };
enum { NA_DEVICE_FLUSH = DEV_CMD_FLUSH };
enum na_vfs_statx_bits {
    NA_VFS_STATX_MODE = STATX_MODE,
    NA_VFS_STATX_UID = STATX_UID,
    NA_VFS_STATX_GID = STATX_GID,
    NA_VFS_STATX_ATIME = STATX_ATIME,
    NA_VFS_STATX_MTIME = STATX_MTIME,
    NA_VFS_STATX_CTIME = STATX_CTIME,
    NA_VFS_STATX_SIZE = STATX_SIZE,
};
enum {
    NA_VFS_RENAME_NOREPLACE = VFS_RENAME_NOREPLACE,
    NA_VFS_RENAME_EXCHANGE = VFS_RENAME_EXCHANGE,
    NA_VFS_RENAME_WHITEOUT = VFS_RENAME_WHITEOUT,
};
enum {
    NA_VFS_QUOTA_USER = USRQUOTA,
    NA_VFS_QUOTA_BLOCK_LIMITS = QIF_BLIMITS,
};

typedef struct statfs na_vfs_statfs;

struct pci_device;
typedef struct pci_device pci_device_t;
typedef struct pci_device PciDevice;

typedef struct na_spinlock {
    uint8_t lock;
    bool irq_state;
} RawSpinLock;

struct na_mutex;
typedef struct na_mutex na_mutex_t;

typedef struct na_acpi_table {
    void *ptr;
    size_t index;
} UacpiTable;

void *na_memory_allocate(uint64_t bytes);
void na_memory_free(void *ptr, uint64_t bytes);
void *na_heap_allocate(size_t bytes);
void *na_heap_allocate_aligned(size_t bytes, size_t alignment);
void *na_heap_reallocate(void *ptr, size_t bytes);
void *na_heap_reallocate_aligned(void *ptr, size_t bytes, size_t alignment);
void na_heap_free(void *ptr);
int na_firmware_request(const char *name, void **data, size_t *size);
uint64_t na_memory_physical_address(const void *ptr);
void na_dma_sync_for_device(void *address, size_t size);
void na_dma_sync_for_cpu(void *address, size_t size);
uint64_t na_monotonic_time_ns(void);
void na_delay_us(uint64_t microseconds);
int na_user_read(uint64_t address, void *destination, size_t size);
int na_user_write(uint64_t address, const void *source, size_t size);
int na_vfs_resolve_device(const struct vfs_fs_context *context,
                          uint64_t *device);
void *na_vfs_general_map(struct vfs_file *file, void *address, size_t offset,
                         size_t size, size_t protection, uint64_t flags);
ssize_t na_vfs_file_cached_read(struct vfs_file *file, void *buffer,
                                size_t count, int64_t *position);
ssize_t na_vfs_file_cached_write(struct vfs_file *file, const void *buffer,
                                 size_t count, int64_t *position);
int na_vfs_file_install_anonymous_inode(struct vfs_file *file,
                                        struct vfs_inode *inode);
int na_vfs_mapping_writeback(struct vfs_inode *inode, uint64_t end);
int na_vfs_mapping_writeback_range(struct vfs_inode *inode, uint64_t start,
                                   uint64_t end, bool datasync);
void na_vfs_mapping_truncate(struct vfs_inode *inode, uint64_t size);
void na_vfs_init_new_inode_owner(struct vfs_inode *parent, uint16_t *mode,
                                 uint32_t *uid, uint32_t *gid);
uint64_t na_vfs_realtime_seconds(void);
uint32_t na_vfs_current_fsuid(void);
uint32_t na_vfs_current_fsgid(void);
int64_t na_vfs_device_open(uint64_t device, struct vfs_file *file);
int64_t na_vfs_device_close(uint64_t device, struct vfs_file *file);
int64_t na_vfs_device_read(uint64_t device, struct vfs_file *file, void *buffer,
                           uint64_t offset, size_t size);
int64_t na_vfs_device_write(uint64_t device, struct vfs_file *file,
                            const void *buffer, uint64_t offset, size_t size);
int64_t na_vfs_device_ioctl(uint64_t device, struct vfs_file *file,
                            uint64_t command, uint64_t argument);
int64_t na_vfs_device_poll(uint64_t device, struct vfs_file *file,
                           uint32_t events);
void *na_vfs_device_map(uint64_t device, struct vfs_file *file, void *address,
                        size_t offset, size_t size, size_t protection);
void na_spin_lock(RawSpinLock *lock);
void na_spin_unlock(RawSpinLock *lock);
na_mutex_t *na_mutex_create(void);
void na_mutex_destroy(na_mutex_t *mutex);
void na_mutex_lock(na_mutex_t *mutex);
void na_mutex_unlock(na_mutex_t *mutex);
int na_acpi_table_find(const char signature[4], UacpiTable *table);
void na_acpi_table_release(UacpiTable *table);
int na_acpi_atrm_read(uint8_t *buf, size_t buf_size, size_t *out_len);

typedef struct na_boot_framebuffer {
    uint64_t physical_address;
    uint64_t width;
    uint64_t height;
    uint64_t bpp;
    uint64_t pitch;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
} na_boot_framebuffer_t;
typedef na_boot_framebuffer_t BootFramebuffer;

int na_boot_get_framebuffer(na_boot_framebuffer_t *out);
int na_tty_rebind_framebuffer(const na_boot_framebuffer_t *framebuffer);

typedef struct na_pci_device_info {
    uint32_t class_code;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint16_t segment;
    uint8_t revision_id;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t irq_line;
    uint8_t irq_pin;
} na_pci_device_info_t;
typedef na_pci_device_info_t PciDeviceInfo;

typedef struct na_pci_bar_info {
    uint64_t address;
    uint64_t size;
    bool is_mmio;
    bool prefetchable;
} na_pci_bar_info_t;
typedef na_pci_bar_info_t PciBarInfo;

typedef struct na_pci_driver_ops {
    void *context;
    bool (*matches)(void *context, pci_device_t *device);
    int (*probe)(void *context, pci_device_t *device);
} na_pci_driver_ops_t;
typedef na_pci_driver_ops_t PciDriverOps;

void *na_mmio_map(uint64_t physical_address, size_t size);
void na_log(const char *message);

int na_pci_device_info(pci_device_t *device, na_pci_device_info_t *info);
int na_pci_bar_info(pci_device_t *device, uint8_t index,
                    na_pci_bar_info_t *info);
int na_pci_rom_bar(pci_device_t *device, na_pci_bar_info_t *info);
int na_pci_config_read(pci_device_t *device, uint16_t offset, uint8_t width,
                       uint32_t *value);
int na_pci_config_write(pci_device_t *device, uint16_t offset, uint8_t width,
                        uint32_t value);
int na_pci_bar_claim(pci_device_t *device, uint8_t index);
void na_pci_bar_release(pci_device_t *device, uint8_t index);
int na_pci_driver_register(const char *name, uint32_t class_id, int flags,
                           const na_pci_driver_ops_t *ops);

typedef void (*na_irq_handler_fn)(uint64_t irq_num, void *data);

int na_msi_setup_irq(pci_device_t *device, bool prefer_msix,
                     na_irq_handler_fn handler, void *handler_data,
                     const char *name, uint64_t *out_handle);
void na_msi_release_irq(uint64_t handle);

struct na_virtio_device;
typedef struct na_virtio_device na_virtio_device_t;
struct na_virtio_queue;
typedef struct na_virtio_queue na_virtio_queue_t;

typedef struct na_virtio_driver_ops {
    void *context;
    int (*probe)(void *context, na_virtio_device_t *device);
} na_virtio_driver_ops_t;

int na_virtio_driver_register(const char *name, uint32_t device_type,
                              uint64_t supported_features,
                              const na_virtio_driver_ops_t *ops);
uint64_t na_virtio_device_features(const na_virtio_device_t *device);
void na_virtio_device_finish(na_virtio_device_t *device);
uint32_t na_virtio_device_config_read(const na_virtio_device_t *device,
                                      uint32_t offset);
void na_virtio_device_config_write(na_virtio_device_t *device, uint32_t offset,
                                   uint32_t value);
pci_device_t *na_virtio_device_pci(const na_virtio_device_t *device);
int na_virtio_device_queue(na_virtio_device_t *device, uint16_t queue_index,
                           na_virtio_queue_t **queue);
int na_virtio_queue_submit(na_virtio_queue_t *queue, const void *request,
                           size_t request_size, const void *extra,
                           size_t extra_size, void *response,
                           size_t response_size);
void na_virtio_device_set_config_handler(na_virtio_device_t *device,
                                         void *context,
                                         void (*handler)(void *context));

struct drm_device;
typedef struct drm_device drm_device_t;
typedef struct drm_device DrmDevice;
struct drm_connector;
typedef struct drm_connector drm_connector_t;
typedef struct drm_connector DrmConnector;
struct drm_crtc;
typedef struct drm_crtc drm_crtc_t;
typedef struct drm_crtc DrmCrtc;
struct drm_encoder;
typedef struct drm_encoder drm_encoder_t;
typedef struct drm_encoder DrmEncoder;
struct drm_plane;
typedef struct drm_plane drm_plane_t;
typedef struct drm_plane DrmPlane;

typedef struct na_drm_dumb_buffer {
    uint32_t height;
    uint32_t width;
    uint32_t bits_per_pixel;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} na_drm_dumb_buffer_t;
typedef na_drm_dumb_buffer_t DrmDumbBuffer;

typedef struct na_drm_mode_info {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t mode_type;
    char name[32];
} na_drm_mode_info_t;
typedef na_drm_mode_info_t DrmModeInfo;

typedef struct na_drm_connector_info {
    uint32_t connector_type;
    uint32_t connection;
    uint32_t encoder_index;
    uint32_t crtc_index;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
} na_drm_connector_info_t;
typedef na_drm_connector_info_t DrmConnectorInfo;

typedef struct na_drm_crtc_info {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t gamma_size;
    bool mode_valid;
    na_drm_mode_info_t mode_info;
} na_drm_crtc_info_t;
typedef na_drm_crtc_info_t DrmCrtcInfo;

typedef struct na_drm_encoder_info {
    uint32_t encoder_type;
    uint32_t crtc_index;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} na_drm_encoder_info_t;
typedef na_drm_encoder_info_t DrmEncoderInfo;

typedef struct na_drm_plane_info {
    uint32_t crtc_index;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t plane_type;
} na_drm_plane_info_t;
typedef na_drm_plane_info_t DrmPlaneInfo;

typedef struct na_drm_framebuffer_request {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t bits_per_pixel;
    uint32_t depth;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifiers[4];
    uint64_t file_id;
    uint32_t driver_handle;
} na_drm_framebuffer_request_t;
typedef na_drm_framebuffer_request_t DrmFramebufferRequest;

typedef struct na_drm_clip {
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
} na_drm_clip_t;
typedef na_drm_clip_t DrmClip;

typedef struct na_drm_plane_update {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t framebuffer_id;
    uint32_t framebuffer_handle;
    uint32_t flags;
    int32_t crtc_x;
    int32_t crtc_y;
    uint32_t crtc_width;
    uint32_t crtc_height;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t source_width;
    uint32_t source_height;
} na_drm_plane_update_t;
typedef na_drm_plane_update_t DrmPlaneUpdate;

typedef struct na_drm_crtc_update {
    uint32_t crtc_id;
    uint32_t framebuffer_id;
    uint32_t framebuffer_handle;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    bool mode_valid;
    na_drm_mode_info_t mode_info;
} na_drm_crtc_update_t;
typedef na_drm_crtc_update_t DrmCrtcUpdate;

typedef struct na_drm_page_flip {
    uint32_t crtc_id;
    uint32_t framebuffer_id;
    uint32_t framebuffer_handle;
    uint32_t flags;
    uint64_t user_data;
} na_drm_page_flip_t;
typedef na_drm_page_flip_t DrmPageFlip;

typedef struct na_drm_cursor_update {
    uint64_t file_id;
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
} na_drm_cursor_update_t;
typedef na_drm_cursor_update_t DrmCursorUpdate;

typedef struct na_drm_atomic_property {
    uint32_t object_id;
    uint32_t property_id;
    uint64_t value;
    uint32_t framebuffer_handle;
} na_drm_atomic_property_t;
typedef na_drm_atomic_property_t DrmAtomicProperty;

typedef struct na_drm_driver_info {
    const char *kernel_name;
    const char *uapi_name;
    const char *date;
    const char *description;
    int version_major;
    int version_minor;
    int version_patchlevel;
} na_drm_driver_info_t;
typedef na_drm_driver_info_t DrmDriverInfo;

typedef struct na_drm_driver_ops {
    void *context;
    bool supports_render_node;
    bool supports_atomic_modeset;
    int (*open)(void *context, uint64_t file_id);
    void (*close)(void *context, uint64_t file_id);
    int (*get_capability)(void *context, uint64_t capability, uint64_t *value);
    int (*get_display_info)(void *context, uint32_t *width, uint32_t *height,
                            uint32_t *bits_per_pixel);
    int (*get_framebuffer)(void *context, uint32_t *width, uint32_t *height,
                           uint32_t *bits_per_pixel,
                           uint64_t *physical_address);
    int (*create_dumb_buffer)(void *context, na_drm_dumb_buffer_t *buffer);
    int (*destroy_dumb_buffer)(void *context, uint32_t handle);
    int (*map_dumb_buffer)(void *context, uint32_t handle, uint64_t *offset);
    int (*get_dumb_buffer_mapping)(void *context, uint32_t handle,
                                   uint64_t *physical_address, uint64_t *size);
    int (*get_connectors)(void *context, drm_device_t *device,
                          drm_connector_t **connectors, uint32_t capacity,
                          uint32_t *count);
    int (*get_crtcs)(void *context, drm_device_t *device, drm_crtc_t **crtcs,
                     uint32_t capacity, uint32_t *count);
    int (*get_encoders)(void *context, drm_device_t *device,
                        drm_encoder_t **encoders, uint32_t capacity,
                        uint32_t *count);
    int (*get_planes)(void *context, drm_device_t *device, drm_plane_t **planes,
                      uint32_t capacity, uint32_t *count);
    int (*create_framebuffer)(void *context,
                              na_drm_framebuffer_request_t *request);
    void (*release_framebuffer)(void *context, uint32_t handle);
    int (*get_framebuffer_handle)(void *context, uint64_t file_id,
                                  uint32_t framebuffer_handle,
                                  uint32_t *gem_handle);
    int (*dirty_framebuffer)(void *context, uint32_t framebuffer_id,
                             uint32_t framebuffer_handle, uint32_t flags,
                             uint32_t color, const na_drm_clip_t *clips,
                             uint32_t clip_count);
    int (*set_plane)(void *context, const na_drm_plane_update_t *update);
    int (*set_crtc)(void *context, const na_drm_crtc_update_t *update,
                    const uint32_t *connector_ids, uint32_t connector_count);
    int (*page_flip)(void *context, const na_drm_page_flip_t *flip);
    int (*set_cursor)(void *context, const na_drm_cursor_update_t *cursor);
    int (*atomic_commit)(void *context, uint32_t flags, uint64_t user_data,
                         const na_drm_atomic_property_t *properties,
                         size_t property_count);
    int (*mmap)(void *context, uint64_t file_id, uint64_t offset,
                uint64_t length, uint64_t *physical_address);
    int (*prime_export)(void *context, uint64_t file_id, uint32_t handle,
                        uint64_t *physical_address, uint64_t *size,
                        uint64_t *token);
    int (*prime_import)(void *context, uint64_t file_id, uint64_t token,
                        uint32_t *handle);
    int (*prime_release)(void *context, uint64_t token);
    int64_t (*driver_ioctl)(void *context, uint32_t command, void *arg,
                            size_t arg_size, bool render_node,
                            uint64_t file_id);
} na_drm_driver_ops_t;
typedef na_drm_driver_ops_t DrmDriverOps;

drm_device_t *na_drm_device_register(const na_drm_driver_ops_t *ops,
                                     const char *node_name,
                                     pci_device_t *pci_device,
                                     const na_drm_driver_info_t *driver_info);
void na_drm_device_unregister(drm_device_t *device);
int na_drm_device_notify_hotplug(drm_device_t *device);
drm_connector_t *na_drm_connector_create(const na_drm_connector_info_t *info);
int na_drm_connector_add_mode(drm_connector_t *connector,
                              const na_drm_mode_info_t *mode);
void na_drm_connector_destroy(drm_connector_t *connector);
drm_crtc_t *na_drm_crtc_create(const na_drm_crtc_info_t *info);
void na_drm_crtc_destroy(drm_crtc_t *crtc);
drm_encoder_t *na_drm_encoder_create(const na_drm_encoder_info_t *info);
void na_drm_encoder_destroy(drm_encoder_t *encoder);
drm_plane_t *na_drm_plane_create(drm_device_t *device,
                                 const na_drm_plane_info_t *info);
int na_drm_plane_add_format(drm_plane_t *plane, uint32_t format);
void na_drm_plane_destroy(drm_plane_t *plane);
int na_drm_syncobj_wait(uint64_t file_id, uint32_t handle, uint64_t point,
                        int64_t timeout_ns);
int na_drm_syncobj_signal(uint64_t file_id, uint32_t handle, uint64_t point);
int na_drm_syncobj_attach_fence(uint64_t file_id, uint32_t handle,
                                uint64_t point, uint32_t timeline,
                                uint64_t cpu_address, uint64_t value);

struct fdt_device;
typedef struct fdt_device fdt_device_t;
typedef struct fdt_device FdtDevice;

typedef struct na_fdt_driver_ops {
    void *context;
    int (*probe)(void *context, fdt_device_t *device, const char *compatible);
} na_fdt_driver_ops_t;
typedef na_fdt_driver_ops_t FdtDriverOps;

int na_fdt_driver_register(const char *name, const char *compatible_blob,
                           size_t compatible_blob_size,
                           const na_fdt_driver_ops_t *ops);
const char *na_fdt_device_name(const fdt_device_t *device);
const void *na_fdt_device_property(const fdt_device_t *device, const char *name,
                                   size_t *length);
int na_fdt_device_reg_cells(const fdt_device_t *device, uint32_t *address_cells,
                            uint32_t *size_cells);
