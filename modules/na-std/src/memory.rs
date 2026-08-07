use core::{
    ffi::c_void,
    marker::PhantomData,
    mem::{ManuallyDrop, size_of},
    ops::{Deref, DerefMut},
    ptr::NonNull,
    slice,
};

use crate::{Error, Result, bindings};

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

pub struct KernelBox<T> {
    ptr: NonNull<T>,
    allocation_size: usize,
}

unsafe impl<T: Send> Send for KernelBox<T> {}
unsafe impl<T: Sync> Sync for KernelBox<T> {}

impl<T> KernelBox<T> {
    pub fn new(value: T) -> Result<Self> {
        let allocation_size = size_of::<T>();
        let ptr = if allocation_size == 0 {
            NonNull::dangling()
        } else {
            let raw = unsafe { bindings::na_heap_allocate(allocation_size) };
            NonNull::new(raw.cast::<T>()).ok_or(Error::OutOfMemory)?
        };
        unsafe { ptr.as_ptr().write(value) };
        Ok(Self {
            ptr,
            allocation_size,
        })
    }

    pub fn leak(this: Self) -> &'static mut T
    where
        T: 'static,
    {
        let this = ManuallyDrop::new(this);
        unsafe { &mut *this.ptr.as_ptr() }
    }
}

impl<T> Deref for KernelBox<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { self.ptr.as_ref() }
    }
}

impl<T> DerefMut for KernelBox<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { self.ptr.as_mut() }
    }
}

impl<T> Drop for KernelBox<T> {
    fn drop(&mut self) {
        unsafe { self.ptr.as_ptr().drop_in_place() };
        if self.allocation_size != 0 {
            unsafe { bindings::na_heap_free(self.ptr.as_ptr().cast::<c_void>()) };
        }
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

pub struct KernelVec<T> {
    ptr: NonNull<T>,
    length: usize,
    capacity: usize,
}

impl<T> KernelVec<T> {
    pub const fn new() -> Self {
        Self {
            ptr: NonNull::dangling(),
            length: 0,
            capacity: if size_of::<T>() == 0 { usize::MAX } else { 0 },
        }
    }

    pub const fn len(&self) -> usize {
        self.length
    }

    pub const fn is_empty(&self) -> bool {
        self.length == 0
    }

    pub fn push(&mut self, value: T) -> Result<usize> {
        if self.length == self.capacity {
            self.grow()?;
        }
        let index = self.length;
        unsafe { self.ptr.as_ptr().add(index).write(value) };
        self.length += 1;
        Ok(index)
    }

    pub fn remove(&mut self, index: usize) -> Option<T> {
        if index >= self.length {
            return None;
        }
        let value = unsafe { self.ptr.as_ptr().add(index).read() };
        let trailing = self.length - index - 1;
        if trailing != 0 {
            unsafe {
                core::ptr::copy(
                    self.ptr.as_ptr().add(index + 1),
                    self.ptr.as_ptr().add(index),
                    trailing,
                )
            };
        }
        self.length -= 1;
        Some(value)
    }

    pub fn get(&self, index: usize) -> Option<&T> {
        (index < self.length).then(|| unsafe { &*self.ptr.as_ptr().add(index) })
    }

    pub fn get_mut(&mut self, index: usize) -> Option<&mut T> {
        (index < self.length).then(|| unsafe { &mut *self.ptr.as_ptr().add(index) })
    }

    pub fn iter(&self) -> slice::Iter<'_, T> {
        self.as_slice().iter()
    }

    pub fn iter_mut(&mut self) -> slice::IterMut<'_, T> {
        self.as_mut_slice().iter_mut()
    }

    pub fn as_slice(&self) -> &[T] {
        unsafe { slice::from_raw_parts(self.ptr.as_ptr(), self.length) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [T] {
        unsafe { slice::from_raw_parts_mut(self.ptr.as_ptr(), self.length) }
    }

    fn grow(&mut self) -> Result<()> {
        let capacity = self.capacity.checked_mul(2).unwrap_or(usize::MAX).max(1);
        let bytes = capacity
            .checked_mul(size_of::<T>())
            .ok_or(Error::OutOfMemory)?;
        let raw = unsafe { bindings::na_heap_allocate(bytes) };
        let replacement = NonNull::new(raw.cast::<T>()).ok_or(Error::OutOfMemory)?;
        unsafe {
            core::ptr::copy_nonoverlapping(self.ptr.as_ptr(), replacement.as_ptr(), self.length)
        };
        if self.capacity != 0 {
            unsafe { bindings::na_heap_free(self.ptr.as_ptr().cast()) };
        }
        self.ptr = replacement;
        self.capacity = capacity;
        Ok(())
    }
}

impl<T> Default for KernelVec<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> Drop for KernelVec<T> {
    fn drop(&mut self) {
        unsafe { core::ptr::drop_in_place(self.as_mut_slice()) };
        if self.capacity != 0 && size_of::<T>() != 0 {
            unsafe { bindings::na_heap_free(self.ptr.as_ptr().cast()) };
        }
    }
}

unsafe impl<T: Send> Send for KernelVec<T> {}
unsafe impl<T: Sync> Sync for KernelVec<T> {}

pub(crate) struct Borrowed<'a, T> {
    pub ptr: NonNull<T>,
    pub lifetime: PhantomData<&'a mut T>,
}
