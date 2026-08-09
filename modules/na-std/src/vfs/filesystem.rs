use core::{cell::UnsafeCell, ffi::CStr, ptr::NonNull};

use crate::{Error, Result, bindings};

use super::{FsContext, Path, StatFs, SuperBlock};

pub trait FileSystem: Sync + 'static {
    fn init_context(&self, context: &mut FsContext<'_>) -> Result<()> {
        context.allocate_super().map(|_| ())
    }

    fn get_tree(&self, context: &mut FsContext<'_>) -> Result<()>;

    fn kill_super(&self, _super_block: &mut SuperBlock<'_>) {}

    fn put_super(&self, _super_block: &mut SuperBlock<'_>) {}

    fn evict_inode(&self, _inode: &mut super::Inode<'_>) {}

    fn sync_super(&self, _super_block: &mut SuperBlock<'_>, _wait: bool) -> Result<()> {
        Ok(())
    }

    fn statfs(&self, _path: &Path<'_>, _stat: &mut StatFs<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn get_quota(
        &self,
        _super_block: &mut SuperBlock<'_>,
        _quota_type: u32,
        _id: u32,
    ) -> Result<Quota> {
        Err(Error::Unsupported)
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Quota {
    pub block_hard_limit: u64,
    pub block_soft_limit: u64,
    pub valid: u32,
}

#[repr(C)]
pub struct FileSystemRegistration<F: FileSystem> {
    raw: UnsafeCell<bindings::vfs_file_system_type>,
    super_operations: UnsafeCell<bindings::vfs_super_operations>,
    driver: &'static F,
}

impl<F: FileSystem> FileSystemRegistration<F> {
    pub const fn new(driver: &'static F, name: &'static CStr, flags: u64) -> Self {
        Self {
            raw: UnsafeCell::new(bindings::vfs_file_system_type {
                name: name.as_ptr(),
                fs_flags: flags,
                init_fs_context: Some(Self::init_context),
                get_tree: Some(Self::get_tree),
                kill_sb: Some(Self::kill_super),
                fs_list: bindings::llist_header {
                    prev: core::ptr::null_mut(),
                    next: core::ptr::null_mut(),
                },
            }),
            super_operations: UnsafeCell::new(bindings::vfs_super_operations {
                alloc_inode: None,
                destroy_inode: None,
                dirty_inode: None,
                evict_inode: Some(Self::evict_inode),
                put_super: Some(Self::put_super),
                sync_fs: Some(Self::sync_super),
                freeze_fs: None,
                thaw_fs: None,
                statfs: Some(Self::statfs),
                get_quota: Some(Self::get_quota),
            }),
            driver,
        }
    }

    pub fn register(&'static self) -> Result<()> {
        let status = unsafe { bindings::vfs_register_filesystem(self.raw.get()) };
        Error::from_status(status)
    }

    fn super_operations(&'static self) -> &'static bindings::vfs_super_operations {
        unsafe { &*self.super_operations.get() }
    }

    unsafe fn from_type(raw: *mut bindings::vfs_file_system_type) -> Option<&'static Self> {
        NonNull::new(raw).map(|raw| unsafe { &*raw.as_ptr().cast::<Self>() })
    }

    unsafe extern "C" fn init_context(raw: *mut bindings::vfs_fs_context) -> i32 {
        let Some(mut context) = (unsafe { FsContext::from_raw(raw) }) else {
            return Error::InvalidArgument.status();
        };
        let fs_type = unsafe { (*raw).fs_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return Error::InvalidArgument.status();
        };
        registration
            .driver
            .init_context(&mut context)
            .map_or_else(Error::status, |_| {
                if let Some(mut super_block) = context.super_block() {
                    super_block.set_operations(registration.super_operations());
                }
                0
            })
    }

    unsafe extern "C" fn get_tree(raw: *mut bindings::vfs_fs_context) -> i32 {
        let Some(mut context) = (unsafe { FsContext::from_raw(raw) }) else {
            return Error::InvalidArgument.status();
        };
        let fs_type = unsafe { (*raw).fs_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return Error::InvalidArgument.status();
        };
        registration
            .driver
            .get_tree(&mut context)
            .map_or_else(Error::status, |()| 0)
    }

    unsafe extern "C" fn kill_super(raw: *mut bindings::vfs_super_block) {
        let Some(mut super_block) = (unsafe { SuperBlock::from_raw(raw) }) else {
            return;
        };
        let fs_type = unsafe { (*raw).s_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return;
        };
        registration.driver.kill_super(&mut super_block);
    }

    unsafe extern "C" fn put_super(raw: *mut bindings::vfs_super_block) {
        let Some(mut super_block) = (unsafe { SuperBlock::from_raw(raw) }) else {
            return;
        };
        let fs_type = unsafe { (*raw).s_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return;
        };
        registration.driver.put_super(&mut super_block);
    }

    unsafe extern "C" fn evict_inode(raw: *mut bindings::vfs_inode) {
        let Some(mut inode) = (unsafe { super::Inode::from_raw(raw) }) else {
            return;
        };
        let super_block = unsafe { (*raw).i_sb };
        if super_block.is_null() {
            return;
        }
        let fs_type = unsafe { (*super_block).s_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return;
        };
        registration.driver.evict_inode(&mut inode);
    }

    unsafe extern "C" fn sync_super(raw: *mut bindings::vfs_super_block, wait: i32) -> i32 {
        let Some(mut super_block) = (unsafe { SuperBlock::from_raw(raw) }) else {
            return Error::InvalidArgument.status();
        };
        let fs_type = unsafe { (*raw).s_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return Error::InvalidArgument.status();
        };
        registration
            .driver
            .sync_super(&mut super_block, wait != 0)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn statfs(
        raw_path: *mut bindings::vfs_path,
        raw_stat: *mut core::ffi::c_void,
    ) -> i32 {
        let (Some(path), Some(mut stat)) = (unsafe { Path::from_raw(raw_path) }, unsafe {
            StatFs::from_raw(raw_stat)
        }) else {
            return Error::InvalidArgument.status();
        };
        let Some(dentry) = path.dentry() else {
            return Error::InvalidArgument.status();
        };
        let Some(inode) = dentry.inode() else {
            return Error::InvalidArgument.status();
        };
        let Some(super_block) = inode.super_block() else {
            return Error::InvalidArgument.status();
        };
        let fs_type = unsafe { (*super_block.raw()).s_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return Error::InvalidArgument.status();
        };
        registration
            .driver
            .statfs(&path, &mut stat)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn get_quota(
        raw_super: *mut bindings::vfs_super_block,
        quota_type: u32,
        id: u32,
        hard_limit: *mut u64,
        soft_limit: *mut u64,
        valid: *mut u32,
    ) -> i32 {
        let (Some(mut super_block), Some(hard_limit), Some(soft_limit), Some(valid)) = (
            unsafe { SuperBlock::from_raw(raw_super) },
            NonNull::new(hard_limit),
            NonNull::new(soft_limit),
            NonNull::new(valid),
        ) else {
            return Error::InvalidArgument.status();
        };
        let fs_type = unsafe { (*raw_super).s_type };
        let Some(registration) = (unsafe { Self::from_type(fs_type) }) else {
            return Error::InvalidArgument.status();
        };
        registration
            .driver
            .get_quota(&mut super_block, quota_type, id)
            .map_or_else(Error::status, |quota| {
                unsafe {
                    hard_limit.as_ptr().write(quota.block_hard_limit);
                    soft_limit.as_ptr().write(quota.block_soft_limit);
                    valid.as_ptr().write(quota.valid);
                }
                0
            })
    }
}

unsafe impl<F: FileSystem> Sync for FileSystemRegistration<F> {}
