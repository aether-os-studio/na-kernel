use crate::{Error, Result, bindings};

pub struct BlockDevice {
    dev: u64,
}

impl BlockDevice {
    pub const fn new(dev: u64) -> Self {
        Self { dev }
    }

    pub fn read(&self, offset: u64, buffer: &mut [u8]) -> Result<()> {
        if buffer.is_empty() {
            return Ok(());
        }
        let ret = unsafe {
            bindings::device_read(
                self.dev,
                buffer.as_mut_ptr().cast(),
                offset,
                buffer.len(),
                core::ptr::null_mut(),
            )
        };
        if ret < 0 {
            return Error::from_status(ret as i32);
        }
        (ret as usize == buffer.len())
            .then_some(())
            .ok_or(Error::Io)
    }

    pub fn write(&self, offset: u64, buffer: &[u8]) -> Result<()> {
        if buffer.is_empty() {
            return Ok(());
        }
        let ret = unsafe {
            bindings::device_write(
                self.dev,
                buffer.as_ptr().cast_mut().cast(),
                offset,
                buffer.len(),
                core::ptr::null_mut(),
            )
        };
        if ret < 0 {
            return Error::from_status(ret as i32);
        }
        (ret as usize == buffer.len())
            .then_some(())
            .ok_or(Error::Io)
    }

    pub fn flush(&self) -> Result<()> {
        let ret = unsafe {
            bindings::na_vfs_device_ioctl(
                self.dev,
                core::ptr::null_mut(),
                bindings::NA_DEVICE_FLUSH as u64,
                0,
            )
        };
        if ret < 0 {
            return Error::from_status(ret as i32);
        }
        Ok(())
    }
}
