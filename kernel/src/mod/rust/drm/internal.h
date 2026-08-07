#pragma once

#include <drivers/drm/drm.h>
#include <libs/klibc.h>
#include <mod/rust/api.h>

typedef struct na_drm_driver_bridge {
    drm_device_op_t device_ops;
    na_drm_driver_ops_t ops;
} na_drm_driver_bridge_t;

static inline na_drm_driver_bridge_t *na_drm_bridge(drm_device_t *device) {
    return device ? device->data : NULL;
}

void na_drm_mode_copy(struct drm_mode_modeinfo *destination,
                      const na_drm_mode_info_t *source);
void na_drm_mode_export(na_drm_mode_info_t *destination,
                        const struct drm_mode_modeinfo *source);

int na_drm_get_connectors(drm_device_t *device, drm_connector_t **connectors,
                          uint32_t *count);
int na_drm_get_crtcs(drm_device_t *device, drm_crtc_t **crtcs, uint32_t *count);
int na_drm_get_encoders(drm_device_t *device, drm_encoder_t **encoders,
                        uint32_t *count);
int na_drm_get_planes(drm_device_t *device, drm_plane_t **planes,
                      uint32_t *count);

int na_drm_add_framebuffer_legacy(drm_device_t *device,
                                  struct drm_mode_fb_cmd *command, fd_t *fd);
int na_drm_add_framebuffer2(drm_device_t *device,
                            struct drm_mode_fb_cmd2 *command, fd_t *fd);
void na_drm_release_framebuffer(drm_device_t *device,
                                drm_framebuffer_t *framebuffer);
int na_drm_dirty_framebuffer(drm_device_t *device,
                             struct drm_mode_fb_dirty_cmd *command, fd_t *fd);
int na_drm_set_plane(drm_device_t *device, struct drm_mode_set_plane *command,
                     fd_t *fd);
int na_drm_set_crtc(drm_device_t *device, struct drm_mode_crtc *command,
                    fd_t *fd);
int na_drm_page_flip(drm_device_t *device,
                     struct drm_mode_crtc_page_flip *command, fd_t *fd);
int na_drm_set_cursor(drm_device_t *device, struct drm_mode_cursor *command);
int na_drm_atomic_commit(drm_device_t *device, struct drm_mode_atomic *command,
                         fd_t *fd);
