//! Register access layer: direct MMIO, the NBIO RSMU indirect window,
//! SMN space, the VRAM (MM_INDEX) window and the doorbell aperture.
//!
//! Register values from the asic_reg headers are dword offsets; the full
//! register number is `discovery_base + reg` and the MMIO byte offset is
//! `full * 4`. Registers beyond the BAR5 aperture are reached through the
//! NBIO RSMU index/data window (Linux `amdgpu_device_indirect_rreg`).

use na_std::arch::fence;
use na_std::io::MmioRegion;
use na_std::sync::SpinLock;
use na_std::{Error, Result};

use crate::ip::{HWIP_COUNT, HwIp, MAX_BASE_ADDR, MAX_INSTANCE};

/// VRAM access window registers (raw, base-0; Linux `mmMM_INDEX` family).
const MM_INDEX: u32 = 0x0;
const MM_INDEX_HI: u32 = 0x6;
const MM_DATA: u32 = 0x1;
/// Bit 31 of MM_INDEX selects the VRAM access mode.
const MM_INDEX_VRAM: u32 = 0x8000_0000;

/// DCN (display controller) base-address segments, indexed by a register's
/// `mm*_BASE_IDX` (Linux `dimgrey_cavefish_ip_offset.h` `DCN_BASE`). The DCN
/// register address is `DCN_BASE_SEG[base_idx] + mmREG`.
const DCN_BASE_SEG: [u32; 6] = [
    0x0000_0012,
    0x0000_00C0,
    0x0000_34C0,
    0x0000_9000,
    0x0240_3C00,
    0x0000_0000,
];

/// All register access is funneled through this type. Access is
/// single-threaded during init (the RSMU window is additionally guarded
/// by a spinlock for later use from multiple contexts).
pub struct Regs {
    mmio: MmioRegion,
    vram: MmioRegion,
    doorbell: MmioRegion,
    reg_offset: [[[u32; MAX_BASE_ADDR]; MAX_INSTANCE]; HWIP_COUNT],
    rsmu_index: u32,
    rsmu_data: u32,
    rsmu_lock: SpinLock<()>,
}

/// Direct BAR5 view used only by the DCN cursor path. Linux serializes this
/// path with display state rather than the global amdgpu device lock, so it
/// must not share ASTRA's submission mutex. Cursor registers are inside the
/// direct BAR5 aperture and never use the RSMU index/data window.
pub struct DcnCursorRegs {
    mmio: MmioRegion,
}

/// Read-modify-write field helpers over the generated shift/mask
/// constants (Linux `REG_SET_FIELD` / `REG_GET_FIELD`).
pub const fn set_field(reg: u32, shift: u64, mask: u64, value: u64) -> u32 {
    let mask = mask as u32;
    (reg & !mask) | (((value << shift) as u32) & mask)
}

pub const fn get_field(reg: u32, shift: u64, mask: u64) -> u32 {
    ((reg & mask as u32) as u64 >> shift) as u32
}

impl Regs {
    pub fn new(mmio: MmioRegion, vram: MmioRegion, doorbell: MmioRegion) -> Self {
        Self {
            mmio,
            vram,
            doorbell,
            reg_offset: [[[0; MAX_BASE_ADDR]; MAX_INSTANCE]; HWIP_COUNT],
            rsmu_index: 0,
            rsmu_data: 0,
            rsmu_lock: SpinLock::new(()),
        }
    }

    pub fn set_reg_offset(&mut self, ip: HwIp, inst: usize, base_idx: usize, value: u32) {
        if inst < MAX_INSTANCE && base_idx < MAX_BASE_ADDR {
            self.reg_offset[ip.index()][inst][base_idx] = value;
        }
    }

    /// Configures the NBIO RSMU index/data window registers (full dword
    /// registers, NBIO base + `regBIF_BX_PF0_RSMU_INDEX`/`DATA`).
    pub fn set_rsmu_window(&mut self, index: u32, data: u32) {
        self.rsmu_index = index;
        self.rsmu_data = data;
    }

    pub fn dcn_cursor_regs(&self) -> DcnCursorRegs {
        // This view is restricted to direct DCN cursor registers and is
        // protected by DisplayDevice's cursor register mutex. It never
        // touches the shared RSMU indirect index/data window.
        let mmio = self.mmio.shared_view();
        DcnCursorRegs { mmio }
    }

    /// Reads a raw (base-0) register, used before discovery fills the
    /// per-IP bases (e.g. `mmMP0_SMN_C2PMSG_33`, `mmRCC_CONFIG_MEMSIZE`).
    pub fn read_raw(&mut self, reg: u32) -> Result<u32> {
        self.read_full(reg)
    }

    pub fn write_raw(&mut self, reg: u32, value: u32) -> Result<()> {
        self.write_full(reg, value)
    }

    /// Reads a register inside an IP block discovered base address.
    pub fn read_ip(&mut self, ip: HwIp, inst: usize, reg: u32, base_idx: usize) -> Result<u32> {
        let full = self
            .base(ip, inst, base_idx)
            .checked_add(reg)
            .ok_or(Error::InvalidArgument)?;
        self.read_full(full)
    }

    /// Writes a register inside an IP block discovered base address.
    pub fn write_ip(
        &mut self,
        ip: HwIp,
        inst: usize,
        reg: u32,
        base_idx: usize,
        value: u32,
    ) -> Result<()> {
        let full = self
            .base(ip, inst, base_idx)
            .checked_add(reg)
            .ok_or(Error::InvalidArgument)?;
        self.write_full(full, value)
    }

    /// Read-modify-write with and/or masks (Linux golden-register RMW).
    pub fn rmw_ip(
        &mut self,
        ip: HwIp,
        inst: usize,
        reg: u32,
        base_idx: usize,
        and_mask: u32,
        or_mask: u32,
    ) -> Result<()> {
        let value = self.read_ip(ip, inst, reg, base_idx)?;
        self.write_ip(ip, inst, reg, base_idx, (value & and_mask) | or_mask)
    }

    fn base(&self, ip: HwIp, inst: usize, base_idx: usize) -> u32 {
        if inst >= MAX_INSTANCE || base_idx >= MAX_BASE_ADDR {
            return 0;
        }
        self.reg_offset[ip.index()][inst][base_idx]
    }

    /// Returns a discovered register base address.
    pub fn base_u32(&self, ip: HwIp, inst: usize, base_idx: usize) -> Result<u32> {
        if inst >= MAX_INSTANCE || base_idx >= MAX_BASE_ADDR {
            return Err(Error::InvalidArgument);
        }
        Ok(self.reg_offset[ip.index()][inst][base_idx])
    }

    /// Reads a full dword register: direct when inside the BAR5 aperture,
    /// otherwise through the RSMU index/data window.
    pub fn read_full(&mut self, full: u32) -> Result<u32> {
        if full as usize * 4 < self.mmio.length() {
            return self.mmio.read::<u32>(full as usize * 4);
        }
        let _guard = self.rsmu_lock.lock();
        self.mmio
            .write::<u32>(self.rsmu_index as usize * 4, full * 4)?;
        fence::mfence();
        self.mmio.read::<u32>(self.rsmu_data as usize * 4)
    }

    /// Writes a full dword register (direct or through the RSMU window).
    pub fn write_full(&mut self, full: u32, value: u32) -> Result<()> {
        if full as usize * 4 < self.mmio.length() {
            return self.mmio.write::<u32>(full as usize * 4, value);
        }
        let _guard = self.rsmu_lock.lock();
        self.mmio
            .write::<u32>(self.rsmu_index as usize * 4, full * 4)?;
        fence::mfence();
        self.mmio.write::<u32>(self.rsmu_data as usize * 4, value)
    }

    /// SMN space access through the RSMU window; the SMN address is
    /// passed verbatim as the byte address (Linux `RREG32_PCIE`).
    pub fn smn_read(&mut self, addr: u32) -> Result<u32> {
        let _guard = self.rsmu_lock.lock();
        self.mmio.write::<u32>(self.rsmu_index as usize * 4, addr)?;
        fence::mfence();
        self.mmio.read::<u32>(self.rsmu_data as usize * 4)
    }

    /// Reads a DCN register: `DCN_BASE_SEG[base_idx] + reg` (Linux
    /// `dcn302_resource.c` BASE/SR macros). Handles direct MMIO or the RSMU
    /// window like `read_full`.
    pub fn read_dcn(&mut self, reg: u32, base_idx: usize) -> Result<u32> {
        let seg = DCN_BASE_SEG
            .get(base_idx)
            .copied()
            .ok_or(Error::InvalidArgument)?;
        self.read_full(seg.wrapping_add(reg))
    }

    /// Writes a DCN register (see `read_dcn`).
    pub fn write_dcn(&mut self, reg: u32, base_idx: usize, value: u32) -> Result<()> {
        let seg = DCN_BASE_SEG
            .get(base_idx)
            .copied()
            .ok_or(Error::InvalidArgument)?;
        self.write_full(seg.wrapping_add(reg), value)
    }

    /// Reads VRAM dwords at `pos` (dword-aligned byte offset into the VRAM
    /// aperture) — direct BAR0 access when inside the aperture, else the
    /// MM_INDEX window (Linux `amdgpu_device_mm_access`).
    pub fn vram_read_dwords(&mut self, pos: u64, out: &mut [u32]) -> Result<()> {
        if !pos.is_multiple_of(4) {
            return Err(Error::InvalidArgument);
        }
        let end = pos
            .checked_add((out.len() as u64) * 4)
            .ok_or(Error::InvalidArgument)?;
        if end <= self.vram.length() as u64 {
            for (i, dword) in out.iter_mut().enumerate() {
                *dword = self.vram.read::<u32>(pos as usize + i * 4)?;
            }
            return Ok(());
        }
        let mut hi = u64::MAX;
        for (i, dword) in out.iter_mut().enumerate() {
            let addr = pos + (i as u64) * 4;
            if addr >> 31 != hi {
                self.write_raw(MM_INDEX_HI, (addr >> 31) as u32)?;
                hi = addr >> 31;
            }
            self.write_raw(MM_INDEX, (addr as u32) | MM_INDEX_VRAM)?;
            *dword = self.read_raw(MM_DATA)?;
        }
        Ok(())
    }

    /// Writes VRAM dwords at `pos` (dword-aligned byte offset into the
    /// VRAM aperture) — direct BAR0 access when inside the aperture, else
    /// the MM_INDEX window.
    pub fn vram_write_dwords(&mut self, pos: u64, data: &[u32]) -> Result<()> {
        if !pos.is_multiple_of(4) {
            return Err(Error::InvalidArgument);
        }
        let end = pos
            .checked_add((data.len() as u64) * 4)
            .ok_or(Error::InvalidArgument)?;
        if end <= self.vram.length() as u64 {
            for (i, dword) in data.iter().enumerate() {
                self.vram.write::<u32>(pos as usize + i * 4, *dword)?;
            }
            return Ok(());
        }
        let mut hi = u64::MAX;
        for (i, dword) in data.iter().enumerate() {
            let addr = pos + (i as u64) * 4;
            if addr >> 31 != hi {
                self.write_raw(MM_INDEX_HI, (addr >> 31) as u32)?;
                hi = addr >> 31;
            }
            self.write_raw(MM_INDEX, (addr as u32) | MM_INDEX_VRAM)?;
            self.write_raw(MM_DATA, *dword)?;
        }
        Ok(())
    }

    /// Rings a 32-bit doorbell; `index` is a dword index into the BAR2
    /// aperture, matching Linux `WDOORBELL32`.
    pub fn doorbell_write32(&mut self, index: u32, value: u32) -> Result<()> {
        let offset = (index as usize)
            .checked_mul(core::mem::size_of::<u32>())
            .ok_or(Error::InvalidArgument)?;
        fence::sfence();
        self.doorbell.write::<u32>(offset, value)
    }

    /// Rings a 64-bit doorbell; Linux still addresses the aperture in
    /// dwords, so the byte offset is `index * sizeof(u32)`.
    pub fn doorbell_write64(&mut self, index: u32, value: u64) -> Result<()> {
        let offset = (index as usize)
            .checked_mul(core::mem::size_of::<u32>())
            .ok_or(Error::InvalidArgument)?;
        fence::sfence();
        self.doorbell.write::<u64>(offset, value)
    }

    pub fn aperture_size(&self) -> usize {
        self.vram.length()
    }
}

impl DcnCursorRegs {
    fn offset(&self, reg: u32, base_idx: usize) -> Result<usize> {
        let segment = DCN_BASE_SEG
            .get(base_idx)
            .copied()
            .ok_or(Error::InvalidArgument)?;
        let full = segment.checked_add(reg).ok_or(Error::InvalidArgument)?;
        let offset = usize::try_from(full)
            .map_err(|_| Error::InvalidArgument)?
            .checked_mul(core::mem::size_of::<u32>())
            .ok_or(Error::InvalidArgument)?;
        if offset
            .checked_add(core::mem::size_of::<u32>())
            .is_none_or(|end| end > self.mmio.length())
        {
            return Err(Error::InvalidArgument);
        }
        Ok(offset)
    }

    pub fn read_dcn(&mut self, reg: u32, base_idx: usize) -> Result<u32> {
        self.mmio.read(self.offset(reg, base_idx)?)
    }

    pub fn write_dcn(&mut self, reg: u32, base_idx: usize, value: u32) -> Result<()> {
        self.mmio.write(self.offset(reg, base_idx)?, value)
    }
}
