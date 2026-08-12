use na_std::{Result, vfs::BlockDevice};

pub trait BlockIo {
    fn read_at(&self, offset: u64, dst: &mut [u8]) -> Result<()>;
}

impl BlockIo for BlockDevice {
    fn read_at(&self, offset: u64, dst: &mut [u8]) -> Result<()> {
        self.read(offset, dst)
    }
}

pub fn le16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
pub fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
pub fn le64(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes([
        b[o],
        b[o + 1],
        b[o + 2],
        b[o + 3],
        b[o + 4],
        b[o + 5],
        b[o + 6],
        b[o + 7],
    ])
}
