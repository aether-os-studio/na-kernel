use core::{
    cell::UnsafeCell,
    ops::{Deref, DerefMut},
    ptr::NonNull,
};

use crate::{Error, Result, bindings};

pub struct SpinLock<T> {
    raw: UnsafeCell<bindings::RawSpinLock>,
    value: UnsafeCell<T>,
}

impl<T> SpinLock<T> {
    pub const fn new(value: T) -> Self {
        Self {
            raw: UnsafeCell::new(bindings::RawSpinLock {
                lock: 0,
                irq_state: false,
            }),
            value: UnsafeCell::new(value),
        }
    }

    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        unsafe { bindings::na_spin_lock(self.raw.get()) };
        SpinLockGuard { lock: self }
    }
}

unsafe impl<T: Send> Send for SpinLock<T> {}
unsafe impl<T: Send> Sync for SpinLock<T> {}

pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
}

impl<T> Deref for SpinLockGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for SpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for SpinLockGuard<'_, T> {
    fn drop(&mut self) {
        unsafe { bindings::na_spin_unlock(self.lock.raw.get()) };
    }
}

pub struct Mutex<T> {
    raw: NonNull<bindings::MutexHandle>,
    value: UnsafeCell<T>,
}

impl<T> Mutex<T> {
    pub fn new(value: T) -> Result<Self> {
        let raw = NonNull::new(unsafe { bindings::na_mutex_create() }).ok_or(Error::OutOfMemory)?;
        Ok(Self {
            raw,
            value: UnsafeCell::new(value),
        })
    }

    pub fn lock(&self) -> MutexGuard<'_, T> {
        unsafe { bindings::na_mutex_lock(self.raw.as_ptr()) };
        MutexGuard { mutex: self }
    }
}

unsafe impl<T: Send> Send for Mutex<T> {}
unsafe impl<T: Send> Sync for Mutex<T> {}

pub struct MutexGuard<'a, T> {
    mutex: &'a Mutex<T>,
}

impl<T> Deref for MutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { &*self.mutex.value.get() }
    }
}

impl<T> DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut *self.mutex.value.get() }
    }
}

impl<T> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        unsafe { bindings::na_mutex_unlock(self.mutex.raw.as_ptr()) };
    }
}

impl<T> Drop for Mutex<T> {
    fn drop(&mut self) {
        unsafe { bindings::na_mutex_destroy(self.raw.as_ptr()) };
    }
}
