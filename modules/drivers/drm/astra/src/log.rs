use alloc::ffi::CString;
use alloc::string::String;
use core::fmt;

use na_std::log::KernelLog;

/// Writes a newline-terminated kernel log message.
pub fn info(args: fmt::Arguments<'_>) {
    write(args);
}

/// Writes a newline-terminated kernel log message (error severity).
pub fn error(args: fmt::Arguments<'_>) {
    write(args);
}

fn write(args: fmt::Arguments<'_>) {
    let mut message = String::new();
    if fmt::write(&mut message, args).is_err() {
        return;
    }
    message.push('\n');
    if let Ok(message) = CString::new(message) {
        KernelLog::write(&message);
    }
}

/// `amdgpu`-style device log, e.g.
/// `dev_info!("astra {:#06x}:{:02x}:{:02x}.{}: ...", ...)`.
#[macro_export]
macro_rules! dev_info {
    ($($arg:tt)*) => {
        $crate::log::info(format_args!($($arg)*))
    };
}

/// Error-severity device log.
#[macro_export]
macro_rules! dev_err {
    ($($arg:tt)*) => {
        $crate::log::error(format_args!($($arg)*))
    };
}
