#pragma once

#include <drivers/drm/drm.h>
#include <drivers/drm/drm_core.h>
#include <drivers/virtio/pci.h>
#include <drivers/virtio/queue.h>
#include <drivers/virtio/virtio.h>
#include <task/wait.h>

#define VIRTIO_GPU_QUEUE_CONTROL 0
#define VIRTIO_GPU_QUEUE_CURSOR 1

#define VIRTIO_GPU_MAX_SCANOUTS 64
#define VIRTIO_GPU_MAX_DUMB_BUFFERS 4096
#define VIRTIO_GPU_FILE_HANDLE_WORDS ((VIRTIO_GPU_MAX_DUMB_BUFFERS + 63) / 64)
#define VIRTIO_GPU_MAX_CONTEXTS 64
#define VIRTIO_GPU_INVALID_RESOURCE_ID 0

#define VIRTIO_GPU_FLAG_FENCE (1U << 0)
#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK 0x000000ffU

#define VIRTIO_GPU_F_VIRGL (1ULL << 0)
#define VIRTIO_GPU_F_EDID (1ULL << 1)
#define VIRTIO_GPU_F_RESOURCE_UUID (1ULL << 2)
#define VIRTIO_GPU_F_RESOURCE_BLOB (1ULL << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT (1ULL << 4)
#define VIRTIO_GPU_F_SUPPORTED_CAPSET_IDS (1ULL << 5)

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM 121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM 134

typedef enum virtio_gpu_ctrl_type {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID = 0x010a,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID = 0x010b,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB = 0x010c,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB = 0x010d,

    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE = 0x0202,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE = 0x0203,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D = 0x0207,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB = 0x0208,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB = 0x0209,

    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET = 0x1103,
    VIRTIO_GPU_RESP_OK_EDID = 0x1104,
    VIRTIO_GPU_RESP_OK_RESOURCE_UUID = 0x1105,
    VIRTIO_GPU_RESP_OK_MAP_INFO = 0x1106,

    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY = 0x1201,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID = 0x1204,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER = 0x1205,
} virtio_gpu_ctrl_type_t;

typedef struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

typedef struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_rect_t;

typedef struct virtio_gpu_display_one {
    virtio_gpu_rect_t rect;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

typedef struct virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

typedef struct virtio_gpu_resource_create_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

typedef struct virtio_gpu_resource_unref {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

typedef struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct virtio_gpu_resource_attach_backing {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

typedef struct virtio_gpu_resource_detach_backing {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_detach_backing_t;

typedef struct virtio_gpu_set_scanout {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

typedef struct virtio_gpu_transfer_to_host_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

typedef struct virtio_gpu_resource_flush {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

typedef struct virtio_gpu_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
} __attribute__((packed)) virtio_gpu_box_t;

typedef struct virtio_gpu_transfer_host_3d {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed)) virtio_gpu_transfer_host_3d_t;

typedef struct virtio_gpu_resource_create_3d {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_create_3d_t;

typedef struct virtio_gpu_ctx_create {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed)) virtio_gpu_ctx_create_t;

typedef struct virtio_gpu_ctx_destroy {
    virtio_gpu_ctrl_hdr_t hdr;
} __attribute__((packed)) virtio_gpu_ctx_destroy_t;

typedef struct virtio_gpu_ctx_resource {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctx_resource_t;

typedef struct virtio_gpu_cmd_submit {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_cmd_submit_t;

typedef struct virtio_gpu_get_capset_info {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_index;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_get_capset_info_t;

typedef struct virtio_gpu_resp_capset_info {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resp_capset_info_t;

typedef struct virtio_gpu_get_capset {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_version;
} __attribute__((packed)) virtio_gpu_get_capset_t;

typedef enum virtio_gpu_object_kind {
    VIRTIO_GPU_OBJECT_DUMB_2D,
    VIRTIO_GPU_OBJECT_PRIVATE_3D,
    VIRTIO_GPU_OBJECT_BLOB,
} virtio_gpu_object_kind_t;

typedef struct virtio_gpu_buffer {
    bool used;
    virtio_gpu_object_kind_t kind;
    uint32_t handle;
    uint32_t resource_id;
    uint64_t paddr;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    int refcount;
    uint64_t attached_context_mask;
} virtio_gpu_buffer_t;

typedef struct virtio_gpu_context {
    bool used;
    uint32_t id;
    uint32_t capset_id;
    char debug_name[64];
} virtio_gpu_context_t;

typedef struct virtio_gpu_file {
    uint32_t ctx_id;
    uint32_t capset_id;
    uint64_t handles[VIRTIO_GPU_FILE_HANDLE_WORDS];
} virtio_gpu_file_t;

typedef struct virtio_gpu_device {
    virtio_driver_t *driver;
    virtqueue_t *control_vq;
    virtqueue_t *cursor_vq;
    bool control_busy;
    wait_queue_head_t control_available_wait;
    wait_queue_head_t control_irq_wait;
    uint64_t control_irq_seq;
    bool control_irq_enabled;
    uint64_t negotiated_features;
    uint32_t num_capsets;
    uint32_t next_resource_id;
    uint32_t next_context_id;
    uint64_t next_fence_id;
    uint64_t supported_capset_ids;
    uint32_t scanout_id;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    bool display_valid;

    virtio_gpu_buffer_t buffers[VIRTIO_GPU_MAX_DUMB_BUFFERS];
    virtio_gpu_context_t contexts[VIRTIO_GPU_MAX_CONTEXTS];

    drm_connector_t *connectors[DRM_MAX_CONNECTORS_PER_DEVICE];
    drm_crtc_t *crtcs[DRM_MAX_CRTCS_PER_DEVICE];
    drm_encoder_t *encoders[DRM_MAX_ENCODERS_PER_DEVICE];
    drm_resource_manager_t resource_mgr;
    drm_device_t *drm_dev;
} virtio_gpu_device_t;

#define DRM_VIRTGPU_MAP 0x01
#define DRM_VIRTGPU_EXECBUFFER 0x02
#define DRM_VIRTGPU_GETPARAM 0x03
#define DRM_VIRTGPU_RESOURCE_CREATE 0x04
#define DRM_VIRTGPU_RESOURCE_INFO 0x05
#define DRM_VIRTGPU_TRANSFER_FROM_HOST 0x06
#define DRM_VIRTGPU_TRANSFER_TO_HOST 0x07
#define DRM_VIRTGPU_WAIT 0x08
#define DRM_VIRTGPU_GET_CAPS 0x09
#define DRM_VIRTGPU_RESOURCE_CREATE_BLOB 0x0A
#define DRM_VIRTGPU_CONTEXT_INIT 0x0B

#define VIRTGPU_EXECBUF_FENCE_FD_IN 0x01
#define VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIRTGPU_EXECBUF_RING_IDX 0x04
#define VIRTGPU_EXECBUF_FLAGS                                                  \
    (VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT |              \
     VIRTGPU_EXECBUF_RING_IDX)

#define VIRTGPU_EXECBUF_SYNCOBJ_RESET 0x01
#define VIRTGPU_EXECBUF_SYNCOBJ_FLAGS (VIRTGPU_EXECBUF_SYNCOBJ_RESET)

struct drm_virtgpu_map {
    __u64 offset;
    __u32 handle;
    __u32 pad;
};

struct drm_virtgpu_execbuffer_syncobj {
    __u32 handle;
    __u32 flags;
    __u64 point;
};

struct drm_virtgpu_execbuffer {
    __u32 flags;
    __u32 size;
    __u64 command;
    __u64 bo_handles;
    __u32 num_bo_handles;
    __s32 fence_fd;
    __u32 ring_idx;
    __u32 syncobj_stride;
    __u32 num_in_syncobjs;
    __u32 num_out_syncobjs;
    __u64 in_syncobjs;
    __u64 out_syncobjs;
};

#define VIRTGPU_PARAM_3D_FEATURES 1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_PARAM_RESOURCE_BLOB 3
#define VIRTGPU_PARAM_HOST_VISIBLE 4
#define VIRTGPU_PARAM_CROSS_DEVICE 5
#define VIRTGPU_PARAM_CONTEXT_INIT 6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME 8

struct drm_virtgpu_getparam {
    __u64 param;
    __u64 value;
};

struct drm_virtgpu_resource_create {
    __u32 target;
    __u32 format;
    __u32 bind;
    __u32 width;
    __u32 height;
    __u32 depth;
    __u32 array_size;
    __u32 last_level;
    __u32 nr_samples;
    __u32 flags;
    __u32 bo_handle;
    __u32 res_handle;
    __u32 size;
    __u32 stride;
};

struct drm_virtgpu_resource_info {
    __u32 bo_handle;
    __u32 res_handle;
    __u32 size;
    __u32 blob_mem;
};

struct drm_virtgpu_3d_box {
    __u32 x;
    __u32 y;
    __u32 z;
    __u32 w;
    __u32 h;
    __u32 d;
};

struct drm_virtgpu_3d_transfer_to_host {
    __u32 bo_handle;
    struct drm_virtgpu_3d_box box;
    __u32 level;
    __u32 offset;
    __u32 stride;
    __u32 layer_stride;
};

struct drm_virtgpu_3d_transfer_from_host {
    __u32 bo_handle;
    struct drm_virtgpu_3d_box box;
    __u32 level;
    __u32 offset;
    __u32 stride;
    __u32 layer_stride;
};

#define VIRTGPU_WAIT_NOWAIT 1
struct drm_virtgpu_3d_wait {
    __u32 handle;
    __u32 flags;
};

#define VIRTGPU_DRM_CAPSET_VIRGL 1
#define VIRTGPU_DRM_CAPSET_VIRGL2 2
#define VIRTGPU_DRM_CAPSET_GFXSTREAM_VULKAN 3
#define VIRTGPU_DRM_CAPSET_VENUS 4
#define VIRTGPU_DRM_CAPSET_CROSS_DOMAIN 5
#define VIRTGPU_DRM_CAPSET_DRM 6

struct drm_virtgpu_get_caps {
    __u32 cap_set_id;
    __u32 cap_set_ver;
    __u64 addr;
    __u32 size;
    __u32 pad;
};

#define VIRTGPU_BLOB_MEM_GUEST 0x0001
#define VIRTGPU_BLOB_MEM_HOST3D 0x0002
#define VIRTGPU_BLOB_MEM_HOST3D_GUEST 0x0003

#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE 0x0002
#define VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004

struct drm_virtgpu_resource_create_blob {
    __u32 blob_mem;
    __u32 blob_flags;
    __u32 bo_handle;
    __u32 res_handle;
    __u64 size;
    __u32 pad;
    __u32 cmd_size;
    __u64 cmd;
    __u64 blob_id;
};

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID 0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS 0x0002
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define VIRTGPU_CONTEXT_PARAM_DEBUG_NAME 0x0004

struct drm_virtgpu_context_set_param {
    __u64 param;
    __u64 value;
};

struct drm_virtgpu_context_init {
    __u32 num_params;
    __u32 pad;
    __u64 ctx_set_params;
};

#define VIRTGPU_EVENT_FENCE_SIGNALED 0x90000000

#define DRM_IOCTL_VIRTGPU_MAP                                                  \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_MAP, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_EXECBUFFER                                           \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER,                        \
             struct drm_virtgpu_execbuffer)
#define DRM_IOCTL_VIRTGPU_GETPARAM                                             \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM,                          \
             struct drm_virtgpu_getparam)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE                                      \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE,                   \
             struct drm_virtgpu_resource_create)
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO                                        \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_INFO,                     \
             struct drm_virtgpu_resource_info)
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST                                   \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_FROM_HOST,                \
             struct drm_virtgpu_3d_transfer_from_host)
#define DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST                                     \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_TO_HOST,                  \
             struct drm_virtgpu_3d_transfer_to_host)
#define DRM_IOCTL_VIRTGPU_WAIT                                                 \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT, struct drm_virtgpu_3d_wait)
#define DRM_IOCTL_VIRTGPU_GET_CAPS                                             \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GET_CAPS,                          \
             struct drm_virtgpu_get_caps)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB                                 \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB,              \
             struct drm_virtgpu_resource_create_blob)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT                                         \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT,                      \
             struct drm_virtgpu_context_init)

int virtio_gpu_init(virtio_driver_t *driver);
