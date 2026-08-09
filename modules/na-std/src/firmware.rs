use alloc::vec::Vec;
use core::{ffi::CStr, ptr};

use crate::{Error, Result, bindings};

/// An owned firmware image returned by a [`FirmwareProvider`].
pub struct FirmwareBlob<'a> {
    name: &'a CStr,
    data: Vec<u8>,
}

impl<'a> FirmwareBlob<'a> {
    pub fn from_bytes(name: &'a CStr, data: Vec<u8>) -> Self {
        Self { name, data }
    }

    pub fn name(&self) -> &CStr {
        self.name
    }

    pub fn data(&self) -> &[u8] {
        &self.data
    }

    pub fn as_slice(&self) -> &[u8] {
        self.data()
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }

    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    pub fn into_data(self) -> Vec<u8> {
        self.data
    }
}

/// Supplies firmware images to a driver. Providers can be backed by the VFS,
/// an embedded image, or a platform-specific firmware service.
pub trait FirmwareProvider {
    fn request<'a>(&self, name: &'a CStr) -> Result<FirmwareBlob<'a>>;
}

/// The kernel-backed provider. Firmware names are looked up below
/// `/lib/firmware/` by the kernel.
#[derive(Clone, Copy, Debug, Default)]
pub struct KernelFirmwareProvider;

impl FirmwareProvider for KernelFirmwareProvider {
    fn request<'a>(&self, name: &'a CStr) -> Result<FirmwareBlob<'a>> {
        let mut raw = ptr::null_mut();
        let mut length = 0usize;
        Error::from_status(unsafe {
            bindings::na_firmware_request(name.as_ptr(), &mut raw, &mut length)
        })?;

        let data = if length == 0 {
            if !raw.is_null() {
                unsafe { bindings::na_heap_free(raw.cast()) };
                return Err(Error::InvalidArgument);
            }
            Vec::new()
        } else {
            let Some(raw) = core::ptr::NonNull::new(raw) else {
                return Err(Error::OutOfMemory);
            };
            unsafe { Vec::from_raw_parts(raw.as_ptr().cast(), length, length) }
        };
        Ok(FirmwareBlob { name, data })
    }
}
