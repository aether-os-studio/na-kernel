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

    drm_plane_t *plane =
        drm_plane_get(&device->resource_mgr, command->plane_id);
    if (!plane)
        return -ENOENT;

    na_drm_plane_update_t update = {
        .plane_id = command->plane_id,
        .plane_type = plane->plane_type,
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

    drm_framebuffer_t *framebuffer = NULL;
    if (command->fb_id != 0) {
        framebuffer =
            drm_framebuffer_get(&device->resource_mgr, command->fb_id);
        if (!framebuffer) {
            drm_plane_free(&device->resource_mgr, plane->id);
            return -ENOENT;
        }
        update.framebuffer_handle = framebuffer->driver_handle;
    }

    int result = bridge->ops.set_plane(bridge->ops.context, &update);
    if (framebuffer)
        drm_framebuffer_free(&device->resource_mgr, framebuffer->id);
    drm_plane_free(&device->resource_mgr, plane->id);
    return result;
}

int na_drm_set_property(drm_device_t *device,
                        struct drm_mode_obj_set_property *command, fd_t *fd) {
    (void)fd;
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !command || !bridge->ops.set_property)
        return -ENOSYS;

    na_drm_property_update_t update = {
        .object_id = command->obj_id,
        .object_type = command->obj_type,
        .property_id = command->prop_id,
        .value = command->value,
    };
    return bridge->ops.set_property(bridge->ops.context, &update);
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
                                 command->crtc_id, command->user_data);
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
    enum {
        ATOMIC_MAX_OBJECTS = DRM_MAX_PLANES_PER_DEVICE +
                             DRM_MAX_CRTCS_PER_DEVICE +
                             DRM_MAX_CONNECTORS_PER_DEVICE,
        ATOMIC_MAX_PROPERTIES = DRM_MAX_PLANES_PER_DEVICE * 13 +
                                DRM_MAX_CRTCS_PER_DEVICE * 2 +
                                DRM_MAX_CONNECTORS_PER_DEVICE * 3,
    };
    if (command->flags & ~DRM_MODE_ATOMIC_FLAGS ||
        command->count_objs > ATOMIC_MAX_OBJECTS)
        return -EINVAL;

    uint32_t object_count = command->count_objs;
    uint32_t object_ids[ATOMIC_MAX_OBJECTS] = {0};
    uint32_t property_counts[ATOMIC_MAX_OBJECTS] = {0};
    uint32_t property_ids[ATOMIC_MAX_PROPERTIES] = {0};
    uint64_t property_values[ATOMIC_MAX_PROPERTIES] = {0};
    na_drm_atomic_property_t properties[ATOMIC_MAX_PROPERTIES] = {0};
    struct atomic_object {
        uint32_t id;
        uint32_t type;
        union {
            drm_plane_t *plane;
            drm_crtc_t *crtc;
            drm_connector_t *connector;
        } resource;
        na_drm_mode_info_t mode;
    } objects[ATOMIC_MAX_OBJECTS] = {0};
    size_t property_count = 0;
    int result = 0;

    if (object_count) {
        if (!command->objs_ptr || !command->count_props_ptr ||
            !command->props_ptr || !command->prop_values_ptr)
            return -EINVAL;

        if (copy_from_user(object_ids, (void *)(uintptr_t)command->objs_ptr,
                           object_count * sizeof(*object_ids)) ||
            copy_from_user(property_counts,
                           (void *)(uintptr_t)command->count_props_ptr,
                           object_count * sizeof(*property_counts))) {
            result = -EFAULT;
            goto out;
        }

        for (uint32_t i = 0; i < object_count; i++) {
            for (uint32_t prior = 0; prior < i; prior++) {
                if (object_ids[prior] == object_ids[i]) {
                    result = -EINVAL;
                    goto out;
                }
            }

            objects[i].id = object_ids[i];
            objects[i].resource.plane =
                drm_plane_get(&device->resource_mgr, object_ids[i]);
            if (objects[i].resource.plane) {
                objects[i].type = DRM_MODE_OBJECT_PLANE;
                if (property_counts[i] > 13) {
                    result = -EINVAL;
                    goto out;
                }
            } else {
                objects[i].resource.crtc =
                    drm_crtc_get(&device->resource_mgr, object_ids[i]);
                if (objects[i].resource.crtc) {
                    objects[i].type = DRM_MODE_OBJECT_CRTC;
                    if (property_counts[i] > 2) {
                        result = -EINVAL;
                        goto out;
                    }
                } else {
                    objects[i].resource.connector =
                        drm_connector_get(&device->resource_mgr, object_ids[i]);
                    if (!objects[i].resource.connector) {
                        result = -ENOENT;
                        goto out;
                    }
                    objects[i].type = DRM_MODE_OBJECT_CONNECTOR;
                    if (property_counts[i] > 3) {
                        result = -EINVAL;
                        goto out;
                    }
                }
            }

            if (property_counts[i] > ATOMIC_MAX_PROPERTIES - property_count) {
                result = -EINVAL;
                goto out;
            }
            property_count += property_counts[i];
        }
    }

    if (property_count) {
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
                    .object_type = objects[object_index].type,
                    .property_id = property_ids[property_index],
                    .plane_type = UINT32_MAX,
                    .value = property_values[property_index],
                };

                uint32_t property_id = property_ids[property_index];
                uint64_t value = property_values[property_index];
                switch (objects[object_index].type) {
                case DRM_MODE_OBJECT_PLANE: {
                    drm_plane_t *plane = objects[object_index].resource.plane;
                    properties[property_index].plane_type = plane->plane_type;
                    switch (property_id) {
                    case DRM_PROPERTY_ID_FB_ID:
                        if (value > UINT32_MAX) {
                            result = -EINVAL;
                            goto out;
                        }
                        if (value != 0) {
                            drm_framebuffer_t *framebuffer =
                                drm_framebuffer_get(&device->resource_mgr,
                                                    (uint32_t)value);
                            if (!framebuffer) {
                                result = -ENOENT;
                                goto out;
                            }
                            properties[property_index].framebuffer_handle =
                                framebuffer->driver_handle;
                            drm_framebuffer_free(&device->resource_mgr,
                                                 framebuffer->id);
                        }
                        break;
                    case DRM_PROPERTY_ID_CRTC_ID:
                        if (value > UINT32_MAX) {
                            result = -EINVAL;
                            goto out;
                        }
                        if (value != 0) {
                            drm_crtc_t *crtc = drm_crtc_get(
                                &device->resource_mgr, (uint32_t)value);
                            if (!crtc) {
                                result = -ENOENT;
                                goto out;
                            }
                            drm_crtc_free(&device->resource_mgr, crtc->id);
                        }
                        break;
                    case DRM_PROPERTY_ID_SRC_X:
                    case DRM_PROPERTY_ID_SRC_Y:
                    case DRM_PROPERTY_ID_SRC_W:
                    case DRM_PROPERTY_ID_SRC_H:
                    case DRM_PROPERTY_ID_CRTC_X:
                    case DRM_PROPERTY_ID_CRTC_Y:
                    case DRM_PROPERTY_ID_CRTC_W:
                    case DRM_PROPERTY_ID_CRTC_H:
                        if (property_id != DRM_PROPERTY_ID_CRTC_X &&
                            property_id != DRM_PROPERTY_ID_CRTC_Y &&
                            value > UINT32_MAX) {
                            result = -EINVAL;
                            goto out;
                        }
                        break;
                    case DRM_PROPERTY_ID_FB_DAMAGE_CLIPS:
                        if (value != 0) {
                            if (value > UINT32_MAX) {
                                result = -EINVAL;
                                goto out;
                            }
                            const void *data = NULL;
                            uint32_t length = 0;
                            result = drm_property_lookup_blob_data(
                                device, (uint32_t)value, &data, &length);
                            if (result != 0)
                                goto out;
                        }
                        break;
                    default:
                        result = -EINVAL;
                        goto out;
                    }
                    break;
                }
                case DRM_MODE_OBJECT_CRTC: {
                    switch (property_id) {
                    case DRM_CRTC_ACTIVE_PROP_ID:
                        if (value > 1) {
                            result = -EINVAL;
                            goto out;
                        }
                        break;
                    case DRM_CRTC_MODE_ID_PROP_ID:
                        if (value != 0) {
                            if (value > UINT32_MAX) {
                                result = -EINVAL;
                                goto out;
                            }
                            struct drm_mode_modeinfo mode;
                            result = drm_property_get_modeinfo_from_blob(
                                device, (uint32_t)value, &mode);
                            if (result != 0)
                                goto out;
                            if (mode.hdisplay == 0 || mode.vdisplay == 0) {
                                result = -EINVAL;
                                goto out;
                            }
                            na_drm_mode_export(&objects[object_index].mode,
                                               &mode);
                            properties[property_index].mode_info =
                                &objects[object_index].mode;
                        }
                        break;
                    default:
                        result = -EINVAL;
                        goto out;
                    }
                    break;
                }
                case DRM_MODE_OBJECT_CONNECTOR: {
                    switch (property_id) {
                    case DRM_CONNECTOR_DPMS_PROP_ID:
                        if (value > DRM_MODE_DPMS_OFF) {
                            result = -EINVAL;
                            goto out;
                        }
                        break;
                    case DRM_CONNECTOR_CRTC_ID_PROP_ID:
                        if (value > UINT32_MAX) {
                            result = -EINVAL;
                            goto out;
                        }
                        if (value != 0) {
                            drm_crtc_t *crtc = drm_crtc_get(
                                &device->resource_mgr, (uint32_t)value);
                            if (!crtc) {
                                result = -ENOENT;
                                goto out;
                            }
                            drm_crtc_free(&device->resource_mgr, crtc->id);
                        }
                        break;
                    default:
                        result = -EINVAL;
                        goto out;
                    }
                    break;
                }
                default:
                    result = -EINVAL;
                    goto out;
                }
                property_index++;
            }
        }
    }

    result = bridge->ops.atomic_commit(bridge->ops.context, command->flags,
                                       command->user_data, properties,
                                       property_count);
    if (result != 0 || (command->flags & DRM_MODE_ATOMIC_TEST_ONLY))
        goto out;

    uint32_t stale_framebuffers[DRM_MAX_PLANES_PER_DEVICE] = {0};
    uint32_t stale_count = 0;
    uint32_t event_crtcs[DRM_MAX_CRTCS_PER_DEVICE] = {0};
    uint32_t event_crtc_count = 0;
    size_t property_index = 0;
    for (uint32_t object_index = 0; object_index < object_count;
         object_index++) {
        struct atomic_object *object = &objects[object_index];
        uint32_t old_plane_fb = object->type == DRM_MODE_OBJECT_PLANE
                                    ? object->resource.plane->fb_id
                                    : 0;
        for (uint32_t i = 0; i < property_counts[object_index]; i++) {
            na_drm_atomic_property_t *property = &properties[property_index++];
            if (object->type == DRM_MODE_OBJECT_PLANE) {
                drm_plane_t *plane = object->resource.plane;
                switch (property->property_id) {
                case DRM_PROPERTY_ID_FB_ID:
                    plane->fb_id = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_CRTC_ID:
                    plane->crtc_id = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_SRC_X:
                    plane->src_x = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_SRC_Y:
                    plane->src_y = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_SRC_W:
                    plane->src_w = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_SRC_H:
                    plane->src_h = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_CRTC_X:
                    plane->crtc_x = (int32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_CRTC_Y:
                    plane->crtc_y = (int32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_CRTC_W:
                    plane->crtc_w = (uint32_t)property->value;
                    break;
                case DRM_PROPERTY_ID_CRTC_H:
                    plane->crtc_h = (uint32_t)property->value;
                    break;
                default:
                    break;
                }
            } else if (object->type == DRM_MODE_OBJECT_CRTC) {
                drm_crtc_t *crtc = object->resource.crtc;
                if (property->property_id == DRM_CRTC_ACTIVE_PROP_ID) {
                    crtc->active = property->value != 0;
                } else if (property->property_id == DRM_CRTC_MODE_ID_PROP_ID) {
                    crtc->mode_valid = property->value != 0;
                    if (property->mode_info)
                        na_drm_mode_copy(&crtc->mode, property->mode_info);
                    else
                        memset(&crtc->mode, 0, sizeof(crtc->mode));
                    crtc->w = crtc->mode_valid ? crtc->mode.hdisplay : 0;
                    crtc->h = crtc->mode_valid ? crtc->mode.vdisplay : 0;
                }
            } else if (object->type == DRM_MODE_OBJECT_CONNECTOR) {
                drm_connector_t *connector = object->resource.connector;
                if (property->property_id == DRM_CONNECTOR_DPMS_PROP_ID)
                    connector->dpms = (uint32_t)property->value;
                else if (property->property_id == DRM_CONNECTOR_CRTC_ID_PROP_ID)
                    connector->crtc_id = (uint32_t)property->value;
            }
        }

        if (object->type == DRM_MODE_OBJECT_PLANE) {
            drm_plane_t *plane = object->resource.plane;
            if (old_plane_fb != 0 && old_plane_fb != plane->fb_id &&
                stale_count < DRM_MAX_PLANES_PER_DEVICE)
                stale_framebuffers[stale_count++] = old_plane_fb;
            if (plane->plane_type == DRM_PLANE_TYPE_PRIMARY &&
                plane->crtc_id != 0) {
                drm_crtc_t *crtc =
                    drm_crtc_get(&device->resource_mgr, plane->crtc_id);
                if (crtc) {
                    crtc->fb_id = plane->fb_id;
                    drm_crtc_free(&device->resource_mgr, crtc->id);
                }
            }
        }

        uint32_t event_crtc_id = 0;
        if (object->type == DRM_MODE_OBJECT_PLANE)
            event_crtc_id = object->resource.plane->crtc_id;
        else if (object->type == DRM_MODE_OBJECT_CRTC)
            event_crtc_id = object->resource.crtc->id;
        else if (object->type == DRM_MODE_OBJECT_CONNECTOR)
            event_crtc_id = object->resource.connector->crtc_id;

        if (event_crtc_id != 0) {
            bool duplicate = false;
            for (uint32_t i = 0; i < event_crtc_count; i++)
                duplicate |= event_crtcs[i] == event_crtc_id;
            if (!duplicate && event_crtc_count < DRM_MAX_CRTCS_PER_DEVICE)
                event_crtcs[event_crtc_count++] = event_crtc_id;
        }
    }

    if (command->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        if (event_crtc_count == 0) {
            result = -EINVAL;
        } else {
            for (uint32_t i = 0; i < event_crtc_count; i++) {
                result = drm_defer_event(device, fd, DRM_EVENT_FLIP_COMPLETE,
                                         event_crtcs[i], command->user_data);
                if (result != 0)
                    break;
            }
        }
    }
    for (uint32_t i = 0; i < stale_count; i++)
        drm_framebuffer_cleanup_closed(device, stale_framebuffers[i]);

out:
    for (uint32_t i = 0; i < object_count; i++) {
        switch (objects[i].type) {
        case DRM_MODE_OBJECT_PLANE:
            drm_plane_free(&device->resource_mgr, objects[i].id);
            break;
        case DRM_MODE_OBJECT_CRTC:
            drm_crtc_free(&device->resource_mgr, objects[i].id);
            break;
        case DRM_MODE_OBJECT_CONNECTOR:
            drm_connector_free(&device->resource_mgr, objects[i].id);
            break;
        default:
            break;
        }
    }
    return result;
}
