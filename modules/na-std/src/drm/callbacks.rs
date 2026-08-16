use core::{marker::PhantomData, ptr::NonNull, slice};

use crate::{Error, bindings};

use super::{
    device::{Driver, FileId, Ioctl, MmapRequest},
    modeset::{
        AtomicCommit, CrtcUpdate, CursorUpdate, DumbBufferRequest, FramebufferRequest,
        FramebufferUpdate, PageFlip, PlaneUpdate,
    },
    resources::{ConnectorList, CrtcList, EncoderList, PlaneList},
};

pub(super) struct Callbacks<D>(PhantomData<D>);

impl<D: Driver> Callbacks<D> {
    unsafe fn driver(context: *mut core::ffi::c_void) -> Option<&'static D> {
        (!context.is_null()).then(|| unsafe { &*context.cast::<D>() })
    }

    pub(super) unsafe extern "C" fn open(context: *mut core::ffi::c_void, file_id: u64) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        driver
            .open(FileId::from_raw(file_id))
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn close(context: *mut core::ffi::c_void, file_id: u64) {
        if let Some(driver) = unsafe { Self::driver(context) } {
            driver.close(FileId::from_raw(file_id));
        }
    }

    pub(super) unsafe extern "C" fn driver_ioctl(
        context: *mut core::ffi::c_void,
        command: u32,
        arg: *mut core::ffi::c_void,
        arg_size: usize,
        render_node: bool,
        file_id: u64,
    ) -> i64 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status() as i64;
        };
        if arg_size != 0 && arg.is_null() {
            return Error::InvalidArgument.status() as i64;
        }
        let bytes = if arg_size == 0 {
            &mut []
        } else {
            unsafe { slice::from_raw_parts_mut(arg.cast::<u8>(), arg_size) }
        };
        match driver.driver_ioctl(Ioctl {
            command,
            arg: bytes,
            render_node,
            file: (file_id != 0).then(|| FileId::from_raw(file_id)),
        }) {
            Ok(size) => size as i64,
            Err(error) => error.status() as i64,
        }
    }

    pub(super) unsafe extern "C" fn mmap(
        context: *mut core::ffi::c_void,
        file_id: u64,
        offset: u64,
        length: u64,
        physical_address: *mut u64,
    ) -> i32 {
        let (Some(driver), Some(physical_address)) = (
            unsafe { Self::driver(context) },
            NonNull::new(physical_address),
        ) else {
            return Error::InvalidArgument.status();
        };
        let Ok(length) = usize::try_from(length) else {
            return Error::InvalidArgument.status();
        };
        if file_id == 0 || length == 0 {
            return Error::InvalidArgument.status();
        }
        match driver.mmap(MmapRequest {
            file: FileId::from_raw(file_id),
            offset,
            length,
        }) {
            Ok(result) => {
                unsafe { physical_address.as_ptr().write(result.get()) };
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn prime_export(
        context: *mut core::ffi::c_void,
        file_id: u64,
        handle: u32,
        physical_address: *mut u64,
        size: *mut u64,
        token: *mut u64,
    ) -> i32 {
        let (Some(driver), Some(physical_address), Some(size), Some(token)) = (
            unsafe { Self::driver(context) },
            NonNull::new(physical_address),
            NonNull::new(size),
            NonNull::new(token),
        ) else {
            return Error::InvalidArgument.status();
        };
        if file_id == 0 {
            return Error::InvalidArgument.status();
        }
        match driver.prime_export(FileId::from_raw(file_id), handle) {
            Ok(buffer) => {
                unsafe {
                    physical_address
                        .as_ptr()
                        .write(buffer.physical_address.get());
                    size.as_ptr().write(buffer.length as u64);
                    token.as_ptr().write(buffer.token);
                }
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn prime_import(
        context: *mut core::ffi::c_void,
        file_id: u64,
        token: u64,
        handle: *mut u32,
    ) -> i32 {
        let (Some(driver), Some(handle)) = (unsafe { Self::driver(context) }, NonNull::new(handle))
        else {
            return Error::InvalidArgument.status();
        };
        if file_id == 0 || token == 0 {
            return Error::InvalidArgument.status();
        }
        match driver.prime_import(FileId::from_raw(file_id), token) {
            Ok(result) => {
                unsafe { handle.as_ptr().write(result) };
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn prime_release(
        context: *mut core::ffi::c_void,
        token: u64,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        driver
            .prime_release(token)
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn get_capability(
        context: *mut core::ffi::c_void,
        capability: u64,
        value: *mut u64,
    ) -> i32 {
        let (Some(driver), Some(value)) = (unsafe { Self::driver(context) }, NonNull::new(value))
        else {
            return Error::InvalidArgument.status();
        };
        match driver.capability(capability) {
            Ok(result) => {
                unsafe { value.as_ptr().write(result) };
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_display_info(
        context: *mut core::ffi::c_void,
        width: *mut u32,
        height: *mut u32,
        bits_per_pixel: *mut u32,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        if width.is_null() || height.is_null() || bits_per_pixel.is_null() {
            return Error::InvalidArgument.status();
        }
        match driver.display_info() {
            Ok(info) => {
                unsafe {
                    width.write(info.width);
                    height.write(info.height);
                    bits_per_pixel.write(info.bits_per_pixel);
                }
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_framebuffer(
        context: *mut core::ffi::c_void,
        width: *mut u32,
        height: *mut u32,
        bits_per_pixel: *mut u32,
        physical_address: *mut u64,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        if width.is_null()
            || height.is_null()
            || bits_per_pixel.is_null()
            || physical_address.is_null()
        {
            return Error::InvalidArgument.status();
        }
        match driver.framebuffer() {
            Ok(info) => {
                unsafe {
                    width.write(info.display.width);
                    height.write(info.display.height);
                    bits_per_pixel.write(info.display.bits_per_pixel);
                    physical_address.write(info.physical_address.get());
                }
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn create_dumb_buffer(
        context: *mut core::ffi::c_void,
        raw: *mut bindings::DrmDumbBuffer,
    ) -> i32 {
        let (Some(driver), Some(mut raw)) = (unsafe { Self::driver(context) }, NonNull::new(raw))
        else {
            return Error::InvalidArgument.status();
        };
        let input = unsafe { raw.as_ref() };
        let request = DumbBufferRequest {
            width: input.width,
            height: input.height,
            bits_per_pixel: input.bits_per_pixel,
            flags: input.flags,
        };
        match driver.create_dumb_buffer(request) {
            Ok(buffer) => {
                let output = unsafe { raw.as_mut() };
                output.handle = buffer.handle;
                output.pitch = buffer.pitch;
                output.size = buffer.size;
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn destroy_dumb_buffer(
        context: *mut core::ffi::c_void,
        handle: u32,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        driver
            .destroy_dumb_buffer(handle)
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn map_dumb_buffer(
        context: *mut core::ffi::c_void,
        handle: u32,
        offset: *mut u64,
    ) -> i32 {
        let (Some(driver), Some(offset)) = (unsafe { Self::driver(context) }, NonNull::new(offset))
        else {
            return Error::InvalidArgument.status();
        };
        match driver.map_dumb_buffer(handle) {
            Ok(result) => {
                unsafe { offset.as_ptr().write(result) };
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_dumb_buffer_mapping(
        context: *mut core::ffi::c_void,
        handle: u32,
        physical_address: *mut u64,
        size: *mut u64,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        if physical_address.is_null() || size.is_null() {
            return Error::InvalidArgument.status();
        }
        match driver.dumb_buffer_mapping(handle) {
            Ok(mapping) => {
                unsafe {
                    physical_address.write(mapping.physical_address.get());
                    size.write(mapping.length as u64);
                }
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_connectors(
        context: *mut core::ffi::c_void,
        _device: *mut bindings::DrmDevice,
        output: *mut *mut bindings::DrmConnector,
        capacity: u32,
        count: *mut u32,
    ) -> i32 {
        let (Some(driver), Some(mut resources)) = (unsafe { Self::driver(context) }, unsafe {
            ConnectorList::from_raw(output, capacity)
        }) else {
            return Error::InvalidArgument.status();
        };
        match driver.connectors(&mut resources) {
            Ok(()) => unsafe { resources.finish(count) },
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_crtcs(
        context: *mut core::ffi::c_void,
        _device: *mut bindings::DrmDevice,
        output: *mut *mut bindings::DrmCrtc,
        capacity: u32,
        count: *mut u32,
    ) -> i32 {
        let (Some(driver), Some(mut resources)) = (unsafe { Self::driver(context) }, unsafe {
            CrtcList::from_raw(output, capacity)
        }) else {
            return Error::InvalidArgument.status();
        };
        match driver.crtcs(&mut resources) {
            Ok(()) => unsafe { resources.finish(count) },
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_encoders(
        context: *mut core::ffi::c_void,
        _device: *mut bindings::DrmDevice,
        output: *mut *mut bindings::DrmEncoder,
        capacity: u32,
        count: *mut u32,
    ) -> i32 {
        let (Some(driver), Some(mut resources)) = (unsafe { Self::driver(context) }, unsafe {
            EncoderList::from_raw(output, capacity)
        }) else {
            return Error::InvalidArgument.status();
        };
        match driver.encoders(&mut resources) {
            Ok(()) => unsafe { resources.finish(count) },
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn get_planes(
        context: *mut core::ffi::c_void,
        device: *mut bindings::DrmDevice,
        output: *mut *mut bindings::DrmPlane,
        capacity: u32,
        count: *mut u32,
    ) -> i32 {
        let (Some(driver), Some(mut resources)) = (unsafe { Self::driver(context) }, unsafe {
            PlaneList::from_raw(device, output, capacity)
        }) else {
            return Error::InvalidArgument.status();
        };
        match driver.planes(&mut resources) {
            Ok(()) => unsafe { resources.finish(count) },
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn create_framebuffer(
        context: *mut core::ffi::c_void,
        raw: *mut bindings::DrmFramebufferRequest,
    ) -> i32 {
        let (Some(driver), Some(mut raw)) = (unsafe { Self::driver(context) }, NonNull::new(raw))
        else {
            return Error::InvalidArgument.status();
        };
        let request = FramebufferRequest::from(unsafe { raw.as_ref() });
        match driver.create_framebuffer(request) {
            Ok(format) => {
                let output = unsafe { raw.as_mut() };
                output.bits_per_pixel = format.bits_per_pixel;
                output.depth = format.depth;
                output.driver_handle = format.driver_handle;
                0
            }
            Err(error) => error.status(),
        }
    }

    pub(super) unsafe extern "C" fn release_framebuffer(
        context: *mut core::ffi::c_void,
        handle: u32,
    ) {
        if let Some(driver) = unsafe { Self::driver(context) } {
            driver.release_framebuffer(handle);
        }
    }

    pub(super) unsafe extern "C" fn dirty_framebuffer(
        context: *mut core::ffi::c_void,
        framebuffer_id: u32,
        framebuffer_handle: u32,
        flags: u32,
        color: u32,
        clips: *const bindings::DrmClip,
        clip_count: u32,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        if clip_count != 0 && clips.is_null() {
            return Error::InvalidArgument.status();
        }
        let update = unsafe {
            FramebufferUpdate::from_raw(
                framebuffer_id,
                framebuffer_handle,
                flags,
                color,
                clips,
                clip_count,
            )
        };
        driver
            .dirty_framebuffer(update)
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn set_plane(
        context: *mut core::ffi::c_void,
        raw: *const bindings::DrmPlaneUpdate,
    ) -> i32 {
        let (Some(driver), Some(raw)) = (unsafe { Self::driver(context) }, unsafe { raw.as_ref() })
        else {
            return Error::InvalidArgument.status();
        };
        driver
            .set_plane(PlaneUpdate::from(raw))
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn get_framebuffer_handle(
        context: *mut core::ffi::c_void,
        file_id: u64,
        framebuffer_handle: u32,
        gem_handle: *mut u32,
    ) -> i32 {
        let (Some(driver), Some(gem_handle)) = (unsafe { Self::driver(context) }, unsafe {
            gem_handle.as_mut()
        }) else {
            return Error::InvalidArgument.status();
        };
        driver
            .framebuffer_handle(FileId::from_raw(file_id), framebuffer_handle)
            .map_or_else(Error::status, |handle| {
                *gem_handle = handle;
                0
            })
    }

    pub(super) unsafe extern "C" fn set_crtc(
        context: *mut core::ffi::c_void,
        raw: *const bindings::DrmCrtcUpdate,
        connector_ids: *const u32,
        connector_count: u32,
    ) -> i32 {
        let (Some(driver), Some(raw)) = (unsafe { Self::driver(context) }, unsafe { raw.as_ref() })
        else {
            return Error::InvalidArgument.status();
        };
        if connector_count != 0 && connector_ids.is_null() {
            return Error::InvalidArgument.status();
        }
        let update = unsafe { CrtcUpdate::from_raw(raw, connector_ids, connector_count) };
        driver.set_crtc(update).map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn page_flip(
        context: *mut core::ffi::c_void,
        raw: *const bindings::DrmPageFlip,
    ) -> i32 {
        let (Some(driver), Some(raw)) = (unsafe { Self::driver(context) }, unsafe { raw.as_ref() })
        else {
            return Error::InvalidArgument.status();
        };
        driver
            .page_flip(PageFlip::from(raw))
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn set_cursor(
        context: *mut core::ffi::c_void,
        raw: *const bindings::DrmCursorUpdate,
    ) -> i32 {
        let (Some(driver), Some(raw)) = (unsafe { Self::driver(context) }, unsafe { raw.as_ref() })
        else {
            return Error::InvalidArgument.status();
        };
        driver
            .set_cursor(CursorUpdate::from(raw))
            .map_or_else(Error::status, |()| 0)
    }

    pub(super) unsafe extern "C" fn atomic_commit(
        context: *mut core::ffi::c_void,
        flags: u32,
        user_data: u64,
        properties: *const bindings::DrmAtomicProperty,
        property_count: usize,
    ) -> i32 {
        let Some(driver) = (unsafe { Self::driver(context) }) else {
            return Error::InvalidArgument.status();
        };
        if property_count != 0 && properties.is_null() {
            return Error::InvalidArgument.status();
        }
        let commit =
            unsafe { AtomicCommit::from_raw(flags, user_data, properties, property_count) };
        driver
            .atomic_commit(commit)
            .map_or_else(Error::status, |()| 0)
    }
}
