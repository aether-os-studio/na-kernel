#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

extern crate alloc;

pub mod acpi;
pub mod arch;
pub mod boot;
#[allow(
    clippy::all,
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
pub mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}
pub mod drm;
pub mod error;
pub mod fdt;
pub mod firmware;
pub mod io;
pub mod log;
pub mod memory;
pub mod net;
pub mod pci;
pub mod sync;
pub mod time;
pub mod usb;
pub mod user;
pub mod vfs;
pub mod virtio;

pub use error::{Error, Result};
pub use log::KernelLog;

#[macro_export]
macro_rules! module_entry {
    ($init:path) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn dlmain() -> i32 {
            match $init() {
                Ok(()) => 0,
                Err(error) => error.status(),
            }
        }
    };
}

#[cfg(feature = "module-runtime")]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    KernelLog::write_fmt(format_args!("{}", info));

    loop {
        core::hint::spin_loop();
    }
}
