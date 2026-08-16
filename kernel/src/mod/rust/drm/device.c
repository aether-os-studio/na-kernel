#include <dev/device.h>
#include <mod/rust/drm/internal.h>

static uint64_t na_drm_file_id(fd_t *fd) {
    return (uint64_t)(uintptr_t)(fd ? device_file_private(fd) : NULL);
}

static int na_drm_open(drm_device_t *device, drm_file_t *file) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !file || !bridge->ops.open)
        return 0;
    return bridge->ops.open(bridge->ops.context, (uint64_t)(uintptr_t)file);
}

static void na_drm_close(drm_device_t *device, drm_file_t *file) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (bridge && file && bridge->ops.close)
        bridge->ops.close(bridge->ops.context, (uint64_t)(uintptr_t)file);
}

static int64_t na_drm_driver_ioctl(drm_device_t *device, uint32_t command,
                                   void *arg, bool render_node, fd_t *fd) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !bridge->ops.driver_ioctl)
        return -ENOTTY;
    return bridge->ops.driver_ioctl(bridge->ops.context, command, arg,
                                    _IOC_SIZE(command), render_node,
                                    na_drm_file_id(fd));
}

static int na_drm_get_capability(drm_device_t *device,
                                 struct drm_get_cap *capability) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !capability || !bridge->ops.get_capability)
        return -ENOSYS;
    return bridge->ops.get_capability(
        bridge->ops.context, capability->capability, &capability->value);
}

static int na_drm_get_display_info(drm_device_t *device, uint32_t *width,
                                   uint32_t *height, uint32_t *bits_per_pixel) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !bridge->ops.get_display_info)
        return -ENOSYS;
    return bridge->ops.get_display_info(bridge->ops.context, width, height,
                                        bits_per_pixel);
}

static int na_drm_get_framebuffer(drm_device_t *device, uint32_t *width,
                                  uint32_t *height, uint32_t *bits_per_pixel,
                                  uint64_t *physical_address) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !bridge->ops.get_framebuffer)
        return -ENOSYS;
    return bridge->ops.get_framebuffer(bridge->ops.context, width, height,
                                       bits_per_pixel, physical_address);
}

static int na_drm_create_dumb_buffer(drm_device_t *device,
                                     struct drm_mode_create_dumb *args,
                                     fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !args || !bridge->ops.create_dumb_buffer)
        return -ENOSYS;

    na_drm_dumb_buffer_t buffer = {
        .height = args->height,
        .width = args->width,
        .bits_per_pixel = args->bpp,
        .flags = args->flags,
        .handle = args->handle,
        .pitch = args->pitch,
        .size = args->size,
    };
    int result = bridge->ops.create_dumb_buffer(bridge->ops.context, &buffer);
    if (result < 0)
        return result;

    args->handle = buffer.handle;
    args->pitch = buffer.pitch;
    args->size = buffer.size;
    return result;
}

static int na_drm_destroy_dumb_buffer(drm_device_t *device, uint32_t handle,
                                      fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !bridge->ops.destroy_dumb_buffer)
        return -ENOSYS;
    return bridge->ops.destroy_dumb_buffer(bridge->ops.context, handle);
}

static int na_drm_map_dumb_buffer(drm_device_t *device,
                                  struct drm_mode_map_dumb *args, fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !args || !bridge->ops.map_dumb_buffer)
        return -ENOSYS;
    return bridge->ops.map_dumb_buffer(bridge->ops.context, args->handle,
                                       &args->offset);
}

static int na_drm_get_dumb_buffer_mapping(drm_device_t *device, uint32_t handle,
                                          uint64_t *physical, uint64_t *size) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !physical || !size || !bridge->ops.get_dumb_buffer_mapping)
        return -ENOSYS;
    return bridge->ops.get_dumb_buffer_mapping(bridge->ops.context, handle,
                                               physical, size);
}

static int na_drm_mmap(drm_device_t *device, drm_file_t *file, uint64_t offset,
                       uint64_t length, uint64_t *physical) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !file || !physical || !bridge->ops.mmap)
        return -ENOSYS;
    return bridge->ops.mmap(bridge->ops.context, (uint64_t)(uintptr_t)file,
                            offset, length, physical);
}

static int na_drm_prime_export(drm_device_t *device, drm_file_t *file,
                               uint32_t handle, uint64_t *physical,
                               uint64_t *size, uint64_t *token) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !file || !physical || !size || !token ||
        !bridge->ops.prime_export)
        return -ENOSYS;
    return bridge->ops.prime_export(bridge->ops.context,
                                    (uint64_t)(uintptr_t)file, handle, physical,
                                    size, token);
}

static int na_drm_prime_import(drm_device_t *device, drm_file_t *file,
                               uint64_t token, uint32_t *handle) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !file || !handle || !bridge->ops.prime_import)
        return -ENOSYS;
    return bridge->ops.prime_import(bridge->ops.context,
                                    (uint64_t)(uintptr_t)file, token, handle);
}

static int na_drm_prime_release(drm_device_t *device, uint64_t token) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !bridge->ops.prime_release)
        return -ENOSYS;
    return bridge->ops.prime_release(bridge->ops.context, token);
}

drm_device_t *na_drm_device_register(const na_drm_driver_ops_t *ops,
                                     const char *node_name,
                                     pci_device_t *pci_device,
                                     const na_drm_driver_info_t *driver_info) {
    if (!ops || !node_name || !driver_info || !driver_info->kernel_name ||
        !driver_info->uapi_name || !driver_info->date ||
        !driver_info->description)
        return NULL;

    na_drm_driver_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return NULL;

    bridge->ops = *ops;
    bridge->device_ops = (drm_device_op_t){
        .supports_render_node = ops->supports_render_node,
        .supports_atomic_modeset = ops->supports_atomic_modeset,
        .open = na_drm_open,
        .close = na_drm_close,
        .get_cap = na_drm_get_capability,
        .get_display_info = na_drm_get_display_info,
        .get_fb = na_drm_get_framebuffer,
        .create_dumb = na_drm_create_dumb_buffer,
        .destroy_dumb = na_drm_destroy_dumb_buffer,
        .dirty_fb = na_drm_dirty_framebuffer,
        .add_fb = na_drm_add_framebuffer_legacy,
        .add_fb2 = na_drm_add_framebuffer2,
        .release_fb = na_drm_release_framebuffer,
        .get_fb_handle = na_drm_get_framebuffer_handle,
        .set_plane = na_drm_set_plane,
        .atomic_commit = na_drm_atomic_commit,
        .map_dumb = na_drm_map_dumb_buffer,
        .set_crtc = na_drm_set_crtc,
        .page_flip = na_drm_page_flip,
        .set_cursor = na_drm_set_cursor,
        .get_connectors = na_drm_get_connectors,
        .get_crtcs = na_drm_get_crtcs,
        .get_encoders = na_drm_get_encoders,
        .get_planes = na_drm_get_planes,
        .mmap = na_drm_mmap,
        .get_dumb_map = na_drm_get_dumb_buffer_mapping,
        .prime_export = na_drm_prime_export,
        .prime_import = na_drm_prime_import,
        .prime_release = na_drm_prime_release,
        .driver_ioctl = na_drm_driver_ioctl,
    };

    const drm_driver_info_t info = {
        .kernel_name = driver_info->kernel_name,
        .uapi_name = driver_info->uapi_name,
        .date = driver_info->date,
        .description = driver_info->description,
        .version_major = driver_info->version_major,
        .version_minor = driver_info->version_minor,
        .version_patchlevel = driver_info->version_patchlevel,
    };
    drm_device_t *device = drm_register_device_with_info(
        bridge, &bridge->device_ops, node_name, pci_device, &info);
    if (!device)
        free(bridge);
    return device;
}

void na_drm_device_unregister(drm_device_t *device) {
    if (!device)
        return;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
        if (device->resource_mgr.connectors[i]) {
            free(device->resource_mgr.connectors[i]->modes);
            device->resource_mgr.connectors[i]->modes = NULL;
            device->resource_mgr.connectors[i]->count_modes = 0;
        }
    }
    for (uint32_t i = 0; i < DRM_MAX_PLANES_PER_DEVICE; i++) {
        if (device->resource_mgr.planes[i]) {
            free(device->resource_mgr.planes[i]->format_types);
            device->resource_mgr.planes[i]->format_types = NULL;
            device->resource_mgr.planes[i]->count_format_types = 0;
        }
    }
    if (bridge && bridge->ops.release_framebuffer) {
        for (uint32_t i = 0; i < DRM_MAX_FRAMEBUFFERS_PER_DEVICE; i++) {
            drm_framebuffer_t *framebuffer =
                device->resource_mgr.framebuffers[i];
            if (framebuffer && framebuffer->driver_data == bridge)
                bridge->ops.release_framebuffer(bridge->ops.context,
                                                framebuffer->driver_handle);
        }
    }
    drm_unregister_device(device);
    free(bridge);
}

int na_drm_device_notify_hotplug(drm_device_t *device) {
    return device ? drm_notify_hotplug(device) : -EINVAL;
}
