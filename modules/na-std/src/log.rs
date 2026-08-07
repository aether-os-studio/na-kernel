use core::ffi::CStr;

use crate::bindings;

pub struct KernelLog;

impl KernelLog {
    pub fn write(message: &CStr) {
        unsafe { bindings::na_log(message.as_ptr()) };
    }
}
