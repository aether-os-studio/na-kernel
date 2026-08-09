use crate::bindings;

pub fn realtime_seconds() -> u64 {
    unsafe { bindings::na_vfs_realtime_seconds() }
}

pub fn current_fsuid() -> u32 {
    unsafe { bindings::na_vfs_current_fsuid() }
}

pub fn current_fsgid() -> u32 {
    unsafe { bindings::na_vfs_current_fsgid() }
}
