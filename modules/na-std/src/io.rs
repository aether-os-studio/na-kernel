use core::{mem::size_of, ptr::NonNull};

use crate::{Error, Result, bindings, memory::PhysicalRange};

mod sealed {
    pub trait Sealed {}

    impl Sealed for u8 {}
    impl Sealed for u16 {}
    impl Sealed for u32 {}
    impl Sealed for u64 {}
}

pub trait RegisterValue: sealed::Sealed + Copy {}

impl RegisterValue for u8 {}
impl RegisterValue for u16 {}
impl RegisterValue for u32 {}
impl RegisterValue for u64 {}

pub struct MmioRegion {
    base: NonNull<u8>,
    length: usize,
}

unsafe impl Send for MmioRegion {}

impl PhysicalRange {
    pub fn map_mmio(self) -> Result<MmioRegion> {
        let raw = unsafe { bindings::na_mmio_map(self.start().get(), self.length()) };
        let base = NonNull::new(raw.cast::<u8>()).ok_or(Error::OutOfMemory)?;
        Ok(MmioRegion {
            base,
            length: self.length(),
        })
    }
}

impl MmioRegion {
    pub const fn length(&self) -> usize {
        self.length
    }

    pub fn read<T: RegisterValue>(&self, offset: usize) -> Result<T> {
        let ptr = self.register::<T>(offset)?;
        Ok(unsafe { ptr.as_ptr().read_volatile() })
    }

    pub fn write<T: RegisterValue>(&mut self, offset: usize, value: T) -> Result<()> {
        let ptr = self.register::<T>(offset)?;
        unsafe { ptr.as_ptr().write_volatile(value) };
        Ok(())
    }

    fn register<T>(&self, offset: usize) -> Result<NonNull<T>> {
        let end = offset
            .checked_add(size_of::<T>())
            .ok_or(Error::InvalidArgument)?;
        if end > self.length {
            return Err(Error::InvalidArgument);
        }

        let raw = unsafe { self.base.as_ptr().add(offset) };
        if !(raw as usize).is_multiple_of(core::mem::align_of::<T>()) {
            return Err(Error::InvalidArgument);
        }
        Ok(unsafe { NonNull::new_unchecked(raw.cast::<T>()) })
    }
}
