#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

extern crate alloc;

mod block;
mod bpb;
mod vfs;
mod volume;

use na_std::vfs::{FileSystem, FileSystemRegistration, FsContext};
use na_std::{Result, module_entry};

static FAT: FatFileSystem = FatFileSystem;
static REGISTRATION: FileSystemRegistration<FatFileSystem> =
    FileSystemRegistration::new(&FAT, c"fat", 0);

struct FatFileSystem;

impl FileSystem for FatFileSystem {
    fn get_tree(&self, context: &mut FsContext<'_>) -> Result<()> {
        vfs::get_tree(context)
    }

    fn put_super(&self, super_block: &mut na_std::vfs::SuperBlock<'_>) {
        vfs::put_super(super_block)
    }

    fn evict_inode(&self, inode: &mut na_std::vfs::Inode<'_>) {
        vfs::evict_inode(inode)
    }

    fn statfs(
        &self,
        path: &na_std::vfs::Path<'_>,
        stat: &mut na_std::vfs::StatFs<'_>,
    ) -> Result<()> {
        vfs::statfs(path, stat)
    }
}

fn init() -> Result<()> {
    REGISTRATION.register()
}

module_entry!(init);
