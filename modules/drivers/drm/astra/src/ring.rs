//! Ring buffer helpers (Linux `amdgpu_ring.c`): CPU-side writes,
//! doorbell commit and the scratch-register ring test.

use na_std::{Error, Result};

use crate::device::Adapter;
use crate::mem::Bo;

/// Engine-specific conventions from the Linux `amdgpu_ring_funcs` tables.
#[derive(Clone, Copy)]
pub enum RingKind {
    Gfx,
    Sdma,
    Vcn { align_mask: u32 },
}

#[derive(Clone, Copy)]
pub struct RingConfig {
    pub doorbell: u32,
    pub rptr_wb: u64,
    pub wptr_wb: u64,
    pub me: u32,
    pub pipe: u32,
    pub queue: u32,
    pub kind: RingKind,
}

/// A ring buffer: a GART BO written by the CPU, consumed by the engine,
/// signaled through a doorbell.
pub struct Ring {
    pub bo: Bo,
    pub gpu_addr: u64,
    pub size: usize,
    /// Write pointer in dwords.
    pub wptr: u32,
    /// BAR2 dword index of the ring doorbell (Linux doorbell units).
    pub doorbell: u32,
    pub kind: RingKind,
    /// GPU address of the rptr writeback slot.
    pub rptr_wb: u64,
    /// GPU address of the wptr writeback slot.
    pub wptr_wb: u64,
    /// Engine / pipe / queue selectors (me = 1-based MEC id).
    pub me: u32,
    pub pipe: u32,
    pub queue: u32,
    /// GFXHUB invalidation engine assigned to this ring. Linux reserves
    /// engines 2/3 for firmware and engine 17 for CPU-side GART flushes;
    /// scheduler rings are assigned from 0, 1, 4..16.
    pub vm_inv_eng: u32,
    /// Per-ring hardware fence writeback and sequence. These mirror
    /// `ring->fence_drv.gpu_addr` / `sync_seq` in Linux and are also used
    /// for pipeline synchronization across context switches.
    pub fence_wb: u64,
    pub fence_seq: u64,
    /// Last userspace context submitted on this ring.
    pub current_ctx: Option<u64>,
    /// MQD buffer (KIQ / compute rings).
    pub mqd: Option<Bo>,
}

impl Ring {
    pub fn new(bo: Bo, config: RingConfig) -> Self {
        let gpu_addr = bo.gpu_addr;
        let size = bo.size;
        Self {
            bo,
            gpu_addr,
            size,
            wptr: 0,
            doorbell: config.doorbell,
            kind: config.kind,
            rptr_wb: config.rptr_wb,
            wptr_wb: config.wptr_wb,
            me: config.me,
            pipe: config.pipe,
            queue: config.queue,
            vm_inv_eng: u32::MAX,
            fence_wb: 0,
            fence_seq: 0,
            current_ctx: None,
            mqd: None,
        }
    }

    pub fn write(&mut self, value: u32) -> Result<()> {
        let cpu = self.bo.cpu.as_mut().ok_or(Error::NoDevice)?;
        let at = (self.wptr as usize * 4) & (self.size - 1);
        let dst = cpu.as_mut_slice().get_mut(at..at + 4).ok_or(Error::Range)?;
        dst.copy_from_slice(&value.to_le_bytes());
        self.wptr += 1;
        Ok(())
    }

    /// Waits until the hardware read pointer leaves enough room for one job.
    /// Linux's `amdgpu_ring_alloc()` performs the same back-pressure before
    /// emitting an asynchronous submission; without it CPU submissions can
    /// wrap and overwrite commands the GPU has not consumed yet.
    pub fn wait_for_space(
        &self,
        dev: &mut Adapter,
        required_dwords: u32,
        timeout_us: u32,
    ) -> Result<()> {
        let ring_dwords = u32::try_from(self.size / 4).map_err(|_| Error::Range)?;
        if required_dwords == 0 || required_dwords >= ring_dwords {
            return Err(Error::InvalidArgument);
        }
        let mask = ring_dwords - 1;
        for _ in 0..timeout_us {
            let rptr = dev
                .wb
                .as_mut()
                .ok_or(Error::NoDevice)?
                .read_u64(self.rptr_wb)? as u32;
            let used = self.wptr.wrapping_sub(rptr) & mask;
            let free = ring_dwords - used - 1;
            if free >= required_dwords {
                return Ok(());
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        Err(Error::Io)
    }

    fn pad_for_commit(&mut self) -> Result<()> {
        let align_mask = match self.kind {
            RingKind::Gfx => 0xff,
            RingKind::Sdma => 0x0f,
            RingKind::Vcn { align_mask } => align_mask,
        };
        let count = (align_mask + 1 - (self.wptr & align_mask)) & align_mask;
        match self.kind {
            RingKind::Gfx if count == 1 => {
                // gfx_v10_ring_insert_nop: a lone header is itself a NOP.
                self.write(packet3(PACKET3_NOP, 0x3fff))?;
            }
            RingKind::Gfx if count > 1 => {
                // The first header covers exactly the remaining padding
                // dwords. The following values are packet payload, matching
                // gfx_v10_ring_insert_nop + amdgpu_ring_insert_nop.
                self.write(packet3(PACKET3_NOP, (count - 2).min(0x3ffe)))?;
                for _ in 0..count - 1 {
                    self.write(packet3(PACKET3_NOP, 0x3fff))?;
                }
            }
            RingKind::Sdma | RingKind::Vcn { .. } => {
                for _ in 0..count {
                    // SDMA_OP_NOP and VCN/JPEG NO_OP are zero.
                    self.write(0)?;
                }
            }
            RingKind::Gfx => {}
        }
        Ok(())
    }

    /// Makes pending ring writes visible, updates the writeback pointer and
    /// rings the engine-specific doorbell exactly like Linux set_wptr.
    pub fn commit(&mut self, dev: &mut Adapter) -> Result<()> {
        self.pad_for_commit()?;
        if let Some(cpu) = self.bo.cpu.as_ref() {
            cpu.sync_for_device();
        }
        match self.kind {
            RingKind::Gfx => {
                dev.wb
                    .as_mut()
                    .ok_or(Error::NoDevice)?
                    .write_u64(self.wptr_wb, self.wptr as u64)?;
                dev.regs.doorbell_write64(self.doorbell, self.wptr as u64)
            }
            RingKind::Sdma => {
                let wptr = (self.wptr as u64) << 2;
                dev.wb
                    .as_mut()
                    .ok_or(Error::NoDevice)?
                    .write_u64(self.wptr_wb, wptr)?;
                dev.regs.doorbell_write64(self.doorbell, wptr)
            }
            RingKind::Vcn { .. } => {
                dev.wb
                    .as_mut()
                    .ok_or(Error::NoDevice)?
                    .write_u32(self.wptr_wb, self.wptr)?;
                dev.regs.doorbell_write32(self.doorbell, self.wptr)
            }
        }
    }

    pub fn reset(&mut self) {
        self.wptr = 0;
    }

    /// Emits the scratch-register ring test (PACKET3_SET_UCONFIG_REG to
    /// mmSCRATCH_REG0) and polls for completion (Linux
    /// `gfx_v10_0_ring_test_ring`).
    pub fn scratch_test(
        &mut self,
        dev: &mut Adapter,
        scratch_reg: u32,
        timeout_us: u32,
    ) -> Result<()> {
        // `scratch_reg` is already SOC15_REG_OFFSET(GC, 0, ...), so raw
        // access is required; read_ip/write_ip would add the GC base twice.
        dev.regs.write_raw(scratch_reg, 0xCAFE_DEAD)?;
        self.write(packet3(PACKET3_SET_UCONFIG_REG, 1))?;
        self.write(scratch_reg - PACKET3_SET_UCONFIG_REG_START)?;
        self.write(0xDEAD_BEEF)?;
        self.commit(dev)?;

        for _ in 0..timeout_us {
            if dev.regs.read_raw(scratch_reg)? == 0xDEAD_BEEF {
                return Ok(());
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        Err(Error::Io)
    }
}

/// PACKET3 opcodes (soc15d.h / amdgpu ring spec).
pub const fn packet3(op: u32, count: u32) -> u32 {
    (0x3 << 30) | (op << 8) | (count << 16)
}

pub const PACKET3_NOP: u32 = 0x10;
pub const PACKET3_SET_BASE: u32 = 0x11;
pub const PACKET3_CONTEXT_CONTROL: u32 = 0x28;
pub const PACKET3_SET_CONTEXT_REG: u32 = 0x69;
pub const PACKET3_SET_UCONFIG_REG: u32 = 0x79;
pub const PACKET3_CLEAR_STATE: u32 = 0x12;
pub const PACKET3_SET_RESOURCES: u32 = 0xA0;
pub const PACKET3_MAP_QUEUES: u32 = 0xA2;
pub const PACKET3_PREAMBLE_CNTL: u32 = 0x4A;

pub const PACKET3_SET_CONTEXT_REG_START: u32 = 0x0000_A000;
pub const PACKET3_SET_UCONFIG_REG_START: u32 = 0x0000_C000;
pub const PACKET3_PREAMBLE_BEGIN_CLEAR_STATE: u32 = 2 << 28;
pub const PACKET3_PREAMBLE_END_CLEAR_STATE: u32 = 3 << 28;
pub const PACKET3_BASE_INDEX_CE_PARTITION: u32 = 3;

/// PACKET3_MAP_QUEUES header field positions (amdgpu_gfx.c).
pub const fn map_queues_dw1(queue: u32, pipe: u32, me: u32) -> u32 {
    (queue << 13) | (pipe << 16) | (me << 18) | (1 << 29)
}

/// PACKET3_MAP_QUEUES CONTROL2 stores the dword doorbell index in bits 31:2.
pub const fn map_queues_doorbell_offset(index: u32) -> u32 {
    index << 2
}
