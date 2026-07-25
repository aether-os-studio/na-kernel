#include "gpu.h"

#include <dev/device.h>
#include <drivers/drm/drm_ioctl.h>
#include <drivers/logger.h>
#include <fs/sys.h>
#include <mm/mm.h>
#include <task/task.h>

#define DRM_DUMB_MMAP_OFFSET_BASE 0x100000000ULL
#define DRM_DUMB_MMAP_OFFSET_STRIDE 0x100000000ULL

#define VIRTIO_GPU_DEFAULT_WIDTH 1024
#define VIRTIO_GPU_DEFAULT_HEIGHT 768
#define VIRTIO_GPU_DISPLAY_FALLBACK_POLL_NS 1000000000LL
#define VIRTIO_GPU_RESPONSE_WATCHDOG_NS 10000000LL
#define VIRTIO_GPU_SUPPORTED_FEATURES                                          \
    (VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_EDID | VIRTIO_GPU_F_CONTEXT_INIT |      \
     VIRTIO_GPU_F_SUPPORTED_CAPSET_IDS | VIRTIO_F_RING_INDIRECT_DESC |         \
     VIRTIO_F_VERSION_1)

static bool virtio_gpu_handle_to_index(uint32_t handle, uint32_t *idx) {
    if (handle == 0 || handle > VIRTIO_GPU_MAX_DUMB_BUFFERS || !idx) {
        return false;
    }

    *idx = handle - 1;
    return true;
}

static uint32_t virtio_gpu_alloc_resource_id(virtio_gpu_device_t *gpu) {
    uint32_t id = gpu->next_resource_id++;
    if (id == VIRTIO_GPU_INVALID_RESOURCE_ID) {
        id = gpu->next_resource_id++;
    }
    return id;
}

static uint32_t virtio_gpu_alloc_context_id(virtio_gpu_device_t *gpu) {
    uint32_t id = gpu->next_context_id++;
    if (id == 0) {
        id = gpu->next_context_id++;
    }
    return id;
}

static uint64_t virtio_gpu_alloc_fence_id(virtio_gpu_device_t *gpu) {
    uint64_t id = __atomic_fetch_add(&gpu->next_fence_id, 1, __ATOMIC_RELAXED);
    if (id == 0)
        id = __atomic_fetch_add(&gpu->next_fence_id, 1, __ATOMIC_RELAXED);
    return id;
}

static void virtio_gpu_hdr_init(virtio_gpu_ctrl_hdr_t *hdr, uint32_t type) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = htole32(type);
}

static void virtio_gpu_hdr_init_ctx(virtio_gpu_ctrl_hdr_t *hdr, uint32_t type,
                                    uint32_t ctx_id) {
    virtio_gpu_hdr_init(hdr, type);
    hdr->ctx_id = htole32(ctx_id);
}

static uint32_t virtio_gpu_resp_type(const virtio_gpu_ctrl_hdr_t *hdr) {
    return le32toh(hdr->type);
}

static void virtio_gpu_control_acquire(virtio_gpu_device_t *gpu) {
    for (;;) {
        bool expected = false;
        if (__atomic_compare_exchange_n(&gpu->control_busy, &expected, true,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;

        if (!current_task) {
            arch_pause();
            continue;
        }

        wait_queue_entry_t wait;
        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&gpu->control_available_wait, &wait);

        expected = false;
        if (__atomic_compare_exchange_n(&gpu->control_busy, &expected, true,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            wait_queue_remove(&gpu->control_available_wait, &wait);
            task_cancel_block_prepare(current_task);
            return;
        }

        (void)task_block(current_task, TASK_BLOCKING, -1, "virtio_gpu_control");
        wait_queue_remove(&gpu->control_available_wait, &wait);
        task_cancel_block_prepare(current_task);
    }
}

static void virtio_gpu_control_release(virtio_gpu_device_t *gpu) {
    __atomic_store_n(&gpu->control_busy, false, __ATOMIC_RELEASE);
    wait_queue_wake_all(&gpu->control_available_wait, 0, EOK);
}

static uint16_t virtio_gpu_control_wait_used(virtio_gpu_device_t *gpu,
                                             uint32_t *used_len) {
    for (;;) {
        uint16_t used_idx = virt_queue_get_used_buf(gpu->control_vq, used_len);
        if (used_idx != 0xFFFF)
            return used_idx;

        bool can_sleep =
            current_task &&
            __atomic_load_n(&gpu->control_irq_enabled, __ATOMIC_ACQUIRE);
        if (!can_sleep) {
            if (current_task)
                schedule(0);
            else
                arch_pause();
            continue;
        }

        wait_queue_entry_t wait;
        uint64_t observed_seq =
            __atomic_load_n(&gpu->control_irq_seq, __ATOMIC_ACQUIRE);
        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&gpu->control_irq_wait, &wait);

        used_idx = virt_queue_get_used_buf(gpu->control_vq, used_len);
        if (used_idx != 0xFFFF ||
            __atomic_load_n(&gpu->control_irq_seq, __ATOMIC_ACQUIRE) !=
                observed_seq) {
            wait_queue_remove(&gpu->control_irq_wait, &wait);
            task_cancel_block_prepare(current_task);
            if (used_idx != 0xFFFF)
                return used_idx;
            continue;
        }

        (void)task_block(current_task, TASK_BLOCKING,
                         VIRTIO_GPU_RESPONSE_WATCHDOG_NS,
                         "virtio_gpu_response");
        wait_queue_remove(&gpu->control_irq_wait, &wait);
        task_cancel_block_prepare(current_task);
    }
}

static void virtio_gpu_irq_handler(void *opaque, uint8_t isr_status) {
    virtio_gpu_device_t *gpu = (virtio_gpu_device_t *)opaque;

    if (!gpu)
        return;

    if (isr_status & 0x1) {
        __atomic_fetch_add(&gpu->control_irq_seq, 1, __ATOMIC_RELEASE);
        wait_queue_wake_all(&gpu->control_irq_wait, 0, EOK);
    }

    if ((isr_status & 0x2) && gpu->driver && gpu->driver->op &&
        gpu->driver->op->read_config_space &&
        gpu->driver->op->write_config_space) {
        uint32_t events = gpu->driver->op->read_config_space(
            gpu->driver->data, VIRTIO_GPU_CONFIG_EVENTS_READ);
        if (events) {
            gpu->driver->op->write_config_space(
                gpu->driver->data, VIRTIO_GPU_CONFIG_EVENTS_CLEAR, events);
        }
        if (events & VIRTIO_GPU_EVENT_DISPLAY) {
            __atomic_store_n(&gpu->display_event_pending, true,
                             __ATOMIC_RELEASE);
            wait_queue_wake_all(&gpu->display_event_wait, 0, EOK);
        }
    }
}

static int virtio_gpu_ctl_send(virtio_gpu_device_t *gpu, const void *req,
                               size_t req_size, void *resp, size_t resp_size,
                               const void *extra, size_t extra_size) {
    void *req_buf = NULL;
    void *resp_buf = NULL;
    void *extra_buf = NULL;
    virtio_buffer_t bufs[3];
    bool writable[3] = {false, false, true};
    uint16_t num_bufs = 0;
    uint16_t desc_idx = 0xFFFF;
    uint32_t used_len = 0;
    uint16_t used_idx = 0xFFFF;
    int ret = -EIO;

    if (!gpu || !gpu->control_vq || !req || !resp || req_size == 0 ||
        resp_size < sizeof(virtio_gpu_ctrl_hdr_t)) {
        return -EINVAL;
    }

    req_buf = alloc_frames_bytes(req_size);
    resp_buf = alloc_frames_bytes(resp_size);
    if (!req_buf || !resp_buf) {
        ret = -ENOMEM;
        goto out;
    }

    memcpy(req_buf, req, req_size);
    memset(resp_buf, 0, resp_size);

    bufs[num_bufs].addr = (uint64_t)req_buf;
    bufs[num_bufs].size = req_size;
    writable[num_bufs++] = false;

    if (extra && extra_size != 0) {
        extra_buf = alloc_frames_bytes(extra_size);
        if (!extra_buf) {
            ret = -ENOMEM;
            goto out;
        }
        memcpy(extra_buf, extra, extra_size);
        bufs[num_bufs].addr = (uint64_t)extra_buf;
        bufs[num_bufs].size = extra_size;
        writable[num_bufs++] = false;
    }

    bufs[num_bufs].addr = (uint64_t)resp_buf;
    bufs[num_bufs].size = resp_size;
    writable[num_bufs++] = true;

    dma_sync_cpu_to_device(req_buf, req_size);
    if (extra_buf) {
        dma_sync_cpu_to_device(extra_buf, extra_size);
    }
    dma_sync_cpu_to_device(resp_buf, resp_size);

    virtio_gpu_control_acquire(gpu);
    desc_idx = virt_queue_add_buf(gpu->control_vq, bufs, num_bufs, writable);
    if (desc_idx == 0xFFFF) {
        virtio_gpu_control_release(gpu);
        ret = -EIO;
        goto out;
    }

    virt_queue_submit_buf(gpu->control_vq, desc_idx);
    virt_queue_notify(gpu->driver, gpu->control_vq);

    used_idx = virtio_gpu_control_wait_used(gpu, &used_len);
    virt_queue_free_desc(gpu->control_vq, used_idx);
    virtio_gpu_control_release(gpu);

    dma_sync_device_to_cpu(resp_buf, resp_size);
    memcpy(resp, resp_buf, resp_size);

    uint32_t type = virtio_gpu_resp_type((virtio_gpu_ctrl_hdr_t *)resp);
    if (type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        printk("virtio_gpu: command 0x%x failed with response 0x%x\n",
               le32toh(((const virtio_gpu_ctrl_hdr_t *)req)->type), type);
        ret = -EIO;
        goto out;
    }

    ret = 0;

out:
    if (req_buf) {
        free_frames_bytes(req_buf, req_size);
    }
    if (resp_buf) {
        free_frames_bytes(resp_buf, resp_size);
    }
    if (extra_buf) {
        free_frames_bytes(extra_buf, extra_size);
    }
    return ret;
}

static int virtio_gpu_simple_cmd(virtio_gpu_device_t *gpu, const void *req,
                                 size_t req_size) {
    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    int ret =
        virtio_gpu_ctl_send(gpu, req, req_size, &resp, sizeof(resp), NULL, 0);
    if (ret != 0) {
        return ret;
    }

    return virtio_gpu_resp_type(&resp) == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -EIO;
}

static void virtio_gpu_build_mode(const virtio_gpu_device_t *gpu,
                                  struct drm_mode_modeinfo *mode) {
    if (!gpu || !mode) {
        return;
    }

    memset(mode, 0, sizeof(*mode));
    mode->hdisplay = gpu->width;
    mode->hsync_start = gpu->width + 16;
    mode->hsync_end = gpu->width + 16 + 96;
    mode->htotal = gpu->width + 16 + 96 + 48;
    mode->vdisplay = gpu->height;
    mode->vsync_start = gpu->height + 10;
    mode->vsync_end = gpu->height + 10 + 2;
    mode->vtotal = gpu->height + 10 + 2 + 33;
    mode->vrefresh = HZ;
    mode->clock =
        (uint32_t)(((uint64_t)mode->htotal * mode->vtotal * mode->vrefresh +
                    500) /
                   1000);
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    snprintf(mode->name, sizeof(mode->name), "%dx%d", gpu->width, gpu->height);
}

static void virtio_gpu_update_modeset(virtio_gpu_device_t *gpu) {
    if (!gpu) {
        return;
    }

    spin_lock(&gpu->resource_mgr.lock);

    drm_connector_t *connector = gpu->connectors[0];
    drm_crtc_t *crtc = gpu->crtcs[0];
    drm_encoder_t *encoder = gpu->encoders[0];

    if (connector) {
        connector->connection =
            gpu->display_valid ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
        connector->mm_width = (gpu->width * 264UL) / 1000UL;
        connector->mm_height = (gpu->height * 264UL) / 1000UL;
        if (connector->mm_width == 0) {
            connector->mm_width = 1;
        }
        if (connector->mm_height == 0) {
            connector->mm_height = 1;
        }

        if (!connector->modes) {
            connector->modes = malloc(sizeof(*connector->modes));
        }
        if (gpu->display_valid && connector->modes) {
            virtio_gpu_build_mode(gpu, &connector->modes[0]);
            connector->count_modes = 1;
        } else if (!gpu->display_valid) {
            connector->count_modes = 0;
        }
    }

    if (crtc) {
        crtc->x = 0;
        crtc->y = 0;
        crtc->w = gpu->width;
        crtc->h = gpu->height;
        if (gpu->display_valid) {
            virtio_gpu_build_mode(gpu, &crtc->mode);
            crtc->mode_valid = 1;
        } else {
            crtc->mode_valid = 0;
        }
    }

    if (encoder) {
        encoder->crtc_id = crtc ? crtc->id : 0;
    }
    if (connector) {
        connector->encoder_id = encoder ? encoder->id : 0;
        connector->crtc_id = crtc ? crtc->id : 0;
    }

    spin_unlock(&gpu->resource_mgr.lock);
}

static int virtio_gpu_refresh_display_info(virtio_gpu_device_t *gpu,
                                           bool *changed) {
    virtio_gpu_ctrl_hdr_t req;
    virtio_gpu_resp_display_info_t resp;
    uint32_t scanout_id = 0;
    uint32_t width = VIRTIO_GPU_DEFAULT_WIDTH;
    uint32_t height = VIRTIO_GPU_DEFAULT_HEIGHT;
    bool display_valid = false;

    virtio_gpu_hdr_init(&req, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    memset(&resp, 0, sizeof(resp));

    int ret = virtio_gpu_ctl_send(gpu, &req, sizeof(req), &resp, sizeof(resp),
                                  NULL, 0);
    if (ret != 0) {
        return ret;
    }
    if (virtio_gpu_resp_type(&resp.hdr) != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        return -EIO;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        uint32_t enabled = le32toh(resp.pmodes[i].enabled);
        uint32_t scanout_width = le32toh(resp.pmodes[i].rect.width);
        uint32_t scanout_height = le32toh(resp.pmodes[i].rect.height);
        if (!enabled || scanout_width == 0 || scanout_height == 0) {
            continue;
        }

        scanout_id = i;
        width = scanout_width;
        height = scanout_height;
        display_valid = true;
        break;
    }

    if (changed) {
        *changed = gpu->display_valid != display_valid ||
                   gpu->scanout_id != scanout_id || gpu->width != width ||
                   gpu->height != height || gpu->bpp != 32;
    }

    gpu->display_valid = display_valid;
    gpu->scanout_id = scanout_id;
    gpu->width = width;
    gpu->height = height;
    gpu->bpp = 32;
    virtio_gpu_update_modeset(gpu);
    return 0;
}

static void virtio_gpu_read_config(virtio_gpu_device_t *gpu) {
    if (!gpu || !gpu->driver || !gpu->driver->op ||
        !gpu->driver->op->read_config_space) {
        return;
    }

    gpu->num_capsets =
        gpu->driver->op->read_config_space(gpu->driver->data, 12);
}

static int virtio_gpu_resource_create_2d(virtio_gpu_device_t *gpu,
                                         virtio_gpu_buffer_t *bo) {
    virtio_gpu_resource_create_2d_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    req.resource_id = htole32(bo->resource_id);
    req.format = htole32(bo->format);
    req.width = htole32(bo->width);
    req.height = htole32(bo->height);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int
virtio_gpu_resource_create_3d(virtio_gpu_device_t *gpu, virtio_gpu_buffer_t *bo,
                              const struct drm_virtgpu_resource_create *args) {
    virtio_gpu_resource_create_3d_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
    req.resource_id = htole32(bo->resource_id);
    req.target = htole32(args->target);
    req.format = htole32(args->format);
    req.bind = htole32(args->bind);
    req.width = htole32(args->width);
    req.height = htole32(args->height);
    req.depth = htole32(args->depth);
    req.array_size = htole32(args->array_size);
    req.last_level = htole32(args->last_level);
    req.nr_samples = htole32(args->nr_samples);
    req.flags = htole32(args->flags);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_resource_unref(virtio_gpu_device_t *gpu,
                                     uint32_t resource_id) {
    virtio_gpu_resource_unref_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    req.resource_id = htole32(resource_id);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_attach_backing(virtio_gpu_device_t *gpu,
                                     virtio_gpu_buffer_t *bo) {
    struct {
        virtio_gpu_resource_attach_backing_t attach;
        virtio_gpu_mem_entry_t entry;
    } req;

    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.attach.hdr,
                        VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    req.attach.resource_id = htole32(bo->resource_id);
    req.attach.nr_entries = htole32(1);
    req.entry.addr = htole64(bo->paddr);
    req.entry.length = htole32((uint32_t)bo->size);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_ctx_create(virtio_gpu_device_t *gpu, uint32_t ctx_id,
                                 uint32_t capset_id, const char *debug_name) {
    virtio_gpu_ctx_create_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init_ctx(&req.hdr, VIRTIO_GPU_CMD_CTX_CREATE, ctx_id);

    if ((gpu->negotiated_features & VIRTIO_GPU_F_CONTEXT_INIT) != 0) {
        req.context_init =
            htole32(capset_id & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK);
    }

    if (debug_name) {
        size_t len = strnlen(debug_name, sizeof(req.debug_name));
        memcpy(req.debug_name, debug_name, len);
        req.nlen = htole32((uint32_t)len);
    }

    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_ctx_destroy(virtio_gpu_device_t *gpu, uint32_t ctx_id) {
    virtio_gpu_ctx_destroy_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init_ctx(&req.hdr, VIRTIO_GPU_CMD_CTX_DESTROY, ctx_id);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_ctx_resource_cmd(virtio_gpu_device_t *gpu,
                                       uint32_t ctx_id, uint32_t resource_id,
                                       uint32_t type) {
    virtio_gpu_ctx_resource_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init_ctx(&req.hdr, type, ctx_id);
    req.resource_id = htole32(resource_id);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_ctx_attach_resource(virtio_gpu_device_t *gpu,
                                          uint32_t ctx_id,
                                          uint32_t resource_id) {
    return virtio_gpu_ctx_resource_cmd(gpu, ctx_id, resource_id,
                                       VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE);
}

static int virtio_gpu_ctx_detach_resource(virtio_gpu_device_t *gpu,
                                          uint32_t ctx_id,
                                          uint32_t resource_id) {
    return virtio_gpu_ctx_resource_cmd(gpu, ctx_id, resource_id,
                                       VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE);
}

static int virtio_gpu_detach_backing(virtio_gpu_device_t *gpu,
                                     uint32_t resource_id) {
    virtio_gpu_resource_detach_backing_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    req.resource_id = htole32(resource_id);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_set_scanout(virtio_gpu_device_t *gpu,
                                  virtio_gpu_buffer_t *bo, uint32_t x,
                                  uint32_t y, uint32_t width, uint32_t height) {
    virtio_gpu_set_scanout_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    req.rect.x = htole32(x);
    req.rect.y = htole32(y);
    req.rect.width = htole32(width);
    req.rect.height = htole32(height);
    req.scanout_id = htole32(gpu->scanout_id);
    req.resource_id = htole32(bo ? bo->resource_id : 0);
    int ret = virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
    if (ret == 0) {
        gpu->bound_scanout_id = gpu->scanout_id;
        gpu->scanout_resource_id = bo ? bo->resource_id : 0;
        gpu->scanout_resource_width = bo ? width : 0;
        gpu->scanout_resource_height = bo ? height : 0;
    }
    return ret;
}

static int virtio_gpu_transfer_to_host_2d(virtio_gpu_device_t *gpu,
                                          virtio_gpu_buffer_t *bo, uint32_t x,
                                          uint32_t y, uint32_t width,
                                          uint32_t height) {
    virtio_gpu_transfer_to_host_2d_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    req.rect.x = htole32(x);
    req.rect.y = htole32(y);
    req.rect.width = htole32(width);
    req.rect.height = htole32(height);
    req.offset = htole64((uint64_t)y * bo->pitch + (uint64_t)x * 4);
    req.resource_id = htole32(bo->resource_id);

    dma_sync_cpu_to_device((void *)phys_to_virt(bo->paddr), bo->size);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_flush(virtio_gpu_device_t *gpu, virtio_gpu_buffer_t *bo,
                            uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height) {
    virtio_gpu_resource_flush_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    req.rect.x = htole32(x);
    req.rect.y = htole32(y);
    req.rect.width = htole32(width);
    req.rect.height = htole32(height);
    req.resource_id = htole32(bo->resource_id);
    return virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
}

static int virtio_gpu_present(virtio_gpu_device_t *gpu, virtio_gpu_buffer_t *bo,
                              uint32_t x, uint32_t y, uint32_t width,
                              uint32_t height, bool set_scanout) {
    if (!gpu || !bo || !bo->used ||
        (bo->kind != VIRTIO_GPU_OBJECT_DUMB_2D &&
         bo->kind != VIRTIO_GPU_OBJECT_PRIVATE_3D)) {
        return -EINVAL;
    }

    if (x >= bo->width || y >= bo->height) {
        return 0;
    }

    if (width == 0) {
        width = bo->width;
    }
    if (height == 0) {
        height = bo->height;
    }

    width = MIN(width, bo->width - x);
    height = MIN(height, bo->height - y);
    if (width == 0 || height == 0) {
        return 0;
    }

    if (set_scanout && (gpu->bound_scanout_id != gpu->scanout_id ||
                        gpu->scanout_resource_id != bo->resource_id ||
                        gpu->scanout_resource_width != bo->width ||
                        gpu->scanout_resource_height != bo->height)) {
        int ret = virtio_gpu_set_scanout(gpu, bo, 0, 0, bo->width, bo->height);
        if (ret != 0) {
            return ret;
        }
        gpu->width = bo->width;
        gpu->height = bo->height;
        gpu->display_valid = true;
        virtio_gpu_update_modeset(gpu);
    }

    if (bo->kind == VIRTIO_GPU_OBJECT_DUMB_2D) {
        int ret = virtio_gpu_transfer_to_host_2d(gpu, bo, x, y, width, height);
        if (ret != 0) {
            return ret;
        }
    }

    return virtio_gpu_flush(gpu, bo, x, y, width, height);
}

static virtio_gpu_buffer_t *virtio_gpu_buffer_get(virtio_gpu_device_t *gpu,
                                                  uint32_t handle) {
    uint32_t idx = 0;
    if (!gpu || !virtio_gpu_handle_to_index(handle, &idx) ||
        !gpu->buffers[idx].used) {
        return NULL;
    }
    return &gpu->buffers[idx];
}

static virtio_gpu_buffer_t *virtio_gpu_alloc_buffer(virtio_gpu_device_t *gpu) {
    if (!gpu) {
        return NULL;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_DUMB_BUFFERS; i++) {
        if (!gpu->buffers[i].used) {
            memset(&gpu->buffers[i], 0, sizeof(gpu->buffers[i]));
            gpu->buffers[i].used = true;
            gpu->buffers[i].handle = i + 1;
            gpu->buffers[i].resource_id = virtio_gpu_alloc_resource_id(gpu);
            gpu->buffers[i].refcount = 1;
            return &gpu->buffers[i];
        }
    }
    return NULL;
}

static int virtio_gpu_buffer_alloc_backing(virtio_gpu_buffer_t *bo,
                                           uint64_t size) {
    if (!bo || size == 0) {
        return -EINVAL;
    }

    bo->size = PADDING_UP(size, PAGE_SIZE);
    bo->paddr = alloc_frames(bo->size / PAGE_SIZE);
    if (!bo->paddr) {
        return -ENOMEM;
    }
    memset((void *)phys_to_virt(bo->paddr), 0, bo->size);
    return 0;
}

static void virtio_gpu_buffer_release(virtio_gpu_device_t *gpu,
                                      virtio_gpu_buffer_t *bo) {
    if (!gpu || !bo || !bo->used) {
        return;
    }

    if (bo->resource_id) {
        virtio_gpu_detach_backing(gpu, bo->resource_id);
        virtio_gpu_resource_unref(gpu, bo->resource_id);
    }
    if (bo->paddr && bo->size) {
        free_frames(bo->paddr, bo->size / PAGE_SIZE);
    }
    memset(bo, 0, sizeof(*bo));
}

static void virtio_gpu_buffer_detach_all_contexts(virtio_gpu_device_t *gpu,
                                                  virtio_gpu_buffer_t *bo) {
    if (!gpu || !bo || !bo->used) {
        return;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        virtio_gpu_context_t *ctx = &gpu->contexts[i];
        if (ctx->used && (bo->attached_context_mask & (1ULL << i))) {
            virtio_gpu_ctx_detach_resource(gpu, ctx->id, bo->resource_id);
        }
    }
    bo->attached_context_mask = 0;
}

static void virtio_gpu_buffer_put(virtio_gpu_device_t *gpu,
                                  virtio_gpu_buffer_t *bo) {
    if (!gpu || !bo || !bo->used) {
        return;
    }

    if (--bo->refcount <= 0) {
        virtio_gpu_buffer_detach_all_contexts(gpu, bo);
        virtio_gpu_buffer_release(gpu, bo);
    }
}

static virtio_gpu_context_t *virtio_gpu_context_get(virtio_gpu_device_t *gpu,
                                                    uint32_t ctx_id) {
    if (!gpu || ctx_id == 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (gpu->contexts[i].used && gpu->contexts[i].id == ctx_id) {
            return &gpu->contexts[i];
        }
    }
    return NULL;
}

static int virtio_gpu_ensure_context(virtio_gpu_device_t *gpu,
                                     uint32_t capset_id, const char *debug_name,
                                     uint32_t *ctx_id_out,
                                     uint32_t *ctx_index_out) {
    if (!gpu || !ctx_id_out) {
        return -EINVAL;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        virtio_gpu_context_t *ctx = &gpu->contexts[i];
        if (ctx->used) {
            continue;
        }

        uint32_t ctx_id = virtio_gpu_alloc_context_id(gpu);
        int ret = virtio_gpu_ctx_create(gpu, ctx_id, capset_id, debug_name);
        if (ret != 0) {
            return ret;
        }

        ctx->used = true;
        ctx->id = ctx_id;
        ctx->capset_id = capset_id;
        if (debug_name) {
            strncpy(ctx->debug_name, debug_name, sizeof(ctx->debug_name));
        }
        *ctx_id_out = ctx_id;
        if (ctx_index_out) {
            *ctx_index_out = i;
        }
        return 0;
    }

    return -ENOSPC;
}

static int virtio_gpu_context_index(virtio_gpu_device_t *gpu, uint32_t ctx_id,
                                    uint32_t *index_out) {
    if (!gpu || ctx_id == 0 || !index_out) {
        return -EINVAL;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_CONTEXTS; i++) {
        if (gpu->contexts[i].used && gpu->contexts[i].id == ctx_id) {
            *index_out = i;
            return 0;
        }
    }

    return -ENOENT;
}

static virtio_gpu_file_t *virtio_gpu_file_from_fd(fd_t *fd) {
    drm_file_t *drm_file = (drm_file_t *)device_file_private(fd);
    if (!drm_file && fd)
        drm_file = (drm_file_t *)fd->private_data;
    if (!drm_file || drm_file->magic != DRM_FILE_MAGIC) {
        return NULL;
    }
    return (virtio_gpu_file_t *)drm_file->driver_priv;
}

static bool virtio_gpu_file_handle_bit(uint32_t handle, uint32_t *word,
                                       uint64_t *mask) {
    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(handle, &idx)) {
        return false;
    }

    if (word) {
        *word = idx / 64;
    }
    if (mask) {
        *mask = 1ULL << (idx % 64);
    }
    return true;
}

static bool virtio_gpu_file_has_handle(virtio_gpu_file_t *vf, uint32_t handle) {
    uint32_t word = 0;
    uint64_t mask = 0;
    if (!vf || !virtio_gpu_file_handle_bit(handle, &word, &mask)) {
        return false;
    }

    return (vf->handles[word] & mask) != 0;
}

static int virtio_gpu_file_track_handle(virtio_gpu_file_t *vf,
                                        uint32_t handle) {
    uint32_t word = 0;
    uint64_t mask = 0;
    if (!vf || !virtio_gpu_file_handle_bit(handle, &word, &mask)) {
        return -EINVAL;
    }

    vf->handles[word] |= mask;
    return 0;
}

static bool virtio_gpu_file_untrack_handle(virtio_gpu_file_t *vf,
                                           uint32_t handle) {
    uint32_t word = 0;
    uint64_t mask = 0;
    if (!vf || !virtio_gpu_file_handle_bit(handle, &word, &mask) ||
        !(vf->handles[word] & mask)) {
        return false;
    }

    vf->handles[word] &= ~mask;
    return true;
}

static int virtio_gpu_file_context(virtio_gpu_device_t *gpu, fd_t *fd,
                                   uint32_t capset_id, const char *debug_name,
                                   uint32_t *ctx_id, uint32_t *ctx_index) {
    virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
    if (!gpu || !vf || !ctx_id) {
        return -EINVAL;
    }

    if (vf->ctx_id != 0) {
        int ret = virtio_gpu_context_index(gpu, vf->ctx_id, ctx_index);
        if (ret != 0) {
            return ret;
        }
        *ctx_id = vf->ctx_id;
        return 0;
    }

    int ret = virtio_gpu_ensure_context(gpu, capset_id, debug_name, ctx_id,
                                        ctx_index);
    if (ret != 0) {
        return ret;
    }

    vf->ctx_id = *ctx_id;
    vf->capset_id = capset_id;
    return 0;
}

static uint32_t virtio_gpu_file_release_handles(virtio_gpu_device_t *gpu,
                                                virtio_gpu_file_t *vf) {
    uint32_t released = 0;
    if (!gpu || !vf) {
        return 0;
    }

    for (uint32_t word = 0; word < VIRTIO_GPU_FILE_HANDLE_WORDS; word++) {
        uint64_t bits = vf->handles[word];
        vf->handles[word] = 0;
        while (bits) {
            uint32_t bit = __builtin_ctzll(bits);
            uint32_t handle = word * 64 + bit + 1;
            bits &= ~(1ULL << bit);

            virtio_gpu_buffer_t *bo = virtio_gpu_buffer_get(gpu, handle);
            if (!bo || bo->kind == VIRTIO_GPU_OBJECT_DUMB_2D) {
                continue;
            }

            virtio_gpu_buffer_put(gpu, bo);
            released++;
        }
    }

    return released;
}

static int virtio_gpu_submit_3d(virtio_gpu_device_t *gpu, uint32_t ctx_id,
                                const void *cmd, uint32_t size) {
    virtio_gpu_cmd_submit_t req;
    virtio_gpu_ctrl_hdr_t resp;
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    virtio_gpu_hdr_init_ctx(&req.hdr, VIRTIO_GPU_CMD_SUBMIT_3D, ctx_id);
    uint64_t fence_id = virtio_gpu_alloc_fence_id(gpu);
    req.hdr.flags = htole32(VIRTIO_GPU_FLAG_FENCE);
    req.hdr.fence_id = htole64(fence_id);
    req.size = htole32(size);

    int ret = virtio_gpu_ctl_send(gpu, &req, sizeof(req), &resp, sizeof(resp),
                                  cmd, size);
    if (ret != 0) {
        return ret;
    }
    if (virtio_gpu_resp_type(&resp) != VIRTIO_GPU_RESP_OK_NODATA ||
        !(le32toh(resp.flags) & VIRTIO_GPU_FLAG_FENCE) ||
        le64toh(resp.fence_id) != fence_id) {
        return -EIO;
    }
    return 0;
}

static int virtio_gpu_transfer_host_3d(virtio_gpu_device_t *gpu,
                                       virtio_gpu_buffer_t *bo,
                                       const struct drm_virtgpu_3d_box *box,
                                       uint32_t level, uint32_t offset,
                                       uint32_t stride, uint32_t layer_stride,
                                       uint32_t type) {
    if (!gpu || !bo || !box || bo->resource_id == 0) {
        return -EINVAL;
    }

    virtio_gpu_transfer_host_3d_t req;
    memset(&req, 0, sizeof(req));
    virtio_gpu_hdr_init(&req.hdr, type);
    req.box.x = htole32(box->x);
    req.box.y = htole32(box->y);
    req.box.z = htole32(box->z);
    req.box.w = htole32(box->w);
    req.box.h = htole32(box->h);
    req.box.d = htole32(box->d);
    req.offset = htole64(offset);
    req.resource_id = htole32(bo->resource_id);
    req.level = htole32(level);
    req.stride = htole32(stride);
    req.layer_stride = htole32(layer_stride);

    if (type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D && bo->paddr && bo->size) {
        dma_sync_cpu_to_device((void *)phys_to_virt(bo->paddr), bo->size);
    }

    int ret = virtio_gpu_simple_cmd(gpu, &req, sizeof(req));
    if (ret == 0 && type == VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D && bo->paddr &&
        bo->size) {
        dma_sync_device_to_cpu((void *)phys_to_virt(bo->paddr), bo->size);
    }
    return ret;
}

static int virtio_gpu_get_capset_info(virtio_gpu_device_t *gpu,
                                      uint32_t capset_index,
                                      virtio_gpu_resp_capset_info_t *info) {
    virtio_gpu_get_capset_info_t req;
    memset(&req, 0, sizeof(req));
    memset(info, 0, sizeof(*info));
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    req.capset_index = htole32(capset_index);

    int ret = virtio_gpu_ctl_send(gpu, &req, sizeof(req), info, sizeof(*info),
                                  NULL, 0);
    if (ret != 0) {
        return ret;
    }
    return virtio_gpu_resp_type(&info->hdr) == VIRTIO_GPU_RESP_OK_CAPSET_INFO
               ? 0
               : -EIO;
}

static int virtio_gpu_get_capset(virtio_gpu_device_t *gpu, uint32_t capset_id,
                                 uint32_t capset_version, void *dst,
                                 uint32_t size) {
    if (!dst || size == 0) {
        return -EINVAL;
    }

    virtio_gpu_get_capset_t req;
    size_t resp_size = sizeof(virtio_gpu_ctrl_hdr_t) + size;
    void *resp = alloc_frames_bytes(resp_size);
    if (!resp) {
        return -ENOMEM;
    }

    memset(&req, 0, sizeof(req));
    memset(resp, 0, resp_size);
    virtio_gpu_hdr_init(&req.hdr, VIRTIO_GPU_CMD_GET_CAPSET);
    req.capset_id = htole32(capset_id);
    req.capset_version = htole32(capset_version);

    int ret =
        virtio_gpu_ctl_send(gpu, &req, sizeof(req), resp, resp_size, NULL, 0);
    if (ret == 0 && virtio_gpu_resp_type((virtio_gpu_ctrl_hdr_t *)resp) !=
                        VIRTIO_GPU_RESP_OK_CAPSET) {
        ret = -EIO;
    }
    if (ret == 0) {
        memcpy(dst, (uint8_t *)resp + sizeof(virtio_gpu_ctrl_hdr_t), size);
    }

    free_frames_bytes(resp, resp_size);
    return ret;
}

static void virtio_gpu_probe_capsets(virtio_gpu_device_t *gpu) {
    if (!gpu || gpu->num_capsets == 0) {
        return;
    }

    uint32_t capset_count = MIN(gpu->num_capsets, 32);
    for (uint32_t i = 0; i < capset_count; i++) {
        virtio_gpu_resp_capset_info_t info;
        if (virtio_gpu_get_capset_info(gpu, i, &info) != 0) {
            continue;
        }

        uint32_t capset_id = le32toh(info.capset_id);
        if (capset_id < 64) {
            gpu->supported_capset_ids |= 1ULL << capset_id;
        }
    }
}

static drm_encoder_t *
virtio_gpu_encoder_for_connector(virtio_gpu_device_t *gpu,
                                 drm_connector_t *connector) {
    if (!gpu || !connector || connector->encoder_id == 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
        if (gpu->encoders[i] && gpu->encoders[i]->id == connector->encoder_id) {
            return gpu->encoders[i];
        }
    }

    return NULL;
}

static void virtio_gpu_bind_connector_crtc(virtio_gpu_device_t *gpu,
                                           drm_connector_t *connector,
                                           uint32_t crtc_id) {
    if (!gpu || !connector) {
        return;
    }

    connector->crtc_id = crtc_id;

    drm_encoder_t *encoder = virtio_gpu_encoder_for_connector(gpu, connector);
    if (encoder) {
        encoder->crtc_id = crtc_id;
    }
}

static int virtio_gpu_drm_get_display_info(drm_device_t *drm_dev,
                                           uint32_t *width, uint32_t *height,
                                           uint32_t *bpp) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !width || !height || !bpp) {
        return -ENODEV;
    }

    *width = gpu->width;
    *height = gpu->height;
    *bpp = gpu->bpp;
    return 0;
}

static void virtio_gpu_display_worker(uint64_t arg) {
    virtio_gpu_device_t *gpu = (virtio_gpu_device_t *)arg;

    while (true) {
        wait_queue_entry_t wait;
        int64_t timeout_ns =
            gpu->control_irq_enabled ? -1 : VIRTIO_GPU_DISPLAY_FALLBACK_POLL_NS;

        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&gpu->display_event_wait, &wait);

        bool pending = __atomic_exchange_n(&gpu->display_event_pending, false,
                                           __ATOMIC_ACQ_REL);
        if (!pending) {
            (void)task_block(current_task, TASK_BLOCKING, timeout_ns,
                             "virtio_gpu_display");
        } else {
            task_cancel_block_prepare(current_task);
        }
        wait_queue_remove(&gpu->display_event_wait, &wait);
        task_cancel_block_prepare(current_task);

        bool changed = false;

        if (virtio_gpu_refresh_display_info(gpu, &changed) == 0 && changed &&
            gpu->drm_dev) {
            drm_notify_hotplug(gpu->drm_dev);
        }
    }
}

static int virtio_gpu_drm_open(drm_device_t *drm_dev, drm_file_t *file) {
    (void)drm_dev;
    if (!file) {
        return -EINVAL;
    }

    virtio_gpu_file_t *vf = calloc(1, sizeof(*vf));
    if (!vf) {
        return -ENOMEM;
    }

    vf->capset_id = VIRTGPU_DRM_CAPSET_VIRGL;
    file->driver_priv = vf;
    return 0;
}

static void virtio_gpu_drm_close(drm_device_t *drm_dev, drm_file_t *file) {
    virtio_gpu_device_t *gpu = drm_dev ? drm_dev->data : NULL;
    virtio_gpu_file_t *vf =
        file ? (virtio_gpu_file_t *)file->driver_priv : NULL;
    if (!gpu || !vf) {
        return;
    }

    if (vf->ctx_id != 0) {
        uint32_t ctx_index = 0;
        if (virtio_gpu_context_index(gpu, vf->ctx_id, &ctx_index) == 0) {
            for (uint32_t i = 0; i < VIRTIO_GPU_MAX_DUMB_BUFFERS; i++) {
                virtio_gpu_buffer_t *bo = &gpu->buffers[i];
                if (!bo->used ||
                    !(bo->attached_context_mask & (1ULL << ctx_index))) {
                    continue;
                }

                virtio_gpu_ctx_detach_resource(gpu, vf->ctx_id,
                                               bo->resource_id);
                bo->attached_context_mask &= ~(1ULL << ctx_index);
            }
            virtio_gpu_ctx_destroy(gpu, vf->ctx_id);
            memset(&gpu->contexts[ctx_index], 0,
                   sizeof(gpu->contexts[ctx_index]));
        }
    }

    virtio_gpu_file_release_handles(gpu, vf);
    file->driver_priv = NULL;
    free(vf);
}

static int virtio_gpu_drm_create_dumb(drm_device_t *drm_dev,
                                      struct drm_mode_create_dumb *args,
                                      fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !args || args->width == 0 || args->height == 0) {
        return -EINVAL;
    }

    if (args->bpp == 0) {
        args->bpp = 32;
    }
    if (args->bpp != 32) {
        return -EINVAL;
    }

    args->pitch = PADDING_UP(args->width * 4, 64);
    args->size = (uint64_t)args->pitch * args->height;
    if (args->size == 0 || args->size > UINT32_MAX) {
        return -EINVAL;
    }

    virtio_gpu_buffer_t *bo = virtio_gpu_alloc_buffer(gpu);
    if (!bo) {
        return -ENOSPC;
    }

    bo->kind = VIRTIO_GPU_OBJECT_DUMB_2D;
    bo->width = args->width;
    bo->height = args->height;
    bo->pitch = args->pitch;
    bo->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;

    int ret = virtio_gpu_buffer_alloc_backing(bo, args->size);
    if (ret == 0) {
        ret = virtio_gpu_resource_create_2d(gpu, bo);
    }
    if (ret == 0) {
        ret = virtio_gpu_attach_backing(gpu, bo);
    }
    if (ret != 0) {
        virtio_gpu_buffer_release(gpu, bo);
        return ret;
    }

    args->handle = bo->handle;
    return 0;
}

static int virtio_gpu_drm_destroy_dumb(drm_device_t *drm_dev, uint32_t handle,
                                       fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    uint32_t idx = 0;

    if (!gpu || !virtio_gpu_handle_to_index(handle, &idx) ||
        !gpu->buffers[idx].used) {
        return -EINVAL;
    }

    virtio_gpu_buffer_t *bo = &gpu->buffers[idx];
    if (bo->kind != VIRTIO_GPU_OBJECT_DUMB_2D) {
        return -EINVAL;
    }

    if (--bo->refcount > 0) {
        return 0;
    }

    virtio_gpu_buffer_release(gpu, bo);
    return 0;
}

static int virtio_gpu_drm_add_fb(drm_device_t *drm_dev,
                                 struct drm_mode_fb_cmd *fb_cmd, fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !fb_cmd || fb_cmd->handle == 0) {
        return -EINVAL;
    }

    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(fb_cmd->handle, &idx) ||
        !gpu->buffers[idx].used) {
        return -EINVAL;
    }

    drm_framebuffer_t *fb = drm_framebuffer_alloc(&drm_dev->resource_mgr, gpu);
    if (!fb) {
        return -ENOMEM;
    }

    fb->width = fb_cmd->width;
    fb->height = fb_cmd->height;
    fb->pitch = fb_cmd->pitch;
    fb->bpp = fb_cmd->bpp;
    fb->depth = fb_cmd->depth;
    fb->handle = fb_cmd->handle;
    fb->format = DRM_FORMAT_XRGB8888;
    fb_cmd->fb_id = fb->id;
    gpu->buffers[idx].refcount++;
    return 0;
}

static int virtio_gpu_drm_add_fb2(drm_device_t *drm_dev,
                                  struct drm_mode_fb_cmd2 *fb_cmd, fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !fb_cmd || fb_cmd->handles[0] == 0 || fb_cmd->width == 0 ||
        fb_cmd->height == 0) {
        return -EINVAL;
    }

    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(fb_cmd->handles[0], &idx) ||
        !gpu->buffers[idx].used) {
        return -EINVAL;
    }

    switch (fb_cmd->pixel_format) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        break;
    default:
        return -EINVAL;
    }

    drm_framebuffer_t *fb = drm_framebuffer_alloc(&drm_dev->resource_mgr, gpu);
    if (!fb) {
        return -ENOMEM;
    }

    fb->width = fb_cmd->width;
    fb->height = fb_cmd->height;
    fb->pitch =
        fb_cmd->pitches[0] ? fb_cmd->pitches[0] : gpu->buffers[idx].pitch;
    fb->bpp = 32;
    fb->depth = (fb_cmd->pixel_format == DRM_FORMAT_ARGB8888 ||
                 fb_cmd->pixel_format == DRM_FORMAT_ABGR8888)
                    ? 32
                    : 24;
    fb->handle = fb_cmd->handles[0];
    fb->format = fb_cmd->pixel_format;
    fb->modifier = fb_cmd->modifier[0];
    fb_cmd->fb_id = fb->id;
    gpu->buffers[idx].refcount++;
    return 0;
}

static void virtio_gpu_drm_release_fb(drm_device_t *drm_dev,
                                      drm_framebuffer_t *fb) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    uint32_t idx = 0;
    if (!gpu || !fb || !virtio_gpu_handle_to_index(fb->handle, &idx) ||
        !gpu->buffers[idx].used) {
        return;
    }

    virtio_gpu_buffer_put(gpu, &gpu->buffers[idx]);
}

static int virtio_gpu_drm_dirty_fb(drm_device_t *drm_dev,
                                   struct drm_mode_fb_dirty_cmd *cmd,
                                   fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !cmd || (cmd->flags & ~DRM_MODE_FB_DIRTY_FLAGS)) {
        return -EINVAL;
    }

    drm_framebuffer_t *fb =
        drm_framebuffer_get(&drm_dev->resource_mgr, cmd->fb_id);
    if (!fb) {
        return -ENOENT;
    }

    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(fb->handle, &idx) ||
        !gpu->buffers[idx].used) {
        drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
        return -EINVAL;
    }

    int ret = 0;
    if (cmd->num_clips == 0 || cmd->clips_ptr == 0) {
        ret = virtio_gpu_present(gpu, &gpu->buffers[idx], 0, 0, 0, 0, false);
    } else {
        uint32_t clips_count = MIN(cmd->num_clips, DRM_MODE_FB_DIRTY_MAX_CLIPS);
        drm_clip_rect_t *clips = (drm_clip_rect_t *)(uintptr_t)cmd->clips_ptr;
        for (uint32_t i = 0; i < clips_count; i++) {
            uint32_t w =
                clips[i].x2 > clips[i].x1 ? clips[i].x2 - clips[i].x1 : 0;
            uint32_t h =
                clips[i].y2 > clips[i].y1 ? clips[i].y2 - clips[i].y1 : 0;
            if (w == 0 || h == 0) {
                continue;
            }
            ret = virtio_gpu_present(gpu, &gpu->buffers[idx], clips[i].x1,
                                     clips[i].y1, w, h, false);
            if (ret != 0) {
                break;
            }
        }
    }

    drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
    return ret;
}

static int virtio_gpu_drm_map_dumb(drm_device_t *drm_dev,
                                   struct drm_mode_map_dumb *args, fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    uint32_t idx = 0;

    if (!gpu || !args || !virtio_gpu_handle_to_index(args->handle, &idx) ||
        !gpu->buffers[idx].used) {
        return -EINVAL;
    }

    args->offset = DRM_DUMB_MMAP_OFFSET_BASE +
                   (uint64_t)args->handle * DRM_DUMB_MMAP_OFFSET_STRIDE;
    return 0;
}

static int virtio_gpu_drm_get_dumb_map(drm_device_t *drm_dev, uint32_t handle,
                                       uint64_t *phys, uint64_t *size) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    uint32_t idx = 0;

    if (!gpu || !phys || !size || !virtio_gpu_handle_to_index(handle, &idx) ||
        !gpu->buffers[idx].used) {
        return -EINVAL;
    }

    *phys = gpu->buffers[idx].paddr;
    *size = gpu->buffers[idx].size;
    return 0;
}

static int virtio_gpu_drm_set_crtc(drm_device_t *drm_dev,
                                   struct drm_mode_crtc *crtc, fd_t *fd) {
    (void)fd;
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !crtc || crtc->fb_id == 0) {
        return 0;
    }

    drm_framebuffer_t *fb =
        drm_framebuffer_get(&drm_dev->resource_mgr, crtc->fb_id);
    if (!fb) {
        return -ENOENT;
    }

    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(fb->handle, &idx) ||
        !gpu->buffers[idx].used) {
        drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
        return -EINVAL;
    }

    int ret = virtio_gpu_present(gpu, &gpu->buffers[idx], 0, 0, 0, 0, true);
    drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
    return ret;
}

static int virtio_gpu_drm_page_flip(drm_device_t *drm_dev,
                                    struct drm_mode_crtc_page_flip *flip,
                                    fd_t *fd) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !flip) {
        return -ENODEV;
    }

    drm_crtc_t *crtc = drm_crtc_get(&gpu->resource_mgr, flip->crtc_id);
    if (!crtc) {
        return -EINVAL;
    }
    uint32_t old_fb_id = crtc->fb_id;
    crtc->fb_id = flip->fb_id;
    drm_crtc_free(&gpu->resource_mgr, crtc->id);

    drm_framebuffer_t *fb =
        drm_framebuffer_get(&drm_dev->resource_mgr, flip->fb_id);
    if (!fb) {
        return -EINVAL;
    }

    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(fb->handle, &idx) ||
        !gpu->buffers[idx].used) {
        drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
        return -EINVAL;
    }

    int ret = virtio_gpu_present(gpu, &gpu->buffers[idx], 0, 0, 0, 0, true);
    drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
    if (ret != 0) {
        return ret;
    }

    if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        ret = drm_defer_event(drm_dev, fd, DRM_EVENT_FLIP_COMPLETE,
                              flip->user_data);
        if (ret < 0) {
            return ret;
        }
    }

    if (old_fb_id != 0 && old_fb_id != flip->fb_id) {
        drm_framebuffer_cleanup_closed(drm_dev, old_fb_id);
    }

    return 0;
}

static int virtio_gpu_drm_atomic_commit(drm_device_t *drm_dev,
                                        struct drm_mode_atomic *atomic,
                                        fd_t *fd) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !atomic) {
        return -ENODEV;
    }
    if (atomic->flags & ~DRM_MODE_ATOMIC_FLAGS) {
        return -EINVAL;
    }
    if (atomic->count_objs == 0) {
        return 0;
    }

    uint32_t *obj_ids = (uint32_t *)(uintptr_t)atomic->objs_ptr;
    uint32_t *obj_prop_counts = (uint32_t *)(uintptr_t)atomic->count_props_ptr;
    uint32_t *prop_ids = (uint32_t *)(uintptr_t)atomic->props_ptr;
    uint64_t *prop_values = (uint64_t *)(uintptr_t)atomic->prop_values_ptr;
    if (!obj_ids || !obj_prop_counts || !prop_ids || !prop_values) {
        return -EINVAL;
    }

    bool test_only = (atomic->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0;
    uint64_t prop_idx = 0;
    uint32_t committed_fb_id = 0;
    bool has_committed_fb = false;
    uint32_t stale_fb_ids[DRM_MAX_PLANES_PER_DEVICE] = {0};
    uint32_t stale_fb_count = 0;
    int err = 0;

    for (uint32_t i = 0; i < atomic->count_objs; i++) {
        uint32_t obj_id = obj_ids[i];
        uint32_t count = obj_prop_counts[i];

        enum {
            ATOMIC_OBJ_UNKNOWN = 0,
            ATOMIC_OBJ_PLANE,
            ATOMIC_OBJ_CRTC,
            ATOMIC_OBJ_CONNECTOR,
        } obj_type = ATOMIC_OBJ_UNKNOWN;

        for (uint32_t j = 0; j < count; j++) {
            switch (prop_ids[prop_idx + j]) {
            case DRM_PROPERTY_ID_PLANE_TYPE:
            case DRM_PROPERTY_ID_FB_ID:
            case DRM_PROPERTY_ID_CRTC_X:
            case DRM_PROPERTY_ID_CRTC_Y:
            case DRM_PROPERTY_ID_CRTC_W:
            case DRM_PROPERTY_ID_CRTC_H:
            case DRM_PROPERTY_ID_CRTC_ID:
                obj_type = ATOMIC_OBJ_PLANE;
                break;
            case DRM_CRTC_ACTIVE_PROP_ID:
            case DRM_CRTC_MODE_ID_PROP_ID:
                obj_type = ATOMIC_OBJ_CRTC;
                break;
            case DRM_CONNECTOR_DPMS_PROP_ID:
            case DRM_CONNECTOR_CRTC_ID_PROP_ID:
                obj_type = ATOMIC_OBJ_CONNECTOR;
                break;
            default:
                break;
            }

            if (obj_type != ATOMIC_OBJ_UNKNOWN) {
                break;
            }
        }

        drm_plane_t *plane = NULL;
        drm_crtc_t *crtc = NULL;
        drm_connector_t *connector = NULL;

        if (obj_type == ATOMIC_OBJ_PLANE) {
            plane = drm_plane_get(&gpu->resource_mgr, obj_id);
            if (!plane) {
                return -ENOENT;
            }
        } else if (obj_type == ATOMIC_OBJ_CRTC) {
            crtc = drm_crtc_get(&gpu->resource_mgr, obj_id);
            if (!crtc) {
                return -ENOENT;
            }
        } else if (obj_type == ATOMIC_OBJ_CONNECTOR) {
            connector = drm_connector_get(&gpu->resource_mgr, obj_id);
            if (!connector) {
                return -ENOENT;
            }
        }

        for (uint32_t j = 0; j < count; j++, prop_idx++) {
            uint32_t prop_id = prop_ids[prop_idx];
            uint64_t value = prop_values[prop_idx];

            switch (prop_id) {
            case DRM_PROPERTY_ID_PLANE_TYPE:
                if (!plane) {
                    err = -EINVAL;
                    goto out_obj;
                }
                if (value != plane->plane_type) {
                    err = -EINVAL;
                    goto out_obj;
                }
                break;
            case DRM_PROPERTY_ID_FB_ID:
                if (plane) {
                    if (value != 0) {
                        drm_framebuffer_t *fb = drm_framebuffer_get(
                            &drm_dev->resource_mgr, (uint32_t)value);
                        if (!fb) {
                            err = -ENOENT;
                            goto out_obj;
                        }
                        uint32_t fb_idx = 0;
                        bool valid =
                            virtio_gpu_handle_to_index(fb->handle, &fb_idx) &&
                            gpu->buffers[fb_idx].used;
                        drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
                        if (!valid) {
                            err = -EINVAL;
                            goto out_obj;
                        }
                    }
                    if (!test_only) {
                        if (plane->fb_id != 0 &&
                            plane->fb_id != (uint32_t)value &&
                            stale_fb_count < DRM_MAX_PLANES_PER_DEVICE) {
                            stale_fb_ids[stale_fb_count++] = plane->fb_id;
                        }
                        plane->fb_id = (uint32_t)value;
                    }
                    committed_fb_id = (uint32_t)value;
                    has_committed_fb = (value != 0);
                }
                break;
            case DRM_PROPERTY_ID_CRTC_ID:
                if (plane && !test_only) {
                    plane->crtc_id = (uint32_t)value;
                }
                break;
            case DRM_PROPERTY_ID_CRTC_X:
            case DRM_PROPERTY_ID_CRTC_Y:
            case DRM_PROPERTY_ID_CRTC_W:
            case DRM_PROPERTY_ID_CRTC_H:
                if (plane) {
                    uint32_t target_crtc_id = plane->crtc_id;
                    if (target_crtc_id) {
                        drm_crtc_t *target_crtc =
                            drm_crtc_get(&gpu->resource_mgr, target_crtc_id);
                        if (!target_crtc) {
                            err = -ENOENT;
                            goto out_obj;
                        }

                        if (!test_only) {
                            if (prop_id == DRM_PROPERTY_ID_CRTC_X) {
                                target_crtc->x = (uint32_t)value;
                            } else if (prop_id == DRM_PROPERTY_ID_CRTC_Y) {
                                target_crtc->y = (uint32_t)value;
                            } else if (prop_id == DRM_PROPERTY_ID_CRTC_W) {
                                target_crtc->w = (uint32_t)value;
                            } else {
                                target_crtc->h = (uint32_t)value;
                            }
                        }

                        drm_crtc_free(&gpu->resource_mgr, target_crtc->id);
                    }
                }
                break;
            case DRM_CRTC_ACTIVE_PROP_ID:
                if (crtc && !test_only) {
                    crtc->mode_valid = (value != 0);
                }
                break;
            case DRM_CRTC_MODE_ID_PROP_ID:
                if (crtc && !test_only) {
                    if (value == 0) {
                        crtc->mode_valid = 0;
                        memset(&crtc->mode, 0, sizeof(crtc->mode));
                    } else {
                        struct drm_mode_modeinfo mode;
                        memset(&mode, 0, sizeof(mode));
                        int ret = drm_property_get_modeinfo_from_blob(
                            drm_dev, (uint32_t)value, &mode);
                        if (ret != 0 || mode.hdisplay == 0 ||
                            mode.vdisplay == 0) {
                            err = ret != 0 ? ret : -EINVAL;
                            goto out_obj;
                        }
                        crtc->mode = mode;
                        crtc->mode_valid = 1;
                        crtc->w = mode.hdisplay;
                        crtc->h = mode.vdisplay;
                    }
                }
                break;
            case DRM_CONNECTOR_DPMS_PROP_ID:
                if (value > DRM_MODE_DPMS_OFF) {
                    err = -EINVAL;
                    goto out_obj;
                }
                break;
            case DRM_CONNECTOR_CRTC_ID_PROP_ID:
                if (connector && !test_only) {
                    virtio_gpu_bind_connector_crtc(gpu, connector,
                                                   (uint32_t)value);
                }
                break;
            default:
                break;
            }
        }

    out_obj:
        if (plane) {
            drm_plane_free(&gpu->resource_mgr, plane->id);
        }
        if (crtc) {
            drm_crtc_free(&gpu->resource_mgr, crtc->id);
        }
        if (connector) {
            drm_connector_free(&gpu->resource_mgr, connector->id);
        }
        if (err) {
            return err;
        }
    }

    if (test_only || !has_committed_fb) {
        return 0;
    }

    drm_framebuffer_t *fb =
        drm_framebuffer_get(&drm_dev->resource_mgr, committed_fb_id);
    if (!fb) {
        return -ENOENT;
    }
    uint32_t idx = 0;
    if (!virtio_gpu_handle_to_index(fb->handle, &idx) ||
        !gpu->buffers[idx].used) {
        drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
        return -EINVAL;
    }

    int ret = virtio_gpu_present(gpu, &gpu->buffers[idx], 0, 0, 0, 0, true);
    drm_framebuffer_free(&drm_dev->resource_mgr, fb->id);
    if (ret != 0) {
        return ret;
    }

    if (atomic->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        ret = drm_defer_event(drm_dev, fd, DRM_EVENT_FLIP_COMPLETE,
                              atomic->user_data);
        if (ret < 0) {
            return ret;
        }
    }

    for (uint32_t i = 0; i < stale_fb_count; i++) {
        drm_framebuffer_cleanup_closed(drm_dev, stale_fb_ids[i]);
    }

    return 0;
}

static int virtio_gpu_drm_get_connectors(drm_device_t *drm_dev,
                                         drm_connector_t **connectors,
                                         uint32_t *count) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !count) {
        return -ENODEV;
    }

    *count = 0;
    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
        if (gpu->connectors[i]) {
            connectors[(*count)++] = gpu->connectors[i];
        }
    }
    return 0;
}

static int virtio_gpu_drm_get_crtcs(drm_device_t *drm_dev, drm_crtc_t **crtcs,
                                    uint32_t *count) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !count) {
        return -ENODEV;
    }

    *count = 0;
    for (uint32_t i = 0; i < DRM_MAX_CRTCS_PER_DEVICE; i++) {
        if (gpu->crtcs[i]) {
            crtcs[(*count)++] = gpu->crtcs[i];
        }
    }
    return 0;
}

static int virtio_gpu_drm_get_encoders(drm_device_t *drm_dev,
                                       drm_encoder_t **encoders,
                                       uint32_t *count) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !count) {
        return -ENODEV;
    }

    *count = 0;
    for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
        if (gpu->encoders[i]) {
            encoders[(*count)++] = gpu->encoders[i];
        }
    }
    return 0;
}

static int virtio_gpu_drm_get_planes(drm_device_t *drm_dev,
                                     drm_plane_t **planes, uint32_t *count) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu || !count) {
        return -ENODEV;
    }

    *count = 1;
    planes[0] = drm_plane_alloc(&gpu->resource_mgr, gpu);
    if (!planes[0]) {
        *count = 0;
        return -ENOMEM;
    }

    planes[0]->crtc_id = gpu->crtcs[0] ? gpu->crtcs[0]->id : 0;
    planes[0]->fb_id = gpu->crtcs[0] ? gpu->crtcs[0]->fb_id : 0;
    planes[0]->possible_crtcs = 1;
    planes[0]->count_format_types = 4;
    planes[0]->format_types =
        malloc(sizeof(uint32_t) * planes[0]->count_format_types);
    if (planes[0]->format_types) {
        planes[0]->format_types[0] = DRM_FORMAT_XRGB8888;
        planes[0]->format_types[1] = DRM_FORMAT_ARGB8888;
        planes[0]->format_types[2] = DRM_FORMAT_XBGR8888;
        planes[0]->format_types[3] = DRM_FORMAT_ABGR8888;
    }
    planes[0]->plane_type = DRM_PLANE_TYPE_PRIMARY;
    return 0;
}

static ssize_t virtio_gpu_drm_driver_ioctl(drm_device_t *drm_dev, uint32_t cmd,
                                           void *arg, bool render_node,
                                           fd_t *fd) {
    virtio_gpu_device_t *gpu = drm_dev->data;
    if (!gpu) {
        return -ENODEV;
    }

    switch (cmd) {
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
        struct drm_prime_handle *prime = arg;
        virtio_gpu_buffer_t *bo =
            prime ? virtio_gpu_buffer_get(gpu, prime->handle) : NULL;
        if (!bo) {
            return -ENOENT;
        }
        virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
        if (!vf || !virtio_gpu_file_has_handle(vf, prime->handle)) {
            return -ENOENT;
        }
        bo->refcount++;
        return -ENOTTY;
    }
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
        struct drm_prime_handle *prime = arg;
        virtio_gpu_buffer_t *bo =
            prime ? virtio_gpu_buffer_get(gpu, prime->handle) : NULL;
        if (!bo) {
            return -ENOENT;
        }
        virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
        if (!vf) {
            return -EINVAL;
        }
        if (!virtio_gpu_file_has_handle(vf, prime->handle)) {
            int ret = virtio_gpu_file_track_handle(vf, prime->handle);
            if (ret != 0) {
                return ret;
            }
            bo->refcount++;
        }
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam *param = arg;
        uint64_t value = 0;
        if (!param) {
            return -EINVAL;
        }

        switch (param->param) {
        case VIRTGPU_PARAM_3D_FEATURES:
            value = (gpu->negotiated_features & VIRTIO_GPU_F_VIRGL) != 0;
            break;
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
            value = 1;
            break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:
            value = 0;
            break;
        case VIRTGPU_PARAM_HOST_VISIBLE:
        case VIRTGPU_PARAM_CROSS_DEVICE:
            value = 0;
            break;
        case VIRTGPU_PARAM_CONTEXT_INIT:
            value = (gpu->negotiated_features & VIRTIO_GPU_F_CONTEXT_INIT) != 0;
            break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
            value = gpu->supported_capset_ids;
            break;
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
            value = 1;
            break;
        default:
            return -EINVAL;
        }

        if (copy_to_user((void *)(uintptr_t)param->value, &value,
                         sizeof(value))) {
            return -EFAULT;
        }
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map *map = arg;
        uint32_t idx = 0;
        if (!map || !virtio_gpu_handle_to_index(map->handle, &idx) ||
            !gpu->buffers[idx].used) {
            return -EINVAL;
        }
        map->offset = DRM_DUMB_MMAP_OFFSET_BASE +
                      (uint64_t)map->handle * DRM_DUMB_MMAP_OFFSET_STRIDE;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info *info = arg;
        uint32_t idx = 0;
        if (!info || !virtio_gpu_handle_to_index(info->bo_handle, &idx) ||
            !gpu->buffers[idx].used) {
            return -EINVAL;
        }
        info->res_handle = gpu->buffers[idx].resource_id;
        info->size = (uint32_t)gpu->buffers[idx].size;
        info->blob_mem = 0;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: {
        struct drm_virtgpu_resource_create *req = arg;
        if (!req || req->width == 0 || req->height == 0 || req->size == 0) {
            return -EINVAL;
        }
        if ((gpu->negotiated_features & VIRTIO_GPU_F_VIRGL) == 0) {
            return -ENODEV;
        }

        virtio_gpu_buffer_t *bo = virtio_gpu_alloc_buffer(gpu);
        if (!bo) {
            printk("virtio_gpu: no free resource slots for 3D buffer\n");
            return -ENOSPC;
        }

        bo->kind = VIRTIO_GPU_OBJECT_PRIVATE_3D;
        bo->width = req->width;
        bo->height = req->height;
        bo->pitch = req->stride;
        bo->format = req->format;

        int ret = virtio_gpu_buffer_alloc_backing(bo, req->size);
        if (ret == -ENOMEM) {
            printk("virtio_gpu: failed to allocate %u bytes for 3D buffer\n",
                   req->size);
        }
        if (ret == 0) {
            ret = virtio_gpu_resource_create_3d(gpu, bo, req);
        }
        if (ret == 0) {
            ret = virtio_gpu_attach_backing(gpu, bo);
        }
        if (ret != 0) {
            virtio_gpu_buffer_release(gpu, bo);
            return ret;
        }

        virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
        if (!vf) {
            virtio_gpu_buffer_release(gpu, bo);
            return -EINVAL;
        }
        ret = virtio_gpu_file_track_handle(vf, bo->handle);
        if (ret != 0) {
            virtio_gpu_buffer_release(gpu, bo);
            return ret;
        }

        req->bo_handle = bo->handle;
        req->res_handle = bo->resource_id;
        req->size = (uint32_t)bo->size;
        req->stride = bo->pitch;

        return 0;
    }
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
        struct drm_virtgpu_execbuffer *exec = arg;
        if (!exec || exec->size == 0 || exec->command == 0 ||
            (exec->flags & ~VIRTGPU_EXECBUF_FLAGS)) {
            return -EINVAL;
        }
        if (exec->flags & VIRTGPU_EXECBUF_RING_IDX) {
            return -EINVAL;
        }
        if (exec->num_in_syncobjs || exec->num_out_syncobjs ||
            exec->in_syncobjs || exec->out_syncobjs) {
            return -ENOSYS;
        }
        if (exec->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
            return -EINVAL;
        }

        uint32_t capset_id = VIRTGPU_DRM_CAPSET_VIRGL;
        virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
        if (vf && vf->capset_id != 0) {
            capset_id = vf->capset_id;
        } else if ((gpu->negotiated_features & VIRTIO_GPU_F_CONTEXT_INIT) &&
                   (gpu->supported_capset_ids &
                    (1ULL << VIRTGPU_DRM_CAPSET_VIRGL2))) {
            capset_id = VIRTGPU_DRM_CAPSET_VIRGL2;
        }

        uint32_t ctx_id = 0;
        uint32_t ctx_index = 0;
        int ret = virtio_gpu_file_context(gpu, fd, capset_id, "naos-virgl",
                                          &ctx_id, &ctx_index);
        if (ret != 0) {
            return ret;
        }

        uint32_t *handles = NULL;
        if (exec->num_bo_handles != 0) {
            if (!exec->bo_handles ||
                exec->num_bo_handles > VIRTIO_GPU_MAX_DUMB_BUFFERS) {
                return -EINVAL;
            }
            handles = malloc(exec->num_bo_handles * sizeof(uint32_t));
            if (!handles) {
                return -ENOMEM;
            }
            if (copy_from_user(handles, (void *)(uintptr_t)exec->bo_handles,
                               exec->num_bo_handles * sizeof(uint32_t))) {
                free(handles);
                return -EFAULT;
            }
            for (uint32_t i = 0; i < exec->num_bo_handles; i++) {
                virtio_gpu_buffer_t *bo =
                    virtio_gpu_buffer_get(gpu, handles[i]);
                if (!bo) {
                    free(handles);
                    return -ENOENT;
                }
                if (!(bo->attached_context_mask & (1ULL << ctx_index))) {
                    ret = virtio_gpu_ctx_attach_resource(gpu, ctx_id,
                                                         bo->resource_id);
                    if (ret != 0) {
                        free(handles);
                        return ret;
                    }
                    bo->attached_context_mask |= (1ULL << ctx_index);
                }
            }
        }

        void *cmd_buf = malloc(exec->size);
        if (!cmd_buf) {
            free(handles);
            return -ENOMEM;
        }
        if (copy_from_user(cmd_buf, (void *)(uintptr_t)exec->command,
                           exec->size)) {
            free(cmd_buf);
            free(handles);
            return -EFAULT;
        }

        ret = virtio_gpu_submit_3d(gpu, ctx_id, cmd_buf, exec->size);
        free(cmd_buf);
        free(handles);

        if (ret != 0) {
            return ret;
        }
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: {
        struct drm_virtgpu_3d_transfer_from_host *req = arg;
        virtio_gpu_buffer_t *bo =
            req ? virtio_gpu_buffer_get(gpu, req->bo_handle) : NULL;
        if (!bo) {
            return -ENOENT;
        }
        return virtio_gpu_transfer_host_3d(
            gpu, bo, &req->box, req->level, req->offset, req->stride,
            req->layer_stride, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST: {
        struct drm_virtgpu_3d_transfer_to_host *req = arg;
        virtio_gpu_buffer_t *bo =
            req ? virtio_gpu_buffer_get(gpu, req->bo_handle) : NULL;
        if (!bo) {
            return -ENOENT;
        }
        return virtio_gpu_transfer_host_3d(
            gpu, bo, &req->box, req->level, req->offset, req->stride,
            req->layer_stride, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
    }
    case DRM_IOCTL_VIRTGPU_WAIT: {
        struct drm_virtgpu_3d_wait *wait = arg;
        if (!wait || (wait->flags & ~VIRTGPU_WAIT_NOWAIT)) {
            return -EINVAL;
        }
        virtio_gpu_buffer_t *bo = virtio_gpu_buffer_get(gpu, wait->handle);
        virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
        if (!bo || !vf || !virtio_gpu_file_has_handle(vf, wait->handle)) {
            return -ENOENT;
        }
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_GET_CAPS: {
        struct drm_virtgpu_get_caps *caps = arg;
        if (!caps || caps->addr == 0 || caps->size == 0) {
            return -EINVAL;
        }
        if (caps->cap_set_id != VIRTGPU_DRM_CAPSET_VIRGL &&
            caps->cap_set_id != VIRTGPU_DRM_CAPSET_VIRGL2) {
            return -EINVAL;
        }

        virtio_gpu_resp_capset_info_t cap_info;
        bool found = false;
        uint32_t capset_count = gpu->num_capsets ? gpu->num_capsets : 8;
        for (uint32_t i = 0; i < capset_count; i++) {
            int info_ret = virtio_gpu_get_capset_info(gpu, i, &cap_info);
            if (info_ret != 0) {
                continue;
            }
            if (le32toh(cap_info.capset_id) == caps->cap_set_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return -EINVAL;
        }
        uint32_t max_version = le32toh(cap_info.capset_max_version);
        uint32_t max_size = le32toh(cap_info.capset_max_size);
        if (caps->cap_set_ver == 0) {
            caps->cap_set_ver = max_version;
        }
        if (caps->cap_set_ver > max_version || caps->size > max_size) {
            return -EINVAL;
        }

        void *cap_data = malloc(caps->size);
        if (!cap_data) {
            return -ENOMEM;
        }
        int ret = virtio_gpu_get_capset(
            gpu, caps->cap_set_id, caps->cap_set_ver, cap_data, caps->size);
        if (ret == 0 &&
            copy_to_user((void *)(uintptr_t)caps->addr, cap_data, caps->size)) {
            ret = -EFAULT;
        }
        free(cap_data);
        return ret;
    }
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: {
        struct drm_virtgpu_context_init *init = arg;
        uint32_t capset_id = VIRTGPU_DRM_CAPSET_VIRGL;
        if (!init) {
            return -EINVAL;
        }
        if (init->num_params > 16) {
            return -EINVAL;
        }
        if (init->num_params && !init->ctx_set_params) {
            return -EINVAL;
        }

        for (uint32_t i = 0; i < init->num_params; i++) {
            struct drm_virtgpu_context_set_param param;
            if (copy_from_user(&param,
                               (void *)(uintptr_t)(init->ctx_set_params +
                                                   i * sizeof(param)),
                               sizeof(param))) {
                return -EFAULT;
            }

            switch (param.param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
                capset_id = (uint32_t)param.value;
                break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
            case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
                if (param.value > 1) {
                    return -EINVAL;
                }
                break;
            case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
                break;
            default:
                return -EINVAL;
            }
        }

        if (capset_id >= 64 ||
            !(gpu->supported_capset_ids & (1ULL << capset_id))) {
            return -EINVAL;
        }

        uint32_t ctx_id = 0;
        uint32_t ctx_index = 0;
        return virtio_gpu_file_context(gpu, fd, capset_id, "naos-virgl",
                                       &ctx_id, &ctx_index);
    }
    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close *close = arg;
        virtio_gpu_buffer_t *bo =
            close ? virtio_gpu_buffer_get(gpu, close->handle) : NULL;
        if (!bo) {
            return -ENOENT;
        }
        if (bo->kind == VIRTIO_GPU_OBJECT_DUMB_2D) {
            return 0;
        }
        virtio_gpu_file_t *vf = virtio_gpu_file_from_fd(fd);
        if (vf) {
            if (!virtio_gpu_file_untrack_handle(vf, close->handle)) {
                return -ENOENT;
            }
            virtio_gpu_buffer_put(gpu, bo);
            return 0;
        }
        virtio_gpu_buffer_put(gpu, bo);
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB:
        return -ENOSYS;
    default:
        return -ENOTTY;
    }
}

static drm_device_op_t virtio_gpu_drm_device_op = {
    .supports_render_node = true,
    .open = virtio_gpu_drm_open,
    .close = virtio_gpu_drm_close,
    .get_display_info = virtio_gpu_drm_get_display_info,
    .create_dumb = virtio_gpu_drm_create_dumb,
    .destroy_dumb = virtio_gpu_drm_destroy_dumb,
    .dirty_fb = virtio_gpu_drm_dirty_fb,
    .add_fb = virtio_gpu_drm_add_fb,
    .add_fb2 = virtio_gpu_drm_add_fb2,
    .release_fb = virtio_gpu_drm_release_fb,
    .atomic_commit = virtio_gpu_drm_atomic_commit,
    .map_dumb = virtio_gpu_drm_map_dumb,
    .set_crtc = virtio_gpu_drm_set_crtc,
    .page_flip = virtio_gpu_drm_page_flip,
    .get_connectors = virtio_gpu_drm_get_connectors,
    .get_crtcs = virtio_gpu_drm_get_crtcs,
    .get_encoders = virtio_gpu_drm_get_encoders,
    .get_planes = virtio_gpu_drm_get_planes,
    .get_dumb_map = virtio_gpu_drm_get_dumb_map,
    .driver_ioctl = virtio_gpu_drm_driver_ioctl,
};

static int virtio_gpu_setup_modeset(virtio_gpu_device_t *gpu) {
    gpu->connectors[0] = drm_connector_alloc(&gpu->resource_mgr,
                                             DRM_MODE_CONNECTOR_VIRTUAL, gpu);
    if (!gpu->connectors[0]) {
        return -ENOMEM;
    }

    gpu->connectors[0]->connection = DRM_MODE_CONNECTED;
    gpu->connectors[0]->mm_width = (gpu->width * 264UL) / 1000UL;
    gpu->connectors[0]->mm_height = (gpu->height * 264UL) / 1000UL;
    if (gpu->connectors[0]->mm_width == 0) {
        gpu->connectors[0]->mm_width = 1;
    }
    if (gpu->connectors[0]->mm_height == 0) {
        gpu->connectors[0]->mm_height = 1;
    }

    gpu->connectors[0]->modes = malloc(sizeof(struct drm_mode_modeinfo));
    if (!gpu->connectors[0]->modes) {
        return -ENOMEM;
    }

    struct drm_mode_modeinfo mode;
    virtio_gpu_build_mode(gpu, &mode);
    memcpy(gpu->connectors[0]->modes, &mode, sizeof(mode));
    gpu->connectors[0]->count_modes = 1;

    gpu->crtcs[0] = drm_crtc_alloc(&gpu->resource_mgr, gpu);
    if (!gpu->crtcs[0]) {
        return -ENOMEM;
    }
    gpu->crtcs[0]->x = 0;
    gpu->crtcs[0]->y = 0;
    gpu->crtcs[0]->w = gpu->width;
    gpu->crtcs[0]->h = gpu->height;
    gpu->crtcs[0]->mode = mode;
    gpu->crtcs[0]->mode_valid = 1;

    gpu->encoders[0] =
        drm_encoder_alloc(&gpu->resource_mgr, DRM_MODE_ENCODER_VIRTUAL, gpu);
    if (!gpu->encoders[0]) {
        return -ENOMEM;
    }
    gpu->encoders[0]->possible_crtcs = 1;
    gpu->encoders[0]->crtc_id = gpu->crtcs[0]->id;
    gpu->connectors[0]->encoder_id = gpu->encoders[0]->id;
    virtio_gpu_bind_connector_crtc(gpu, gpu->connectors[0], gpu->crtcs[0]->id);
    return 0;
}

int virtio_gpu_init(virtio_driver_t *driver) {
    if (!driver) {
        return -EINVAL;
    }

    uint64_t features =
        virtio_begin_init(driver, VIRTIO_GPU_SUPPORTED_FEATURES);
    if (!features) {
        printk("virtio_gpu: failed to negotiate features\n");
        return -ENODEV;
    }

    virtio_gpu_device_t *gpu = calloc(1, sizeof(*gpu));
    if (!gpu) {
        return -ENOMEM;
    }

    gpu->driver = driver;
    gpu->negotiated_features = features;
    gpu->next_resource_id = 1;
    gpu->next_context_id = 1;
    gpu->next_fence_id = 1;
    wait_queue_init(&gpu->control_available_wait);
    wait_queue_init(&gpu->control_irq_wait);
    wait_queue_init(&gpu->display_event_wait);
    drm_resource_manager_init(&gpu->resource_mgr);
    if ((features & VIRTIO_GPU_F_VIRGL) == 0) {
        printk("virtio_gpu: host did not offer virgl; Mesa will use software "
               "rendering\n");
    }

    gpu->control_vq = virt_queue_new(driver, VIRTIO_GPU_QUEUE_CONTROL,
                                     !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                                     !!(features & VIRTIO_F_RING_EVENT_IDX));
    gpu->cursor_vq = virt_queue_new(driver, VIRTIO_GPU_QUEUE_CURSOR,
                                    !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                                    !!(features & VIRTIO_F_RING_EVENT_IDX));
    if (!gpu->control_vq) {
        printk("virtio_gpu: failed to create control queue\n");
        free(gpu);
        return -ENODEV;
    }

    virtio_finish_init(driver);
    if (driver->op->supports_interrupts &&
        driver->op->supports_interrupts(driver->data) &&
        driver->op->set_interrupt_handler) {
        driver->op->set_interrupt_handler(driver->data, virtio_gpu_irq_handler,
                                          gpu);
        __atomic_store_n(&gpu->control_irq_enabled, true, __ATOMIC_RELEASE);
    }
    virtio_gpu_read_config(gpu);
    virtio_gpu_probe_capsets(gpu);
    if ((features & VIRTIO_GPU_F_VIRGL) != 0 &&
        !(gpu->supported_capset_ids & ((1ULL << VIRTGPU_DRM_CAPSET_VIRGL) |
                                       (1ULL << VIRTGPU_DRM_CAPSET_VIRGL2)))) {
        printk(
            "virtio_gpu: host offered virgl but no virgl capset was found\n");
    }

    int ret = virtio_gpu_refresh_display_info(gpu, NULL);
    if (ret != 0) {
        printk("virtio_gpu: failed to get display info: %d\n", ret);
        free(gpu);
        return ret;
    }

    ret = virtio_gpu_setup_modeset(gpu);
    if (ret != 0) {
        free(gpu);
        return ret;
    }

    pci_device_t *pci = NULL;
    if (driver->data) {
        virtio_pci_device_t *vpci = (virtio_pci_device_t *)driver->data;
        pci = vpci->pci_dev;
    }

    gpu->drm_dev = drm_register_device_with_info(
        gpu, &virtio_gpu_drm_device_op, "dri/card", pci, "virtio_gpu",
        "20260610", "NaOS virtio GPU DRM");
    if (!gpu->drm_dev) {
        free(gpu);
        return -ENODEV;
    }

    printk("virtio_gpu: initialized %ux%u scanout %u\n", gpu->width,
           gpu->height, gpu->scanout_id);
    task_create("virtio_gpu", virtio_gpu_display_worker, (uint64_t)gpu,
                KTHREAD_PRIORITY);
    return 0;
}

static virtio_device_driver_t virtio_gpu_driver = {
    .name = "virtio-gpu",
    .device_type = VIRTIO_DEVICE_TYPE_GPU,
    .probe = virtio_gpu_init,
    .remove = NULL,
    .shutdown = NULL,
};

int dlmain(void) { return virtio_register_device_driver(&virtio_gpu_driver); }
