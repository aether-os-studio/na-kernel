#include <mod/rust/drm/internal.h>

static int na_drm_add_framebuffer(drm_device_t *device,
                                  na_drm_framebuffer_request_t *request,
                                  uint32_t *framebuffer_id, fd_t *fd) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !request || !framebuffer_id ||
        !bridge->ops.create_framebuffer)
        return -ENOSYS;

    request->file_id =
        (uint64_t)(uintptr_t)(fd ? device_file_private(fd) : NULL);
    int result = bridge->ops.create_framebuffer(bridge->ops.context, request);
    if (result < 0)
        return result;

    drm_framebuffer_t *framebuffer =
        drm_framebuffer_alloc(&device->resource_mgr, bridge);
    if (!framebuffer) {
        if (bridge->ops.release_framebuffer)
            bridge->ops.release_framebuffer(bridge->ops.context,
                                            request->driver_handle
                                                ? request->driver_handle
                                                : request->handles[0]);
        return -ENOMEM;
    }

    framebuffer->width = request->width;
    framebuffer->height = request->height;
    framebuffer->pitch = request->pitches[0];
    framebuffer->bpp = request->bits_per_pixel;
    framebuffer->depth = request->depth;
    framebuffer->handle = request->handles[0];
    framebuffer->driver_handle =
        request->driver_handle ? request->driver_handle : request->handles[0];
    framebuffer->modifier = request->modifiers[0];
    framebuffer->format = request->pixel_format;
    framebuffer->flags = request->flags;
    *framebuffer_id = framebuffer->id;
    return 0;
}

int na_drm_add_framebuffer_legacy(drm_device_t *device,
                                  struct drm_mode_fb_cmd *command, fd_t *fd) {
    (void)fd;
    if (!command)
        return -EINVAL;

    na_drm_framebuffer_request_t request = {
        .width = command->width,
        .height = command->height,
        .bits_per_pixel = command->bpp,
        .depth = command->depth,
        .handles = {command->handle},
        .pitches = {command->pitch},
    };
    return na_drm_add_framebuffer(device, &request, &command->fb_id, fd);
}

int na_drm_add_framebuffer2(drm_device_t *device,
                            struct drm_mode_fb_cmd2 *command, fd_t *fd) {
    (void)fd;
    if (!command)
        return -EINVAL;

    na_drm_framebuffer_request_t request = {
        .width = command->width,
        .height = command->height,
        .pixel_format = command->pixel_format,
        .flags = command->flags,
    };
    memcpy(request.handles, command->handles, sizeof(request.handles));
    memcpy(request.pitches, command->pitches, sizeof(request.pitches));
    memcpy(request.offsets, command->offsets, sizeof(request.offsets));
    memcpy(request.modifiers, command->modifier, sizeof(request.modifiers));
    return na_drm_add_framebuffer(device, &request, &command->fb_id, fd);
}

void na_drm_release_framebuffer(drm_device_t *device,
                                drm_framebuffer_t *framebuffer) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (bridge && framebuffer && bridge->ops.release_framebuffer)
        bridge->ops.release_framebuffer(bridge->ops.context,
                                        framebuffer->driver_handle);
}

int na_drm_get_framebuffer_handle(drm_device_t *device,
                                  drm_framebuffer_t *framebuffer, fd_t *fd,
                                  uint32_t *handle) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !framebuffer || !handle ||
        !bridge->ops.get_framebuffer_handle)
        return -ENOSYS;
    uint64_t file_id =
        (uint64_t)(uintptr_t)(fd ? device_file_private(fd) : NULL);
    return bridge->ops.get_framebuffer_handle(
        bridge->ops.context, file_id, framebuffer->driver_handle, handle);
}

int na_drm_dirty_framebuffer(drm_device_t *device,
                             struct drm_mode_fb_dirty_cmd *command, fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.dirty_framebuffer)
        return -ENOSYS;
    if (command->num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS)
        return -EINVAL;

    na_drm_clip_t *clips = NULL;
    if (command->num_clips) {
        if (!command->clips_ptr)
            return -EINVAL;
        clips = calloc(command->num_clips, sizeof(*clips));
        if (!clips)
            return -ENOMEM;
        if (copy_from_user(clips, (void *)(uintptr_t)command->clips_ptr,
                           command->num_clips * sizeof(*clips))) {
            free(clips);
            return -EFAULT;
        }
    }

    drm_framebuffer_t *framebuffer =
        drm_framebuffer_get(&device->resource_mgr, command->fb_id);
    if (!framebuffer) {
        free(clips);
        return -ENOENT;
    }
    int result = bridge->ops.dirty_framebuffer(
        bridge->ops.context, command->fb_id, framebuffer->driver_handle,
        command->flags, command->color, clips, command->num_clips);
    drm_framebuffer_free(&device->resource_mgr, framebuffer->id);
    free(clips);
    return result;
}

int na_drm_set_plane(drm_device_t *device, struct drm_mode_set_plane *command,
                     fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.set_plane)
        return -ENOSYS;

    na_drm_plane_update_t update = {
        .plane_id = command->plane_id,
        .crtc_id = command->crtc_id,
        .framebuffer_id = command->fb_id,
        .flags = command->flags,
        .crtc_x = command->crtc_x,
        .crtc_y = command->crtc_y,
        .crtc_width = command->crtc_w,
        .crtc_height = command->crtc_h,
        .source_x = command->src_x,
        .source_y = command->src_y,
        .source_width = command->src_w,
        .source_height = command->src_h,
    };
    drm_framebuffer_t *framebuffer =
        drm_framebuffer_get(&device->resource_mgr, command->fb_id);
    if (!framebuffer)
        return -ENOENT;
    update.framebuffer_handle = framebuffer->driver_handle;
    int result = bridge->ops.set_plane(bridge->ops.context, &update);
    drm_framebuffer_free(&device->resource_mgr, framebuffer->id);
    return result;
}

int na_drm_set_crtc(drm_device_t *device, struct drm_mode_crtc *command,
                    fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.set_crtc)
        return -ENOSYS;
    if (command->count_connectors > DRM_MAX_CONNECTORS_PER_DEVICE)
        return -EINVAL;

    uint32_t connector_ids[DRM_MAX_CONNECTORS_PER_DEVICE];
    if (command->count_connectors &&
        (!command->set_connectors_ptr ||
         copy_from_user(connector_ids,
                        (void *)(uintptr_t)command->set_connectors_ptr,
                        command->count_connectors * sizeof(uint32_t))))
        return -EFAULT;

    na_drm_crtc_update_t update = {
        .crtc_id = command->crtc_id,
        .framebuffer_id = command->fb_id,
        .x = command->x,
        .y = command->y,
        .gamma_size = command->gamma_size,
        .mode_valid = command->mode_valid != 0,
    };
    drm_framebuffer_t *framebuffer = NULL;
    if (command->fb_id) {
        framebuffer =
            drm_framebuffer_get(&device->resource_mgr, command->fb_id);
        if (!framebuffer)
            return -ENOENT;
        update.framebuffer_handle = framebuffer->driver_handle;
    }
    if (command->mode_valid)
        na_drm_mode_export(&update.mode_info, &command->mode);
    int result = bridge->ops.set_crtc(bridge->ops.context, &update,
                                      connector_ids, command->count_connectors);
    if (framebuffer)
        drm_framebuffer_free(&device->resource_mgr, framebuffer->id);
    return result;
}

int na_drm_page_flip(drm_device_t *device,
                     struct drm_mode_crtc_page_flip *command, fd_t *fd) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.page_flip)
        return -ENOSYS;

    na_drm_page_flip_t flip = {
        .crtc_id = command->crtc_id,
        .framebuffer_id = command->fb_id,
        .flags = command->flags,
        .user_data = command->user_data,
    };
    drm_framebuffer_t *framebuffer =
        drm_framebuffer_get(&device->resource_mgr, command->fb_id);
    if (!framebuffer)
        return -ENOENT;
    flip.framebuffer_handle = framebuffer->driver_handle;
    int result = bridge->ops.page_flip(bridge->ops.context, &flip);
    drm_framebuffer_free(&device->resource_mgr, framebuffer->id);
    if (result == 0 && (command->flags & DRM_MODE_PAGE_FLIP_EVENT))
        result = drm_defer_event(device, fd, DRM_EVENT_FLIP_COMPLETE,
                                 command->user_data);
    return result;
}

int na_drm_set_cursor(drm_device_t *device, struct drm_mode_cursor *command,
                      fd_t *fd) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.set_cursor)
        return -ENOSYS;

    na_drm_cursor_update_t update = {
        .file_id = (uint64_t)(uintptr_t)(fd ? device_file_private(fd) : NULL),
        .flags = command->flags,
        .crtc_id = command->crtc_id,
        .x = command->x,
        .y = command->y,
        .width = command->width,
        .height = command->height,
        .handle = command->handle,
    };
    return bridge->ops.set_cursor(bridge->ops.context, &update);
}

int na_drm_atomic_commit(drm_device_t *device, struct drm_mode_atomic *command,
                         fd_t *fd) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.atomic_commit)
        return -ENOSYS;
    if (command->flags & ~DRM_MODE_ATOMIC_FLAGS ||
        command->count_objs > DRM_MAX_LEASE_OBJECTS)
        return -EINVAL;

    uint32_t object_count = command->count_objs;
    uint32_t *object_ids = NULL;
    uint32_t *property_counts = NULL;
    uint32_t *property_ids = NULL;
    uint64_t *property_values = NULL;
    na_drm_atomic_property_t *properties = NULL;
    size_t property_count = 0;
    int result = 0;

    if (object_count) {
        if (!command->objs_ptr || !command->count_props_ptr ||
            !command->props_ptr || !command->prop_values_ptr)
            return -EINVAL;

        object_ids = calloc(object_count, sizeof(*object_ids));
        property_counts = calloc(object_count, sizeof(*property_counts));
        if (!object_ids || !property_counts) {
            result = -ENOMEM;
            goto out;
        }
        if (copy_from_user(object_ids, (void *)(uintptr_t)command->objs_ptr,
                           object_count * sizeof(*object_ids)) ||
            copy_from_user(property_counts,
                           (void *)(uintptr_t)command->count_props_ptr,
                           object_count * sizeof(*property_counts))) {
            result = -EFAULT;
            goto out;
        }

        for (uint32_t i = 0; i < object_count; i++) {
            if (property_counts[i] > SIZE_MAX - property_count) {
                result = -EINVAL;
                goto out;
            }
            property_count += property_counts[i];
        }
    }

    if (property_count) {
        if (property_count > SIZE_MAX / sizeof(*properties)) {
            result = -EINVAL;
            goto out;
        }
        property_ids = calloc(property_count, sizeof(*property_ids));
        property_values = calloc(property_count, sizeof(*property_values));
        properties = calloc(property_count, sizeof(*properties));
        if (!property_ids || !property_values || !properties) {
            result = -ENOMEM;
            goto out;
        }
        if (copy_from_user(property_ids, (void *)(uintptr_t)command->props_ptr,
                           property_count * sizeof(*property_ids)) ||
            copy_from_user(property_values,
                           (void *)(uintptr_t)command->prop_values_ptr,
                           property_count * sizeof(*property_values))) {
            result = -EFAULT;
            goto out;
        }

        size_t property_index = 0;
        for (uint32_t object_index = 0; object_index < object_count;
             object_index++) {
            for (uint32_t i = 0; i < property_counts[object_index]; i++) {
                properties[property_index] = (na_drm_atomic_property_t){
                    .object_id = object_ids[object_index],
                    .property_id = property_ids[property_index],
                    .value = property_values[property_index],
                };
                if (properties[property_index].property_id ==
                        DRM_PROPERTY_ID_FB_ID &&
                    properties[property_index].value != 0) {
                    drm_framebuffer_t *framebuffer = drm_framebuffer_get(
                        &device->resource_mgr,
                        (uint32_t)properties[property_index].value);
                    if (!framebuffer) {
                        result = -ENOENT;
                        goto out;
                    }
                    properties[property_index].framebuffer_handle =
                        framebuffer->driver_handle;
                    drm_framebuffer_free(&device->resource_mgr,
                                         framebuffer->id);
                }
                property_index++;
            }
        }
    }

    result = bridge->ops.atomic_commit(bridge->ops.context, command->flags,
                                       command->user_data, properties,
                                       property_count);
    if (result == 0 && !(command->flags & DRM_MODE_ATOMIC_TEST_ONLY) &&
        (command->flags & DRM_MODE_PAGE_FLIP_EVENT))
        result = drm_defer_event(device, fd, DRM_EVENT_FLIP_COMPLETE,
                                 command->user_data);

out:
    free(properties);
    free(property_values);
    free(property_ids);
    free(property_counts);
    free(object_ids);
    return result;
}
