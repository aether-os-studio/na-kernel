use core::slice;

use crate::{Error, Result, bindings, memory::PhysicalAddress};

use super::device::FileId;

use super::resources::{Mode, PlaneType};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DisplayInfo {
    pub width: u32,
    pub height: u32,
    pub bits_per_pixel: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FramebufferInfo {
    pub display: DisplayInfo,
    pub physical_address: PhysicalAddress,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DumbBufferRequest {
    pub width: u32,
    pub height: u32,
    pub bits_per_pixel: u32,
    pub flags: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DumbBuffer {
    pub handle: u32,
    pub pitch: u32,
    pub size: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DumbBufferMapping {
    pub physical_address: PhysicalAddress,
    pub length: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FramebufferFormat {
    pub bits_per_pixel: u32,
    pub depth: u32,
    /// Driver-private reference retained by the KMS framebuffer object.
    pub driver_handle: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FramebufferRequest {
    pub file: Option<FileId>,
    pub width: u32,
    pub height: u32,
    pub pixel_format: u32,
    pub flags: u32,
    pub handles: [u32; 4],
    pub pitches: [u32; 4],
    pub offsets: [u32; 4],
    pub modifiers: [u64; 4],
}

impl From<&bindings::DrmFramebufferRequest> for FramebufferRequest {
    fn from(raw: &bindings::DrmFramebufferRequest) -> Self {
        Self {
            file: (raw.file_id != 0).then(|| FileId::from_raw(raw.file_id)),
            width: raw.width,
            height: raw.height,
            pixel_format: raw.pixel_format,
            flags: raw.flags,
            handles: raw.handles,
            pitches: raw.pitches,
            offsets: raw.offsets,
            modifiers: raw.modifiers,
        }
    }
}

#[repr(transparent)]
#[derive(Clone, Copy, Default)]
pub struct Clip(bindings::DrmClip);

impl Clip {
    pub const fn x1(&self) -> u16 {
        self.0.x1
    }

    pub const fn y1(&self) -> u16 {
        self.0.y1
    }

    pub const fn x2(&self) -> u16 {
        self.0.x2
    }

    pub const fn y2(&self) -> u16 {
        self.0.y2
    }
}

pub struct FramebufferUpdate<'a> {
    pub framebuffer_id: u32,
    pub framebuffer_handle: u32,
    pub flags: u32,
    pub color: u32,
    pub clips: &'a [Clip],
}

impl FramebufferUpdate<'_> {
    pub(crate) unsafe fn from_raw(
        framebuffer_id: u32,
        framebuffer_handle: u32,
        flags: u32,
        color: u32,
        clips: *const bindings::DrmClip,
        clip_count: u32,
    ) -> Self {
        let clips = if clip_count == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(clips.cast::<Clip>(), clip_count as usize) }
        };
        Self {
            framebuffer_id,
            framebuffer_handle,
            flags,
            color,
            clips,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PlaneUpdate {
    pub plane_id: u32,
    pub plane_type: PlaneType,
    pub crtc_id: u32,
    pub framebuffer_id: u32,
    pub framebuffer_handle: u32,
    pub flags: u32,
    pub crtc_x: i32,
    pub crtc_y: i32,
    pub crtc_width: u32,
    pub crtc_height: u32,
    pub source_x: u32,
    pub source_y: u32,
    pub source_width: u32,
    pub source_height: u32,
}

impl From<&bindings::DrmPlaneUpdate> for PlaneUpdate {
    fn from(raw: &bindings::DrmPlaneUpdate) -> Self {
        Self {
            plane_id: raw.plane_id,
            plane_type: match raw.plane_type {
                1 => PlaneType::Primary,
                2 => PlaneType::Cursor,
                _ => PlaneType::Overlay,
            },
            crtc_id: raw.crtc_id,
            framebuffer_id: raw.framebuffer_id,
            framebuffer_handle: raw.framebuffer_handle,
            flags: raw.flags,
            crtc_x: raw.crtc_x,
            crtc_y: raw.crtc_y,
            crtc_width: raw.crtc_width,
            crtc_height: raw.crtc_height,
            source_x: raw.source_x,
            source_y: raw.source_y,
            source_width: raw.source_width,
            source_height: raw.source_height,
        }
    }
}

pub struct CrtcUpdate<'a> {
    pub crtc_id: u32,
    pub framebuffer_id: u32,
    pub framebuffer_handle: u32,
    pub x: u32,
    pub y: u32,
    pub gamma_size: u32,
    pub mode: Option<Mode>,
    pub connector_ids: &'a [u32],
}

impl CrtcUpdate<'_> {
    pub(crate) unsafe fn from_raw<'a>(
        raw: &bindings::DrmCrtcUpdate,
        connector_ids: *const u32,
        connector_count: u32,
    ) -> CrtcUpdate<'a> {
        let connector_ids = if connector_count == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(connector_ids, connector_count as usize) }
        };
        CrtcUpdate {
            crtc_id: raw.crtc_id,
            framebuffer_id: raw.framebuffer_id,
            framebuffer_handle: raw.framebuffer_handle,
            x: raw.x,
            y: raw.y,
            gamma_size: raw.gamma_size,
            mode: raw.mode_valid.then(|| Mode::from_raw(&raw.mode_info)),
            connector_ids,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PageFlip {
    pub crtc_id: u32,
    pub framebuffer_id: u32,
    pub framebuffer_handle: u32,
    pub flags: u32,
    pub user_data: u64,
}

impl From<&bindings::DrmPageFlip> for PageFlip {
    fn from(raw: &bindings::DrmPageFlip) -> Self {
        Self {
            crtc_id: raw.crtc_id,
            framebuffer_id: raw.framebuffer_id,
            framebuffer_handle: raw.framebuffer_handle,
            flags: raw.flags,
            user_data: raw.user_data,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CursorUpdate {
    pub file: FileId,
    pub flags: u32,
    pub crtc_id: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub handle: u32,
}

impl From<&bindings::DrmCursorUpdate> for CursorUpdate {
    fn from(raw: &bindings::DrmCursorUpdate) -> Self {
        Self {
            file: FileId::from_raw(raw.file_id),
            flags: raw.flags,
            crtc_id: raw.crtc_id,
            x: raw.x,
            y: raw.y,
            width: raw.width,
            height: raw.height,
            handle: raw.handle,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PropertyObject {
    Crtc,
    Connector,
    Plane,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PropertyUpdate {
    pub object_id: u32,
    pub object: PropertyObject,
    pub property_id: u32,
    pub value: u64,
}

impl TryFrom<&bindings::DrmPropertyUpdate> for PropertyUpdate {
    type Error = Error;

    fn try_from(raw: &bindings::DrmPropertyUpdate) -> Result<Self> {
        let object = match raw.object_type {
            0xcccc_cccc => PropertyObject::Crtc,
            0xc0c0_c0c0 => PropertyObject::Connector,
            0xeeee_eeee => PropertyObject::Plane,
            _ => return Err(Error::InvalidArgument),
        };
        Ok(Self {
            object_id: raw.object_id,
            object,
            property_id: raw.property_id,
            value: raw.value,
        })
    }
}

#[repr(transparent)]
#[derive(Clone, Copy, Default)]
pub struct AtomicProperty(bindings::DrmAtomicProperty);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AtomicObject {
    Plane(PlaneType),
    Crtc,
    Connector,
}

impl AtomicProperty {
    pub const fn object_id(&self) -> u32 {
        self.0.object_id
    }

    pub const fn property_id(&self) -> u32 {
        self.0.property_id
    }

    pub fn object(&self) -> Result<AtomicObject> {
        match self.0.object_type {
            0xeeee_eeee => match self.0.plane_type {
                0 => Ok(AtomicObject::Plane(PlaneType::Overlay)),
                1 => Ok(AtomicObject::Plane(PlaneType::Primary)),
                2 => Ok(AtomicObject::Plane(PlaneType::Cursor)),
                _ => Err(Error::InvalidArgument),
            },
            0xcccc_cccc => Ok(AtomicObject::Crtc),
            0xc0c0_c0c0 => Ok(AtomicObject::Connector),
            _ => Err(Error::InvalidArgument),
        }
    }

    pub const fn value(&self) -> u64 {
        self.0.value
    }

    pub const fn framebuffer_handle(&self) -> Option<u32> {
        if self.0.framebuffer_handle == 0 {
            None
        } else {
            Some(self.0.framebuffer_handle)
        }
    }

    pub fn mode(&self) -> Option<Mode> {
        unsafe { self.0.mode_info.as_ref() }.map(Mode::from_raw)
    }
}

pub struct AtomicCommit<'a> {
    pub flags: u32,
    pub user_data: u64,
    pub properties: &'a [AtomicProperty],
}

impl AtomicCommit<'_> {
    pub(crate) unsafe fn from_raw<'a>(
        flags: u32,
        user_data: u64,
        properties: *const bindings::DrmAtomicProperty,
        property_count: usize,
    ) -> AtomicCommit<'a> {
        let properties = if property_count == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(properties.cast::<AtomicProperty>(), property_count) }
        };
        AtomicCommit {
            flags,
            user_data,
            properties,
        }
    }
}
