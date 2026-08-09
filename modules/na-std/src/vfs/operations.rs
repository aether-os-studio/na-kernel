use core::{
    cell::UnsafeCell,
    ffi::{c_char, c_void},
    marker::PhantomData,
    mem::ManuallyDrop,
    ptr::NonNull,
    slice,
};

use crate::{Error, Result, bindings};

use super::{Dentry, DirContext, File, Inode, Kstat, MmapRequest, Path};

pub trait InodeOperations: Sync + 'static {
    fn lookup(
        &self,
        _directory: &mut Inode<'_>,
        _dentry: &mut Dentry<'_>,
        _flags: u32,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn permission(&self, _inode: &mut Inode<'_>, _mask: i32) -> Result<()> {
        Ok(())
    }

    fn create(
        &self,
        _directory: &mut Inode<'_>,
        _dentry: &mut Dentry<'_>,
        _mode: u16,
        _exclusive: bool,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn mkdir(
        &self,
        _directory: &mut Inode<'_>,
        _dentry: &mut Dentry<'_>,
        _mode: u16,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn mknod(
        &self,
        _directory: &mut Inode<'_>,
        _dentry: &mut Dentry<'_>,
        _mode: u16,
        _device: u64,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn symlink(
        &self,
        _directory: &mut Inode<'_>,
        _dentry: &mut Dentry<'_>,
        _target: &[u8],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn link(
        &self,
        _old_dentry: &mut Dentry<'_>,
        _directory: &mut Inode<'_>,
        _new_dentry: &mut Dentry<'_>,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn unlink(&self, _directory: &mut Inode<'_>, _dentry: &mut Dentry<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn rmdir(&self, _directory: &mut Inode<'_>, _dentry: &mut Dentry<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn rename(&self, _context: &mut RenameContext<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn get_link(&self, _inode: &mut Inode<'_>) -> Result<Link> {
        Err(Error::Unsupported)
    }

    fn getattr(
        &self,
        _path: &Path<'_>,
        _stat: &mut Kstat<'_>,
        _request_mask: u32,
        _flags: u32,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn setattr(&self, _dentry: &mut Dentry<'_>, _stat: &Kstat<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn tmpfile(&self, _directory: &mut Inode<'_>, _file: &mut File<'_>, _mode: u16) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn get_xattr(
        &self,
        _inode: &mut Inode<'_>,
        _name: &[u8],
        _value: Option<&mut [u8]>,
    ) -> Result<usize> {
        Err(Error::Unsupported)
    }

    fn list_xattr(&self, _inode: &mut Inode<'_>, _list: Option<&mut [u8]>) -> Result<usize> {
        Err(Error::Unsupported)
    }

    fn set_xattr(
        &self,
        _inode: &mut Inode<'_>,
        _name: &[u8],
        _value: &[u8],
        _flags: i32,
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn remove_xattr(&self, _inode: &mut Inode<'_>, _name: &[u8]) -> Result<()> {
        Err(Error::Unsupported)
    }
}

#[repr(C)]
pub struct InodeOperationsTable<D: InodeOperations> {
    raw: UnsafeCell<bindings::vfs_inode_operations>,
    driver: &'static D,
}

impl<D: InodeOperations> InodeOperationsTable<D> {
    pub const fn new(driver: &'static D) -> Self {
        Self {
            raw: UnsafeCell::new(bindings::vfs_inode_operations {
                lookup: Some(Self::lookup),
                create: Some(Self::create),
                link: Some(Self::link),
                unlink: Some(Self::unlink),
                symlink: Some(Self::symlink),
                mkdir: Some(Self::mkdir),
                rmdir: Some(Self::rmdir),
                mknod: Some(Self::mknod),
                rename: Some(Self::rename),
                get_link: Some(Self::get_link),
                put_link: Some(Self::put_link),
                permission: Some(Self::permission),
                getattr: Some(Self::getattr),
                setattr: Some(Self::setattr),
                atomic_open: None,
                tmpfile: Some(Self::tmpfile),
                getxattr: Some(Self::get_xattr),
                listxattr: Some(Self::list_xattr),
                setxattr: Some(Self::set_xattr),
                removexattr: Some(Self::remove_xattr),
            }),
            driver,
        }
    }

    pub fn raw(&'static self) -> &'static bindings::vfs_inode_operations {
        unsafe { &*self.raw.get() }
    }

    unsafe fn from_raw(raw: *const bindings::vfs_inode_operations) -> Option<&'static Self> {
        NonNull::new(raw.cast_mut()).map(|raw| unsafe { &*raw.as_ptr().cast::<Self>() })
    }

    unsafe extern "C" fn lookup(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
        flags: u32,
    ) -> *mut bindings::vfs_dentry {
        let Some(mut directory) = (unsafe { Inode::from_raw(raw_directory) }) else {
            return error_pointer(Error::InvalidArgument);
        };
        let Some(mut dentry) = (unsafe { Dentry::from_raw(raw_dentry) }) else {
            return error_pointer(Error::InvalidArgument);
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return error_pointer(Error::InvalidArgument);
        };
        table
            .driver
            .lookup(&mut directory, &mut dentry, flags)
            .map_or_else(error_pointer, |_| raw_dentry)
    }

    unsafe extern "C" fn create(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
        mode: u16,
        exclusive: bool,
    ) -> i32 {
        let (Some(mut directory), Some(mut dentry)) =
            (unsafe { Inode::from_raw(raw_directory) }, unsafe {
                Dentry::from_raw(raw_dentry)
            })
        else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .create(&mut directory, &mut dentry, mode, exclusive)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn mkdir(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
        mode: u16,
    ) -> i32 {
        let (Some(mut directory), Some(mut dentry)) =
            (unsafe { Inode::from_raw(raw_directory) }, unsafe {
                Dentry::from_raw(raw_dentry)
            })
        else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .mkdir(&mut directory, &mut dentry, mode)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn mknod(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
        mode: u16,
        device: u64,
    ) -> i32 {
        let (Some(mut directory), Some(mut dentry)) =
            (unsafe { Inode::from_raw(raw_directory) }, unsafe {
                Dentry::from_raw(raw_dentry)
            })
        else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .mknod(&mut directory, &mut dentry, mode, device)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn rename(raw_context: *mut bindings::vfs_rename_ctx) -> i32 {
        let Some(mut context) = (unsafe { RenameContext::from_raw(raw_context) }) else {
            return Error::InvalidArgument.status();
        };
        let old_directory = unsafe { (*raw_context).old_dir };
        if old_directory.is_null() {
            return Error::InvalidArgument.status();
        }
        let Some(table) = (unsafe { Self::from_raw((*old_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .rename(&mut context)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn get_xattr(
        raw_inode: *mut bindings::vfs_inode,
        raw_name: *const c_char,
        raw_value: *mut c_void,
        size: usize,
    ) -> isize {
        let (Some(mut inode), Some(name)) = (
            unsafe { Inode::from_raw(raw_inode) },
            NonNull::new(raw_name.cast_mut().cast::<u8>()),
        ) else {
            return Error::InvalidArgument.status() as isize;
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_inode).i_op) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let value = if raw_value.is_null() {
            if size != 0 {
                return Error::InvalidArgument.status() as isize;
            }
            None
        } else {
            Some(unsafe { slice::from_raw_parts_mut(raw_value.cast::<u8>(), size) })
        };
        table
            .driver
            .get_xattr(&mut inode, unsafe { c_string_bytes(name.as_ptr()) }, value)
            .map_or_else(|error| error.status() as isize, |length| length as isize)
    }

    unsafe extern "C" fn list_xattr(
        raw_inode: *mut bindings::vfs_inode,
        raw_list: *mut c_char,
        size: usize,
    ) -> isize {
        let Some(mut inode) = (unsafe { Inode::from_raw(raw_inode) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_inode).i_op) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let list = if raw_list.is_null() {
            if size != 0 {
                return Error::InvalidArgument.status() as isize;
            }
            None
        } else {
            Some(unsafe { slice::from_raw_parts_mut(raw_list.cast::<u8>(), size) })
        };
        table
            .driver
            .list_xattr(&mut inode, list)
            .map_or_else(|error| error.status() as isize, |length| length as isize)
    }

    unsafe extern "C" fn set_xattr(
        raw_inode: *mut bindings::vfs_inode,
        raw_name: *const c_char,
        raw_value: *const c_void,
        size: usize,
        flags: i32,
    ) -> i32 {
        let (Some(mut inode), Some(name)) = (
            unsafe { Inode::from_raw(raw_inode) },
            NonNull::new(raw_name.cast_mut().cast::<u8>()),
        ) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_inode).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        let value = if raw_value.is_null() {
            if size != 0 {
                return Error::InvalidArgument.status();
            }
            &[]
        } else {
            unsafe { slice::from_raw_parts(raw_value.cast::<u8>(), size) }
        };
        table
            .driver
            .set_xattr(
                &mut inode,
                unsafe { c_string_bytes(name.as_ptr()) },
                value,
                flags,
            )
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn remove_xattr(
        raw_inode: *mut bindings::vfs_inode,
        raw_name: *const c_char,
    ) -> i32 {
        let (Some(mut inode), Some(name)) = (
            unsafe { Inode::from_raw(raw_inode) },
            NonNull::new(raw_name.cast_mut().cast::<u8>()),
        ) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_inode).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .remove_xattr(&mut inode, unsafe { c_string_bytes(name.as_ptr()) })
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn symlink(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
        target: *const c_char,
    ) -> i32 {
        let (Some(mut directory), Some(mut dentry), Some(target)) = (
            unsafe { Inode::from_raw(raw_directory) },
            unsafe { Dentry::from_raw(raw_dentry) },
            NonNull::new(target.cast_mut().cast::<u8>()),
        ) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .symlink(&mut directory, &mut dentry, unsafe {
                c_string_bytes(target.as_ptr())
            })
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn link(
        raw_old_dentry: *mut bindings::vfs_dentry,
        raw_directory: *mut bindings::vfs_inode,
        raw_new_dentry: *mut bindings::vfs_dentry,
    ) -> i32 {
        let (Some(mut old_dentry), Some(mut directory), Some(mut new_dentry)) = (
            unsafe { Dentry::from_raw(raw_old_dentry) },
            unsafe { Inode::from_raw(raw_directory) },
            unsafe { Dentry::from_raw(raw_new_dentry) },
        ) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .link(&mut old_dentry, &mut directory, &mut new_dentry)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn unlink(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
    ) -> i32 {
        let (Some(mut directory), Some(mut dentry)) =
            (unsafe { Inode::from_raw(raw_directory) }, unsafe {
                Dentry::from_raw(raw_dentry)
            })
        else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .unlink(&mut directory, &mut dentry)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn rmdir(
        raw_directory: *mut bindings::vfs_inode,
        raw_dentry: *mut bindings::vfs_dentry,
    ) -> i32 {
        let (Some(mut directory), Some(mut dentry)) =
            (unsafe { Inode::from_raw(raw_directory) }, unsafe {
                Dentry::from_raw(raw_dentry)
            })
        else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .rmdir(&mut directory, &mut dentry)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn permission(raw: *mut bindings::vfs_inode, mask: i32) -> i32 {
        let Some(mut inode) = (unsafe { Inode::from_raw(raw) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .permission(&mut inode, mask)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn get_link(
        _dentry: *mut bindings::vfs_dentry,
        raw_inode: *mut bindings::vfs_inode,
        _nameidata: *mut bindings::vfs_nameidata,
    ) -> *const c_char {
        let Some(mut inode) = (unsafe { Inode::from_raw(raw_inode) }) else {
            return error_link_pointer(Error::InvalidArgument);
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_inode).i_op) }) else {
            return error_link_pointer(Error::InvalidArgument);
        };
        table
            .driver
            .get_link(&mut inode)
            .map_or_else(error_link_pointer, Link::into_raw)
    }

    unsafe extern "C" fn put_link(_inode: *mut bindings::vfs_inode, link: *const c_char) {
        unsafe { Link::release(link) };
    }

    unsafe extern "C" fn getattr(
        raw_path: *const bindings::vfs_path,
        raw_stat: *mut bindings::vfs_kstat,
        request_mask: u32,
        flags: u32,
    ) -> i32 {
        let Some(path) = (unsafe { Path::from_raw(raw_path) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(mut stat) = (unsafe { Kstat::from_raw(raw_stat) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(dentry) = path.dentry() else {
            return Error::NotFound.status();
        };
        let Some(inode_raw) = dentry.inode().map(|inode| inode.raw()) else {
            return Error::NotFound.status();
        };
        let Some(inode) = (unsafe { Inode::from_raw(inode_raw) }) else {
            return Error::NotFound.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*inode.raw()).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        drop(inode);
        table
            .driver
            .getattr(&path, &mut stat, request_mask, flags)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn setattr(
        raw_dentry: *mut bindings::vfs_dentry,
        raw_stat: *const bindings::vfs_kstat,
    ) -> i32 {
        let Some(mut dentry) = (unsafe { Dentry::from_raw(raw_dentry) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(stat) = (unsafe { Kstat::from_raw(raw_stat.cast_mut()) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(inode) = dentry.inode() else {
            return Error::NotFound.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*inode.raw()).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        drop(inode);
        table
            .driver
            .setattr(&mut dentry, &stat)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn tmpfile(
        raw_directory: *mut bindings::vfs_inode,
        raw_file: *mut bindings::vfs_file,
        mode: u16,
    ) -> i32 {
        let (Some(mut directory), Some(mut file)) =
            (unsafe { Inode::from_raw(raw_directory) }, unsafe {
                File::from_raw(raw_file)
            })
        else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_directory).i_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .tmpfile(&mut directory, &mut file, mode)
            .map_or_else(Error::status, |_| 0)
    }
}

unsafe impl<D: InodeOperations> Sync for InodeOperationsTable<D> {}

pub struct RenameContext<'a> {
    raw: NonNull<bindings::vfs_rename_ctx>,
    lifetime: PhantomData<&'a mut bindings::vfs_rename_ctx>,
}

impl RenameContext<'_> {
    /// # Safety
    /// `raw` must remain valid for the duration of the rename callback.
    unsafe fn from_raw(raw: *mut bindings::vfs_rename_ctx) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn old_directory(&mut self) -> Option<Inode<'_>> {
        unsafe { Inode::from_raw(self.raw.as_ref().old_dir) }
    }

    pub fn old_dentry(&mut self) -> Option<Dentry<'_>> {
        unsafe { Dentry::from_raw(self.raw.as_ref().old_dentry) }
    }

    pub fn new_directory(&mut self) -> Option<Inode<'_>> {
        unsafe { Inode::from_raw(self.raw.as_ref().new_dir) }
    }

    pub fn new_dentry(&mut self) -> Option<Dentry<'_>> {
        unsafe { Dentry::from_raw(self.raw.as_ref().new_dentry) }
    }

    pub fn no_replace(&self) -> bool {
        unsafe { self.raw.as_ref().flags & bindings::NA_VFS_RENAME_NOREPLACE != 0 }
    }

    pub fn exchange(&self) -> bool {
        unsafe { self.raw.as_ref().flags & bindings::NA_VFS_RENAME_EXCHANGE != 0 }
    }

    pub fn whiteout(&self) -> bool {
        unsafe { self.raw.as_ref().flags & bindings::NA_VFS_RENAME_WHITEOUT != 0 }
    }
}

pub trait FileOperations: Sync + 'static {
    fn llseek(&self, _file: &mut File<'_>, _offset: i64, _whence: i32) -> Result<i64> {
        Err(Error::Unsupported)
    }

    fn read(&self, _file: &mut File<'_>, _buffer: &mut [u8], _position: &mut i64) -> Result<usize> {
        Err(Error::Unsupported)
    }

    fn write(&self, _file: &mut File<'_>, _buffer: &[u8], _position: &mut i64) -> Result<usize> {
        Err(Error::Unsupported)
    }

    fn iterate(&self, _file: &mut File<'_>, _context: &mut DirContext<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn ioctl(&self, _file: &mut File<'_>, _command: u64, _argument: u64) -> Result<i64> {
        Err(Error::NotATerminal)
    }

    fn poll(&self, _file: &mut File<'_>, _events: u32) -> Result<u32> {
        Ok(bindings::EPOLLIN | bindings::EPOLLOUT | bindings::EPOLLRDNORM | bindings::EPOLLWRNORM)
    }

    fn mmap(&self, file: &mut File<'_>, request: MmapRequest) -> Result<*mut c_void> {
        file.general_map(request)
    }

    fn flush(&self, _file: &mut File<'_>) -> Result<()> {
        Ok(())
    }

    fn open(&self, _inode: &mut Inode<'_>, _file: &mut File<'_>) -> Result<()> {
        Ok(())
    }

    fn release(&self, _inode: &mut Inode<'_>, _file: &mut File<'_>) -> Result<()> {
        Ok(())
    }

    fn fsync(&self, file: &mut File<'_>, start: i64, end: i64, datasync: bool) -> Result<()> {
        let inode = file.inode().ok_or(Error::InvalidArgument)?;
        let start = u64::try_from(start.max(0)).map_err(|_| Error::InvalidArgument)?;
        let end = if end < 0 {
            u64::MAX
        } else {
            u64::try_from(end)
                .map_err(|_| Error::InvalidArgument)?
                .saturating_add(1)
        };
        inode.writeback_mapping_range(start, end, datasync)
    }
}

#[repr(C)]
pub struct FileOperationsTable<D: FileOperations> {
    raw: UnsafeCell<bindings::vfs_file_operations>,
    driver: &'static D,
}

impl<D: FileOperations> FileOperationsTable<D> {
    pub const fn new(driver: &'static D) -> Self {
        Self {
            raw: UnsafeCell::new(bindings::vfs_file_operations {
                llseek: Some(Self::llseek),
                read: Some(Self::read),
                write: Some(Self::write),
                iterate_shared: Some(Self::iterate),
                unlocked_ioctl: Some(Self::ioctl),
                poll: Some(Self::poll),
                mmap: Some(Self::mmap),
                open: Some(Self::open),
                flush: Some(Self::flush),
                release: Some(Self::release),
                fsync: Some(Self::fsync),
                show_fdinfo: None,
            }),
            driver,
        }
    }

    pub fn raw(&'static self) -> &'static bindings::vfs_file_operations {
        unsafe { &*self.raw.get() }
    }

    unsafe fn from_raw(raw: *const bindings::vfs_file_operations) -> Option<&'static Self> {
        NonNull::new(raw.cast_mut()).map(|raw| unsafe { &*raw.as_ptr().cast::<Self>() })
    }

    unsafe extern "C" fn llseek(raw: *mut bindings::vfs_file, offset: i64, whence: i32) -> i64 {
        let Some(mut file) = (unsafe { File::from_raw(raw) }) else {
            return Error::InvalidArgument.status() as i64;
        };
        let Some(table) = (unsafe { Self::from_raw((*raw).f_op) }) else {
            return Error::InvalidArgument.status() as i64;
        };
        table
            .driver
            .llseek(&mut file, offset, whence)
            .unwrap_or_else(|error| i64::from(error.status()))
    }

    unsafe extern "C" fn read(
        raw: *mut bindings::vfs_file,
        buffer: *mut c_void,
        count: usize,
        position: *mut i64,
    ) -> isize {
        let Some(mut file) = (unsafe { File::from_raw(raw) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let Some(table) = (unsafe { Self::from_raw((*raw).f_op) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let (Some(buffer), Some(position)) =
            (NonNull::new(buffer.cast::<u8>()), NonNull::new(position))
        else {
            return Error::InvalidArgument.status() as isize;
        };
        let buffer = unsafe { slice::from_raw_parts_mut(buffer.as_ptr(), count) };
        let position = unsafe { &mut *position.as_ptr() };
        table
            .driver
            .read(&mut file, buffer, position)
            .map_or_else(|error| error.status() as isize, |value| value as isize)
    }

    unsafe extern "C" fn write(
        raw: *mut bindings::vfs_file,
        buffer: *const c_void,
        count: usize,
        position: *mut i64,
    ) -> isize {
        let Some(mut file) = (unsafe { File::from_raw(raw) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let Some(table) = (unsafe { Self::from_raw((*raw).f_op) }) else {
            return Error::InvalidArgument.status() as isize;
        };
        let (Some(buffer), Some(position)) = (
            NonNull::new(buffer.cast_mut().cast::<u8>()),
            NonNull::new(position),
        ) else {
            return Error::InvalidArgument.status() as isize;
        };
        let buffer = unsafe { slice::from_raw_parts(buffer.as_ptr(), count) };
        let position = unsafe { &mut *position.as_ptr() };
        table
            .driver
            .write(&mut file, buffer, position)
            .map_or_else(|error| error.status() as isize, |value| value as isize)
    }

    unsafe extern "C" fn iterate(
        raw_file: *mut bindings::vfs_file,
        raw_context: *mut bindings::vfs_dir_context,
    ) -> i32 {
        let Some(mut file) = (unsafe { File::from_raw(raw_file) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(mut context) = (unsafe { DirContext::from_raw(raw_context) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_file).f_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .iterate(&mut file, &mut context)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn ioctl(
        raw_file: *mut bindings::vfs_file,
        command: u64,
        argument: u64,
    ) -> i64 {
        let Some(mut file) = (unsafe { File::from_raw(raw_file) }) else {
            return i64::from(Error::InvalidArgument.status());
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_file).f_op) }) else {
            return i64::from(Error::InvalidArgument.status());
        };
        table
            .driver
            .ioctl(&mut file, command, argument)
            .unwrap_or_else(|error| i64::from(error.status()))
    }

    unsafe extern "C" fn poll(
        raw_file: *mut bindings::vfs_file,
        raw_table: *mut bindings::vfs_poll_table,
    ) -> u32 {
        let Some(mut file) = (unsafe { File::from_raw(raw_file) }) else {
            return bindings::EPOLLNVAL;
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_file).f_op) }) else {
            return bindings::EPOLLNVAL;
        };
        let events = if raw_table.is_null() {
            bindings::EPOLLIN
                | bindings::EPOLLOUT
                | bindings::EPOLLPRI
                | bindings::EPOLLERR
                | bindings::EPOLLHUP
                | bindings::EPOLLNVAL
        } else {
            unsafe { (*raw_table).events }
        };
        table
            .driver
            .poll(&mut file, events)
            .unwrap_or(bindings::EPOLLERR)
    }

    unsafe extern "C" fn mmap(
        raw_file: *mut bindings::vfs_file,
        address: *mut c_void,
        offset: usize,
        size: usize,
        protection: usize,
        flags: u64,
    ) -> *mut c_void {
        let Some(mut file) = (unsafe { File::from_raw(raw_file) }) else {
            return error_void_pointer(Error::InvalidArgument);
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_file).f_op) }) else {
            return error_void_pointer(Error::InvalidArgument);
        };
        table
            .driver
            .mmap(
                &mut file,
                MmapRequest {
                    address,
                    offset,
                    size,
                    protection,
                    flags,
                },
            )
            .unwrap_or_else(error_void_pointer)
    }

    unsafe extern "C" fn flush(raw: *mut bindings::vfs_file) -> i32 {
        let Some(mut file) = (unsafe { File::from_raw(raw) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw).f_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .flush(&mut file)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn open(
        raw_inode: *mut bindings::vfs_inode,
        raw_file: *mut bindings::vfs_file,
    ) -> i32 {
        let (Some(mut inode), Some(mut file)) = (unsafe { Inode::from_raw(raw_inode) }, unsafe {
            File::from_raw(raw_file)
        }) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_file).f_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .open(&mut inode, &mut file)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn release(
        raw_inode: *mut bindings::vfs_inode,
        raw_file: *mut bindings::vfs_file,
    ) -> i32 {
        let (Some(mut inode), Some(mut file)) = (unsafe { Inode::from_raw(raw_inode) }, unsafe {
            File::from_raw(raw_file)
        }) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw_file).f_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .release(&mut inode, &mut file)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn fsync(
        raw: *mut bindings::vfs_file,
        start: i64,
        end: i64,
        datasync: i32,
    ) -> i32 {
        let Some(mut file) = (unsafe { File::from_raw(raw) }) else {
            return Error::InvalidArgument.status();
        };
        let Some(table) = (unsafe { Self::from_raw((*raw).f_op) }) else {
            return Error::InvalidArgument.status();
        };
        table
            .driver
            .fsync(&mut file, start, end, datasync != 0)
            .map_or_else(Error::status, |_| 0)
    }
}

unsafe impl<D: FileOperations> Sync for FileOperationsTable<D> {}

pub trait AddressSpaceOperations: Sync + 'static {
    fn read_page(
        &self,
        _file: Option<&mut File<'_>>,
        _inode: &mut Inode<'_>,
        _index: u64,
        _page: &mut [u8],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn write_page(
        &self,
        _file: Option<&mut File<'_>>,
        _inode: &mut Inode<'_>,
        _index: u64,
        _page: &[u8],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }
}

#[repr(C)]
pub struct AddressSpaceOperationsTable<D: AddressSpaceOperations> {
    raw: UnsafeCell<bindings::vfs_address_space_operations>,
    driver: &'static D,
    marker: PhantomData<D>,
}

impl<D: AddressSpaceOperations> AddressSpaceOperationsTable<D> {
    pub const fn new(driver: &'static D) -> Self {
        Self {
            raw: UnsafeCell::new(bindings::vfs_address_space_operations {
                readpage: Some(Self::read_page),
                writepage: Some(Self::write_page),
                invalidatepage: None,
            }),
            driver,
            marker: PhantomData,
        }
    }

    pub fn raw(&'static self) -> &'static bindings::vfs_address_space_operations {
        unsafe { &*self.raw.get() }
    }

    unsafe fn from_raw(
        raw: *const bindings::vfs_address_space_operations,
    ) -> Option<&'static Self> {
        NonNull::new(raw.cast_mut()).map(|raw| unsafe { &*raw.as_ptr().cast::<Self>() })
    }

    unsafe extern "C" fn read_page(
        file: *mut bindings::vfs_file,
        mapping: *mut bindings::vfs_address_space,
        index: u64,
        page: *mut c_void,
    ) -> i32 {
        let (Some(mapping), Some(page)) = (NonNull::new(mapping), NonNull::new(page.cast::<u8>()))
        else {
            return Error::InvalidArgument.status();
        };
        let mut file = unsafe { File::from_raw(file) };
        let Some(table) = (unsafe { Self::from_raw((*mapping.as_ptr()).a_ops) }) else {
            return Error::InvalidArgument.status();
        };
        let host = unsafe { (*mapping.as_ptr()).host };
        let Some(mut inode) = (unsafe { Inode::from_raw(host) }) else {
            return Error::InvalidArgument.status();
        };
        let page = unsafe { slice::from_raw_parts_mut(page.as_ptr(), 4096) };
        table
            .driver
            .read_page(file.as_mut(), &mut inode, index, page)
            .map_or_else(Error::status, |_| 0)
    }

    unsafe extern "C" fn write_page(
        file: *mut bindings::vfs_file,
        mapping: *mut bindings::vfs_address_space,
        index: u64,
        page: *const c_void,
    ) -> i32 {
        let (Some(mapping), Some(page)) = (
            NonNull::new(mapping),
            NonNull::new(page.cast_mut().cast::<u8>()),
        ) else {
            return Error::InvalidArgument.status();
        };
        let mut file = unsafe { File::from_raw(file) };
        let Some(table) = (unsafe { Self::from_raw((*mapping.as_ptr()).a_ops) }) else {
            return Error::InvalidArgument.status();
        };
        let host = unsafe { (*mapping.as_ptr()).host };
        let Some(mut inode) = (unsafe { Inode::from_raw(host) }) else {
            return Error::InvalidArgument.status();
        };
        let page = unsafe { slice::from_raw_parts(page.as_ptr(), 4096) };
        table
            .driver
            .write_page(file.as_mut(), &mut inode, index, page)
            .map_or_else(Error::status, |_| 0)
    }
}

unsafe impl<D: AddressSpaceOperations> Sync for AddressSpaceOperationsTable<D> {}

fn error_pointer(error: Error) -> *mut bindings::vfs_dentry {
    (error.status() as isize as usize) as *mut bindings::vfs_dentry
}

fn error_link_pointer(error: Error) -> *const c_char {
    (error.status() as isize as usize) as *const c_char
}

fn error_void_pointer(error: Error) -> *mut c_void {
    (error.status() as isize as usize) as *mut c_void
}

unsafe fn c_string_bytes<'a>(pointer: *const u8) -> &'a [u8] {
    let mut length = 0;
    unsafe {
        while *pointer.add(length) != 0 {
            length += 1;
        }
        slice::from_raw_parts(pointer, length)
    }
}

pub struct Link {
    allocation: NonNull<u8>,
}

impl Link {
    pub fn new(target: &[u8]) -> Result<Self> {
        if target.contains(&0) {
            return Err(Error::InvalidArgument);
        }
        let size = target.len().checked_add(1).ok_or(Error::OutOfMemory)?;
        let allocation = NonNull::new(unsafe { bindings::na_heap_allocate(size) }.cast::<u8>())
            .ok_or(Error::OutOfMemory)?;
        unsafe {
            allocation
                .as_ptr()
                .copy_from_nonoverlapping(target.as_ptr(), target.len());
            allocation.as_ptr().add(target.len()).write(0);
        }
        Ok(Self { allocation })
    }

    fn into_raw(self) -> *const c_char {
        let this = ManuallyDrop::new(self);
        this.allocation.as_ptr().cast()
    }

    unsafe fn release(link: *const c_char) {
        if link.is_null() || (link as usize) >= usize::MAX - 4095 {
            return;
        }
        unsafe { bindings::na_heap_free(link.cast_mut().cast()) };
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        unsafe { bindings::na_heap_free(self.allocation.as_ptr().cast()) };
    }
}
