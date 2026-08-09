use core::{ffi::CStr, marker::PhantomData, ptr::NonNull, slice};

use crate::{
    Error, Result, bindings,
    memory::{Borrowed, PhysicalRange},
};

pub struct Node<'a> {
    raw: Borrowed<'a, bindings::FdtDevice>,
}

impl Node<'_> {
    pub fn name(&self) -> Option<&CStr> {
        let ptr = unsafe { bindings::na_fdt_device_name(self.raw.ptr.as_ptr()) };
        (!ptr.is_null()).then(|| unsafe { CStr::from_ptr(ptr) })
    }

    pub fn property(&self, name: &CStr) -> Option<&[u8]> {
        let mut length = 0usize;
        let ptr = unsafe {
            bindings::na_fdt_device_property(self.raw.ptr.as_ptr(), name.as_ptr(), &mut length)
        };
        if ptr.is_null() {
            return None;
        }
        Some(unsafe { slice::from_raw_parts(ptr.cast::<u8>(), length) })
    }

    pub fn property_u32(&self, name: &CStr) -> Option<u32> {
        let bytes = self.property(name)?;
        let bytes: [u8; 4] = bytes.try_into().ok()?;
        Some(u32::from_be_bytes(bytes))
    }

    pub fn register(&self, index: usize) -> Result<PhysicalRange> {
        let mut address_cells = 0;
        let mut size_cells = 0;
        let status = unsafe {
            bindings::na_fdt_device_reg_cells(
                self.raw.ptr.as_ptr(),
                &mut address_cells,
                &mut size_cells,
            )
        };
        Error::from_status(status)?;
        if !(1..=2).contains(&address_cells) || !(1..=2).contains(&size_cells) {
            return Err(Error::Unsupported);
        }

        let bytes = self.property(c"reg").ok_or(Error::NotFound)?;
        let tuple_cells = (address_cells + size_cells) as usize;
        let tuple_bytes = tuple_cells.checked_mul(4).ok_or(Error::InvalidArgument)?;
        let start = index
            .checked_mul(tuple_bytes)
            .ok_or(Error::InvalidArgument)?;
        let end = start
            .checked_add(tuple_bytes)
            .ok_or(Error::InvalidArgument)?;
        let tuple = bytes.get(start..end).ok_or(Error::NotFound)?;
        let address = Self::cells(&tuple[..address_cells as usize * 4]);
        let size = Self::cells(&tuple[address_cells as usize * 4..]);
        PhysicalRange::new(address, size)
    }

    fn cells(bytes: &[u8]) -> u64 {
        bytes.as_chunks::<4>().0.iter().fold(0, |value, cell| {
            value << 32 | u64::from(u32::from_be_bytes(*cell))
        })
    }

    fn from_raw(raw: *mut bindings::FdtDevice) -> Option<Self> {
        let ptr = NonNull::new(raw)?;
        Some(Self {
            raw: Borrowed {
                ptr,
                lifetime: PhantomData,
            },
        })
    }
}

pub trait Driver: Sync + 'static {
    fn probe(&self, node: Node<'_>, compatible: &CStr) -> Result<()>;
}

pub struct DriverBuilder<D: Driver> {
    driver: &'static D,
    name: &'static CStr,
    compatible: &'static [u8],
}

impl<D: Driver> DriverBuilder<D> {
    pub fn new(driver: &'static D, name: &'static CStr, compatible: &'static [u8]) -> Result<Self> {
        let valid = compatible.last() == Some(&0)
            && compatible
                .split_inclusive(|byte| *byte == 0)
                .all(|entry| entry.len() > 1 && entry.last() == Some(&0));
        valid
            .then_some(Self {
                driver,
                name,
                compatible,
            })
            .ok_or(Error::InvalidArgument)
    }

    pub fn register(self) -> Result<()> {
        let ops = bindings::FdtDriverOps {
            context: (self.driver as *const D).cast_mut().cast(),
            probe: Some(Self::probe),
        };
        let status = unsafe {
            bindings::na_fdt_driver_register(
                self.name.as_ptr(),
                self.compatible.as_ptr().cast(),
                self.compatible.len(),
                &ops,
            )
        };
        Error::from_status(status)
    }

    unsafe extern "C" fn probe(
        context: *mut core::ffi::c_void,
        raw: *mut bindings::FdtDevice,
        compatible: *const core::ffi::c_char,
    ) -> i32 {
        let Some(node) = Node::from_raw(raw) else {
            return Error::NoDevice.status();
        };
        if compatible.is_null() {
            return Error::InvalidArgument.status();
        }
        let compatible = unsafe { CStr::from_ptr(compatible) };
        let driver = unsafe { &*context.cast::<D>() };
        match driver.probe(node, compatible) {
            Ok(()) => 0,
            Err(error) => error.status(),
        }
    }
}
