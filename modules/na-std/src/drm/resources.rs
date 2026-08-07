use core::{ffi::CStr, marker::PhantomData, ptr::NonNull};

use crate::{Error, Result, bindings};

const NO_RESOURCE: u32 = u32::MAX;

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConnectorType {
    Unknown = 0,
    Vga = 1,
    DviIntegrated = 2,
    DviDigital = 3,
    DviAnalog = 4,
    Composite = 5,
    SVideo = 6,
    Lvds = 7,
    Component = 8,
    Din9 = 9,
    DisplayPort = 10,
    HdmiA = 11,
    HdmiB = 12,
    Tv = 13,
    EmbeddedDisplayPort = 14,
    Virtual = 15,
    Dsi = 16,
    Dpi = 17,
    Writeback = 18,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Connection {
    Connected = 1,
    Disconnected = 2,
    Unknown = 3,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EncoderType {
    None = 0,
    Dac = 1,
    Tmds = 2,
    Lvds = 3,
    TvDac = 4,
    Virtual = 5,
    Dsi = 6,
    DisplayPortMst = 7,
    Dpi = 8,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlaneType {
    Overlay = 0,
    Primary = 1,
    Cursor = 2,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Mode {
    pub clock: u32,
    pub hdisplay: u16,
    pub hsync_start: u16,
    pub hsync_end: u16,
    pub htotal: u16,
    pub hskew: u16,
    pub vdisplay: u16,
    pub vsync_start: u16,
    pub vsync_end: u16,
    pub vtotal: u16,
    pub vscan: u16,
    pub vrefresh: u32,
    pub flags: u32,
    pub mode_type: u32,
    name: [core::ffi::c_char; 32],
}

impl Mode {
    pub const fn new(width: u16, height: u16, refresh: u32) -> Self {
        Self {
            clock: 0,
            hdisplay: width,
            hsync_start: width,
            hsync_end: width,
            htotal: width,
            hskew: 0,
            vdisplay: height,
            vsync_start: height,
            vsync_end: height,
            vtotal: height,
            vscan: 0,
            vrefresh: refresh,
            flags: 0,
            mode_type: 0,
            name: [0; 32],
        }
    }

    pub fn with_name(mut self, name: &CStr) -> Result<Self> {
        let bytes = name.to_bytes();
        if bytes.len() >= self.name.len() {
            return Err(Error::InvalidArgument);
        }
        self.name = [0; 32];
        for (destination, source) in self.name.iter_mut().zip(bytes) {
            *destination = *source as core::ffi::c_char;
        }
        Ok(self)
    }

    pub fn name(&self) -> Option<&CStr> {
        (self.name[0] != 0).then(|| unsafe { CStr::from_ptr(self.name.as_ptr()) })
    }

    pub(crate) fn from_raw(raw: &bindings::DrmModeInfo) -> Self {
        let mut name = raw.name;
        name[31] = 0;
        Self {
            clock: raw.clock,
            hdisplay: raw.hdisplay,
            hsync_start: raw.hsync_start,
            hsync_end: raw.hsync_end,
            htotal: raw.htotal,
            hskew: raw.hskew,
            vdisplay: raw.vdisplay,
            vsync_start: raw.vsync_start,
            vsync_end: raw.vsync_end,
            vtotal: raw.vtotal,
            vscan: raw.vscan,
            vrefresh: raw.vrefresh,
            flags: raw.flags,
            mode_type: raw.mode_type,
            name,
        }
    }

    fn raw(self) -> bindings::DrmModeInfo {
        bindings::DrmModeInfo {
            clock: self.clock,
            hdisplay: self.hdisplay,
            hsync_start: self.hsync_start,
            hsync_end: self.hsync_end,
            htotal: self.htotal,
            hskew: self.hskew,
            vdisplay: self.vdisplay,
            vsync_start: self.vsync_start,
            vsync_end: self.vsync_end,
            vtotal: self.vtotal,
            vscan: self.vscan,
            vrefresh: self.vrefresh,
            flags: self.flags,
            mode_type: self.mode_type,
            name: self.name,
        }
    }
}

#[derive(Clone, Copy)]
pub struct Connector<'a> {
    pub connector_type: ConnectorType,
    pub connection: Connection,
    pub encoder_index: Option<u32>,
    pub crtc_index: Option<u32>,
    pub mm_width: u32,
    pub mm_height: u32,
    pub subpixel: u32,
    pub modes: &'a [Mode],
}

#[derive(Clone, Copy)]
pub struct Crtc {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub gamma_size: u32,
    pub mode: Option<Mode>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Encoder {
    pub encoder_type: EncoderType,
    pub crtc_index: Option<u32>,
    pub possible_crtcs: u32,
    pub possible_clones: u32,
}

#[derive(Clone, Copy)]
pub struct Plane<'a> {
    pub crtc_index: Option<u32>,
    pub possible_crtcs: u32,
    pub gamma_size: u32,
    pub plane_type: PlaneType,
    pub formats: &'a [u32],
}

trait RawResource {
    unsafe fn destroy(raw: *mut Self);
}

impl RawResource for bindings::DrmConnector {
    unsafe fn destroy(raw: *mut Self) {
        unsafe { bindings::na_drm_connector_destroy(raw) };
    }
}

impl RawResource for bindings::DrmCrtc {
    unsafe fn destroy(raw: *mut Self) {
        unsafe { bindings::na_drm_crtc_destroy(raw) };
    }
}

impl RawResource for bindings::DrmEncoder {
    unsafe fn destroy(raw: *mut Self) {
        unsafe { bindings::na_drm_encoder_destroy(raw) };
    }
}

impl RawResource for bindings::DrmPlane {
    unsafe fn destroy(raw: *mut Self) {
        unsafe { bindings::na_drm_plane_destroy(raw) };
    }
}

struct ResourceList<'a, T: RawResource> {
    output: NonNull<*mut T>,
    length: u32,
    capacity: u32,
    committed: bool,
    lifetime: PhantomData<&'a mut [*mut T]>,
}

impl<'a, T: RawResource> ResourceList<'a, T> {
    unsafe fn from_raw(output: *mut *mut T, capacity: u32) -> Option<Self> {
        NonNull::new(output).map(|output| Self {
            output,
            length: 0,
            capacity,
            committed: false,
            lifetime: PhantomData,
        })
    }

    fn push(&mut self, raw: NonNull<T>) -> Result<()> {
        if self.length == self.capacity {
            unsafe { T::destroy(raw.as_ptr()) };
            return Err(Error::NoSpace);
        }
        unsafe {
            self.output
                .as_ptr()
                .add(self.length as usize)
                .write(raw.as_ptr())
        };
        self.length += 1;
        Ok(())
    }

    unsafe fn finish(mut self, count: *mut u32) -> i32 {
        let Some(count) = NonNull::new(count) else {
            return Error::InvalidArgument.status();
        };
        unsafe { count.as_ptr().write(self.length) };
        self.committed = true;
        0
    }
}

impl<T: RawResource> Drop for ResourceList<'_, T> {
    fn drop(&mut self) {
        if self.committed {
            return;
        }
        for index in 0..self.length {
            let raw = unsafe { self.output.as_ptr().add(index as usize).read() };
            unsafe { T::destroy(raw) };
        }
    }
}

pub struct ConnectorList<'a>(ResourceList<'a, bindings::DrmConnector>);

impl ConnectorList<'_> {
    pub fn push(&mut self, connector: Connector<'_>) -> Result<()> {
        let info = bindings::DrmConnectorInfo {
            connector_type: connector.connector_type as u32,
            connection: connector.connection as u32,
            encoder_index: connector.encoder_index.unwrap_or(NO_RESOURCE),
            crtc_index: connector.crtc_index.unwrap_or(NO_RESOURCE),
            mm_width: connector.mm_width,
            mm_height: connector.mm_height,
            subpixel: connector.subpixel,
        };
        let raw = NonNull::new(unsafe { bindings::na_drm_connector_create(&info) })
            .ok_or(Error::OutOfMemory)?;
        for mode in connector.modes {
            let status = unsafe { bindings::na_drm_connector_add_mode(raw.as_ptr(), &mode.raw()) };
            if let Err(error) = Error::from_status(status) {
                unsafe { bindings::na_drm_connector_destroy(raw.as_ptr()) };
                return Err(error);
            }
        }
        self.0.push(raw)
    }

    pub(crate) unsafe fn from_raw(
        output: *mut *mut bindings::DrmConnector,
        capacity: u32,
    ) -> Option<Self> {
        unsafe { ResourceList::from_raw(output, capacity) }.map(Self)
    }

    pub(crate) unsafe fn finish(self, count: *mut u32) -> i32 {
        unsafe { self.0.finish(count) }
    }
}

pub struct CrtcList<'a>(ResourceList<'a, bindings::DrmCrtc>);

impl CrtcList<'_> {
    pub fn push(&mut self, crtc: Crtc) -> Result<()> {
        let info = bindings::DrmCrtcInfo {
            x: crtc.x,
            y: crtc.y,
            width: crtc.width,
            height: crtc.height,
            gamma_size: crtc.gamma_size,
            mode_valid: crtc.mode.is_some(),
            mode_info: crtc.mode.map_or_else(Default::default, Mode::raw),
        };
        let raw = NonNull::new(unsafe { bindings::na_drm_crtc_create(&info) })
            .ok_or(Error::OutOfMemory)?;
        self.0.push(raw)
    }

    pub(crate) unsafe fn from_raw(
        output: *mut *mut bindings::DrmCrtc,
        capacity: u32,
    ) -> Option<Self> {
        unsafe { ResourceList::from_raw(output, capacity) }.map(Self)
    }

    pub(crate) unsafe fn finish(self, count: *mut u32) -> i32 {
        unsafe { self.0.finish(count) }
    }
}

pub struct EncoderList<'a>(ResourceList<'a, bindings::DrmEncoder>);

impl EncoderList<'_> {
    pub fn push(&mut self, encoder: Encoder) -> Result<()> {
        let info = bindings::DrmEncoderInfo {
            encoder_type: encoder.encoder_type as u32,
            crtc_index: encoder.crtc_index.unwrap_or(NO_RESOURCE),
            possible_crtcs: encoder.possible_crtcs,
            possible_clones: encoder.possible_clones,
        };
        let raw = NonNull::new(unsafe { bindings::na_drm_encoder_create(&info) })
            .ok_or(Error::OutOfMemory)?;
        self.0.push(raw)
    }

    pub(crate) unsafe fn from_raw(
        output: *mut *mut bindings::DrmEncoder,
        capacity: u32,
    ) -> Option<Self> {
        unsafe { ResourceList::from_raw(output, capacity) }.map(Self)
    }

    pub(crate) unsafe fn finish(self, count: *mut u32) -> i32 {
        unsafe { self.0.finish(count) }
    }
}

pub struct PlaneList<'a> {
    resources: ResourceList<'a, bindings::DrmPlane>,
    device: NonNull<bindings::DrmDevice>,
}

impl PlaneList<'_> {
    pub fn push(&mut self, plane: Plane<'_>) -> Result<()> {
        let info = bindings::DrmPlaneInfo {
            crtc_index: plane.crtc_index.unwrap_or(NO_RESOURCE),
            possible_crtcs: plane.possible_crtcs,
            gamma_size: plane.gamma_size,
            plane_type: plane.plane_type as u32,
        };
        let raw =
            NonNull::new(unsafe { bindings::na_drm_plane_create(self.device.as_ptr(), &info) })
                .ok_or(Error::OutOfMemory)?;
        for format in plane.formats {
            let status = unsafe { bindings::na_drm_plane_add_format(raw.as_ptr(), *format) };
            if let Err(error) = Error::from_status(status) {
                unsafe { bindings::na_drm_plane_destroy(raw.as_ptr()) };
                return Err(error);
            }
        }
        self.resources.push(raw)
    }

    pub(crate) unsafe fn from_raw(
        device: *mut bindings::DrmDevice,
        output: *mut *mut bindings::DrmPlane,
        capacity: u32,
    ) -> Option<Self> {
        Some(Self {
            resources: unsafe { ResourceList::from_raw(output, capacity) }?,
            device: NonNull::new(device)?,
        })
    }

    pub(crate) unsafe fn finish(self, count: *mut u32) -> i32 {
        unsafe { self.resources.finish(count) }
    }
}
