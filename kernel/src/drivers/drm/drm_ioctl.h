/*
 * DRM ioctl declarations
 *
 * This file contains declarations for DRM ioctl handling functions.
 * It separates the ioctl implementation from the core driver framework.
 */

#ifndef _DRM_IOCTL_H
#define _DRM_IOCTL_H

#include "drm.h"

/**
 * drm_ioctl - Main DRM ioctl handler
 * @data: DRM device pointer
 * @cmd: IOCTL command
 * @arg: IOCTL argument
 *
 * Handles all DRM ioctl commands. Returns 0 on success, negative error code on
 * failure.
 */
ssize_t drm_ioctl(void *data, ssize_t cmd, ssize_t arg, fd_t *fd);

/**
 * drm_ioctl_version - Handle DRM_IOCTL_VERSION
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_version(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_get_cap - Handle DRM_IOCTL_GET_CAP
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_get_cap(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_gem_close - Handle DRM_IOCTL_GEM_CLOSE
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_gem_close(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_prime_handle_to_fd - Handle DRM_IOCTL_PRIME_HANDLE_TO_FD
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_prime_handle_to_fd(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_prime_fd_to_handle - Handle DRM_IOCTL_PRIME_FD_TO_HANDLE
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_prime_fd_to_handle(drm_device_t *dev, void *arg, fd_t *fd);

ssize_t drm_primefd_create(drm_device_t *dev, uint32_t handle, uint64_t phys,
                           uint64_t size, uint64_t token, uint32_t flags);

/* Driver-facing syncobj bridge used by private command submission ioctls. */
int na_drm_syncobj_wait(uint64_t file_id, uint32_t handle, uint64_t point,
                        int64_t timeout_ns);
int na_drm_syncobj_signal(uint64_t file_id, uint32_t handle, uint64_t point);
int na_drm_syncobj_attach_fence(uint64_t file_id, uint32_t handle,
                                uint64_t point, uint32_t timeline,
                                uint64_t cpu_address, uint64_t value);

/**
 * drm_ioctl_mode_getresources - Handle DRM_IOCTL_MODE_GETRESOURCES
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getresources(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_getcrtc - Handle DRM_IOCTL_MODE_GETCRTC
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getcrtc(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_getencoder - Handle DRM_IOCTL_MODE_GETENCODER
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getencoder(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_create_dumb - Handle DRM_IOCTL_MODE_CREATE_DUMB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_create_dumb(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_map_dumb - Handle DRM_IOCTL_MODE_MAP_DUMB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_map_dumb(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_getconnector - Handle DRM_IOCTL_MODE_GETCONNECTOR
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getconnector(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_getfb - Handle DRM_IOCTL_MODE_GETFB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getfb(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_addfb - Handle DRM_IOCTL_MODE_ADDFB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_addfb(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_addfb2 - Handle DRM_IOCTL_MODE_ADDFB2
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_addfb2(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_closefb - Handle DRM_IOCTL_MODE_CLOSEFB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_closefb(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_setcrtc - Handle DRM_IOCTL_MODE_SETCRTC
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_setcrtc(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_getplaneresources - Handle DRM_IOCTL_MODE_GETPLANERESOURCES
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getplaneresources(drm_device_t *dev, void *arg,
                                         fd_t *fd);

/**
 * drm_ioctl_mode_getplane - Handle DRM_IOCTL_MODE_GETPLANE
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getplane(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_setplane - Handle DRM_IOCTL_MODE_SETPLANE
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_setplane(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_getproperty - Handle DRM_IOCTL_MODE_GETPROPERTY
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getproperty(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_getpropblob - Handle DRM_IOCTL_MODE_GETPROPBLOB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_getpropblob(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_createpropblob - Handle DRM_IOCTL_MODE_CREATEPROPBLOB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_createpropblob(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_destroypropblob - Handle DRM_IOCTL_MODE_DESTROYPROPBLOB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_destroypropblob(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_mode_obj_getproperties - Handle DRM_IOCTL_MODE_OBJ_GETPROPERTIES
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_obj_getproperties(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_set_client_cap - Handle DRM_IOCTL_SET_CLIENT_CAP
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_set_client_cap(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_wait_vblank - Handle DRM_IOCTL_WAIT_VBLANK
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_wait_vblank(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_get_unique - Handle DRM_IOCTL_GET_UNIQUE
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_get_unique(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_page_flip - Handle DRM_IOCTL_MODE_PAGE_FLIP
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_page_flip(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_cursor - Handle DRM_IOCTL_MODE_CURSOR
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_cursor(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_curso2r - Handle DRM_IOCTL_MODE_CURSOR2
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_cursor2(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_get_magic - Handle DRM_IOCTL_GET_MAGIC
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_get_magic(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_get_client - Query the current DRM file authentication state
 */
ssize_t drm_ioctl_get_client(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_auth_magic - Handle DRM_IOCTL_AUTH_MAGIC
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_auth_magic(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_set_master - Handle DRM_IOCTL_SET_MASTER
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_set_master(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_drop_master - Handle DRM_IOCTL_DROP_MASTER
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_drop_master(drm_device_t *dev, void *arg);

/**
 * drm_ioctl_gamma - Handle DRM_IOCTL_MODE_GETGAMMA/DRM_IOCTL_MODE_SETGAMMA
 * @dev: DRM device
 * @arg: ioctl argument
 * @cmd: command (GET or SET)
 */
ssize_t drm_ioctl_gamma(drm_device_t *dev, void *arg, ssize_t cmd);

/**
 * drm_ioctl_dirtyfb - Handle DRM_IOCTL_MODE_DIRTYFB
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_dirtyfb(drm_device_t *dev, void *arg, fd_t *fd);

/**
 * drm_ioctl_mode_list_lessees - Handle DRM_IOCTL_MODE_LIST_LESSEES
 * @dev: DRM device
 * @arg: ioctl argument
 */
ssize_t drm_ioctl_mode_list_lessees(drm_device_t *dev, void *arg, fd_t *fd);
ssize_t drm_ioctl_mode_create_lease(drm_device_t *dev, void *arg, fd_t *fd);
ssize_t drm_ioctl_mode_get_lease(drm_device_t *dev, void *arg, fd_t *fd);
ssize_t drm_ioctl_mode_revoke_lease(drm_device_t *dev, void *arg);

/**
 * drm_property_lookup_blob_data - Look up blob data by blob ID
 * @dev: DRM device
 * @blob_id: blob property ID
 * @data: output pointer to blob data (caller must NOT free)
 * @length: output pointer to blob data length
 *
 * Returns 0 on success, negative error code on failure.
 * The returned data pointer is valid until the blob is destroyed.
 */
int drm_property_lookup_blob_data(drm_device_t *dev, uint32_t blob_id,
                                  const void **data, uint32_t *length);

/**
 * drm_property_get_modeinfo_from_blob - Resolve a mode blob into modeinfo
 * @dev: DRM device
 * @blob_id: blob property ID
 * @mode: output mode info
 *
 * Supports user-created mode blobs and synthetic CRTC MODE_ID blobs.
 * Returns 0 on success, negative error code on failure.
 */
int drm_property_get_modeinfo_from_blob(drm_device_t *dev, uint32_t blob_id,
                                        struct drm_mode_modeinfo *mode);

#endif /* _DRM_IOCTL_H */
