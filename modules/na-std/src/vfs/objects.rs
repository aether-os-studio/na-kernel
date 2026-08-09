use core::{ffi::c_void, marker::PhantomData, ptr::NonNull, slice};

use crate::{Result, bindings};

pub struct FsContext<'a> {
    raw: NonNull<bindings::vfs_fs_context>,
    lifetime: PhantomData<&'a mut bindings::vfs_fs_context>,
}

pub struct StatFs<'a> {
    raw: NonNull<bindings::na_vfs_statfs>,
    lifetime: PhantomData<&'a mut bindings::na_vfs_statfs>,
}

impl StatFs<'_> {
    /// # Safety
    /// `raw` must point to a live kernel `statfs` output buffer.
    pub unsafe fn from_raw(raw: *mut core::ffi::c_void) -> Option<Self> {
        NonNull::new(raw.cast::<bindings::na_vfs_statfs>()).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn set(
        &mut self,
        filesystem_type: u64,
        block_size: u64,
        blocks: u64,
        free_blocks: u64,
        available_blocks: u64,
        files: u64,
        free_files: u64,
        name_length: u64,
    ) {
        let raw = unsafe { self.raw.as_mut() };
        *raw = bindings::na_vfs_statfs::default();
        raw.f_type = filesystem_type;
        raw.f_bsize = block_size;
        raw.f_blocks = blocks;
        raw.f_bfree = free_blocks;
        raw.f_bavail = available_blocks;
        raw.f_files = files;
        raw.f_ffree = free_files;
        raw.f_namelen = name_length;
        raw.f_frsize = block_size;
    }
}

impl FsContext<'_> {
    /// Wrap a VFS context pointer supplied by the kernel callback ABI.
    ///
    /// # Safety
    /// The pointer must remain valid for the returned borrow's lifetime.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_fs_context) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn source(&self) -> Option<&[u8]> {
        let pointer = unsafe { self.raw.as_ref().source };
        if pointer.is_null() {
            return None;
        }
        Some(unsafe { c_string_bytes(pointer.cast()) })
    }

    pub fn data(&self) -> *const c_void {
        unsafe { self.raw.as_ref().data }
    }

    pub fn device_data(&self) -> Option<u64> {
        let data = self.data() as usize;
        (data != 0).then_some(data as u64)
    }

    pub fn resolve_device(&self) -> Result<u64> {
        let mut device = 0;
        let status = unsafe { bindings::na_vfs_resolve_device(self.raw.as_ptr(), &mut device) };
        crate::Error::from_status(status)?;
        (device != 0)
            .then_some(device)
            .ok_or(crate::Error::NoDevice)
    }

    pub fn super_block(&self) -> Option<SuperBlock<'_>> {
        NonNull::new(unsafe { self.raw.as_ref().sb }).map(|raw| SuperBlock {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn flags(&self) -> u64 {
        unsafe { self.raw.as_ref().sb_flags }
    }

    pub fn set_private(&mut self, private: *mut c_void) {
        unsafe { self.raw.as_mut().fs_private = private };
    }

    pub fn private(&self) -> *mut c_void {
        unsafe { self.raw.as_ref().fs_private }
    }

    pub fn allocate_super(&mut self) -> Result<SuperBlock<'_>> {
        let raw = unsafe {
            bindings::vfs_alloc_super(self.raw.as_ref().fs_type, self.raw.as_ref().sb_flags)
        };
        let raw = NonNull::new(raw).ok_or(crate::Error::OutOfMemory)?;
        unsafe { self.raw.as_mut().sb = raw.as_ptr() };
        Ok(SuperBlock {
            raw,
            lifetime: PhantomData,
        })
    }
}

pub struct SuperBlock<'a> {
    raw: NonNull<bindings::vfs_super_block>,
    lifetime: PhantomData<&'a mut bindings::vfs_super_block>,
}

impl SuperBlock<'_> {
    /// Wrap a VFS superblock pointer supplied by the kernel callback ABI.
    ///
    /// # Safety
    /// The pointer must remain valid for the returned borrow's lifetime.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_super_block) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn raw(&self) -> *mut bindings::vfs_super_block {
        self.raw.as_ptr()
    }

    pub fn device(&self) -> u64 {
        unsafe { self.raw.as_ref().s_dev }
    }

    pub fn set_device(&mut self, dev: u64) {
        unsafe { self.raw.as_mut().s_dev = dev };
    }

    pub fn set_magic(&mut self, magic: u64) {
        unsafe { self.raw.as_mut().s_magic = magic };
    }

    pub fn set_private(&mut self, private: *mut c_void) {
        unsafe { self.raw.as_mut().s_fs_info = private };
    }

    pub fn set_operations(&mut self, operations: &'static bindings::vfs_super_operations) {
        unsafe { self.raw.as_mut().s_op = operations };
    }

    pub fn private(&self) -> *mut c_void {
        unsafe { self.raw.as_ref().s_fs_info }
    }

    pub fn root(&self) -> Option<Dentry<'_>> {
        NonNull::new(unsafe { self.raw.as_ref().s_root }).map(|raw| Dentry {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn set_root(&mut self, dentry: &Dentry<'_>) {
        unsafe { self.raw.as_mut().s_root = dentry.raw() };
    }

    pub fn allocate_inode(&mut self) -> Result<Inode<'static>> {
        let raw = unsafe { bindings::vfs_alloc_inode(self.raw.as_ptr()) };
        let raw = NonNull::new(raw).ok_or(crate::Error::OutOfMemory)?;
        Ok(Inode {
            raw,
            owned: true,
            lifetime: PhantomData,
        })
    }

    pub fn allocate_root(&mut self) -> Result<Dentry<'static>> {
        let name = bindings::vfs_qstr {
            name: core::ptr::null(),
            len: 0,
            hash: 0,
        };
        let raw = unsafe { bindings::vfs_d_alloc(self.raw.as_ptr(), core::ptr::null_mut(), &name) };
        let raw = NonNull::new(raw).ok_or(crate::Error::OutOfMemory)?;
        Ok(Dentry {
            raw,
            lifetime: PhantomData,
        })
    }
}

pub struct Inode<'a> {
    raw: NonNull<bindings::vfs_inode>,
    owned: bool,
    lifetime: PhantomData<&'a mut bindings::vfs_inode>,
}

impl Inode<'_> {
    /// Wrap a VFS inode pointer supplied by the kernel callback ABI.
    ///
    /// # Safety
    /// The pointer must remain valid for the returned borrow's lifetime.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_inode) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            owned: false,
            lifetime: PhantomData,
        })
    }

    pub fn raw(&self) -> *mut bindings::vfs_inode {
        self.raw.as_ptr()
    }

    pub fn number(&self) -> u64 {
        unsafe { self.raw.as_ref().i_ino }
    }

    pub fn set_number(&mut self, number: u64) {
        unsafe { self.raw.as_mut().i_ino = number };
    }

    pub fn mode(&self) -> u16 {
        unsafe { self.raw.as_ref().i_mode }
    }

    pub fn set_mode(&mut self, mode: u16) {
        unsafe { self.raw.as_mut().i_mode = mode };
    }

    pub fn size(&self) -> u64 {
        unsafe { self.raw.as_ref().i_size }
    }

    pub fn set_size(&mut self, size: u64) {
        unsafe { self.raw.as_mut().i_size = size };
    }

    pub fn set_owner(&mut self, uid: u32, gid: u32) {
        let raw = unsafe { self.raw.as_mut() };
        raw.i_uid = uid;
        raw.i_gid = gid;
    }

    pub fn set_links(&mut self, links: u32) {
        unsafe { self.raw.as_mut().i_nlink = links };
    }

    pub fn links(&self) -> u32 {
        unsafe { self.raw.as_ref().i_nlink }
    }

    pub fn set_blocks(&mut self, blocks: u64) {
        unsafe { self.raw.as_mut().i_blocks = blocks };
    }

    pub fn set_times(&mut self, atime: i64, ctime: i64, mtime: i64) {
        let raw = unsafe { self.raw.as_mut() };
        raw.i_atime.sec = atime;
        raw.i_atime.nsec = 0;
        raw.i_ctime.sec = ctime;
        raw.i_ctime.nsec = 0;
        raw.i_mtime.sec = mtime;
        raw.i_mtime.nsec = 0;
    }

    pub fn device(&self) -> u64 {
        unsafe { self.raw.as_ref().i_rdev }
    }

    pub fn filesystem_device(&self) -> u64 {
        let super_block = unsafe { self.raw.as_ref().i_sb };
        if super_block.is_null() {
            return 0;
        }
        unsafe { (*super_block).s_dev }
    }

    pub fn super_block(&self) -> Option<SuperBlock<'_>> {
        NonNull::new(unsafe { self.raw.as_ref().i_sb }).map(|raw| SuperBlock {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn set_device(&mut self, dev: u64) {
        unsafe { self.raw.as_mut().i_rdev = dev };
    }

    pub fn set_private(&mut self, private: *mut c_void) {
        unsafe { self.raw.as_mut().i_private = private };
    }

    pub fn set_operations(
        &mut self,
        inode_operations: Option<&'static bindings::vfs_inode_operations>,
        file_operations: Option<&'static bindings::vfs_file_operations>,
    ) {
        let raw = unsafe { self.raw.as_mut() };
        raw.i_op = inode_operations.map_or(core::ptr::null(), |ops| ops);
        raw.i_fop = file_operations.map_or(core::ptr::null(), |ops| ops);
    }

    pub fn set_inode_operations(&mut self, operations: &'static bindings::vfs_inode_operations) {
        unsafe { self.raw.as_mut().i_op = operations };
    }

    pub fn set_address_operations(
        &mut self,
        operations: Option<&'static bindings::vfs_address_space_operations>,
    ) {
        unsafe {
            self.raw.as_mut().i_mapping.a_ops = operations.map_or(core::ptr::null(), |ops| ops);
        }
    }

    pub fn writeback_mapping(&self, end: u64) -> Result<()> {
        let status = unsafe { bindings::na_vfs_mapping_writeback(self.raw.as_ptr(), end) };
        crate::Error::from_status(status)
    }

    pub fn writeback_mapping_range(&self, start: u64, end: u64, datasync: bool) -> Result<()> {
        let status = unsafe {
            bindings::na_vfs_mapping_writeback_range(self.raw.as_ptr(), start, end, datasync)
        };
        crate::Error::from_status(status)
    }

    pub fn truncate_mapping(&mut self, size: u64) {
        unsafe { bindings::na_vfs_mapping_truncate(self.raw.as_ptr(), size) };
    }

    pub fn initialize_child_owner(&self, mut mode: u16) -> (u16, u32, u32) {
        let mut uid = 0;
        let mut gid = 0;
        unsafe {
            bindings::na_vfs_init_new_inode_owner(self.raw.as_ptr(), &mut mode, &mut uid, &mut gid)
        };
        (mode, uid, gid)
    }
}

impl Drop for Inode<'_> {
    fn drop(&mut self) {
        if self.owned {
            unsafe { bindings::vfs_iput(self.raw.as_ptr()) };
            self.owned = false;
        }
    }
}

pub struct File<'a> {
    raw: NonNull<bindings::vfs_file>,
    lifetime: PhantomData<&'a mut bindings::vfs_file>,
}

impl File<'_> {
    /// Wrap a kernel-owned file pointer for the duration of a callback.
    ///
    /// # Safety
    /// `raw` must point to a live VFS file object.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_file) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn raw(&self) -> *mut bindings::vfs_file {
        self.raw.as_ptr()
    }

    pub fn inode(&self) -> Option<Inode<'_>> {
        NonNull::new(unsafe { self.raw.as_ref().f_inode }).map(|raw| Inode {
            raw,
            owned: false,
            lifetime: PhantomData,
        })
    }

    pub fn private(&self) -> *mut c_void {
        unsafe { self.raw.as_ref().private_data }
    }

    pub fn set_private(&mut self, private: *mut c_void) {
        unsafe { self.raw.as_mut().private_data = private };
    }

    pub fn position(&self) -> i64 {
        unsafe { self.raw.as_ref().f_pos }
    }

    pub fn set_position(&mut self, position: i64) {
        unsafe { self.raw.as_mut().f_pos = position };
    }

    pub fn cached_read(&mut self, buffer: &mut [u8], position: &mut i64) -> Result<usize> {
        let result = unsafe {
            bindings::na_vfs_file_cached_read(
                self.raw.as_ptr(),
                buffer.as_mut_ptr().cast(),
                buffer.len(),
                position,
            )
        };
        if result < 0 {
            return Err(crate::Error::from_status(result as i32)
                .err()
                .unwrap_or(crate::Error::Kernel(result as i32)));
        }
        usize::try_from(result).map_err(|_| crate::Error::InvalidArgument)
    }

    pub fn cached_write(&mut self, buffer: &[u8], position: &mut i64) -> Result<usize> {
        let result = unsafe {
            bindings::na_vfs_file_cached_write(
                self.raw.as_ptr(),
                buffer.as_ptr().cast(),
                buffer.len(),
                position,
            )
        };
        if result < 0 {
            return Err(crate::Error::from_status(result as i32)
                .err()
                .unwrap_or(crate::Error::Kernel(result as i32)));
        }
        usize::try_from(result).map_err(|_| crate::Error::InvalidArgument)
    }

    pub fn install_anonymous_inode(&mut self, inode: &mut Inode<'_>) -> Result<()> {
        let status = unsafe {
            bindings::na_vfs_file_install_anonymous_inode(self.raw.as_ptr(), inode.raw())
        };
        crate::Error::from_status(status)
    }

    pub fn general_map(&mut self, request: MmapRequest) -> Result<*mut c_void> {
        let result = unsafe {
            bindings::na_vfs_general_map(
                self.raw.as_ptr(),
                request.address,
                request.offset,
                request.size,
                request.protection,
                request.flags,
            )
        };
        let value = result as usize;
        if value >= usize::MAX - 4095 {
            return Err(crate::Error::from_status(value as i32)
                .err()
                .unwrap_or(crate::Error::Kernel(value as i32)));
        }
        Ok(result)
    }

    pub fn open_device(&mut self, device: u64) -> Result<()> {
        let result = unsafe { bindings::na_vfs_device_open(device, self.raw.as_ptr()) };
        status64(result).map(|_| ())
    }

    pub fn close_device(&mut self, device: u64) -> Result<()> {
        let result = unsafe { bindings::na_vfs_device_close(device, self.raw.as_ptr()) };
        status64(result).map(|_| ())
    }

    pub fn read_device(&mut self, device: u64, buffer: &mut [u8], offset: u64) -> Result<usize> {
        let result = unsafe {
            bindings::na_vfs_device_read(
                device,
                self.raw.as_ptr(),
                buffer.as_mut_ptr().cast(),
                offset,
                buffer.len(),
            )
        };
        usize::try_from(status64(result)?).map_err(|_| crate::Error::InvalidArgument)
    }

    pub fn write_device(&mut self, device: u64, buffer: &[u8], offset: u64) -> Result<usize> {
        let result = unsafe {
            bindings::na_vfs_device_write(
                device,
                self.raw.as_ptr(),
                buffer.as_ptr().cast(),
                offset,
                buffer.len(),
            )
        };
        usize::try_from(status64(result)?).map_err(|_| crate::Error::InvalidArgument)
    }

    pub fn ioctl_device(&mut self, device: u64, command: u64, argument: u64) -> Result<i64> {
        status64(unsafe {
            bindings::na_vfs_device_ioctl(device, self.raw.as_ptr(), command, argument)
        })
    }

    pub fn poll_device(&mut self, device: u64, events: u32) -> Result<u32> {
        let result = unsafe { bindings::na_vfs_device_poll(device, self.raw.as_ptr(), events) };
        u32::try_from(status64(result)?).map_err(|_| crate::Error::InvalidArgument)
    }

    pub fn map_device(&mut self, device: u64, request: MmapRequest) -> Result<*mut c_void> {
        let result = unsafe {
            bindings::na_vfs_device_map(
                device,
                self.raw.as_ptr(),
                request.address,
                request.offset,
                request.size,
                request.protection,
            )
        };
        let value = result as usize;
        if value >= usize::MAX - 4095 {
            return Err(crate::Error::from_status(value as i32)
                .err()
                .unwrap_or(crate::Error::Kernel(value as i32)));
        }
        Ok(result)
    }
}

fn status64(status: i64) -> Result<i64> {
    if status < 0 {
        let status = i32::try_from(status).unwrap_or(i32::MIN);
        return Err(crate::Error::from_status(status)
            .err()
            .unwrap_or(crate::Error::Kernel(status)));
    }
    Ok(status)
}

#[derive(Clone, Copy)]
pub struct MmapRequest {
    pub address: *mut c_void,
    pub offset: usize,
    pub size: usize,
    pub protection: usize,
    pub flags: u64,
}

pub struct Path<'a> {
    raw: NonNull<bindings::vfs_path>,
    lifetime: PhantomData<&'a bindings::vfs_path>,
}

impl Path<'_> {
    /// Wrap a kernel-owned path pointer for the duration of a callback.
    ///
    /// # Safety
    /// `raw` must point to a live VFS path object.
    pub unsafe fn from_raw(raw: *const bindings::vfs_path) -> Option<Self> {
        NonNull::new(raw.cast_mut()).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn raw(&self) -> *const bindings::vfs_path {
        self.raw.as_ptr()
    }

    pub fn dentry(&self) -> Option<Dentry<'_>> {
        NonNull::new(unsafe { self.raw.as_ref().dentry }).map(|raw| Dentry {
            raw,
            lifetime: PhantomData,
        })
    }
}

pub struct Kstat<'a> {
    raw: NonNull<bindings::vfs_kstat>,
    lifetime: PhantomData<&'a mut bindings::vfs_kstat>,
}

impl Kstat<'_> {
    /// Wrap a kernel-owned stat buffer for the duration of a callback.
    ///
    /// # Safety
    /// `raw` must point to writable storage for a VFS stat object.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_kstat) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn raw(&mut self) -> *mut bindings::vfs_kstat {
        self.raw.as_ptr()
    }

    pub fn set_size(&mut self, size: u64) {
        unsafe { self.raw.as_mut().size = size };
    }

    pub fn set_mode(&mut self, mode: u16) {
        unsafe { self.raw.as_mut().mode = mode };
    }

    pub fn mask(&self) -> u64 {
        unsafe { self.raw.as_ref().mask }
    }

    fn selected(&self, field: bindings::na_vfs_statx_bits) -> bool {
        self.mask() == 0 || self.mask() & u64::from(field) != 0
    }

    fn explicitly_selected(&self, field: bindings::na_vfs_statx_bits) -> bool {
        self.mask() & u64::from(field) != 0
    }

    pub fn selects_mode(&self) -> bool {
        self.selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_MODE)
    }

    pub fn selects_uid(&self) -> bool {
        self.selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_UID)
    }

    pub fn selects_gid(&self) -> bool {
        self.selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_GID)
    }

    pub fn selects_size(&self) -> bool {
        self.selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_SIZE)
    }

    pub fn selects_atime(&self) -> bool {
        self.explicitly_selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_ATIME)
    }

    pub fn selects_ctime(&self) -> bool {
        self.explicitly_selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_CTIME)
    }

    pub fn selects_mtime(&self) -> bool {
        self.explicitly_selected(bindings::na_vfs_statx_bits_NA_VFS_STATX_MTIME)
    }

    pub fn mode(&self) -> u16 {
        unsafe { self.raw.as_ref().mode }
    }

    pub fn uid(&self) -> u32 {
        unsafe { self.raw.as_ref().uid }
    }

    pub fn gid(&self) -> u32 {
        unsafe { self.raw.as_ref().gid }
    }

    pub fn size(&self) -> u64 {
        unsafe { self.raw.as_ref().size }
    }

    pub fn atime_seconds(&self) -> i64 {
        unsafe { self.raw.as_ref().atime.sec }
    }

    pub fn ctime_seconds(&self) -> i64 {
        unsafe { self.raw.as_ref().ctime.sec }
    }

    pub fn mtime_seconds(&self) -> i64 {
        unsafe { self.raw.as_ref().mtime.sec }
    }

    pub fn fill_from_inode(&mut self, inode: &Inode<'_>) {
        let source = unsafe { inode.raw.as_ref() };
        let target = unsafe { self.raw.as_mut() };
        target.ino = source.i_ino;
        target.dev = unsafe { source.i_sb.as_ref() }.map_or(0, |sb| sb.s_dev);
        target.rdev = source.i_rdev;
        target.mode = source.i_mode;
        target.uid = source.i_uid;
        target.gid = source.i_gid;
        target.nlink = source.i_nlink;
        target.size = source.i_size;
        target.blocks = source.i_blocks;
        target.blksize = 1u32.checked_shl(source.i_blkbits).unwrap_or(0);
        target.atime = source.i_atime;
        target.btime = source.i_btime;
        target.ctime = source.i_ctime;
        target.mtime = source.i_mtime;
    }
}

pub struct Dentry<'a> {
    raw: NonNull<bindings::vfs_dentry>,
    lifetime: PhantomData<&'a mut bindings::vfs_dentry>,
}

impl Dentry<'_> {
    /// Wrap a VFS dentry pointer supplied by the kernel callback ABI.
    ///
    /// # Safety
    /// The pointer must remain valid for the returned borrow's lifetime.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_dentry) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn raw(&self) -> *mut bindings::vfs_dentry {
        self.raw.as_ptr()
    }

    pub fn inode(&self) -> Option<Inode<'_>> {
        NonNull::new(unsafe { self.raw.as_ref().d_inode }).map(|raw| Inode {
            raw,
            owned: false,
            lifetime: PhantomData,
        })
    }

    pub fn name(&self) -> &[u8] {
        let name = unsafe { &self.raw.as_ref().d_name };
        if name.name.is_null() || name.len == 0 {
            return &[];
        }
        unsafe { slice::from_raw_parts(name.name.cast::<u8>(), name.len as usize) }
    }

    pub fn instantiate(&mut self, inode: &mut Inode<'_>) {
        unsafe { bindings::vfs_d_instantiate(self.raw.as_ptr(), inode.raw()) };
    }

    pub fn instantiate_none(&mut self) {
        unsafe { bindings::vfs_d_instantiate(self.raw.as_ptr(), core::ptr::null_mut()) };
    }
}

pub struct DirContext<'a> {
    raw: NonNull<bindings::vfs_dir_context>,
    lifetime: PhantomData<&'a mut bindings::vfs_dir_context>,
}

impl DirContext<'_> {
    /// Wrap a kernel-owned directory iterator context.
    ///
    /// # Safety
    /// `raw` must point to a live directory context for the callback duration.
    pub unsafe fn from_raw(raw: *mut bindings::vfs_dir_context) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self {
            raw,
            lifetime: PhantomData,
        })
    }

    pub fn position(&self) -> i64 {
        unsafe { self.raw.as_ref().pos }
    }

    pub fn emit(&mut self, name: &[u8], next_position: i64, inode: u64, kind: u32) -> bool {
        let Some(actor) = (unsafe { self.raw.as_ref().actor }) else {
            return false;
        };
        let Ok(length) = i32::try_from(name.len()) else {
            return false;
        };
        let accepted = unsafe {
            actor(
                self.raw.as_ptr(),
                name.as_ptr().cast(),
                length,
                next_position,
                inode,
                kind,
            )
        } == 0;
        if accepted {
            unsafe { self.raw.as_mut().pos = next_position };
        }
        accepted
    }
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
