use crate::volume::{Node, Volume};
use alloc::boxed::Box;
use na_std::{
    Error, Result, bindings,
    vfs::{
        AddressSpaceOperationsTable, Dentry, DirContext, File, FileOperations, FileOperationsTable,
        FsContext, Inode, InodeOperations, InodeOperationsTable, Kstat, Path, StatFs, SuperBlock,
    },
};

const S_IFREG: u16 = 0o100000;
const S_IFDIR: u16 = 0o040000;
const DT_DIR: u32 = 4;
const DT_REG: u32 = 8;
const MAGIC: u64 = 0x4641_5431_3233_2f00;

static INODE_OPS: InodeOperationsTable<FatInodeOps> = InodeOperationsTable::new(&FatInodeOps);
static FILE_OPS: FileOperationsTable<FatFileOps> = FileOperationsTable::new(&FatFileOps);
static AOPS: AddressSpaceOperationsTable<FatAddressOps> =
    AddressSpaceOperationsTable::new(&FatAddressOps);

pub fn get_tree(context: &mut FsContext<'_>) -> Result<()> {
    let dev = context.resolve_device()?;
    let volume = Box::new(Volume::open(dev)?);
    let root = volume.root();
    let mut sb = context.super_block().ok_or(Error::InvalidArgument)?;
    sb.set_device(dev);
    sb.set_magic(MAGIC);
    sb.set_private(Box::into_raw(volume).cast());
    let mut inode = sb.allocate_inode()?;
    set_inode(&mut inode, root);
    let mut dentry = sb.allocate_root()?;
    dentry.instantiate(&mut inode);
    sb.set_root(&dentry);
    Ok(())
}

pub fn put_super(sb: &mut SuperBlock<'_>) {
    let p = sb.private();
    if !p.is_null() {
        unsafe {
            drop(Box::from_raw(p.cast::<Volume>()));
        }
        sb.set_private(core::ptr::null_mut());
    }
}

pub fn evict_inode(inode: &mut Inode<'_>) {
    let p = unsafe { (*inode.raw()).i_private };
    if !p.is_null() {
        unsafe {
            drop(Box::from_raw(p.cast::<Node>()));
            (*inode.raw()).i_private = core::ptr::null_mut();
        }
    }
}

pub fn statfs(path: &Path<'_>, stat: &mut StatFs<'_>) -> Result<()> {
    let dentry = path.dentry().ok_or(Error::InvalidArgument)?;
    let inode = dentry.inode().ok_or(Error::InvalidArgument)?;
    let sb = inode.super_block().ok_or(Error::InvalidArgument)?;
    let volume = unsafe { &*(sb.private().cast::<Volume>()) };
    let (blocks, free, files) = volume.stats();
    stat.set(
        MAGIC,
        volume.block_size() as u64,
        blocks,
        free,
        free,
        files,
        free,
        255,
    );
    Ok(())
}

fn set_inode(inode: &mut Inode<'_>, node: Node) {
    inode.set_number(node.cluster as u64 + 1);
    inode.set_mode(if node.dir {
        S_IFDIR | 0o755
    } else {
        S_IFREG | 0o644
    });
    inode.set_size(node.size);
    inode.set_links(1);
    inode.set_private(Box::into_raw(Box::new(node)).cast());
    inode.set_operations(Some(INODE_OPS.raw()), Some(FILE_OPS.raw()));
    inode.set_address_operations(Some(AOPS.raw()));
}

struct FatInodeOps;
impl InodeOperations for FatInodeOps {
    fn lookup(
        &self,
        directory: &mut Inode<'_>,
        dentry: &mut Dentry<'_>,
        _flags: u32,
    ) -> Result<()> {
        let mut sb = directory.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &*(sb.private().cast::<Volume>()) };
        let parent = unsafe { &*(directory.raw().cast::<bindings::vfs_inode>()) };
        let pp = parent.i_private.cast::<Node>();
        match volume.lookup(unsafe { *pp }, dentry.name()) {
            Ok(node) => {
                let mut inode = sb.allocate_inode()?;
                set_inode(&mut inode, node);
                dentry.instantiate(&mut inode);
            }
            // A missing child is a normal result for lookup: the VFS needs
            // the negative dentry so create/mkdir can populate it.
            Err(Error::NotFound) => dentry.instantiate_none(),
            Err(error) => return Err(error),
        }
        Ok(())
    }
    fn getattr(
        &self,
        path: &Path<'_>,
        stat: &mut Kstat<'_>,
        _mask: u32,
        _flags: u32,
    ) -> Result<()> {
        let d = path.dentry().ok_or(Error::InvalidArgument)?;
        let inode = d.inode().ok_or(Error::InvalidArgument)?;
        stat.fill_from_inode(&inode);
        Ok(())
    }
    fn create(
        &self,
        directory: &mut Inode<'_>,
        dentry: &mut Dentry<'_>,
        mode: u16,
        _exclusive: bool,
    ) -> Result<()> {
        self.make(directory, dentry, mode, false)
    }
    fn mkdir(&self, directory: &mut Inode<'_>, dentry: &mut Dentry<'_>, mode: u16) -> Result<()> {
        self.make(directory, dentry, mode, true)
    }
    fn unlink(&self, directory: &mut Inode<'_>, dentry: &mut Dentry<'_>) -> Result<()> {
        self.remove(directory, dentry, false)
    }
    fn rmdir(&self, directory: &mut Inode<'_>, dentry: &mut Dentry<'_>) -> Result<()> {
        self.remove(directory, dentry, true)
    }
    fn rename(&self, context: &mut na_std::vfs::RenameContext<'_>) -> Result<()> {
        let od_raw = { context.old_directory().ok_or(Error::InvalidArgument)?.raw() };
        let old_name = {
            context
                .old_dentry()
                .ok_or(Error::InvalidArgument)?
                .name()
                .to_vec()
        };
        let nd_raw = { context.new_directory().ok_or(Error::InvalidArgument)?.raw() };
        let new_name = {
            context
                .new_dentry()
                .ok_or(Error::InvalidArgument)?
                .name()
                .to_vec()
        };
        let od = unsafe { Inode::from_raw(od_raw) }.ok_or(Error::InvalidArgument)?;
        let nd = unsafe { Inode::from_raw(nd_raw) }.ok_or(Error::InvalidArgument)?;
        let old_node = node_of(&od)?;
        let new_node = node_of(&nd)?;
        if old_node.cluster != new_node.cluster {
            return Err(Error::Unsupported);
        }
        let sb = od.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &mut *(sb.private().cast::<Volume>()) };
        volume.rename(old_node, &old_name, new_node, &new_name)
    }
    fn setattr(&self, dentry: &mut Dentry<'_>, stat: &Kstat<'_>) -> Result<()> {
        let inode = dentry.inode().ok_or(Error::NotFound)?;
        let sb = inode.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &mut *(sb.private().cast::<Volume>()) };
        let node = node_of(&inode)?;
        let updated = volume.truncate(node, stat.size())?;
        unsafe {
            *((*inode.raw()).i_private.cast::<Node>()) = updated;
            (*inode.raw()).i_size = updated.size;
        }
        Ok(())
    }
}

impl FatInodeOps {
    fn make(
        &self,
        directory: &mut Inode<'_>,
        dentry: &mut Dentry<'_>,
        _mode: u16,
        is_dir: bool,
    ) -> Result<()> {
        let mut sb = directory.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &mut *(sb.private().cast::<Volume>()) };
        let node = volume.create(node_of(directory)?, dentry.name(), is_dir)?;
        let mut inode = sb.allocate_inode()?;
        set_inode(
            &mut inode,
            Node {
                size: node.size,
                ..node
            },
        );
        dentry.instantiate(&mut inode);
        Ok(())
    }
    fn remove(
        &self,
        directory: &mut Inode<'_>,
        dentry: &mut Dentry<'_>,
        is_dir: bool,
    ) -> Result<()> {
        let sb = directory.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &mut *(sb.private().cast::<Volume>()) };
        volume.remove(node_of(directory)?, dentry.name(), is_dir)
    }
}
fn node_of(inode: &Inode<'_>) -> Result<Node> {
    let p = unsafe { (*inode.raw()).i_private.cast::<Node>() };
    if p.is_null() {
        Err(Error::InvalidArgument)
    } else {
        Ok(unsafe { *p })
    }
}

struct FatFileOps;
impl FileOperations for FatFileOps {
    fn read(&self, file: &mut File<'_>, buffer: &mut [u8], position: &mut i64) -> Result<usize> {
        let inode = file.inode().ok_or(Error::InvalidArgument)?;
        let sb = inode.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &*(sb.private().cast::<Volume>()) };
        let node = unsafe { *((*inode.raw()).i_private.cast::<Node>()) };
        if node.dir {
            return Err(Error::IsDirectory);
        }
        let pos = u64::try_from((*position).max(0)).map_err(|_| Error::InvalidArgument)?;
        let n = volume.read(node, pos, buffer)?;
        *position += n as i64;
        Ok(n)
    }
    fn write(&self, file: &mut File<'_>, buffer: &[u8], position: &mut i64) -> Result<usize> {
        let inode = file.inode().ok_or(Error::InvalidArgument)?;
        let sb = inode.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &mut *(sb.private().cast::<Volume>()) };
        let node = node_of(&inode)?;
        let pos = u64::try_from(*position).map_err(|_| Error::InvalidArgument)?;
        let written = i64::try_from(buffer.len()).map_err(|_| Error::Range)?;
        let next_position = position.checked_add(written).ok_or(Error::Range)?;
        let updated = volume.write(node, pos, buffer)?;
        unsafe {
            *((*inode.raw()).i_private.cast::<Node>()) = updated;
            (*inode.raw()).i_size = updated.size;
        }
        *position = next_position;
        Ok(buffer.len())
    }
    fn llseek(&self, file: &mut File<'_>, offset: i64, whence: i32) -> Result<i64> {
        let size = file
            .inode()
            .map(|inode| inode.size())
            .ok_or(Error::InvalidArgument)?;
        let base = match whence {
            0 => 0,
            1 => file.position(),
            2 => i64::try_from(size).map_err(|_| Error::Range)?,
            _ => return Err(Error::InvalidArgument),
        };
        let next = base.checked_add(offset).ok_or(Error::Range)?;
        if next < 0 {
            return Err(Error::Range);
        }
        file.set_position(next);
        Ok(next)
    }
    fn flush(&self, file: &mut File<'_>) -> Result<()> {
        let inode = file.inode().ok_or(Error::InvalidArgument)?;
        let sb = inode.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &*(sb.private().cast::<Volume>()) };
        volume.flush()
    }
    fn fsync(&self, file: &mut File<'_>, _start: i64, _end: i64, _datasync: bool) -> Result<()> {
        self.flush(file)
    }
    fn iterate(&self, file: &mut File<'_>, context: &mut DirContext<'_>) -> Result<()> {
        let inode = file.inode().ok_or(Error::InvalidArgument)?;
        let sb = inode.super_block().ok_or(Error::InvalidArgument)?;
        let volume = unsafe { &*(sb.private().cast::<Volume>()) };
        let node = unsafe { *((*inode.raw()).i_private.cast::<Node>()) };
        let pos = usize::try_from(context.position().max(0)).map_err(|_| Error::InvalidArgument)?;
        let mut next = pos;
        let _ = volume.list(node, pos, &mut |name, n| {
            next += 1;
            context.emit(
                name,
                next as i64,
                n.cluster as u64 + 1,
                if n.dir { DT_DIR } else { DT_REG },
            )
        })?;
        let final_pos = context.position();
        drop(inode);
        file.set_position(final_pos);
        Ok(())
    }
}

struct FatAddressOps;
impl na_std::vfs::AddressSpaceOperations for FatAddressOps {}
