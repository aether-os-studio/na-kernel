use core::{ffi::CStr, ptr::NonNull};

use crate::{Error, Result, bindings, pci};

pub const DEVICE_GPU: u32 = 16;

pub trait Driver: Sync + 'static {
    const DEVICE_TYPE: u32;
    const FEATURES: u64;

    fn probe(&self, device: Device) -> Result<()>;
}

pub trait ConfigHandler: Sync + 'static {
    fn changed(&self);
}

pub struct DriverBuilder<D: Driver> {
    driver: &'static D,
    name: &'static CStr,
}

impl<D: Driver> DriverBuilder<D> {
    pub const fn new(driver: &'static D, name: &'static CStr) -> Self {
        Self { driver, name }
    }

    pub fn register(self) -> Result<()> {
        let ops = bindings::VirtioDriverOps {
            context: (self.driver as *const D).cast_mut().cast(),
            probe: Some(Self::probe),
        };
        let status = unsafe {
            bindings::na_virtio_driver_register(
                self.name.as_ptr(),
                D::DEVICE_TYPE,
                D::FEATURES,
                &ops,
            )
        };
        Error::from_status(status)
    }

    unsafe extern "C" fn probe(
        context: *mut core::ffi::c_void,
        raw: *mut bindings::VirtioDevice,
    ) -> i32 {
        let Some(device) = Device::from_raw(raw) else {
            return Error::NoDevice.status();
        };
        let driver = unsafe { &*context.cast::<D>() };
        driver.probe(device).map_or_else(Error::status, |()| 0)
    }
}

pub struct Device {
    raw: NonNull<bindings::VirtioDevice>,
}

impl Device {
    fn from_raw(raw: *mut bindings::VirtioDevice) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self { raw })
    }

    pub fn features(&self) -> u64 {
        unsafe { bindings::na_virtio_device_features(self.raw.as_ptr()) }
    }

    pub fn finish(&mut self) {
        unsafe { bindings::na_virtio_device_finish(self.raw.as_ptr()) };
    }

    pub fn config_read(&self, offset: u32) -> u32 {
        unsafe { bindings::na_virtio_device_config_read(self.raw.as_ptr(), offset) }
    }

    pub fn config_write(&self, offset: u32, value: u32) {
        unsafe { bindings::na_virtio_device_config_write(self.raw.as_ptr(), offset, value) };
    }

    pub fn pci_device(&self) -> Option<pci::Device<'_>> {
        let raw = unsafe { bindings::na_virtio_device_pci(self.raw.as_ptr()) };
        pci::Device::from_raw(raw)
    }

    pub fn queue(&self, index: u16) -> Result<Queue> {
        let mut raw = core::ptr::null_mut();
        let status =
            unsafe { bindings::na_virtio_device_queue(self.raw.as_ptr(), index, &mut raw) };
        Error::from_status(status)?;
        NonNull::new(raw)
            .map(|raw| Queue { raw })
            .ok_or(Error::NoDevice)
    }

    pub fn set_config_handler<H: ConfigHandler>(&self, handler: &'static H) {
        unsafe {
            bindings::na_virtio_device_set_config_handler(
                self.raw.as_ptr(),
                (handler as *const H).cast_mut().cast(),
                Some(config_changed::<H>),
            )
        };
    }
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}

unsafe extern "C" fn config_changed<H: ConfigHandler>(context: *mut core::ffi::c_void) {
    let handler = unsafe { &*context.cast::<H>() };
    handler.changed();
}

pub struct Queue {
    raw: NonNull<bindings::VirtioQueue>,
}

impl Queue {
    pub fn submit(&self, request: &[u8], extra: Option<&[u8]>, response: &mut [u8]) -> Result<()> {
        let extra = extra.unwrap_or_default();
        let extra_ptr = if extra.is_empty() {
            core::ptr::null()
        } else {
            extra.as_ptr()
        };
        let status = unsafe {
            bindings::na_virtio_queue_submit(
                self.raw.as_ptr(),
                request.as_ptr().cast(),
                request.len(),
                extra_ptr.cast(),
                extra.len(),
                response.as_mut_ptr().cast(),
                response.len(),
            )
        };
        Error::from_status(status)
    }
}

unsafe impl Send for Queue {}
unsafe impl Sync for Queue {}
