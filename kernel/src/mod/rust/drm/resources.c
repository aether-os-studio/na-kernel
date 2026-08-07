#include <mod/rust/drm/internal.h>

void na_drm_mode_copy(struct drm_mode_modeinfo *destination,
                      const na_drm_mode_info_t *source) {
    *destination = (struct drm_mode_modeinfo){
        .clock = source->clock,
        .hdisplay = source->hdisplay,
        .hsync_start = source->hsync_start,
        .hsync_end = source->hsync_end,
        .htotal = source->htotal,
        .hskew = source->hskew,
        .vdisplay = source->vdisplay,
        .vsync_start = source->vsync_start,
        .vsync_end = source->vsync_end,
        .vtotal = source->vtotal,
        .vscan = source->vscan,
        .vrefresh = source->vrefresh,
        .flags = source->flags,
        .type = source->mode_type,
    };
    memcpy(destination->name, source->name, sizeof(destination->name));
}

void na_drm_mode_export(na_drm_mode_info_t *destination,
                        const struct drm_mode_modeinfo *source) {
    *destination = (na_drm_mode_info_t){
        .clock = source->clock,
        .hdisplay = source->hdisplay,
        .hsync_start = source->hsync_start,
        .hsync_end = source->hsync_end,
        .htotal = source->htotal,
        .hskew = source->hskew,
        .vdisplay = source->vdisplay,
        .vsync_start = source->vsync_start,
        .vsync_end = source->vsync_end,
        .vtotal = source->vtotal,
        .vscan = source->vscan,
        .vrefresh = source->vrefresh,
        .flags = source->flags,
        .mode_type = source->type,
    };
    memcpy(destination->name, source->name, sizeof(destination->name));
}

static uint32_t na_drm_resource_token(uint32_t index, uint32_t capacity) {
    return index < capacity ? index + 1 : 0;
}

drm_connector_t *na_drm_connector_create(const na_drm_connector_info_t *info) {
    if (!info ||
        (info->encoder_index != UINT32_MAX &&
         info->encoder_index >= DRM_MAX_ENCODERS_PER_DEVICE) ||
        (info->crtc_index != UINT32_MAX &&
         info->crtc_index >= DRM_MAX_CRTCS_PER_DEVICE))
        return NULL;

    drm_connector_t *connector = calloc(1, sizeof(*connector));
    if (!connector)
        return NULL;

    connector->type = info->connector_type;
    connector->connection = info->connection;
    connector->encoder_id =
        na_drm_resource_token(info->encoder_index, DRM_MAX_ENCODERS_PER_DEVICE);
    connector->crtc_id =
        na_drm_resource_token(info->crtc_index, DRM_MAX_CRTCS_PER_DEVICE);
    connector->mm_width = info->mm_width;
    connector->mm_height = info->mm_height;
    connector->subpixel = info->subpixel;
    connector->refcount = 1;
    return connector;
}

int na_drm_connector_add_mode(drm_connector_t *connector,
                              const na_drm_mode_info_t *mode) {
    if (!connector || !mode || connector->count_modes == UINT32_MAX)
        return -EINVAL;

    size_t count = (size_t)connector->count_modes + 1;
    if (count > SIZE_MAX / sizeof(*connector->modes))
        return -EINVAL;
    struct drm_mode_modeinfo *modes =
        realloc(connector->modes, count * sizeof(*modes));
    if (!modes)
        return -ENOMEM;

    connector->modes = modes;
    na_drm_mode_copy(&modes[connector->count_modes], mode);
    connector->count_modes++;
    return 0;
}

void na_drm_connector_destroy(drm_connector_t *connector) {
    if (!connector)
        return;
    free(connector->modes);
    free(connector);
}

drm_crtc_t *na_drm_crtc_create(const na_drm_crtc_info_t *info) {
    if (!info)
        return NULL;

    drm_crtc_t *crtc = calloc(1, sizeof(*crtc));
    if (!crtc)
        return NULL;

    crtc->x = info->x;
    crtc->y = info->y;
    crtc->w = info->width;
    crtc->h = info->height;
    crtc->gamma_size = info->gamma_size;
    crtc->mode_valid = info->mode_valid;
    if (info->mode_valid)
        na_drm_mode_copy(&crtc->mode, &info->mode_info);
    crtc->refcount = 1;
    return crtc;
}

void na_drm_crtc_destroy(drm_crtc_t *crtc) { free(crtc); }

drm_encoder_t *na_drm_encoder_create(const na_drm_encoder_info_t *info) {
    if (!info || (info->crtc_index != UINT32_MAX &&
                  info->crtc_index >= DRM_MAX_CRTCS_PER_DEVICE))
        return NULL;

    drm_encoder_t *encoder = calloc(1, sizeof(*encoder));
    if (!encoder)
        return NULL;

    encoder->type = info->encoder_type;
    encoder->crtc_id =
        na_drm_resource_token(info->crtc_index, DRM_MAX_CRTCS_PER_DEVICE);
    encoder->possible_crtcs = info->possible_crtcs;
    encoder->possible_clones = info->possible_clones;
    encoder->refcount = 1;
    return encoder;
}

void na_drm_encoder_destroy(drm_encoder_t *encoder) { free(encoder); }

drm_plane_t *na_drm_plane_create(drm_device_t *device,
                                 const na_drm_plane_info_t *info) {
    if (!device || !info ||
        (info->crtc_index != UINT32_MAX &&
         info->crtc_index >= DRM_MAX_CRTCS_PER_DEVICE))
        return NULL;

    drm_plane_t *plane = calloc(1, sizeof(*plane));
    if (!plane)
        return NULL;

    if (info->crtc_index != UINT32_MAX &&
        device->resource_mgr.crtcs[info->crtc_index])
        plane->crtc_id = device->resource_mgr.crtcs[info->crtc_index]->id;
    plane->possible_crtcs = info->possible_crtcs;
    plane->gamma_size = info->gamma_size;
    plane->plane_type = info->plane_type;
    plane->refcount = 1;
    return plane;
}

int na_drm_plane_add_format(drm_plane_t *plane, uint32_t format) {
    if (!plane || plane->count_format_types == UINT32_MAX)
        return -EINVAL;

    size_t count = (size_t)plane->count_format_types + 1;
    if (count > SIZE_MAX / sizeof(*plane->format_types))
        return -EINVAL;
    uint32_t *formats = realloc(plane->format_types, count * sizeof(*formats));
    if (!formats)
        return -ENOMEM;

    plane->format_types = formats;
    formats[plane->count_format_types++] = format;
    return 0;
}

void na_drm_plane_destroy(drm_plane_t *plane) {
    if (!plane)
        return;
    free(plane->format_types);
    free(plane);
}

int na_drm_get_connectors(drm_device_t *device, drm_connector_t **connectors,
                          uint32_t *count) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !connectors || !count || !bridge->ops.get_connectors)
        return -ENOSYS;
    int result =
        bridge->ops.get_connectors(bridge->ops.context, device, connectors,
                                   DRM_MAX_CONNECTORS_PER_DEVICE, count);
    if (result < 0 || *count <= DRM_MAX_CONNECTORS_PER_DEVICE)
        return result;
    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++)
        na_drm_connector_destroy(connectors[i]);
    *count = 0;
    return -EINVAL;
}

int na_drm_get_crtcs(drm_device_t *device, drm_crtc_t **crtcs,
                     uint32_t *count) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !crtcs || !count || !bridge->ops.get_crtcs)
        return -ENOSYS;
    int result = bridge->ops.get_crtcs(bridge->ops.context, device, crtcs,
                                       DRM_MAX_CRTCS_PER_DEVICE, count);
    if (result < 0 || *count <= DRM_MAX_CRTCS_PER_DEVICE)
        return result;
    for (uint32_t i = 0; i < DRM_MAX_CRTCS_PER_DEVICE; i++)
        na_drm_crtc_destroy(crtcs[i]);
    *count = 0;
    return -EINVAL;
}

int na_drm_get_encoders(drm_device_t *device, drm_encoder_t **encoders,
                        uint32_t *count) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !encoders || !count || !bridge->ops.get_encoders)
        return -ENOSYS;
    int result = bridge->ops.get_encoders(bridge->ops.context, device, encoders,
                                          DRM_MAX_ENCODERS_PER_DEVICE, count);
    if (result < 0 || *count <= DRM_MAX_ENCODERS_PER_DEVICE)
        return result;
    for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++)
        na_drm_encoder_destroy(encoders[i]);
    *count = 0;
    return -EINVAL;
}

static void na_drm_finalize_links(drm_device_t *device) {
    for (uint32_t i = 0; i < DRM_MAX_CONNECTORS_PER_DEVICE; i++) {
        drm_connector_t *connector = device->resource_mgr.connectors[i];
        if (!connector)
            continue;

        uint32_t encoder_index = connector->encoder_id;
        connector->encoder_id =
            encoder_index && encoder_index <= DRM_MAX_ENCODERS_PER_DEVICE &&
                    device->resource_mgr.encoders[encoder_index - 1]
                ? device->resource_mgr.encoders[encoder_index - 1]->id
                : 0;

        uint32_t crtc_index = connector->crtc_id;
        connector->crtc_id =
            crtc_index && crtc_index <= DRM_MAX_CRTCS_PER_DEVICE &&
                    device->resource_mgr.crtcs[crtc_index - 1]
                ? device->resource_mgr.crtcs[crtc_index - 1]->id
                : 0;
    }

    for (uint32_t i = 0; i < DRM_MAX_ENCODERS_PER_DEVICE; i++) {
        drm_encoder_t *encoder = device->resource_mgr.encoders[i];
        if (!encoder)
            continue;
        uint32_t crtc_index = encoder->crtc_id;
        encoder->crtc_id = crtc_index &&
                                   crtc_index <= DRM_MAX_CRTCS_PER_DEVICE &&
                                   device->resource_mgr.crtcs[crtc_index - 1]
                               ? device->resource_mgr.crtcs[crtc_index - 1]->id
                               : 0;
    }
}

int na_drm_get_planes(drm_device_t *device, drm_plane_t **planes,
                      uint32_t *count) {
    na_drm_driver_bridge_t *bridge = na_drm_bridge(device);
    if (!bridge || !planes || !count || !bridge->ops.get_planes)
        return -ENOSYS;
    na_drm_finalize_links(device);
    int result = bridge->ops.get_planes(bridge->ops.context, device, planes,
                                        DRM_MAX_PLANES_PER_DEVICE, count);
    if (result < 0 || *count <= DRM_MAX_PLANES_PER_DEVICE)
        return result;
    for (uint32_t i = 0; i < DRM_MAX_PLANES_PER_DEVICE; i++)
        na_drm_plane_destroy(planes[i]);
    *count = 0;
    return -EINVAL;
}
