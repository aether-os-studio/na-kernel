use alloc::boxed::Box;
use core::{ffi::CStr, pin::Pin, ptr::NonNull};

use crate::memory::PhysicalAddress;
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

    /// Waits for one DRM syncobj owned by this DRM file's process. A zero
    /// point has binary-syncobj semantics and therefore waits for point one.
    pub fn wait_syncobj(self, handle: u32, point: u64, timeout_ns: i64) -> Result<()> {
        let status = unsafe { bindings::na_drm_syncobj_wait(self.0, handle, point, timeout_ns) };
        Error::from_status(status)
    }

    /// Advances a DRM syncobj after a hardware submission has completed.
    pub fn signal_syncobj(self, handle: u32, point: u64) -> Result<()> {
        let status = unsafe { bindings::na_drm_syncobj_signal(self.0, handle, point) };
        Error::from_status(status)
    }

    /// Backs a syncobj point with a monotonically increasing 64-bit GPU
    /// writeback fence.  Wait and sync-file paths observe the fence directly,
    /// so command submission can return before the GPU completes.
    pub fn attach_syncobj_fence(
        self,
        handle: u32,
        point: u64,
        timeline: bool,
        cpu_address: u64,
        value: u64,
    ) -> Result<()> {
        let status = unsafe {
            bindings::na_drm_syncobj_attach_fence(
                self.0,
                handle,
                point,
                timeline as u32,
                cpu_address,
                value,
            )
        };
        Error::from_status(status)
    }
}

pub struct Ioctl<'a> {
    pub command: u32,
    pub arg: &'a mut [u8],
    pub render_node: bool,
    pub file: Option<FileId>,
}

#[derive(Clone, Copy, Debug)]
pub struct MmapRequest {
    pub file: FileId,
    pub offset: u64,
    pub length: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PrimeBuffer {
    pub physical_address: PhysicalAddress,
    pub length: usize,
    /// Driver-private object reference held by the dma-buf file.
    pub token: u64,
}

pub trait Driver: Sync + 'static {
    fn open(&self, _file: FileId) -> Result<()> {
        Ok(())
    }

    fn close(&self, _file: FileId) {}

    fn driver_ioctl(&self, _ioctl: Ioctl<'_>) -> Result<usize> {
        Err(Error::NotATerminal)
    }

    fn mmap(&self, _request: MmapRequest) -> Result<PhysicalAddress> {
        Err(Error::Unsupported)
    }

    fn prime_export(&self, _file: FileId, _handle: u32) -> Result<PrimeBuffer> {
        Err(Error::Unsupported)
    }

    fn prime_import(&self, _file: FileId, _token: u64) -> Result<u32> {
        Err(Error::Unsupported)
    }

    fn prime_release(&self, _token: u64) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn capability(&self, _capability: u64) -> Result<u64> {
        Err(Error::Unsupported)
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

    fn framebuffer_handle(&self, _file: FileId, _framebuffer_handle: u32) -> Result<u32> {
        Err(Error::Unsupported)
    }

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

pub struct OwnedDevice<D: Driver + Send> {
    raw: NonNull<bindings::DrmDevice>,
    _driver: Pin<Box<D>>,
}

impl<D: Driver + Send> OwnedDevice<D> {
    pub fn notify_hotplug(&self) -> Result<()> {
        let status = unsafe { bindings::na_drm_device_notify_hotplug(self.raw.as_ptr()) };
        Error::from_status(status)
    }
}

impl<D: Driver + Send> Drop for OwnedDevice<D> {
    fn drop(&mut self) {
        unsafe { bindings::na_drm_device_unregister(self.raw.as_ptr()) };
    }
}

unsafe impl<D: Driver + Send> Send for OwnedDevice<D> {}

#[derive(Clone, Copy)]
pub struct DriverInfo {
    kernel_name: &'static CStr,
    uapi_name: &'static CStr,
    date: &'static CStr,
    description: &'static CStr,
    version_major: i32,
    version_minor: i32,
    version_patchlevel: i32,
}

impl DriverInfo {
    pub const fn new(
        kernel_name: &'static CStr,
        uapi_name: &'static CStr,
        date: &'static CStr,
        description: &'static CStr,
        version_major: i32,
        version_minor: i32,
        version_patchlevel: i32,
    ) -> Self {
        Self {
            kernel_name,
            uapi_name,
            date,
            description,
            version_major,
            version_minor,
            version_patchlevel,
        }
    }
}

#[derive(Clone, Copy)]
struct RegistrationConfig {
    node_name: &'static CStr,
    driver_info: DriverInfo,
    supports_render_node: bool,
    supports_atomic_modeset: bool,
}

impl RegistrationConfig {
    const fn new(node_name: &'static CStr, driver_info: DriverInfo) -> Self {
        Self {
            node_name,
            driver_info,
            supports_render_node: false,
            supports_atomic_modeset: false,
        }
    }

    fn register<D: Driver>(
        self,
        driver: &D,
        pci_device: Option<&pci::Device<'_>>,
    ) -> Result<NonNull<bindings::DrmDevice>> {
        let driver_info = bindings::DrmDriverInfo {
            kernel_name: self.driver_info.kernel_name.as_ptr(),
            uapi_name: self.driver_info.uapi_name.as_ptr(),
            date: self.driver_info.date.as_ptr(),
            description: self.driver_info.description.as_ptr(),
            version_major: self.driver_info.version_major,
            version_minor: self.driver_info.version_minor,
            version_patchlevel: self.driver_info.version_patchlevel,
        };
        let ops = bindings::DrmDriverOps {
            context: (driver as *const D).cast_mut().cast(),
            supports_render_node: self.supports_render_node,
            supports_atomic_modeset: self.supports_atomic_modeset,
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
            get_framebuffer_handle: Some(Callbacks::<D>::get_framebuffer_handle),
            dirty_framebuffer: Some(Callbacks::<D>::dirty_framebuffer),
            set_plane: Some(Callbacks::<D>::set_plane),
            set_crtc: Some(Callbacks::<D>::set_crtc),
            page_flip: Some(Callbacks::<D>::page_flip),
            set_cursor: Some(Callbacks::<D>::set_cursor),
            atomic_commit: Some(Callbacks::<D>::atomic_commit),
            mmap: Some(Callbacks::<D>::mmap),
            prime_export: Some(Callbacks::<D>::prime_export),
            prime_import: Some(Callbacks::<D>::prime_import),
            prime_release: Some(Callbacks::<D>::prime_release),
            driver_ioctl: Some(Callbacks::<D>::driver_ioctl),
        };
        let pci_device = pci_device.map_or(core::ptr::null_mut(), pci::Device::raw_ptr);
        let raw = unsafe {
            bindings::na_drm_device_register(
                &ops,
                self.node_name.as_ptr(),
                pci_device,
                &driver_info,
            )
        };
        NonNull::new(raw).ok_or(Error::OutOfMemory)
    }
}

pub struct DeviceBuilder<D: Driver> {
    driver: &'static D,
    config: RegistrationConfig,
}

impl<D: Driver> DeviceBuilder<D> {
    pub const fn new(
        driver: &'static D,
        node_name: &'static CStr,
        driver_info: DriverInfo,
    ) -> Self {
        Self {
            driver,
            config: RegistrationConfig::new(node_name, driver_info),
        }
    }

    pub const fn render_node(mut self, enabled: bool) -> Self {
        self.config.supports_render_node = enabled;
        self
    }

    pub const fn atomic_modeset(mut self, enabled: bool) -> Self {
        self.config.supports_atomic_modeset = enabled;
        self
    }

    pub fn register(self, pci_device: Option<&pci::Device<'_>>) -> Result<Device> {
        self.config
            .register(self.driver, pci_device)
            .map(|raw| Device { raw })
    }
}

pub struct OwnedDeviceBuilder<D: Driver + Send> {
    driver: Pin<Box<D>>,
    config: RegistrationConfig,
}

impl<D: Driver + Send> OwnedDeviceBuilder<D> {
    pub fn new(driver: D, node_name: &'static CStr, driver_info: DriverInfo) -> Self {
        Self {
            driver: Box::pin(driver),
            config: RegistrationConfig::new(node_name, driver_info),
        }
    }

    pub fn render_node(mut self, enabled: bool) -> Self {
        self.config.supports_render_node = enabled;
        self
    }

    pub fn atomic_modeset(mut self, enabled: bool) -> Self {
        self.config.supports_atomic_modeset = enabled;
        self
    }

    pub fn register(self, pci_device: Option<&pci::Device<'_>>) -> Result<OwnedDevice<D>> {
        let raw = self
            .config
            .register(self.driver.as_ref().get_ref(), pci_device)?;
        Ok(OwnedDevice {
            raw,
            _driver: self.driver,
        })
    }
}
