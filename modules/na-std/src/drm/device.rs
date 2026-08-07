use core::{ffi::CStr, ptr::NonNull};

use crate::{Error, Result, bindings, pci};

use super::{
    callbacks::Callbacks,
    modeset::{
        AtomicCommit, CrtcUpdate, CursorUpdate, DisplayInfo, DumbBuffer, DumbBufferMapping,
        DumbBufferRequest, FramebufferFormat, FramebufferInfo, FramebufferRequest,
        FramebufferUpdate, PageFlip, PlaneUpdate,
    },
    resources::{ConnectorList, CrtcList, EncoderList, PlaneList},
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FileId(u64);

impl FileId {
    pub(crate) const fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    pub const fn raw(self) -> u64 {
        self.0
    }
}

pub struct Ioctl<'a> {
    pub command: u32,
    pub arg: &'a mut [u8],
    pub render_node: bool,
    pub file: Option<FileId>,
}

pub trait Driver: Sync + 'static {
    fn open(&self, _file: FileId) -> Result<()> {
        Ok(())
    }

    fn close(&self, _file: FileId) {}

    fn driver_ioctl(&self, _ioctl: Ioctl<'_>) -> Result<usize> {
        Err(Error::Kernel(-(bindings::ENOTTY as i32)))
    }

    fn capability(&self, _capability: u64) -> Result<u64> {
        Err(Error::Kernel(-(bindings::ENOTSUP as i32)))
    }

    fn display_info(&self) -> Result<DisplayInfo> {
        Err(Error::Unsupported)
    }

    fn framebuffer(&self) -> Result<FramebufferInfo> {
        Err(Error::Unsupported)
    }

    fn create_dumb_buffer(&self, _request: DumbBufferRequest) -> Result<DumbBuffer> {
        Err(Error::Unsupported)
    }

    fn destroy_dumb_buffer(&self, _handle: u32) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn map_dumb_buffer(&self, _handle: u32) -> Result<u64> {
        Err(Error::Unsupported)
    }

    fn dumb_buffer_mapping(&self, _handle: u32) -> Result<DumbBufferMapping> {
        Err(Error::Unsupported)
    }

    fn connectors(&self, _resources: &mut ConnectorList<'_>) -> Result<()> {
        Ok(())
    }

    fn crtcs(&self, _resources: &mut CrtcList<'_>) -> Result<()> {
        Ok(())
    }

    fn encoders(&self, _resources: &mut EncoderList<'_>) -> Result<()> {
        Ok(())
    }

    fn planes(&self, _resources: &mut PlaneList<'_>) -> Result<()> {
        Ok(())
    }

    fn create_framebuffer(&self, _request: FramebufferRequest) -> Result<FramebufferFormat> {
        Err(Error::Unsupported)
    }

    fn release_framebuffer(&self, _handle: u32) {}

    fn dirty_framebuffer(&self, _update: FramebufferUpdate<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn set_plane(&self, _update: PlaneUpdate) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn set_crtc(&self, _update: CrtcUpdate<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn page_flip(&self, _flip: PageFlip) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn set_cursor(&self, _cursor: CursorUpdate) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn atomic_commit(&self, _commit: AtomicCommit<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }
}

pub struct Device {
    raw: NonNull<bindings::DrmDevice>,
}

impl Device {
    pub fn notify_hotplug(&self) -> Result<()> {
        let status = unsafe { bindings::na_drm_device_notify_hotplug(self.raw.as_ptr()) };
        Error::from_status(status)
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        unsafe { bindings::na_drm_device_unregister(self.raw.as_ptr()) };
    }
}

unsafe impl Send for Device {}

pub struct DeviceBuilder<D: Driver> {
    driver: &'static D,
    node_name: &'static CStr,
    driver_name: &'static CStr,
    driver_date: &'static CStr,
    driver_description: &'static CStr,
    supports_render_node: bool,
}

impl<D: Driver> DeviceBuilder<D> {
    pub const fn new(
        driver: &'static D,
        node_name: &'static CStr,
        driver_name: &'static CStr,
        driver_date: &'static CStr,
        driver_description: &'static CStr,
    ) -> Self {
        Self {
            driver,
            node_name,
            driver_name,
            driver_date,
            driver_description,
            supports_render_node: false,
        }
    }

    pub const fn render_node(mut self, enabled: bool) -> Self {
        self.supports_render_node = enabled;
        self
    }

    pub fn register(self, pci_device: Option<&pci::Device<'_>>) -> Result<Device> {
        let ops = bindings::DrmDriverOps {
            context: (self.driver as *const D).cast_mut().cast(),
            supports_render_node: self.supports_render_node,
            open: Some(Callbacks::<D>::open),
            close: Some(Callbacks::<D>::close),
            get_capability: Some(Callbacks::<D>::get_capability),
            get_display_info: Some(Callbacks::<D>::get_display_info),
            get_framebuffer: Some(Callbacks::<D>::get_framebuffer),
            create_dumb_buffer: Some(Callbacks::<D>::create_dumb_buffer),
            destroy_dumb_buffer: Some(Callbacks::<D>::destroy_dumb_buffer),
            map_dumb_buffer: Some(Callbacks::<D>::map_dumb_buffer),
            get_dumb_buffer_mapping: Some(Callbacks::<D>::get_dumb_buffer_mapping),
            get_connectors: Some(Callbacks::<D>::get_connectors),
            get_crtcs: Some(Callbacks::<D>::get_crtcs),
            get_encoders: Some(Callbacks::<D>::get_encoders),
            get_planes: Some(Callbacks::<D>::get_planes),
            create_framebuffer: Some(Callbacks::<D>::create_framebuffer),
            release_framebuffer: Some(Callbacks::<D>::release_framebuffer),
            dirty_framebuffer: Some(Callbacks::<D>::dirty_framebuffer),
            set_plane: Some(Callbacks::<D>::set_plane),
            set_crtc: Some(Callbacks::<D>::set_crtc),
            page_flip: Some(Callbacks::<D>::page_flip),
            set_cursor: Some(Callbacks::<D>::set_cursor),
            atomic_commit: Some(Callbacks::<D>::atomic_commit),
            driver_ioctl: Some(Callbacks::<D>::driver_ioctl),
        };
        let pci_device = pci_device.map_or(core::ptr::null_mut(), pci::Device::raw_ptr);
        let raw = unsafe {
            bindings::na_drm_device_register(
                &ops,
                self.node_name.as_ptr(),
                pci_device,
                self.driver_name.as_ptr(),
                self.driver_date.as_ptr(),
                self.driver_description.as_ptr(),
            )
        };
        NonNull::new(raw)
            .map(|raw| Device { raw })
            .ok_or(Error::OutOfMemory)
    }
}
