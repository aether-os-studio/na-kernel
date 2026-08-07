use na_std::{Error, Result, memory::DmaBuffer};

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum BufferKind {
    Dumb2D,
    Virgl3D,
}

pub struct Buffer {
    pub handle: u32,
    pub resource_id: u32,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub kind: BufferKind,
    pub ref_count: u32,
    pub memory: DmaBuffer,
}

impl Buffer {
    pub fn new(handle: u32, resource_id: u32, width: u32, height: u32, pitch: u32) -> Result<Self> {
        let size = (pitch as usize)
            .checked_mul(height as usize)
            .ok_or(Error::OutOfMemory)?;
        Self::sized(
            handle,
            resource_id,
            width,
            height,
            pitch,
            size,
            BufferKind::Dumb2D,
        )
    }

    pub fn sized(
        handle: u32,
        resource_id: u32,
        width: u32,
        height: u32,
        pitch: u32,
        size: usize,
        kind: BufferKind,
    ) -> Result<Self> {
        const PAGE_SIZE: usize = 4096;
        let size = size
            .checked_add(PAGE_SIZE - 1)
            .map(|value| value & !(PAGE_SIZE - 1))
            .ok_or(Error::OutOfMemory)?;
        Ok(Self {
            handle,
            resource_id,
            width,
            height,
            pitch,
            kind,
            ref_count: 1,
            memory: DmaBuffer::zeroed(size)?,
        })
    }

    pub fn attach_entry(&self) -> [u8; 16] {
        let mut entry = [0; 16];
        entry[..8].copy_from_slice(&self.memory.physical_address().get().to_le_bytes());
        entry[8..12].copy_from_slice(&(self.memory.length() as u32).to_le_bytes());
        entry
    }
}
