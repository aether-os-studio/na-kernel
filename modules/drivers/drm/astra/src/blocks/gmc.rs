//! GMC IP block (Linux `gmc_v10_0.c` + gfxhub_v2_1/mmhub_v2_0).

use core::time::Duration;

use na_std::memory::DmaBuffer;
use na_std::time;
use na_std::{Error, Result};

use crate::atom::FIRMWARE_CAP_ENABLE_2STAGE_BIST_TRAINING;
use crate::dev_info;
use crate::device::Adapter;
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::mem::GPU_PAGE_SIZE;
use crate::regs::dcn3_0_2 as dcn;
use crate::regs::gc10_3_0 as gc;
use crate::regs::mmhub2_0_0 as mm;
use crate::regs::nbio4_3_0 as nbio;
use crate::regs::{Regs, get_field, set_field};

/// 512 MiB GART for GC 10.3.4 (Linux gmc_v10_0_mc_init default branch).
const GART_SIZE: u64 = 512 << 20;
/// Best-fit placement max MC address bound (Linux AMDGPU_GMC_HOLE_START).
const MC_HOLE_START: u64 = 0x0000_8000_0000_0000;
/// 48-bit MC address space.
const MC_MASK: u64 = 0xffff_ffff_ffff;

/// Number of VMIDs configured (Linux gfxhub setup_vmid_config, 0..=14).
const NUM_VMIDS: u32 = 15;
/// Invalidation engine used for the GART (Linux: "Use register 17").
const INV_ENG: u32 = 17;
/// Number of invalidation engines (Linux program_invalidation loop).
const NUM_INV_ENGS: u32 = 18;
/// Valid bit in a root page-directory entry (`AMDGPU_PTE_VALID`).
const PTE_VALID: u64 = 1;
/// Linux AMDGPU_VBIOS_VGA_ALLOCATION.
const VBIOS_VGA_ALLOCATION: u64 = 9 << 20;
/// Linux `DISCOVERY_TMR_OFFSET`, used when the VBIOS does not advertise a
/// larger firmware-owned top-of-VRAM region.
const DISCOVERY_TMR_OFFSET: u64 = 64 << 10;
const MEM_TRAIN_DATA_SIZE: u64 = 0x1000;
const ONE_MIB: u64 = 1 << 20;

/// VM sizing (Linux amdgpu_vm_adjust_size(256TB, 9, 3, 48)).
const BLOCK_SIZE: u32 = 9;
const NUM_LEVEL: u32 = 3;

pub struct GmcBlock {
    _version: IpVersion,
    _mmhub_version: IpVersion,
    // gfxhub distances and IP-relative invalidation registers
    ctx_distance: u32,
    ctx_addr_distance: u32,
    eng_distance: u32,
    eng_addr_distance: u32,
    vm_inv_eng0_sem: u32,
    vm_inv_eng0_req: u32,
    vm_inv_eng0_ack: u32,
    // mmhub equivalents
    mm_ctx_distance: u32,
    mm_ctx_addr_distance: u32,
    mm_eng_distance: u32,
    mm_eng_addr_distance: u32,
    mm_inv_eng0_sem: u32,
    mm_inv_eng0_req: u32,
    mm_inv_eng0_ack: u32,
    max_pfn: u64,
}

impl GmcBlock {
    pub fn new(version: IpVersion, mmhub_version: IpVersion) -> Self {
        Self {
            _version: version,
            _mmhub_version: mmhub_version,
            ctx_distance: 0,
            ctx_addr_distance: 0,
            eng_distance: 0,
            eng_addr_distance: 0,
            vm_inv_eng0_sem: 0,
            vm_inv_eng0_req: 0,
            vm_inv_eng0_ack: 0,
            mm_ctx_distance: 0,
            mm_ctx_addr_distance: 0,
            mm_eng_distance: 0,
            mm_eng_addr_distance: 0,
            mm_inv_eng0_sem: 0,
            mm_inv_eng0_req: 0,
            mm_inv_eng0_ack: 0,
            max_pfn: 0,
        }
    }

    /// Linux gfxhub_v2_1_init: hub register layout.
    fn gfxhub_init(&mut self, _regs: &Regs) {
        self.ctx_distance = gc::mmGCVM_CONTEXT1_CNTL - gc::mmGCVM_CONTEXT0_CNTL;
        self.ctx_addr_distance = gc::mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32
            - gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
        self.eng_distance = gc::mmGCVM_INVALIDATE_ENG1_REQ - gc::mmGCVM_INVALIDATE_ENG0_REQ;
        self.eng_addr_distance =
            gc::mmGCVM_INVALIDATE_ENG1_ADDR_RANGE_LO32 - gc::mmGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32;
        // Keep IP-relative offsets here. `Regs::read_ip`/`write_ip` add the
        // discovery base; storing a full SOC15 offset would add it twice and
        // make every invalidate poll access the wrong register.
        self.vm_inv_eng0_sem = gc::mmGCVM_INVALIDATE_ENG0_SEM;
        self.vm_inv_eng0_req = gc::mmGCVM_INVALIDATE_ENG0_REQ;
        self.vm_inv_eng0_ack = gc::mmGCVM_INVALIDATE_ENG0_ACK;
    }

    /// Linux mmhub_v2_0_init.
    fn mmhub_init(&mut self, _regs: &Regs) {
        self.mm_ctx_distance = mm::mmMMVM_CONTEXT1_CNTL - mm::mmMMVM_CONTEXT0_CNTL;
        self.mm_ctx_addr_distance = mm::mmMMVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32
            - mm::mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
        self.mm_eng_distance = mm::mmMMVM_INVALIDATE_ENG1_REQ - mm::mmMMVM_INVALIDATE_ENG0_REQ;
        self.mm_eng_addr_distance =
            mm::mmMMVM_INVALIDATE_ENG1_ADDR_RANGE_LO32 - mm::mmMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32;
        self.mm_inv_eng0_sem = mm::mmMMVM_INVALIDATE_ENG0_SEM;
        self.mm_inv_eng0_req = mm::mmMMVM_INVALIDATE_ENG0_REQ;
        self.mm_inv_eng0_ack = mm::mmMMVM_INVALIDATE_ENG0_ACK;
    }

    /// Linux gmc_v10_0_mc_init + vram_gtt_location.
    fn mc_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let vram_size =
            dev.regs
                .read_ip(HwIp::Nbio, 0, nbio::regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE, 2)?
                as u64
                * 1024
                * 1024;
        let (vram_reserved_size, vga_control, viewport, pitch, height, pitch_pixels) =
            Self::vbios_fb_size(dev, vram_size)?;

        let aper_size = dev.regs.aperture_size() as u64;
        let gmc = &mut dev.gmc;
        gmc.mc_vram_size = vram_size;
        gmc.real_vram_size = vram_size;
        gmc.aper_size = aper_size;
        gmc.visible_vram_size = aper_size;
        gmc.vram_reserved_size = vram_reserved_size;
        gmc.gart_size = GART_SIZE;

        // fb base from the hardware (gfxhub get_fb_location).
        let fb_base = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmGCMC_VM_FB_LOCATION_BASE, 0)? as u64;
        let fb_base = (fb_base & gc::GCMC_VM_FB_LOCATION_BASE__FB_BASE_MASK as u32 as u64) << 24;
        // Linux gfxhub_v2_1_get_mc_fb_offset(). This is the physical base
        // used by VM page-table walkers and is not necessarily `fb_base`.
        let fb_offset_raw = dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCMC_VM_FB_OFFSET, 0)?;
        let vram_base_offset = (fb_offset_raw as u64) << 24;

        gmc.vram_start = fb_base;
        gmc.vram_end = fb_base + vram_size - 1;
        gmc.fb_start = fb_base;
        gmc.fb_end = gmc.vram_end;
        gmc.vram_base_offset = vram_base_offset;

        // GART best-fit placement (amdgpu_gmc_gart_location).
        let max_mc = MC_MASK.min(MC_HOLE_START - 1);
        let size_bf = gmc.fb_start;
        let size_af = max_mc + 1 - gmc.fb_end.saturating_add(1).next_multiple_of(1 << 32);
        if size_bf >= gmc.gart_size && size_bf < size_af {
            gmc.gart_start = 0;
        } else {
            gmc.gart_start = max_mc - gmc.gart_size + 1;
        }
        gmc.gart_end = gmc.gart_start + gmc.gart_size - 1;

        dev_info!(
            "astra: VRAM: {}M 0x{:016X} - 0x{:016X} ({}M visible), GART {}M at 0x{:016X}",
            gmc.mc_vram_size >> 20,
            gmc.vram_start,
            gmc.vram_end,
            gmc.visible_vram_size >> 20,
            gmc.gart_size >> 20,
            gmc.gart_start,
        );
        dev_info!(
            "astra: GMC FB: fb_start=0x{:016X}, GCMC_VM_FB_OFFSET=0x{:08X}, vram_base_offset=0x{:016X}",
            gmc.fb_start,
            fb_offset_raw,
            gmc.vram_base_offset,
        );
        dev_info!(
            "astra: pre-OS VRAM reservation: {} bytes (height={}, pitch_pixels={}, D1VGA_CONTROL=0x{:08X}, viewport=0x{:08X}, pitch=0x{:08X})",
            gmc.vram_reserved_size,
            height,
            pitch_pixels,
            vga_control,
            viewport,
            pitch,
        );
        Ok(())
    }

    /// Linux gmc_v10_0_gart_init + wb/mem_scratch equivalents.
    fn gart_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let gmc = dev.gmc;
        let mut allocator = core::mem::take(&mut dev.mem);
        allocator.init_ranges(
            gmc.gart_start,
            gmc.gart_end,
            gmc.aper_size.min(gmc.mc_vram_size),
            gmc.vram_reserved_size,
        );

        // Linux amdgpu_ttm_init_vram_resv_regions(): the discovery/firmware
        // block at the top of VRAM and the fixed C2P memory-training block
        // must be reserved before any top-down BO such as the PSP TMR.
        let fw_info = dev.atom.as_ref().and_then(|atom| atom.firmware_info());
        let fw_reserved_size = fw_info
            .map(|info| info.fw_reserved_size)
            .filter(|size| *size != 0)
            .unwrap_or(DISCOVERY_TMR_OFFSET);
        let fw_reserved_start = gmc
            .mc_vram_size
            .checked_sub(fw_reserved_size)
            .ok_or(Error::Range)?;
        allocator.reserve_vram(fw_reserved_start, fw_reserved_size)?;
        dev_info!(
            "astra: firmware VRAM reservation: {} bytes at offset {:#x}",
            fw_reserved_size,
            fw_reserved_start,
        );

        if fw_info
            .map(|info| info.firmware_capability & FIRMWARE_CAP_ENABLE_2STAGE_BIST_TRAINING != 0)
            .unwrap_or(false)
        {
            let before_training = gmc
                .mc_vram_size
                .checked_sub(fw_reserved_size)
                .and_then(|value| value.checked_sub(ONE_MIB))
                .ok_or(Error::Range)?;
            let c2p_offset = before_training
                .checked_add(ONE_MIB - 1)
                .map(|value| value & !(ONE_MIB - 1))
                .ok_or(Error::Range)?;
            allocator.reserve_vram(c2p_offset, MEM_TRAIN_DATA_SIZE)?;
            dev_info!(
                "astra: memory-training VRAM reservation: {} bytes at offset {:#x}",
                MEM_TRAIN_DATA_SIZE,
                c2p_offset,
            );
        }

        // Zero dummy page in system memory (amdgpu_gart_dummy_page_init).
        let dummy_page = DmaBuffer::zeroed(GPU_PAGE_SIZE)?;
        allocator.init_table(&mut dev.regs, dummy_page)?;
        dev.mem = allocator;

        dev.gmc.dummy_page_addr = dev.mem.dummy_page_addr;

        Ok(())
    }

    /// Linux gmc_v10_0_get_vbios_fb_size plus
    /// amdgpu_gmc_init_vga_resv_regions for a display-capable dGPU.
    fn vbios_fb_size(dev: &mut Adapter, vram_size: u64) -> Result<(u64, u32, u32, u32, u32, u32)> {
        let vga_control = dev
            .regs
            .read_dcn(dcn::mmD1VGA_CONTROL, dcn::mmD1VGA_CONTROL_BASE_IDX as usize)?;
        let viewport = dev.regs.read_dcn(
            dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,
            dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_BASE_IDX as usize,
        )?;
        let pitch = dev.regs.read_dcn(
            dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH,
            dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH_BASE_IDX as usize,
        )?;

        let height = get_field(
            viewport,
            dcn::HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT__SHIFT,
            dcn::HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK,
        );
        let pitch_pixels = get_field(
            pitch,
            dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK,
        );

        let mut size = if get_field(
            vga_control,
            dcn::D1VGA_CONTROL__D1VGA_MODE_ENABLE__SHIFT,
            dcn::D1VGA_CONTROL__D1VGA_MODE_ENABLE_MASK,
        ) != 0
        {
            VBIOS_VGA_ALLOCATION
        } else {
            (height as u64)
                .saturating_mul(pitch_pixels as u64)
                .saturating_mul(4)
        };

        // Linux drops the reservation if it would consume all but 8 MiB.
        if vram_size.saturating_sub(size) < (8 << 20) {
            size = 0;
        }
        Ok((size, vga_control, viewport, pitch, height, pitch_pixels))
    }

    /// Linux amdgpu_gmc_vram_mc2pa.
    fn vram_mc2pa(gmc: crate::device::GmcInfo, mc_addr: u64) -> u64 {
        mc_addr - gmc.vram_start + gmc.vram_base_offset
    }

    /// Linux amdgpu_gmc_pd_addr -> get_pde_for_bo -> get_vm_pde.
    fn gart_table_addresses(dev: &Adapter) -> Result<(u64, u64, u64)> {
        let gmc = dev.gmc;
        let table_offset = dev.mem.table.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let table_mc = gmc.vram_start + table_offset;
        let table_pa = Self::vram_mc2pa(gmc, table_mc);
        Ok((table_mc, table_pa, table_pa | PTE_VALID))
    }

    /// Linux gfxhub_v2_1_gart_enable.
    fn gfxhub_gart_enable(&mut self, dev: &mut Adapter) -> Result<()> {
        let gmc = dev.gmc;
        let (_, _, table_addr) = Self::gart_table_addresses(dev)?;
        let gart_start = gmc.gart_start;
        let gart_end = gmc.gart_end;
        let dummy_page_addr = gmc.dummy_page_addr;
        let scratch_mc = gmc.vram_start + gmc.mem_scratch_gpu_addr;
        let scratch_addr = Self::vram_mc2pa(gmc, scratch_mc);

        // init_gart_aperture_regs
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
            0,
            table_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
            0,
            (table_addr >> 32) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32,
            0,
            (gart_start >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32,
            0,
            (gart_start >> 44) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32,
            0,
            (gart_end >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32,
            0,
            (gart_end >> 44) as u32,
        )?;

        // init_system_aperture_regs. Linux leaves AGP disabled by default:
        // agp_start=mc_mask and agp_end=0, so BOT is greater than TOP.
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCMC_VM_AGP_BASE, 0, 0)?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCMC_VM_AGP_BOT,
            0,
            (MC_MASK >> 24) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCMC_VM_AGP_TOP, 0, 0)?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCMC_VM_SYSTEM_APERTURE_LOW_ADDR,
            0,
            (gmc.fb_start >> 18) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
            0,
            (gmc.fb_end >> 18) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB,
            0,
            (scratch_addr >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB,
            0,
            (scratch_addr >> 44) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32,
            0,
            (dummy_page_addr >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32,
            0,
            (dummy_page_addr >> 44) as u32,
        )?;
        dev.regs.rmw_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_PROTECTION_FAULT_CNTL2,
            0,
            u32::MAX,
            set_field(
                0,
                gc::GCVM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY__SHIFT,
                gc::GCVM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY_MASK,
                1,
            ),
        )?;

        // init_tlb_regs
        let value = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmGCMC_VM_MX_L1_TLB_CNTL, 0)?;
        let mut value = set_field(
            value,
            gc::GCMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB__SHIFT,
            gc::GCMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE__SHIFT,
            gc::GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK,
            3,
        );
        value = set_field(
            value,
            gc::GCMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL__SHIFT,
            gc::GCMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS__SHIFT,
            gc::GCMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK,
            0,
        );
        value = set_field(
            value,
            gc::GCMC_VM_MX_L1_TLB_CNTL__MTYPE__SHIFT,
            gc::GCMC_VM_MX_L1_TLB_CNTL__MTYPE_MASK,
            3,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCMC_VM_MX_L1_TLB_CNTL, 0, value)?;

        // init_cache_regs
        let value = dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL, 0)?;
        let mut value = set_field(
            value,
            gc::GCVM_L2_CNTL__ENABLE_L2_CACHE__SHIFT,
            gc::GCVM_L2_CNTL__ENABLE_L2_CACHE_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING__SHIFT,
            gc::GCVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK,
            0,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY__SHIFT,
            gc::GCVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL__L2_PDE0_CACHE_TAG_GENERATION_MODE__SHIFT,
            gc::GCVM_L2_CNTL__L2_PDE0_CACHE_TAG_GENERATION_MODE_MASK,
            0,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL__PDE_FAULT_CLASSIFICATION__SHIFT,
            gc::GCVM_L2_CNTL__PDE_FAULT_CLASSIFICATION_MASK,
            0,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE__SHIFT,
            gc::GCVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL__IDENTITY_MODE_FRAGMENT_SIZE__SHIFT,
            gc::GCVM_L2_CNTL__IDENTITY_MODE_FRAGMENT_SIZE_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL, 0, value)?;

        let value = dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL2, 0)?;
        let mut value = set_field(
            value,
            gc::GCVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS__SHIFT,
            gc::GCVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL2__INVALIDATE_L2_CACHE__SHIFT,
            gc::GCVM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL2, 0, value)?;

        // L2_CNTL3/4/5 from the reset defaults with the standard tuning.
        let mut value = gc::mmGCVM_L2_CNTL3_DEFAULT;
        value = set_field(
            value,
            gc::GCVM_L2_CNTL3__BANK_SELECT__SHIFT,
            gc::GCVM_L2_CNTL3__BANK_SELECT_MASK,
            9,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE__SHIFT,
            gc::GCVM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE_MASK,
            6,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL3, 0, value)?;

        let mut value = gc::mmGCVM_L2_CNTL4_DEFAULT;
        value = set_field(
            value,
            gc::GCVM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL__SHIFT,
            gc::GCVM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL_MASK,
            0,
        );
        value = set_field(
            value,
            gc::GCVM_L2_CNTL4__VMC_TAP_PTE_REQUEST_PHYSICAL__SHIFT,
            gc::GCVM_L2_CNTL4__VMC_TAP_PTE_REQUEST_PHYSICAL_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL4, 0, value)?;

        let mut value = gc::mmGCVM_L2_CNTL5_DEFAULT;
        value = set_field(
            value,
            gc::GCVM_L2_CNTL5__L2_CACHE_SMALLK_FRAGMENT_SIZE__SHIFT,
            gc::GCVM_L2_CNTL5__L2_CACHE_SMALLK_FRAGMENT_SIZE_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL5, 0, value)?;

        // enable_system_domain
        let value = dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCVM_CONTEXT0_CNTL, 0)?;
        let mut value = set_field(
            value,
            gc::GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT__SHIFT,
            gc::GCVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK,
            1,
        );
        value = set_field(
            value,
            gc::GCVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH__SHIFT,
            gc::GCVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK,
            0,
        );
        value = set_field(
            value,
            gc::GCVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT,
            gc::GCVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGCVM_CONTEXT0_CNTL, 0, value)?;

        // disable_identity_aperture
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32,
            0,
            0xffff_ffff,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32,
            0,
            0x0000_000f,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32,
            0,
            0,
        )?;

        // setup_vmid_config (VMIDs 0..=14)
        let ctx_distance = self.ctx_distance;
        let ctx_addr_distance = self.ctx_addr_distance;
        let max_pfn = self.max_pfn;
        for vmid in 0..NUM_VMIDS {
            let off = vmid * ctx_distance;
            let value = dev
                .regs
                .read_ip(HwIp::Gc, 0, gc::mmGCVM_CONTEXT1_CNTL + off, 0)?;
            let mut value = set_field(
                value,
                gc::GCVM_CONTEXT1_CNTL__ENABLE_CONTEXT__SHIFT,
                gc::GCVM_CONTEXT1_CNTL__ENABLE_CONTEXT_MASK,
                1,
            );
            value = set_field(
                value,
                gc::GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT,
                gc::GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH_MASK,
                NUM_LEVEL as u64,
            );
            for (shift, mask) in [
                (
                    gc::GCVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    gc::GCVM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    gc::GCVM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    gc::GCVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    gc::GCVM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    gc::GCVM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    gc::GCVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    gc::GCVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
            ] {
                value = set_field(value, shift, mask, 1);
            }
            value = set_field(
                value,
                gc::GCVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT,
                gc::GCVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE_MASK,
                0,
            );
            value = set_field(
                value,
                gc::GCVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT,
                gc::GCVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK,
                1,
            );
            dev.regs
                .write_ip(HwIp::Gc, 0, gc::mmGCVM_CONTEXT1_CNTL + off, 0, value)?;

            let addr_off = vmid * ctx_addr_distance;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32 + addr_off,
                0,
                0,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32 + addr_off,
                0,
                0,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32 + addr_off,
                0,
                (max_pfn - 1) as u32,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32 + addr_off,
                0,
                ((max_pfn - 1) >> 32) as u32,
            )?;
        }

        // program_invalidation
        for eng in 0..NUM_INV_ENGS {
            let off = eng * self.eng_addr_distance;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 + off,
                0,
                0xffff_ffff,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_INVALIDATE_ENG0_ADDR_RANGE_HI32 + off,
                0,
                0x1f,
            )?;
        }
        Ok(())
    }

    /// Linux mmhub_v2_0_gart_enable (same structure over MMHUB regs).
    fn mmhub_gart_enable(&mut self, dev: &mut Adapter) -> Result<()> {
        let gmc = dev.gmc;
        let (_, _, table_addr) = Self::gart_table_addresses(dev)?;
        let gart_start = gmc.gart_start;
        let gart_end = gmc.gart_end;
        let dummy_page_addr = gmc.dummy_page_addr;
        let scratch_mc = gmc.vram_start + gmc.mem_scratch_gpu_addr;
        let scratch_addr = Self::vram_mc2pa(gmc, scratch_mc);

        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
            0,
            table_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
            0,
            (table_addr >> 32) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32,
            0,
            (gart_start >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32,
            0,
            (gart_start >> 44) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32,
            0,
            (gart_end >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32,
            0,
            (gart_end >> 44) as u32,
        )?;

        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_AGP_BASE, 0, 0)?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMMC_VM_AGP_BOT,
            0,
            (MC_MASK >> 24) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_AGP_TOP, 0, 0)?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMMC_VM_SYSTEM_APERTURE_LOW_ADDR,
            0,
            (gmc.fb_start >> 18) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
            0,
            (gmc.fb_end >> 18) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB,
            0,
            (scratch_addr >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB,
            0,
            (scratch_addr >> 44) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32,
            0,
            (dummy_page_addr >> 12) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32,
            0,
            (dummy_page_addr >> 44) as u32,
        )?;
        dev.regs.rmw_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_PROTECTION_FAULT_CNTL2,
            0,
            u32::MAX,
            set_field(
                0,
                mm::MMVM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY__SHIFT,
                mm::MMVM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY_MASK,
                1,
            ),
        )?;

        let value = dev
            .regs
            .read_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_MX_L1_TLB_CNTL, 0)?;
        let mut value = set_field(
            value,
            mm::MMMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB__SHIFT,
            mm::MMMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE__SHIFT,
            mm::MMMC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK,
            3,
        );
        value = set_field(
            value,
            mm::MMMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL__SHIFT,
            mm::MMMC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS__SHIFT,
            mm::MMMC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK,
            0,
        );
        value = set_field(
            value,
            mm::MMMC_VM_MX_L1_TLB_CNTL__MTYPE__SHIFT,
            mm::MMMC_VM_MX_L1_TLB_CNTL__MTYPE_MASK,
            3,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_MX_L1_TLB_CNTL, 0, value)?;

        let value = dev.regs.read_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL, 0)?;
        let mut value = set_field(
            value,
            mm::MMVM_L2_CNTL__ENABLE_L2_CACHE__SHIFT,
            mm::MMVM_L2_CNTL__ENABLE_L2_CACHE_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING__SHIFT,
            mm::MMVM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK,
            0,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY__SHIFT,
            mm::MMVM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL__L2_PDE0_CACHE_TAG_GENERATION_MODE__SHIFT,
            mm::MMVM_L2_CNTL__L2_PDE0_CACHE_TAG_GENERATION_MODE_MASK,
            0,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL__PDE_FAULT_CLASSIFICATION__SHIFT,
            mm::MMVM_L2_CNTL__PDE_FAULT_CLASSIFICATION_MASK,
            0,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE__SHIFT,
            mm::MMVM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL__IDENTITY_MODE_FRAGMENT_SIZE__SHIFT,
            mm::MMVM_L2_CNTL__IDENTITY_MODE_FRAGMENT_SIZE_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL, 0, value)?;

        let value = dev.regs.read_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL2, 0)?;
        let mut value = set_field(
            value,
            mm::MMVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS__SHIFT,
            mm::MMVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL2__INVALIDATE_L2_CACHE__SHIFT,
            mm::MMVM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL2, 0, value)?;

        let mut value = mm::mmMMVM_L2_CNTL3_DEFAULT;
        value = set_field(
            value,
            mm::MMVM_L2_CNTL3__BANK_SELECT__SHIFT,
            mm::MMVM_L2_CNTL3__BANK_SELECT_MASK,
            9,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE__SHIFT,
            mm::MMVM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE_MASK,
            6,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL3, 0, value)?;

        let mut value = mm::mmMMVM_L2_CNTL4_DEFAULT;
        value = set_field(
            value,
            mm::MMVM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL__SHIFT,
            mm::MMVM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL_MASK,
            0,
        );
        value = set_field(
            value,
            mm::MMVM_L2_CNTL4__VMC_TAP_PTE_REQUEST_PHYSICAL__SHIFT,
            mm::MMVM_L2_CNTL4__VMC_TAP_PTE_REQUEST_PHYSICAL_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL4, 0, value)?;

        let mut value = mm::mmMMVM_L2_CNTL5_DEFAULT;
        value = set_field(
            value,
            mm::MMVM_L2_CNTL5__L2_CACHE_SMALLK_FRAGMENT_SIZE__SHIFT,
            mm::MMVM_L2_CNTL5__L2_CACHE_SMALLK_FRAGMENT_SIZE_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL5, 0, value)?;

        let value = dev
            .regs
            .read_ip(HwIp::Mmhub, 0, mm::mmMMVM_CONTEXT0_CNTL, 0)?;
        let mut value = set_field(
            value,
            mm::MMVM_CONTEXT0_CNTL__ENABLE_CONTEXT__SHIFT,
            mm::MMVM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK,
            1,
        );
        value = set_field(
            value,
            mm::MMVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH__SHIFT,
            mm::MMVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK,
            0,
        );
        value = set_field(
            value,
            mm::MMVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT,
            mm::MMVM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_CONTEXT0_CNTL, 0, value)?;

        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32,
            0,
            0xffff_ffff,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32,
            0,
            0x0000_000f,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32,
            0,
            0,
        )?;

        let ctx_distance = self.mm_ctx_distance;
        let ctx_addr_distance = self.mm_ctx_addr_distance;
        let max_pfn = self.max_pfn;
        for vmid in 0..NUM_VMIDS {
            let off = vmid * ctx_distance;
            let value = dev
                .regs
                .read_ip(HwIp::Mmhub, 0, mm::mmMMVM_CONTEXT1_CNTL + off, 0)?;
            let mut value = set_field(
                value,
                mm::MMVM_CONTEXT1_CNTL__ENABLE_CONTEXT__SHIFT,
                mm::MMVM_CONTEXT1_CNTL__ENABLE_CONTEXT_MASK,
                1,
            );
            value = set_field(
                value,
                mm::MMVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT,
                mm::MMVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH_MASK,
                NUM_LEVEL as u64,
            );
            for (shift, mask) in [
                (
                    mm::MMVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    mm::MMVM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    mm::MMVM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    mm::MMVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    mm::MMVM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    mm::MMVM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
                (
                    mm::MMVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT,
                    mm::MMVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK,
                ),
            ] {
                value = set_field(value, shift, mask, 1);
            }
            value = set_field(
                value,
                mm::MMVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT,
                mm::MMVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE_MASK,
                0,
            );
            value = set_field(
                value,
                mm::MMVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT,
                mm::MMVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK,
                1,
            );
            dev.regs
                .write_ip(HwIp::Mmhub, 0, mm::mmMMVM_CONTEXT1_CNTL + off, 0, value)?;

            let addr_off = vmid * ctx_addr_distance;
            dev.regs.write_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32 + addr_off,
                0,
                0,
            )?;
            dev.regs.write_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32 + addr_off,
                0,
                0,
            )?;
            dev.regs.write_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32 + addr_off,
                0,
                (max_pfn - 1) as u32,
            )?;
            dev.regs.write_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32 + addr_off,
                0,
                ((max_pfn - 1) >> 32) as u32,
            )?;
        }

        for eng in 0..NUM_INV_ENGS {
            let off = eng * self.mm_eng_addr_distance;
            dev.regs.write_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 + off,
                0,
                0xffff_ffff,
            )?;
            dev.regs.write_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32 + off,
                0,
                0x1f,
            )?;
        }
        Ok(())
    }

    /// Linux gfxhub_v2_1/mmhub_v2_0_set_fault_enable_default.
    fn set_fault_enable_default(dev: &mut Adapter, value: u32) -> Result<()> {
        for (ip, reg, fields) in [
            (
                HwIp::Gc,
                gc::mmGCVM_L2_PROTECTION_FAULT_CNTL,
                [
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__PDE1_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__PDE1_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__PDE2_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__PDE2_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__NACK_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__NACK_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT_MASK),
                    (gc::GCVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT__SHIFT, gc::GCVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT_MASK),
                ],
            ),
            (
                HwIp::Mmhub,
                mm::mmMMVM_L2_PROTECTION_FAULT_CNTL,
                [
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__PDE1_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__PDE1_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__PDE2_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__PDE2_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__NACK_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__NACK_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT_MASK),
                    (mm::MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT__SHIFT, mm::MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT_MASK),
                ],
            ),
        ] {
            let current = dev.regs.read_ip(ip, 0, reg, 0)?;
            let mut next = current;
            for (index, (shift, mask)) in fields.iter().enumerate() {
                next = set_field(next, *shift, *mask, if index < 11 { value as u64 } else { (value ^ 1) as u64 });
            }
            dev.regs.write_ip(ip, 0, reg, 0, next)?;
        }
        Ok(())
    }

    /// Linux gmc_v10_0_flush_gpu_tlb (legacy register path).
    fn flush_gpu_tlb(&mut self, dev: &mut Adapter, mmhub: bool, vmid: u32) -> Result<()> {
        let (ip, sem, req, ack, eng_distance, use_semaphore) = if mmhub {
            (
                HwIp::Mmhub,
                self.mm_inv_eng0_sem,
                self.mm_inv_eng0_req,
                self.mm_inv_eng0_ack,
                self.mm_eng_distance,
                true,
            )
        } else {
            (
                HwIp::Gc,
                self.vm_inv_eng0_sem,
                self.vm_inv_eng0_req,
                self.vm_inv_eng0_ack,
                self.eng_distance,
                false,
            )
        };
        Self::flush_gpu_tlb_layout(
            dev,
            ip,
            sem,
            req,
            ack,
            eng_distance,
            use_semaphore,
            vmid,
            false,
        )
    }

    fn flush_gpu_tlb_layout(
        dev: &mut Adapter,
        ip: HwIp,
        sem: u32,
        req: u32,
        ack: u32,
        eng_distance: u32,
        use_semaphore: bool,
        vmid: u32,
        strict: bool,
    ) -> Result<()> {
        // Flush HDP first (amdgpu_device_flush_hdp).
        Self::flush_hdp(dev)?;

        let sem = sem + eng_distance * INV_ENG;
        let req = req + eng_distance * INV_ENG;
        let ack = ack + eng_distance * INV_ENG;

        // Invalidate request (hub get_invalidate_req(vmid, 0)).
        let mut inv_req = 1u32 << vmid;
        for shift in [
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE0__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE1__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE2__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L1_PTES__SHIFT,
        ] {
            inv_req |= 1 << shift;
        }

        if use_semaphore {
            let mut acquired = false;
            for _ in 0..10_000 {
                if dev.regs.read_ip(ip, 0, sem, 0)? & 0x1 != 0 {
                    acquired = true;
                    break;
                }
                time::delay(Duration::from_micros(1));
            }
            if !acquired {
                dev_info!("astra: timeout waiting for sem acquire in VM flush");
                if strict {
                    return Err(Error::Io);
                }
            }
        }

        dev.regs.write_ip(ip, 0, req, 0, inv_req)?;

        let mut done = false;
        for _ in 0..10_000 {
            if dev.regs.read_ip(ip, 0, ack, 0)? & (1 << vmid) != 0 {
                done = true;
                break;
            }
            time::delay(Duration::from_micros(1));
        }
        if !done {
            dev_info!("astra: timeout waiting for VM flush");
            if strict {
                if use_semaphore {
                    dev.regs.write_ip(ip, 0, sem, 0, 0)?;
                }
                return Err(Error::Io);
            }
        }

        if use_semaphore {
            dev.regs.write_ip(ip, 0, sem, 0, 0)?;
        }
        Ok(())
    }

    /// HDP flush through the native NBIO register (hdp generic flush).
    fn flush_hdp(dev: &mut Adapter) -> Result<()> {
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            nbio::regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL,
            2,
            0,
        )?;
        // Mirror the read-back of the memory size used as a barrier.
        let _ = dev
            .regs
            .read_ip(HwIp::Nbio, 0, nbio::regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE, 2)?;
        Ok(())
    }
}

/// Flushes PTE writes made after GART enable, matching the TLB sync done by
/// Linux when a GTT BO is bound. Calls are cheap when no PTE changed.
pub(crate) fn flush_pending_gart(dev: &mut Adapter) -> Result<()> {
    // BO Drop only transfers ownership to the allocator retire queue. Apply
    // the PTE invalidations while register access is available, then retain
    // DMA backing until both hubs have acknowledged the invalidate below.
    dev.mem.prepare_retirements(&mut dev.regs)?;
    if !dev.mem.needs_gart_flush() {
        dev.mem.complete_retirements();
        return Ok(());
    }

    GmcBlock::flush_gpu_tlb_layout(
        dev,
        HwIp::Mmhub,
        mm::mmMMVM_INVALIDATE_ENG0_SEM,
        mm::mmMMVM_INVALIDATE_ENG0_REQ,
        mm::mmMMVM_INVALIDATE_ENG0_ACK,
        mm::mmMMVM_INVALIDATE_ENG1_REQ - mm::mmMMVM_INVALIDATE_ENG0_REQ,
        true,
        0,
        true,
    )?;
    GmcBlock::flush_gpu_tlb_layout(
        dev,
        HwIp::Gc,
        gc::mmGCVM_INVALIDATE_ENG0_SEM,
        gc::mmGCVM_INVALIDATE_ENG0_REQ,
        gc::mmGCVM_INVALIDATE_ENG0_ACK,
        gc::mmGCVM_INVALIDATE_ENG1_REQ - gc::mmGCVM_INVALIDATE_ENG0_REQ,
        false,
        0,
        true,
    )?;
    dev.mem.mark_gart_flushed();
    dev.mem.complete_retirements();
    Ok(())
}

impl IpBlock for GmcBlock {
    fn hw_ip(&self) -> HwIp {
        HwIp::Gmc
    }

    fn name(&self) -> &'static str {
        "GMC 10.3"
    }

    /// Linux gmc_v10_0_early_init + sw_init.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.gfxhub_init(&dev.regs);
        self.mmhub_init(&dev.regs);

        // Linux amdgpu_vm_adjust_size(256TB, 9, 3, 48) stores max_pfn as
        // an exclusive 4 KiB-page count.  Three directory levels plus the
        // PTB index cover 36 page-number bits; setup_vmid_config writes
        // max_pfn - 1 to the inclusive hardware end registers.
        self.max_pfn = 1u64 << (BLOCK_SIZE * (NUM_LEVEL + 1));

        // VRAM characteristics from the ATOM vram_info table.
        if let Some(info) = dev.atom.as_ref().and_then(|atom| atom.vram_info()) {
            dev.gmc.vram_type = info.memory_type as u32;
            dev.gmc.vram_width = (info.channel_num as u32) * (1u32 << info.channel_width);
            dev.gmc.vram_vendor = 0;
            dev_info!(
                "astra: vram type {}, width {} (atom vram_info)",
                dev.gmc.vram_type,
                dev.gmc.vram_width,
            );
        }

        self.mc_init(dev)?;
        self.gart_init(dev)
    }

    /// Linux gmc_v10_0_hw_init.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // Linux amdgpu_device_mem_scratch_init immediately before GMC
        // hw_init. The BO is VRAM-backed and its physical address is passed
        // through amdgpu_gmc_vram_mc2pa by both VM hubs.
        let scratch = dev.mem.alloc_vram(&mut dev.regs, GPU_PAGE_SIZE)?;
        dev.gmc.mem_scratch_gpu_addr = scratch.gpu_addr;
        dev.mem_scratch = Some(scratch);

        // init_golden_registers: none for GC 10.3.4; utcl2_harvest: no-op.
        self.gfxhub_gart_enable(dev)?;
        self.mmhub_gart_enable(dev)?;

        // hdp_v5_0_init_registers
        dev.regs.rmw_ip(
            HwIp::Hdp,
            0,
            crate::regs::hdp5_0_0::mmHDP_MISC_CNTL,
            0,
            u32::MAX,
            crate::regs::hdp5_0_0::HDP_MISC_CNTL__FLUSH_INVALIDATE_CACHE_MASK as u32,
        )?;

        Self::flush_hdp(dev)?;

        Self::set_fault_enable_default(dev, 1)?;
        self.flush_gpu_tlb(dev, true, 0)?;
        self.flush_gpu_tlb(dev, false, 0)?;
        dev.mem.mark_gart_enabled_and_flushed();

        let (table_mc, table_pa, root_pde) = Self::gart_table_addresses(dev)?;
        dev_info!(
            "astra: GART table_mc=0x{:016X}, table_pa=0x{:016X}, root_pde=0x{:016X}",
            table_mc,
            table_pa,
            root_pde,
        );
        let gfx_root = dev.regs.read_ip(
            HwIp::Gc,
            0,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
            0,
        )? as u64
            | ((dev.regs.read_ip(
                HwIp::Gc,
                0,
                gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
                0,
            )? as u64)
                << 32);
        let mm_root = dev.regs.read_ip(
            HwIp::Mmhub,
            0,
            mm::mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
            0,
        )? as u64
            | ((dev.regs.read_ip(
                HwIp::Mmhub,
                0,
                mm::mmMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
                0,
            )? as u64)
                << 32);
        dev_info!(
            "astra: GART root readback: GFXHUB=0x{:016X}, MMHUB=0x{:016X}",
            gfx_root,
            mm_root,
        );
        dev_info!(
            "astra: GART GFX config: ctx0=0x{:08X} tlb=0x{:08X} l2=0x{:08X} agp_bot=0x{:08X} agp_top=0x{:08X}",
            dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCVM_CONTEXT0_CNTL, 0)?,
            dev.regs
                .read_ip(HwIp::Gc, 0, gc::mmGCMC_VM_MX_L1_TLB_CNTL, 0)?,
            dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCVM_L2_CNTL, 0)?,
            dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCMC_VM_AGP_BOT, 0)?,
            dev.regs.read_ip(HwIp::Gc, 0, gc::mmGCMC_VM_AGP_TOP, 0)?,
        );
        dev_info!(
            "astra: GART MM config: ctx0=0x{:08X} tlb=0x{:08X} l2=0x{:08X} agp_bot=0x{:08X} agp_top=0x{:08X}",
            dev.regs
                .read_ip(HwIp::Mmhub, 0, mm::mmMMVM_CONTEXT0_CNTL, 0)?,
            dev.regs
                .read_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_MX_L1_TLB_CNTL, 0)?,
            dev.regs.read_ip(HwIp::Mmhub, 0, mm::mmMMVM_L2_CNTL, 0)?,
            dev.regs.read_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_AGP_BOT, 0)?,
            dev.regs.read_ip(HwIp::Mmhub, 0, mm::mmMMMC_VM_AGP_TOP, 0)?,
        );
        dev_info!(
            "astra: PCIE GART of {}M enabled (table at 0x{:016X})",
            dev.gmc.gart_size >> 20,
            table_mc,
        );

        // Linux amdgpu_device_wb_init immediately after GMC hw_init. Binding
        // this GTT BO updates the live GART, so perform the same TLB sync.
        let wb_bo = dev.mem.alloc_gart(&mut dev.regs, GPU_PAGE_SIZE)?;
        dev.wb = Some(crate::mem::Wb::new(wb_bo));
        flush_pending_gart(dev)?;
        Ok(())
    }
}
