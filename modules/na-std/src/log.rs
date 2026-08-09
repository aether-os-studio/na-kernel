use alloc::{ffi::CString, string::String};
use core::{ffi::CStr, fmt};

use crate::bindings;

pub struct KernelLog;

impl KernelLog {
    pub fn write(message: &CStr) {
        unsafe { bindings::na_log(message.as_ptr()) };
    }

    pub fn write_fmt(arguments: fmt::Arguments<'_>) {
        let mut message = String::new();
        if fmt::write(&mut message, arguments).is_err() {
            return;
        }
        if let Ok(message) = CString::new(message) {
            Self::write(&message);
        }
    }
}
