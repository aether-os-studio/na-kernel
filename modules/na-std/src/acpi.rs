use core::{ptr::NonNull, slice};

use crate::{Error, Result, bindings};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Signature([u8; 4]);

impl Signature {
    pub const fn new(value: [u8; 4]) -> Self {
        Self(value)
    }
}

pub struct Table {
    raw: bindings::UacpiTable,
    ptr: NonNull<u8>,
}

unsafe impl Send for Table {}

impl Table {
    pub fn find(signature: Signature) -> Result<Self> {
        let mut raw = bindings::UacpiTable {
            ptr: core::ptr::null_mut(),
            index: 0,
        };
        let status = unsafe { bindings::na_acpi_table_find(signature.0.as_ptr().cast(), &mut raw) };
        if status != 0 {
            return Err(Error::NotFound);
        }
        let ptr = NonNull::new(raw.ptr.cast::<u8>()).ok_or(Error::NotFound)?;
        Ok(Self { raw, ptr })
    }

    pub fn signature(&self) -> [u8; 4] {
        let bytes = unsafe { slice::from_raw_parts(self.ptr.as_ptr(), 4) };
        [bytes[0], bytes[1], bytes[2], bytes[3]]
    }

    pub fn bytes(&self) -> Result<&[u8]> {
        let length_ptr = unsafe { self.ptr.as_ptr().add(4).cast::<u32>() };
        let length = u32::from_le(unsafe { length_ptr.read_unaligned() }) as usize;
        if length < 36 {
            return Err(Error::InvalidArgument);
        }
        Ok(unsafe { slice::from_raw_parts(self.ptr.as_ptr(), length) })
    }
}

impl Drop for Table {
    fn drop(&mut self) {
        unsafe {
            bindings::na_acpi_table_release(&mut self.raw);
        }
    }
}
