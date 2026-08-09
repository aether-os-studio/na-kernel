use core::{
    alloc::{GlobalAlloc, Layout},
    ffi::c_void,
    marker::PhantomData,
    ptr::NonNull,
    slice,
};

use crate::{Error, Result, bindings};

pub struct KernelAllocator;

#[global_allocator]
static GLOBAL_ALLOCATOR: KernelAllocator = KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.size() == 0 {
            return core::ptr::null_mut();
        }
        let ptr = if layout.align() <= 16 {
            unsafe { bindings::na_heap_allocate(layout.size()) }
        } else {
            unsafe { bindings::na_heap_allocate_aligned(layout.size(), layout.align()) }
        };
        ptr.cast()
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { bindings::na_heap_free(ptr.cast()) };
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if ptr.is_null() {
            let Ok(layout) = Layout::from_size_align(new_size, layout.align()) else {
                return core::ptr::null_mut();
            };
            return unsafe { self.alloc(layout) };
        }
        if new_size == 0 {
            unsafe { self.dealloc(ptr, layout) };
            return core::ptr::null_mut();
        }
        let ptr = if layout.align() <= 16 {
            unsafe { bindings::na_heap_reallocate(ptr.cast(), new_size) }
        } else {
            unsafe { bindings::na_heap_reallocate_aligned(ptr.cast(), new_size, layout.align()) }
        };
        ptr.cast()
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct PhysicalAddress(u64);

impl PhysicalAddress {
    pub const fn get(self) -> u64 {
        self.0
    }

    pub(crate) const fn new(address: u64) -> Self {
        Self(address)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PhysicalRange {
    start: PhysicalAddress,
    length: usize,
}

impl PhysicalRange {
    pub const fn start(self) -> PhysicalAddress {
        self.start
    }

    pub const fn length(self) -> usize {
        self.length
    }

    pub fn subrange(self, offset: usize, length: usize) -> Result<Self> {
        let end = offset.checked_add(length).ok_or(Error::InvalidArgument)?;
        if length == 0 || end > self.length {
            return Err(Error::InvalidArgument);
        }
        let offset = u64::try_from(offset).map_err(|_| Error::InvalidArgument)?;
        let length = u64::try_from(length).map_err(|_| Error::InvalidArgument)?;
        let start = self
            .start
            .get()
            .checked_add(offset)
            .ok_or(Error::InvalidArgument)?;
        Self::new(start, length)
    }

    pub(crate) fn new(start: u64, length: u64) -> Result<Self> {
        let length = usize::try_from(length).map_err(|_| Error::InvalidArgument)?;
        if length == 0 || start.checked_add(length as u64).is_none() {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            start: PhysicalAddress::new(start),
            length,
        })
    }
}

pub struct DmaBuffer {
    ptr: NonNull<u8>,
    length: usize,
}

pub struct KernelBuffer {
    ptr: NonNull<u8>,
    length: usize,
}

impl KernelBuffer {
    pub fn zeroed(length: usize) -> Result<Self> {
        if length == 0 {
            return Err(Error::InvalidArgument);
        }
        let raw = unsafe { bindings::na_heap_allocate(length) };
        let ptr = NonNull::new(raw.cast::<u8>()).ok_or(Error::OutOfMemory)?;
        unsafe { ptr.as_ptr().write_bytes(0, length) };
        Ok(Self { ptr, length })
    }

    pub fn as_slice(&self) -> &[u8] {
        unsafe { slice::from_raw_parts(self.ptr.as_ptr(), self.length) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { slice::from_raw_parts_mut(self.ptr.as_ptr(), self.length) }
    }
}

impl Drop for KernelBuffer {
    fn drop(&mut self) {
        unsafe { bindings::na_heap_free(self.ptr.as_ptr().cast()) };
    }
}

unsafe impl Send for KernelBuffer {}

unsafe impl Send for DmaBuffer {}

impl DmaBuffer {
    pub fn zeroed(length: usize) -> Result<Self> {
        if length == 0 {
            return Err(Error::InvalidArgument);
        }

        let raw = unsafe { bindings::na_memory_allocate(length as u64) };
        let ptr = NonNull::new(raw.cast::<u8>()).ok_or(Error::OutOfMemory)?;
        unsafe { ptr.as_ptr().write_bytes(0, length) };
        Ok(Self { ptr, length })
    }

    pub fn physical_address(&self) -> PhysicalAddress {
        let address = unsafe { bindings::na_memory_physical_address(self.ptr.as_ptr().cast()) };
        PhysicalAddress::new(address)
    }

    pub const fn length(&self) -> usize {
        self.length
    }

    pub fn as_slice(&self) -> &[u8] {
        unsafe { slice::from_raw_parts(self.ptr.as_ptr(), self.length) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { slice::from_raw_parts_mut(self.ptr.as_ptr(), self.length) }
    }

    pub fn sync_for_device(&self) {
        unsafe { bindings::na_dma_sync_for_device(self.ptr.as_ptr().cast(), self.length) };
    }

    pub fn sync_for_cpu(&self) {
        unsafe { bindings::na_dma_sync_for_cpu(self.ptr.as_ptr().cast(), self.length) };
    }
}

impl Drop for DmaBuffer {
    fn drop(&mut self) {
        unsafe { bindings::na_memory_free(self.ptr.as_ptr().cast::<c_void>(), self.length as u64) };
    }
}

pub(crate) struct Borrowed<'a, T> {
    pub ptr: NonNull<T>,
    pub lifetime: PhantomData<&'a mut T>,
}
