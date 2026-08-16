//! SDMA IP block (Linux `sdma_v5_2.c`): engine start, ring setup and
//! ring tests for all instances.

use alloc::vec::Vec;
use core::time::Duration;

use na_std::time;
use na_std::{Error, Result};

use crate::device::Adapter;
use crate::doorbell;
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::regs::gc10_3_0 as gc;
use crate::regs::hdp5_0_0 as hdp;
use crate::regs::nbio2_3 as nbio23;
use crate::regs::nbio4_3_0 as nbio43;
use crate::regs::set_field;
use crate::ridx;
use crate::ring::{Ring, RingKind};
use crate::{dev_err, dev_info};

/// Ring size in dwords (sdma_v5_2_sw_init).
const SDMA_RING_DWORDS: usize = 1024;
/// SDMA doorbell range size (Linux `sdma_doorbell_range = 20`).
const SDMA_DOORBELL_RANGE_SIZE: u32 = 20;
/// NBIO main / S2A base indexes (see blocks/common.rs).
const NBIO_BASE_MAIN: usize = 2;
const NBIO_BASE_S2A: usize = 3;
/// nbio_v2_3.c defines these locally because only SDMA0/1 are present in the
/// generated NBIO 2.3 header used by older ASICs.
const MM_BIF_SDMA2_DOORBELL_RANGE: u32 = 0x01d6;
const MM_BIF_SDMA3_DOORBELL_RANGE: u32 = 0x01d7;

/// SDMA_OP_WRITE / SUBOP_WRITE_LINEAR packet header (sid.h).
const SDMA_PKT_WRITE_HEADER: u32 = 2;
const SDMA_OP_INDIRECT: u32 = 4;
const SDMA_OP_FENCE: u32 = 5;
const SDMA_OP_POLL_REGMEM: u32 = 8;
const SDMA_OP_PTEPDE: u32 = 12;
const SDMA_OP_SRBM_WRITE: u32 = 14;
const SDMA_OP_GCR_REQ: u32 = 17;
const SDMA_FENCE_MTYPE_UC: u32 = 3 << 16;

const SDMA_GCR_GL2_WB: u32 = 1 << 15;
const SDMA_GCR_GL2_INV: u32 = 1 << 14;
const SDMA_GCR_GL1_INV: u32 = 1 << 9;
const SDMA_GCR_GLV_INV: u32 = 1 << 8;
const SDMA_GCR_GLK_INV: u32 = 1 << 7;
const SDMA_GCR_GLM_INV: u32 = 1 << 5;
const SDMA_GCR_GLI_INV: u32 = 1;

pub struct SdmaV52 {
    _version: IpVersion,
    pub instances: u32,
    rings: Vec<Ring>,
    /// Register distance between SDMA instances.
    inst_distance: u32,
    logged_first_vm_update: bool,
}

impl SdmaV52 {
    pub fn new(version: IpVersion, instances: u32) -> Self {
        Self {
            _version: version,
            instances,
            rings: Vec::new(),
            inst_distance: 0,
            logged_first_vm_update: false,
        }
    }

    fn reg(&self, base_reg: u32, instance: u32) -> u32 {
        base_reg + instance * self.inst_distance
    }

    /// Linux `nbio_v2_3_sdma_doorbell_range` /
    /// `nbio_v4_3_sdma_doorbell_range`, selected from the discovered NBIO IP.
    fn program_doorbell_range(
        &self,
        dev: &mut Adapter,
        instance: u32,
        doorbell_index: u32,
    ) -> Result<u32> {
        if super::uses_nbio_v2_3(dev) {
            let reg = match instance {
                0 => nbio23::mmBIF_SDMA0_DOORBELL_RANGE,
                1 => nbio23::mmBIF_SDMA1_DOORBELL_RANGE,
                2 => MM_BIF_SDMA2_DOORBELL_RANGE,
                _ => MM_BIF_SDMA3_DOORBELL_RANGE,
            };
            let value = dev.regs.read_ip(HwIp::Nbio, 0, reg, NBIO_BASE_MAIN)?;
            let value = set_field(
                value,
                nbio23::BIF_SDMA0_DOORBELL_RANGE__OFFSET__SHIFT,
                nbio23::BIF_SDMA0_DOORBELL_RANGE__OFFSET_MASK,
                doorbell_index as u64,
            );
            let value = set_field(
                value,
                nbio23::BIF_SDMA0_DOORBELL_RANGE__SIZE__SHIFT,
                nbio23::BIF_SDMA0_DOORBELL_RANGE__SIZE_MASK,
                SDMA_DOORBELL_RANGE_SIZE as u64,
            );
            dev.regs
                .write_ip(HwIp::Nbio, 0, reg, NBIO_BASE_MAIN, value)?;
            return dev.regs.read_ip(HwIp::Nbio, 0, reg, NBIO_BASE_MAIN);
        }

        // NBIO 4.3 exposes one S2A range covering all SDMA instances and
        // Linux only programs it while resuming instance zero.
        if instance != 0 {
            return dev.regs.read_ip(
                HwIp::Nbio,
                0,
                nbio43::regS2A_DOORBELL_ENTRY_2_CTRL,
                NBIO_BASE_S2A,
            );
        }
        let value = dev.regs.read_ip(
            HwIp::Nbio,
            0,
            nbio43::regS2A_DOORBELL_ENTRY_2_CTRL,
            NBIO_BASE_S2A,
        )?;
        let mut value = set_field(
            value,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_ENABLE__SHIFT,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_ENABLE_MASK,
            1,
        );
        value = set_field(
            value,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWID__SHIFT,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWID_MASK,
            0xe,
        );
        value = set_field(
            value,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET__SHIFT,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET_MASK,
            doorbell_index as u64,
        );
        value = set_field(
            value,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE__SHIFT,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE_MASK,
            SDMA_DOORBELL_RANGE_SIZE as u64,
        );
        value = set_field(
            value,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE__SHIFT,
            nbio43::S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE_MASK,
            0x3,
        );
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            nbio43::regS2A_DOORBELL_ENTRY_2_CTRL,
            NBIO_BASE_S2A,
            value,
        )?;
        dev.regs.read_ip(
            HwIp::Nbio,
            0,
            nbio43::regS2A_DOORBELL_ENTRY_2_CTRL,
            NBIO_BASE_S2A,
        )
    }

    /// Linux sdma_v5_2_soft_reset (per engine).
    fn soft_reset(&self, dev: &mut Adapter, instance: u32) -> Result<()> {
        let bit = 1u32 << (gc::GRBM_SOFT_RESET__SOFT_RESET_SDMA0__SHIFT as u32 + instance);
        dev.regs
            .rmw_ip(HwIp::Gc, 0, gc::mmGRBM_SOFT_RESET, 0, u32::MAX, bit)?;
        time::delay(Duration::from_micros(50));
        dev.regs
            .rmw_ip(HwIp::Gc, 0, gc::mmGRBM_SOFT_RESET, 0, !bit, 0)?;
        Ok(())
    }

    /// Linux sdma_v5_2_enable (F32_CNTL HALT = 0).
    fn enable(&self, dev: &mut Adapter, enable: bool) -> Result<()> {
        for instance in 0..self.instances {
            let reg = self.reg(gc::mmSDMA0_F32_CNTL, instance);
            let value = dev.regs.read_ip(HwIp::Gc, 0, reg, 0)?;
            let value = set_field(
                value,
                gc::SDMA0_F32_CNTL__HALT__SHIFT,
                gc::SDMA0_F32_CNTL__HALT_MASK,
                (!enable) as u64,
            );
            dev.regs.write_ip(HwIp::Gc, 0, reg, 0, value)?;
        }
        Ok(())
    }

    /// Linux sdma_v5_2_ctx_switch_enable.
    fn ctx_switch_enable(&self, dev: &mut Adapter, enable: bool) -> Result<()> {
        for instance in 0..self.instances {
            let reg = self.reg(gc::mmSDMA0_CNTL, instance);
            let value = dev.regs.read_ip(HwIp::Gc, 0, reg, 0)?;
            let value = set_field(
                value,
                gc::SDMA0_CNTL__AUTO_CTXSW_ENABLE__SHIFT,
                gc::SDMA0_CNTL__AUTO_CTXSW_ENABLE_MASK,
                enable as u64,
            );
            dev.regs.write_ip(HwIp::Gc, 0, reg, 0, value)?;
        }
        Ok(())
    }

    /// Linux sdma_v5_2_gfx_resume (per instance).
    fn gfx_resume(&mut self, dev: &mut Adapter, instance: u32) -> Result<()> {
        let ring = &self.rings[instance as usize];
        let gpu_addr = ring.gpu_addr;
        let rptr_wb = ring.rptr_wb;
        let wptr_wb = ring.wptr_wb;
        let doorbell_index = ring.doorbell;
        self.rings[instance as usize].reset();

        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL, instance),
            0,
            0,
        )?;

        let rb_cntl_reg = self.reg(gc::mmSDMA0_GFX_RB_CNTL, instance);
        let rb_size = SDMA_RING_DWORDS.trailing_zeros() as u64;
        let mut rb_cntl = dev.regs.read_ip(HwIp::Gc, 0, rb_cntl_reg, 0)?;
        rb_cntl = set_field(
            rb_cntl,
            gc::SDMA0_GFX_RB_CNTL__RB_SIZE__SHIFT,
            gc::SDMA0_GFX_RB_CNTL__RB_SIZE_MASK,
            rb_size,
        );
        dev.regs.write_ip(HwIp::Gc, 0, rb_cntl_reg, 0, rb_cntl)?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_RPTR, instance),
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_RPTR_HI, instance),
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_WPTR, instance),
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_WPTR_HI, instance),
            0,
            0,
        )?;

        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO, instance),
            0,
            wptr_wb as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI, instance),
            0,
            (wptr_wb >> 32) as u32,
        )?;
        let wptr_poll_reg = self.reg(gc::mmSDMA0_GFX_RB_WPTR_POLL_CNTL, instance);
        let wptr_poll = dev.regs.read_ip(HwIp::Gc, 0, wptr_poll_reg, 0)?;
        let wptr_poll = set_field(
            wptr_poll,
            gc::SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT,
            gc::SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, wptr_poll_reg, 0, wptr_poll)?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_RPTR_ADDR_HI, instance),
            0,
            (rptr_wb >> 32) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_RPTR_ADDR_LO, instance),
            0,
            (rptr_wb as u32) & !3,
        )?;
        rb_cntl = set_field(
            rb_cntl,
            gc::SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT,
            gc::SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK,
            1,
        );
        let rb_base = gpu_addr >> 8;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_BASE, instance),
            0,
            rb_base as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_BASE_HI, instance),
            0,
            (rb_base >> 32) as u32,
        )?;
        // minor_ptr_update before programming the wptr.
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_MINOR_PTR_UPDATE, instance),
            0,
            1,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_RB_WPTR, instance),
            0,
            0,
        )?;

        // Doorbell (ENABLE in the DOORBELL reg, OFFSET in its own reg).
        let doorbell =
            dev.regs
                .read_ip(HwIp::Gc, 0, self.reg(gc::mmSDMA0_GFX_DOORBELL, instance), 0)?;
        let doorbell = set_field(
            doorbell,
            gc::SDMA0_GFX_DOORBELL__ENABLE__SHIFT,
            gc::SDMA0_GFX_DOORBELL__ENABLE_MASK,
            1,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_DOORBELL, instance),
            0,
            doorbell,
        )?;
        let doorbell_offset = dev.regs.read_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_DOORBELL_OFFSET, instance),
            0,
        )?;
        let doorbell_offset = set_field(
            doorbell_offset,
            gc::SDMA0_GFX_DOORBELL_OFFSET__OFFSET__SHIFT,
            gc::SDMA0_GFX_DOORBELL_OFFSET__OFFSET_MASK,
            doorbell_index as u64,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_DOORBELL_OFFSET, instance),
            0,
            doorbell_offset,
        )?;

        let range = self.program_doorbell_range(dev, instance, doorbell_index)?;
        dev_info!(
            "astra: SDMA{} NBIO doorbell range programmed to {:#010x}",
            instance,
            range
        );

        // minor_ptr_update back to 0 after the wptr is programmed.
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            self.reg(gc::mmSDMA0_GFX_MINOR_PTR_UPDATE, instance),
            0,
            0,
        )?;

        let cntl_reg = self.reg(gc::mmSDMA0_CNTL, instance);
        let cntl = dev.regs.read_ip(HwIp::Gc, 0, cntl_reg, 0)?;
        let cntl = set_field(
            cntl,
            gc::SDMA0_CNTL__UTC_L1_ENABLE__SHIFT,
            gc::SDMA0_CNTL__UTC_L1_ENABLE_MASK,
            1,
        );
        let cntl = set_field(
            cntl,
            gc::SDMA0_CNTL__MIDCMD_PREEMPT_ENABLE__SHIFT,
            gc::SDMA0_CNTL__MIDCMD_PREEMPT_ENABLE_MASK,
            1,
        );
        dev.regs.write_ip(HwIp::Gc, 0, cntl_reg, 0, cntl)?;

        let utcl1_cntl = self.reg(gc::mmSDMA0_UTCL1_CNTL, instance);
        let utcl1 = dev.regs.read_ip(HwIp::Gc, 0, utcl1_cntl, 0)?;
        let utcl1 = set_field(
            utcl1,
            gc::SDMA0_UTCL1_CNTL__RESP_MODE__SHIFT,
            gc::SDMA0_UTCL1_CNTL__RESP_MODE_MASK,
            3,
        );
        let utcl1 = set_field(
            utcl1,
            gc::SDMA0_UTCL1_CNTL__REDO_DELAY__SHIFT,
            gc::SDMA0_UTCL1_CNTL__REDO_DELAY_MASK,
            9,
        );
        dev.regs.write_ip(HwIp::Gc, 0, utcl1_cntl, 0, utcl1)?;

        let utcl1_page = self.reg(gc::mmSDMA0_UTCL1_PAGE, instance);
        dev.regs.rmw_ip(
            HwIp::Gc,
            0,
            utcl1_page,
            0,
            0x00ff_0fff,
            set_field(
                0,
                gc::SDMA0_UTCL1_PAGE__LLC_NOALLOC__SHIFT,
                gc::SDMA0_UTCL1_PAGE__LLC_NOALLOC_MASK,
                1,
            ),
        )?;

        let f32_reg = self.reg(gc::mmSDMA0_F32_CNTL, instance);
        let f32 = dev.regs.read_ip(HwIp::Gc, 0, f32_reg, 0)?;
        let f32 = set_field(
            f32,
            gc::SDMA0_F32_CNTL__HALT__SHIFT,
            gc::SDMA0_F32_CNTL__HALT_MASK,
            0,
        );
        dev.regs.write_ip(HwIp::Gc, 0, f32_reg, 0, f32)?;

        rb_cntl = set_field(
            rb_cntl,
            gc::SDMA0_GFX_RB_CNTL__RB_ENABLE__SHIFT,
            gc::SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK,
            1,
        );
        dev.regs.write_ip(HwIp::Gc, 0, rb_cntl_reg, 0, rb_cntl)?;

        let ib_reg = self.reg(gc::mmSDMA0_GFX_IB_CNTL, instance);
        let ib_cntl = dev.regs.read_ip(HwIp::Gc, 0, ib_reg, 0)?;
        let ib_cntl = set_field(
            ib_cntl,
            gc::SDMA0_GFX_IB_CNTL__IB_ENABLE__SHIFT,
            gc::SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK,
            1,
        );
        dev.regs.write_ip(HwIp::Gc, 0, ib_reg, 0, ib_cntl)?;
        Ok(())
    }

    /// Linux sdma_v5_2_ring_test_ring: SDMA_OP_WRITE into a WB slot.
    fn ring_test(&mut self, dev: &mut Adapter, instance: u32, wb_addr: u64) -> Result<()> {
        // Linux initializes the WB target before submitting the packet so a
        // stale value can never make the test pass.
        dev.wb
            .as_mut()
            .ok_or(Error::NoDevice)?
            .write_u32(wb_addr, 0xCAFE_DEAD)?;

        {
            let ring = &mut self.rings[instance as usize];
            ring.write(SDMA_PKT_WRITE_HEADER)?;
            ring.write(wb_addr as u32)?;
            ring.write((wb_addr >> 32) as u32)?;
            ring.write(0)?; // SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(0)
            ring.write(0xDEAD_BEEF)?;
            ring.commit(dev)?;
        }

        // Poll the WB slot through its CPU mapping.
        let offset = {
            let wb = dev.wb.as_ref().ok_or(Error::NoDevice)?;
            (wb_addr - wb.bo.gpu_addr) as usize
        };
        for _ in 0..1_000_000 {
            let done = dev
                .wb
                .as_ref()
                .and_then(|wb| wb.bo.cpu.as_ref())
                .and_then(|cpu| {
                    cpu.sync_for_cpu();
                    cpu.as_slice().get(offset..offset + 4)
                })
                .map(|bytes| {
                    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) == 0xDEAD_BEEF
                })
                .unwrap_or(false);
            if done {
                return Ok(());
            }
            time::delay(Duration::from_micros(1));
        }

        Err(Error::Io)
    }

    fn full_reg(dev: &Adapter, ip: HwIp, reg: u32, base_idx: usize) -> Result<u32> {
        dev.regs
            .base_u32(ip, 0, base_idx)?
            .checked_add(reg)
            .ok_or(Error::Range)
    }

    /// Linux `sdma_v5_2_ring_emit_pipeline_sync`. The VM-update scheduler
    /// entity changes the ring context on its first job, so Linux emits this
    /// before the first update IB.
    fn emit_pipeline_sync(ring: &mut Ring) -> Result<()> {
        ring.write(SDMA_OP_POLL_REGMEM | (3 << 28) | (1 << 31))?;
        ring.write(ring.fence_wb as u32 & 0xffff_fffc)?;
        ring.write((ring.fence_wb >> 32) as u32)?;
        ring.write(ring.fence_seq as u32)?;
        ring.write(u32::MAX)?;
        ring.write((0xfff << 16) | 4)
    }

    /// Linux `sdma_v5_2_ring_emit_mem_sync`. Every IB returned by
    /// `amdgpu_ib_get` carries `AMDGPU_IB_FLAG_EMIT_MEM_SYNC`, so the
    /// scheduler emits this GCR request before HDP flush and INDIRECT.
    fn emit_mem_sync(ring: &mut Ring) -> Result<()> {
        let gcr_cntl = SDMA_GCR_GL2_INV
            | SDMA_GCR_GL2_WB
            | SDMA_GCR_GLM_INV
            | SDMA_GCR_GL1_INV
            | SDMA_GCR_GLV_INV
            | SDMA_GCR_GLK_INV
            | SDMA_GCR_GLI_INV;
        ring.write(SDMA_OP_GCR_REQ)?;
        ring.write(0)?; // base_va[31:7]
        ring.write((gcr_cntl & 0xffff) << 16)?; // control[15:0], base_va[47:32]
        ring.write((gcr_cntl >> 16) & 0x7)?; // limit_va[31:7], control[18:16]
        ring.write(0) // VMID 0, limit_va[47:32]
    }

    /// Linux `sdma_v5_2_ring_emit_hdp_flush`, using the NBIO function table
    /// selected for the discovered ASIC.
    fn emit_hdp_flush(ring: &mut Ring, dev: &Adapter, instance: u32) -> Result<()> {
        let (done, request, sdma0_mask) = if super::uses_nbio_v2_3(dev) {
            (
                Self::full_reg(
                    dev,
                    HwIp::Nbio,
                    nbio23::mmBIF_BX_PF_GPU_HDP_FLUSH_DONE,
                    ridx!(nbio23::mmBIF_BX_PF_GPU_HDP_FLUSH_DONE),
                )?,
                Self::full_reg(
                    dev,
                    HwIp::Nbio,
                    nbio23::mmBIF_BX_PF_GPU_HDP_FLUSH_REQ,
                    ridx!(nbio23::mmBIF_BX_PF_GPU_HDP_FLUSH_REQ),
                )?,
                nbio23::BIF_BX_PF_GPU_HDP_FLUSH_DONE__SDMA0_MASK as u32,
            )
        } else {
            (
                Self::full_reg(
                    dev,
                    HwIp::Nbio,
                    nbio43::regBIF_BX_PF0_GPU_HDP_FLUSH_DONE,
                    ridx!(nbio43::regBIF_BX_PF0_GPU_HDP_FLUSH_DONE),
                )?,
                Self::full_reg(
                    dev,
                    HwIp::Nbio,
                    nbio43::regBIF_BX_PF0_GPU_HDP_FLUSH_REQ,
                    ridx!(nbio43::regBIF_BX_PF0_GPU_HDP_FLUSH_REQ),
                )?,
                nbio43::BIF_BX_PF0_GPU_HDP_FLUSH_DONE__SDMA0_MASK as u32,
            )
        };
        let reference = sdma0_mask.checked_shl(instance).ok_or(Error::Range)?;
        ring.write(SDMA_OP_POLL_REGMEM | (1 << 26) | (3 << 28))?;
        ring.write(done << 2)?;
        ring.write(request << 2)?;
        ring.write(reference)?;
        ring.write(reference)?;
        ring.write((0xfff << 16) | 10)
    }

    /// Linux `amdgpu_device_invalidate_hdp` -> `hdp_v5_0_invalidate_hdp`
    /// -> `sdma_v5_2_ring_emit_wreg`.
    fn emit_hdp_invalidate(ring: &mut Ring, dev: &Adapter) -> Result<()> {
        let reg = Self::full_reg(
            dev,
            HwIp::Hdp,
            hdp::mmHDP_READ_CACHE_INVALIDATE,
            ridx!(hdp::mmHDP_READ_CACHE_INVALIDATE),
        )?;
        ring.write(SDMA_OP_SRBM_WRITE | (0xf << 28))?;
        ring.write(reg)?;
        ring.write(1)
    }

    /// Linux `amdgpu_vm_sdma_update` + `sdma_v5_2_vm_{write,set}_pte`.
    /// The update commands live in a GART IB and are scheduled on SDMA0 with
    /// VMID 0; completion is the SDMA ring fence dependency that Linux waits
    /// before making the GFX job runnable.
    fn submit_vm_update(
        &mut self,
        dev: &mut Adapter,
        dst: u64,
        addr: u64,
        count: u32,
        incr: u32,
        flags: u64,
    ) -> Result<()> {
        if count == 0 || self.rings.is_empty() {
            return Err(Error::InvalidArgument);
        }

        let command_dw = if count < 3 {
            4usize.checked_add(count as usize * 2).ok_or(Error::Range)?
        } else {
            10
        };
        let ib_dw = command_dw.next_multiple_of(8);
        let mut ib = dev.mem.alloc_gart_aligned(&mut dev.regs, ib_dw * 4, 32)?;
        let cpu = ib.cpu.as_mut().ok_or(Error::NoDevice)?;
        {
            let bytes = cpu.as_mut_slice();
            let mut put = |index: usize, value: u32| -> Result<()> {
                let at = index.checked_mul(4).ok_or(Error::Range)?;
                let dst = bytes.get_mut(at..at + 4).ok_or(Error::Range)?;
                dst.copy_from_slice(&value.to_le_bytes());
                Ok(())
            };
            let mut at = 0usize;
            if count < 3 {
                put(at, SDMA_PKT_WRITE_HEADER)?;
                at += 1;
                put(at, dst as u32)?;
                at += 1;
                put(at, (dst >> 32) as u32)?;
                at += 1;
                put(at, count * 2 - 1)?;
                at += 1;
                let mut value = addr | flags;
                for _ in 0..count {
                    put(at, value as u32)?;
                    put(at + 1, (value >> 32) as u32)?;
                    at += 2;
                    value = value.checked_add(incr as u64).ok_or(Error::Range)?;
                }
            } else {
                put(at, SDMA_OP_PTEPDE)?;
                put(at + 1, dst as u32)?;
                put(at + 2, (dst >> 32) as u32)?;
                put(at + 3, flags as u32)?;
                put(at + 4, (flags >> 32) as u32)?;
                put(at + 5, addr as u32)?;
                put(at + 6, (addr >> 32) as u32)?;
                put(at + 7, incr)?;
                put(at + 8, 0)?;
                put(at + 9, count - 1)?;
            }
        }
        cpu.sync_for_device();
        super::flush_pending_gart(dev)?;

        let result = (|| -> Result<()> {
            let ring = &mut self.rings[0];
            if ring.fence_wb == 0 {
                return Err(Error::NoDevice);
            }
            let sequence = ring.fence_seq.checked_add(1).ok_or(Error::Range)?;
            dev.wb
                .as_mut()
                .ok_or(Error::NoDevice)?
                .write_u64(ring.fence_wb, 0)?;

            if ring.current_ctx != Some(0) {
                Self::emit_pipeline_sync(ring)?;
            }
            Self::emit_mem_sync(ring)?;
            Self::emit_hdp_flush(ring, dev, 0)?;
            for _ in 0..((2u32.wrapping_sub(ring.wptr)) & 7) {
                ring.write(0)?;
            }
            ring.write(SDMA_OP_INDIRECT)?; // VMID 0, PRIV 0
            ring.write(ib.gpu_addr as u32 & 0xffff_ffe0)?;
            ring.write((ib.gpu_addr >> 32) as u32)?;
            ring.write(ib_dw as u32)?;
            ring.write(0)?; // VMID0 CSA address
            ring.write(0)?;

            Self::emit_hdp_invalidate(ring, dev)?;
            ring.write(SDMA_OP_FENCE | SDMA_FENCE_MTYPE_UC)?;
            ring.write(ring.fence_wb as u32)?;
            ring.write((ring.fence_wb >> 32) as u32)?;
            ring.write(sequence as u32)?;
            ring.commit(dev)?;
            ring.fence_seq = sequence;
            ring.current_ctx = Some(0);

            if !self.logged_first_vm_update {
                dev_info!(
                    "astra: first Linux SDMA VM update: ring=sdma0 gfx queue, IB={:#018x} dst={:#018x} count={} op={} fence={:#018x}:{}",
                    ib.gpu_addr,
                    dst,
                    count,
                    if count < 3 { "WRITE" } else { "PTEPDE" },
                    ring.fence_wb,
                    sequence,
                );
                self.logged_first_vm_update = true;
            }

            for _ in 0..1_000_000 {
                if dev
                    .wb
                    .as_mut()
                    .ok_or(Error::NoDevice)?
                    .read_u64(ring.fence_wb)? as u32
                    == sequence as u32
                {
                    return Ok(());
                }
                time::delay(Duration::from_micros(1));
            }
            let observed = dev
                .wb
                .as_mut()
                .ok_or(Error::NoDevice)?
                .read_u64(ring.fence_wb)?;
            dev_err!(
                "astra: SDMA VM update timeout dst={:#018x} count={} fence={} observed={}",
                dst,
                count,
                sequence,
                observed,
            );
            Err(Error::Io)
        })();

        drop(ib);
        let retire = super::flush_pending_gart(dev);
        result.and(retire)
    }
}

impl IpBlock for SdmaV52 {
    fn hw_ip(&self) -> HwIp {
        HwIp::Sdma0
    }

    fn name(&self) -> &'static str {
        "SDMA 5.2"
    }

    /// Linux sdma_v5_2_sw_init: one ring per instance, shared firmware.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.inst_distance = gc::mmSDMA1_F32_CNTL - gc::mmSDMA0_F32_CNTL;
        let sdma_doors = [
            doorbell::DOORBELL_SDMA_ENGINE0,
            doorbell::DOORBELL_SDMA_ENGINE1,
            doorbell::DOORBELL_SDMA_ENGINE2,
            doorbell::DOORBELL_SDMA_ENGINE3,
        ];
        for instance in 0..self.instances {
            let bo = dev.mem.alloc_gart(&mut dev.regs, SDMA_RING_DWORDS * 4)?;
            let rptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
            let wptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
            let mut ring = Ring::new(
                bo,
                doorbell::ring_doorbell(sdma_doors[instance as usize]),
                rptr_wb,
                wptr_wb,
                instance,
                0,
                0,
                RingKind::Sdma,
            );
            ring.fence_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
            self.rings.push(ring);
        }
        Ok(())
    }

    /// Linux sdma_v5_2_hw_init / sdma_v5_2_start.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        for instance in 0..self.instances {
            self.soft_reset(dev, instance)?;
            // sdma_v5_2_soft_reset adds another 50 us between engines after
            // the 50 us reset assertion delay in soft_reset_engine.
            time::delay(Duration::from_micros(50));
        }
        self.enable(dev, true)?;
        self.ctx_switch_enable(dev, true)?;
        // Linux's gfx_resume_instance ends in amdgpu_ring_test_helper, so
        // each engine is tested before moving on to the next instance.
        for instance in 0..self.instances {
            self.gfx_resume(dev, instance)?;
            let slot = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
            self.ring_test(dev, instance, slot)?;
            dev_info!("astra: ring test on sdma{} succeeded", instance);
        }
        Ok(())
    }

    fn update_vm_table(
        &mut self,
        dev: &mut Adapter,
        dst: u64,
        addr: u64,
        count: u32,
        incr: u32,
        flags: u64,
    ) -> Result<()> {
        self.submit_vm_update(dev, dst, addr, count, incr, flags)
    }
}
