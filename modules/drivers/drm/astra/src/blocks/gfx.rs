//! GFX IP block (Linux `gfx_v10_0.c`): golden registers, RLC autoload,
//! clear-state buffer, KIQ + compute queues, gfx rings and ring tests.

use alloc::vec::Vec;
use core::time::Duration;

use na_std::time;
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;
use crate::doorbell;
use crate::ip::{HwIp, IpBlock, IpVersion, UserSubmission};
use crate::mem::Bo;
use crate::regs::gc10_3_0 as gc;
use crate::regs::hdp5_0_0 as hdp;
use crate::regs::nbio4_3_0 as nbio;
use crate::regs::{get_field, set_field};
use crate::ridx;
use crate::ring::{
    PACKET3_BASE_INDEX_CE_PARTITION, PACKET3_CLEAR_STATE, PACKET3_CONTEXT_CONTROL,
    PACKET3_MAP_QUEUES, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE, PACKET3_PREAMBLE_CNTL,
    PACKET3_PREAMBLE_END_CLEAR_STATE, PACKET3_SET_BASE, PACKET3_SET_CONTEXT_REG,
    PACKET3_SET_CONTEXT_REG_START, PACKET3_SET_RESOURCES, Ring, RingConfig, RingKind,
    map_queues_doorbell_offset, map_queues_dw1, packet3,
};

/// Register aliases defined locally by Linux gfx_v10_0.c (not present in
/// the generated gc_10_3_0 headers).
mod local_regs {
    pub const CGTT_SPI_CS_CLK_CTRL: u32 = 0x507c;
    pub const CGTT_SPI_RA0_CLK_CTRL: u32 = 0x507a;
    pub const CGTT_SPI_RA1_CLK_CTRL: u32 = 0x507b;
    pub const GCR_GENERAL_CNTL_SIENNA_CICHLID: u32 = 0x1580;
    pub const GL2C_CTRL3: u32 = 0x2e0c;
    pub const TA_CNTL_AUX: u32 = 0x12e2;
    pub const CP_MEC_CNTL_SIENNA_CICHLID: u32 = 0x0f55;
    pub const RLC_CP_SCHEDULERS_SIENNA_CICHLID: u32 = 0x4ca1;
    pub const SPI_CONFIG_CNTL_SIENNA_CICHLID: u32 = 0x11ec;
    pub const VGT_ESGS_RING_SIZE_SIENNA_CICHLID: u32 = 0x0fc1;
    pub const VGT_GSVS_RING_SIZE_SIENNA_CICHLID: u32 = 0x0fc2;
    pub const VGT_TF_RING_SIZE_SIENNA_CICHLID: u32 = 0x0fc3;
    pub const VGT_HS_OFFCHIP_PARAM_SIENNA_CICHLID: u32 = 0x0fc4;
    pub const VGT_TF_MEMORY_BASE_SIENNA_CICHLID: u32 = 0x0fc5;
    pub const VGT_TF_MEMORY_BASE_HI_SIENNA_CICHLID: u32 = 0x0fc6;
    /// Base address index of the RLC/CP window (BASE_IDX 1).
    pub const BASE_RLC: usize = 1;
    pub const CP_RB_DOORBELL_RANGE_LOWER_SHIFT: u64 = 0x2;
    pub const CP_RB_DOORBELL_RANGE_LOWER_MASK: u64 = 0x0000_0FFC;
    pub const CP_RB_DOORBELL_RANGE_UPPER_MASK: u64 = 0x0000_0FFC;

    pub fn base_idx(name: &str) -> usize {
        match name {
            "CGTT_SPI_CS_CLK_CTRL" => BASE_RLC,
            "CGTT_SPI_RA0_CLK_CTRL" => BASE_RLC,
            "CGTT_SPI_RA1_CLK_CTRL" => BASE_RLC,
            "GL2C_CTRL3" => BASE_RLC,
            "RLC_CP_SCHEDULERS_SIENNA_CICHLID" => BASE_RLC,
            _ => 0,
        }
    }
}

/// Generated clear-state data (build.rs from clearstate_gfx10.h).
mod clearstate {
    #![allow(non_upper_case_globals)]
    include!(concat!(env!("OUT_DIR"), "/gfx10_clearstate.rs"));
}
/// Generated cleaner-shader blobs (build.rs).
mod cleaner_shader {
    #![allow(dead_code, non_upper_case_globals)]
    include!(concat!(env!("OUT_DIR"), "/gfx10_cleaner_shader.rs"));
}

/// Ring sizes (Linux defaults for gfx10).
const RING_SIZE: usize = 32 << 10;
/// MEC hpd (EOP) size in bytes per queue.
const MEC_HPD_SIZE: usize = 2048;
/// Number of compute queues (sienna cichlid).
const NUM_COMPUTE_RINGS: usize = 8;
/// Golden registers for GC 10.3.4 (gfx_v10_0.c:3507), as
/// `(reg, base_idx, and_mask, or_mask)`.
const GOLDEN_GC_10_3_4: &[(u32, u32, u32, u32)] = &[
    (
        local_regs::CGTT_SPI_CS_CLK_CTRL,
        local_regs::BASE_RLC as u32,
        0x7800_0000,
        0x7800_0100,
    ),
    (
        local_regs::CGTT_SPI_RA0_CLK_CTRL,
        local_regs::BASE_RLC as u32,
        0x3000_0000,
        0x3000_0100,
    ),
    (
        local_regs::CGTT_SPI_RA1_CLK_CTRL,
        local_regs::BASE_RLC as u32,
        0x7e00_0000,
        0x7e00_0100,
    ),
    (gc::mmCPF_GCR_CNTL, 0, 0x0007_ffff, 0x0000_c000),
    (gc::mmDB_DEBUG3, 0, 0x0000_0280, 0x0000_0280),
    (gc::mmDB_DEBUG4, 0, 0x0780_0000, 0x0080_0000),
    (
        local_regs::GCR_GENERAL_CNTL_SIENNA_CICHLID,
        0,
        0x0000_1d00,
        0x0000_0500,
    ),
    (gc::mmGE_PC_CNTL, 0, 0x003c_0000, 0x0028_0400),
    (gc::mmGL2A_ADDR_MATCH_MASK, 0, 0xffff_ffff, 0xffff_ffcf),
    (gc::mmGL2C_ADDR_MATCH_MASK, 0, 0xffff_ffff, 0xffff_ffcf),
    (gc::mmGL2C_CM_CTRL1, 0, 0x4000_0000, 0x580f_1008),
    (
        local_regs::GL2C_CTRL3,
        local_regs::BASE_RLC as u32,
        0x0004_0000,
        0x00f8_0988,
    ),
    (gc::mmPA_CL_ENHANCE, 0, 0x0100_0000, 0x0120_0007),
    (
        gc::mmPA_SC_BINNER_TIMEOUT_COUNTER,
        0,
        0xffff_ffff,
        0x0000_0800,
    ),
    (gc::mmPA_SC_ENHANCE_2, 0, 0x0000_0800, 0x0000_0820),
    (gc::mmSQ_CONFIG, 0, 0x0000_001f, 0x0018_0070),
    (gc::mmSX_DEBUG_1, 0, 0x0001_0000, 0x0001_0020),
    (local_regs::TA_CNTL_AUX, 0, 0x0103_0000, 0x0103_0000),
    (gc::mmUTCL1_CTRL, 0, 0x03a0_0000, 0x00a0_0000),
    (gc::mmLDS_CONFIG, 0, 0x0000_0020, 0x0000_0020),
];

/// MQD field dword offsets (struct v10_compute_mqd, v10_structs.h:675).
const MQD_HEADER: usize = 0;
const MQD_PIPELINESTAT_ENABLE: usize = 11;
const MQD_STATIC_THREAD_MGMT_SE0: usize = 23;
const MQD_STATIC_THREAD_MGMT_SE1: usize = 24;
const MQD_STATIC_THREAD_MGMT_SE2: usize = 26;
const MQD_STATIC_THREAD_MGMT_SE3: usize = 27;
const MQD_MISC_RESERVED: usize = 32;
const MQD_MQD_BASE_ADDR_LO: usize = 128;
const MQD_MQD_BASE_ADDR_HI: usize = 129;
const MQD_ACTIVE: usize = 130;
const MQD_VMID: usize = 131;
const MQD_PERSISTENT_STATE: usize = 132;
const MQD_PIPE_PRIORITY: usize = 133;
const MQD_QUEUE_PRIORITY: usize = 134;
const MQD_QUANTUM: usize = 135;
const MQD_PQ_BASE_LO: usize = 136;
const MQD_PQ_BASE_HI: usize = 137;
const MQD_PQ_RPTR: usize = 138;
const MQD_PQ_RPTR_REPORT_ADDR_LO: usize = 139;
const MQD_PQ_RPTR_REPORT_ADDR_HI: usize = 140;
const MQD_PQ_WPTR_POLL_ADDR_LO: usize = 141;
const MQD_PQ_WPTR_POLL_ADDR_HI: usize = 142;
const MQD_PQ_DOORBELL_CONTROL: usize = 143;
const MQD_PQ_CONTROL: usize = 145;
const MQD_IB_CONTROL: usize = 149;
const MQD_DEQUEUE_REQUEST: usize = 152;
const MQD_MQD_CONTROL: usize = 162;
const MQD_EOP_BASE_ADDR_LO: usize = 165;
const MQD_EOP_BASE_ADDR_HI: usize = 166;
const MQD_EOP_CONTROL: usize = 167;
const MQD_PQ_WPTR_LO: usize = 182;
const MQD_PQ_WPTR_HI: usize = 183;

/// GRBM CAM remapping pairs for 10.3.4 (setup_grbm_cam_remapping).
const CAM_PAIRS: &[(u32, u32)] = &[
    (
        gc::mmVGT_TF_RING_SIZE_UMD,
        local_regs::VGT_TF_RING_SIZE_SIENNA_CICHLID,
    ),
    (
        gc::mmVGT_TF_MEMORY_BASE_UMD,
        local_regs::VGT_TF_MEMORY_BASE_SIENNA_CICHLID,
    ),
    (
        gc::mmVGT_TF_MEMORY_BASE_HI_UMD,
        local_regs::VGT_TF_MEMORY_BASE_HI_SIENNA_CICHLID,
    ),
    (
        gc::mmVGT_HS_OFFCHIP_PARAM_UMD,
        local_regs::VGT_HS_OFFCHIP_PARAM_SIENNA_CICHLID,
    ),
    (
        gc::mmVGT_ESGS_RING_SIZE_UMD,
        local_regs::VGT_ESGS_RING_SIZE_SIENNA_CICHLID,
    ),
    (
        gc::mmVGT_GSVS_RING_SIZE_UMD,
        local_regs::VGT_GSVS_RING_SIZE_SIENNA_CICHLID,
    ),
    (
        gc::mmSPI_CONFIG_CNTL_REMAP,
        local_regs::SPI_CONFIG_CNTL_SIENNA_CICHLID,
    ),
];

pub struct GfxV10 {
    _version: IpVersion,
    gfx_rings: Vec<Ring>,
    kiq: Option<Ring>,
    compute_rings: Vec<Ring>,
    hpd_eop: Option<Bo>,
    csb: Option<Bo>,
    csb_dwords: usize,
    cleaner: Option<Bo>,
    gb_addr_config: u32,
    max_shader_engines: u32,
    max_sh_per_se: u32,
    max_backends_per_se: u32,
    num_pipe_per_me: u32,
    pa_sc_tile_steering_override: u32,
    max_hw_contexts: u32,
}

#[derive(Clone, Copy)]
struct WaitRegMem {
    engine: u32,
    memory_space: u32,
    operation: u32,
    address0: u32,
    address1: u32,
    reference: u32,
    mask: u32,
    interval: u32,
}

#[derive(Clone, Copy)]
struct MqdConfig {
    is_kiq: bool,
    mqd_gpu_addr: u64,
    ring_gpu_addr: u64,
    doorbell_index: u32,
    rptr_wb: u64,
    wptr_wb: u64,
    queue_size: usize,
    eop_base: u64,
}

impl GfxV10 {
    pub fn new(version: IpVersion) -> Self {
        Self {
            _version: version,
            gfx_rings: Vec::new(),
            kiq: None,
            compute_rings: Vec::new(),
            hpd_eop: None,
            csb: None,
            csb_dwords: 0,
            cleaner: None,
            gb_addr_config: 0,
            max_shader_engines: 0,
            max_sh_per_se: 0,
            max_backends_per_se: 0,
            num_pipe_per_me: 2,
            pa_sc_tile_steering_override: 0,
            max_hw_contexts: 8,
        }
    }

    fn emit_release_mem(
        ring: &mut Ring,
        address: u64,
        sequence: u64,
        interrupt: bool,
    ) -> Result<()> {
        const PACKET3_RELEASE_MEM: u32 = 0x49;
        const CACHE_FLUSH_AND_INV_TS_EVENT: u32 = 0x14;
        let event = (1 << 22)
            | (1 << 21)
            | (1 << 13)
            | (1 << 12)
            | (3 << 25)
            | CACHE_FLUSH_AND_INV_TS_EVENT
            | (5 << 8);
        ring.write(packet3(PACKET3_RELEASE_MEM, 6))?;
        ring.write(event)?;
        // Linux gfx_v10_0_ring_emit_fence(): INT_SEL=2 requests a completion
        // interrupt, DATA_SEL=2 writes the full 64-bit sequence.
        ring.write((2 << 29) | if interrupt { 2 << 24 } else { 0 })?;
        ring.write(address as u32)?;
        ring.write((address >> 32) as u32)?;
        ring.write(sequence as u32)?;
        ring.write((sequence >> 32) as u32)?;
        ring.write(0)
    }

    /// Linux `gfx_v10_0_wait_reg_mem`.
    fn emit_wait_reg_mem(ring: &mut Ring, wait: WaitRegMem) -> Result<()> {
        const PACKET3_WAIT_REG_MEM: u32 = 0x3c;
        ring.write(packet3(PACKET3_WAIT_REG_MEM, 5))?;
        ring.write((wait.memory_space << 4) | (wait.operation << 6) | 3 | (wait.engine << 8))?;
        ring.write(wait.address0)?;
        ring.write(wait.address1)?;
        ring.write(wait.reference)?;
        ring.write(wait.mask)?;
        ring.write(wait.interval)
    }

    /// Linux `gfx_v10_0_ring_emit_wreg`. Register addresses passed to PM4
    /// are full SOC15 dword offsets, not IP-relative offsets.
    fn emit_wreg(ring: &mut Ring, compute: bool, reg: u32, value: u32) -> Result<()> {
        const PACKET3_WRITE_DATA: u32 = 0x37;
        const WR_CONFIRM: u32 = 1 << 20;
        let command = if compute {
            WR_CONFIRM
        } else {
            (1 << 30) | WR_CONFIRM
        };
        ring.write(packet3(PACKET3_WRITE_DATA, 3))?;
        ring.write(command)?;
        ring.write(reg)?;
        ring.write(0)?;
        ring.write(value)
    }

    fn emit_reg_wait(ring: &mut Ring, reg: u32, value: u32, mask: u32) -> Result<()> {
        Self::emit_wait_reg_mem(
            ring,
            WaitRegMem {
                engine: 0,
                memory_space: 0,
                operation: 0,
                address0: reg,
                address1: 0,
                reference: value,
                mask,
                interval: 0x20,
            },
        )
    }

    /// Linux's fallback `amdgpu_ring_emit_reg_write_reg_wait_helper`.
    /// Whether this Navi23 firmware supports the combined write/wait form
    /// is not currently tracked, so use the architecturally equivalent
    /// WRITE_DATA followed by WAIT_REG_MEM sequence.
    fn emit_reg_write_reg_wait(
        ring: &mut Ring,
        compute: bool,
        write_reg: u32,
        wait_reg: u32,
        reference: u32,
        mask: u32,
    ) -> Result<()> {
        Self::emit_wreg(ring, compute, write_reg, reference)?;
        Self::emit_reg_wait(ring, wait_reg, mask, mask)
    }

    fn full_reg(dev: &Adapter, ip: HwIp, reg: u32, base_idx: usize) -> Result<u32> {
        dev.regs
            .base_u32(ip, 0, base_idx)?
            .checked_add(reg)
            .ok_or(Error::Range)
    }

    fn invalidate_request(vmid: u32) -> u32 {
        let mut request = 1 << vmid;
        for shift in [
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE0__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE1__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE2__SHIFT,
            gc::GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L1_PTES__SHIFT,
        ] {
            request |= 1 << shift;
        }
        request
    }

    /// Linux `gmc_v10_0_emit_flush_gpu_tlb` followed by
    /// `gfx_v10_0_ring_emit_vm_flush`.
    fn emit_vm_flush(
        ring: &mut Ring,
        dev: &Adapter,
        compute: bool,
        vmid: u32,
        root_pde: u64,
    ) -> Result<()> {
        const PACKET3_PFP_SYNC_ME: u32 = 0x42;
        if ring.vm_inv_eng == u32::MAX || ring.vm_inv_eng >= 17 {
            return Err(Error::InvalidArgument);
        }

        let context_distance = gc::mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32
            - gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
        let context_offset = context_distance.checked_mul(vmid).ok_or(Error::Range)?;
        let ptb_lo = Self::full_reg(
            dev,
            HwIp::Gc,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 + context_offset,
            ridx!(gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32),
        )?;
        let ptb_hi = Self::full_reg(
            dev,
            HwIp::Gc,
            gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 + context_offset,
            ridx!(gc::mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32),
        )?;
        let engine_distance = gc::mmGCVM_INVALIDATE_ENG1_REQ - gc::mmGCVM_INVALIDATE_ENG0_REQ;
        let engine_offset = engine_distance
            .checked_mul(ring.vm_inv_eng)
            .ok_or(Error::Range)?;
        let invalidate_req = Self::full_reg(
            dev,
            HwIp::Gc,
            gc::mmGCVM_INVALIDATE_ENG0_REQ + engine_offset,
            ridx!(gc::mmGCVM_INVALIDATE_ENG0_REQ),
        )?;
        let invalidate_ack = Self::full_reg(
            dev,
            HwIp::Gc,
            gc::mmGCVM_INVALIDATE_ENG0_ACK + engine_offset,
            ridx!(gc::mmGCVM_INVALIDATE_ENG0_ACK),
        )?;

        Self::emit_wreg(ring, compute, ptb_lo, root_pde as u32)?;
        Self::emit_wreg(ring, compute, ptb_hi, (root_pde >> 32) as u32)?;
        Self::emit_reg_write_reg_wait(
            ring,
            compute,
            invalidate_req,
            invalidate_ack,
            Self::invalidate_request(vmid),
            1 << vmid,
        )?;

        // Compute rings do not have a PFP.
        if !compute {
            ring.write(packet3(PACKET3_PFP_SYNC_ME, 0))?;
            ring.write(0)?;
        }
        Ok(())
    }

    fn emit_pipeline_sync(ring: &mut Ring, compute: bool) -> Result<()> {
        Self::emit_wait_reg_mem(
            ring,
            WaitRegMem {
                engine: if compute { 0 } else { 1 },
                memory_space: 1,
                operation: 0,
                address0: ring.fence_wb as u32,
                address1: (ring.fence_wb >> 32) as u32,
                reference: ring.fence_seq as u32,
                mask: u32::MAX,
                interval: 4,
            },
        )
    }

    /// Linux `gfx_v10_0_ring_emit_hdp_flush` using the NBIO 4.3 masks.
    fn emit_hdp_flush(ring: &mut Ring, dev: &Adapter, compute: bool) -> Result<()> {
        let request = Self::full_reg(
            dev,
            HwIp::Nbio,
            nbio::regBIF_BX_PF0_GPU_HDP_FLUSH_REQ,
            ridx!(nbio::regBIF_BX_PF0_GPU_HDP_FLUSH_REQ),
        )?;
        let done = Self::full_reg(
            dev,
            HwIp::Nbio,
            nbio::regBIF_BX_PF0_GPU_HDP_FLUSH_DONE,
            ridx!(nbio::regBIF_BX_PF0_GPU_HDP_FLUSH_DONE),
        )?;
        let mask = if compute {
            0x4u32.checked_shl(ring.pipe).ok_or(Error::Range)?
        } else {
            0x1u32.checked_shl(ring.pipe).ok_or(Error::Range)?
        };
        Self::emit_wait_reg_mem(
            ring,
            WaitRegMem {
                engine: if compute { 0 } else { 1 },
                memory_space: 0,
                operation: 1,
                address0: request,
                address1: done,
                reference: mask,
                mask,
                interval: 0x20,
            },
        )
    }

    fn emit_hdp_invalidate(ring: &mut Ring, dev: &Adapter, compute: bool) -> Result<()> {
        let reg = Self::full_reg(
            dev,
            HwIp::Hdp,
            hdp::mmHDP_READ_CACHE_INVALIDATE,
            ridx!(hdp::mmHDP_READ_CACHE_INVALIDATE),
        )?;
        Self::emit_wreg(ring, compute, reg, 1)
    }

    fn emit_mem_sync(ring: &mut Ring) -> Result<()> {
        const PACKET3_ACQUIRE_MEM: u32 = 0x58;
        // gfx_v10_0_emit_mem_sync GCR_CNTL: GL2 INV/WB, GLM INV/WB and
        // GL1/GLV/GLK/GLI invalidation.
        const GCR_CNTL: u32 = 0x0000_c3b1;
        ring.write(packet3(PACKET3_ACQUIRE_MEM, 6))?;
        ring.write(0)?;
        ring.write(u32::MAX)?;
        ring.write(0x00ff_ffff)?;
        ring.write(0)?;
        ring.write(0)?;
        ring.write(0x0a)?;
        ring.write(GCR_CNTL)
    }

    fn emit_context_control(ring: &mut Ring, context_switch: bool, preamble: bool) -> Result<()> {
        // gfx_v10_0_ring_emit_cntxcntl. Linux emits this packet for every
        // GFX job; the broader register-load mask is conditional on an
        // actual scheduler context switch.
        let mut load = 0x8000_0000;
        if context_switch {
            load |= 0x0000_8001 | 0x0100_0000 | 0x0001_0002;
            if preamble {
                load |= 0x1000_0000;
            }
        }
        ring.write(packet3(PACKET3_CONTEXT_CONTROL, 1))?;
        ring.write(load)?;
        ring.write(0)
    }

    fn emit_switch_buffer(ring: &mut Ring) -> Result<()> {
        const PACKET3_SWITCH_BUFFER: u32 = 0x8b;
        ring.write(packet3(PACKET3_SWITCH_BUFFER, 0))?;
        ring.write(0)
    }

    fn submit_ring_ibs(
        ring: &mut Ring,
        dev: &mut Adapter,
        compute: bool,
        submission: UserSubmission<'_>,
    ) -> Result<(u64, u64)> {
        const PACKET3_INDIRECT_BUFFER_CNST: u32 = 0x33;
        const PACKET3_INDIRECT_BUFFER: u32 = 0x3f;
        const INDIRECT_BUFFER_VALID: u32 = 1 << 23;

        if ring.fence_wb == 0 {
            return Err(Error::NoDevice);
        }
        // The job body plus Linux's 256-dword commit alignment.  Keep a
        // conservative extra packet budget so asynchronous ioctl return can
        // never overwrite an in-flight ring segment.
        let reserve = 512u32
            .checked_add(
                u32::try_from(submission.ibs.len())
                    .map_err(|_| Error::Range)?
                    .checked_mul(4)
                    .ok_or(Error::Range)?,
            )
            .ok_or(Error::Range)?;
        ring.wait_for_space(dev, reserve, 1_000_000)?;
        let context_key = ((submission.vmid as u64) << 32) | submission.context_id as u64;
        let need_context_switch = ring.current_ctx != Some(context_key);
        if ring.fence_seq == 0 {
            dev_info!(
                "astra: first Linux-style CS frame on {} ring pipe {}: VMID {} inv_eng {} root {:#018x} fence {:#018x}",
                if compute { "compute" } else { "gfx" },
                ring.pipe,
                submission.vmid,
                ring.vm_inv_eng,
                submission.root_pde,
                ring.fence_wb,
            );
        }
        if need_context_switch && ring.fence_seq != 0 {
            Self::emit_pipeline_sync(ring, compute)?;
        }

        // VM sequence from amdgpu_vm_flush(): ring-side PTB programming,
        // invalidate, VM fence, and the non-conditional double switch.
        Self::emit_vm_flush(ring, dev, compute, submission.vmid, submission.root_pde)?;
        let vm_fence = ring.fence_seq.checked_add(1).ok_or(Error::Range)?;
        Self::emit_release_mem(ring, ring.fence_wb, vm_fence, false)?;
        if !compute {
            Self::emit_switch_buffer(ring)?;
            Self::emit_switch_buffer(ring)?;
        }

        // IB sequence from amdgpu_ib_schedule().
        if submission.ibs[0].flags & crate::uapi::AMDGPU_IB_FLAG_EMIT_MEM_SYNC != 0 {
            Self::emit_mem_sync(ring)?;
        }
        Self::emit_hdp_flush(ring, dev, compute)?;
        if !compute {
            let preamble = submission
                .ibs
                .iter()
                .any(|ib| ib.flags & crate::uapi::AMDGPU_IB_FLAG_PREAMBLE != 0);
            Self::emit_context_control(ring, need_context_switch, preamble)?;
        }

        for ib in submission.ibs {
            let opcode = if !compute && ib.flags & crate::uapi::AMDGPU_IB_FLAG_CE != 0 {
                PACKET3_INDIRECT_BUFFER_CNST
            } else {
                PACKET3_INDIRECT_BUFFER
            };
            let mut control = ib.length_dw | (submission.vmid << 24);
            if compute {
                control |= INDIRECT_BUFFER_VALID;
            }
            ring.write(packet3(opcode, 2))?;
            ring.write(ib.va_start as u32)?;
            ring.write((ib.va_start >> 32) as u32)?;
            ring.write(control)?;
        }
        Self::emit_hdp_invalidate(ring, dev, compute)?;
        if let Some(fence) = submission.user_fence {
            Self::emit_release_mem(ring, fence.gpu_addr, fence.sequence, false)?;
        }
        let completion_value = vm_fence.checked_add(1).ok_or(Error::Range)?;
        Self::emit_release_mem(ring, ring.fence_wb, completion_value, true)?;
        if !compute {
            Self::emit_switch_buffer(ring)?;
        }
        ring.commit(dev)?;
        ring.fence_seq = completion_value;
        ring.current_ctx = Some(context_key);
        Ok((ring.fence_wb, completion_value))
    }

    fn wb_slot(&self, dev: &mut Adapter) -> Result<u64> {
        dev.wb.as_mut().ok_or(Error::NoDevice)?.get()
    }

    /// Linux nv_grbm_select.
    fn grbm_select(
        &self,
        dev: &mut Adapter,
        me: u32,
        pipe: u32,
        queue: u32,
        vmid: u32,
    ) -> Result<()> {
        let mut value = 0;
        value = set_field(
            value,
            gc::GRBM_GFX_CNTL__PIPEID__SHIFT,
            gc::GRBM_GFX_CNTL__PIPEID_MASK,
            pipe as u64,
        );
        value = set_field(
            value,
            gc::GRBM_GFX_CNTL__MEID__SHIFT,
            gc::GRBM_GFX_CNTL__MEID_MASK,
            me as u64,
        );
        value = set_field(
            value,
            gc::GRBM_GFX_CNTL__VMID__SHIFT,
            gc::GRBM_GFX_CNTL__VMID_MASK,
            vmid as u64,
        );
        value = set_field(
            value,
            gc::GRBM_GFX_CNTL__QUEUEID__SHIFT,
            gc::GRBM_GFX_CNTL__QUEUEID_MASK,
            queue as u64,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGRBM_GFX_CNTL,
            ridx!(gc::mmGRBM_GFX_CNTL),
            value,
        )
    }

    /// Linux gfx_v10_0_select_se_sh.
    fn select_se_sh(&self, dev: &mut Adapter, se: u32, sh: u32, instance: u32) -> Result<()> {
        let mut value = 0;
        if instance == u32::MAX {
            value = set_field(
                value,
                gc::GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES__SHIFT,
                gc::GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK,
                1,
            );
        } else {
            value = set_field(
                value,
                gc::GRBM_GFX_INDEX__INSTANCE_INDEX__SHIFT,
                gc::GRBM_GFX_INDEX__INSTANCE_INDEX_MASK,
                instance as u64,
            );
        }
        if se == u32::MAX {
            value = set_field(
                value,
                gc::GRBM_GFX_INDEX__SE_BROADCAST_WRITES__SHIFT,
                gc::GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK,
                1,
            );
        } else {
            value = set_field(
                value,
                gc::GRBM_GFX_INDEX__SE_INDEX__SHIFT,
                gc::GRBM_GFX_INDEX__SE_INDEX_MASK,
                se as u64,
            );
        }
        if sh == u32::MAX {
            value = set_field(
                value,
                gc::GRBM_GFX_INDEX__SA_BROADCAST_WRITES__SHIFT,
                gc::GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK,
                1,
            );
        } else {
            value = set_field(
                value,
                gc::GRBM_GFX_INDEX__SA_INDEX__SHIFT,
                gc::GRBM_GFX_INDEX__SA_INDEX_MASK,
                sh as u64,
            );
        }
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGRBM_GFX_INDEX,
            ridx!(gc::mmGRBM_GFX_INDEX),
            value,
        )
    }

    /// Linux gfx_v10_0_constants_init: RB config from GB_ADDR_CONFIG,
    /// DB ring control, SH_MEM config per VMID, compute/GDS VMIDs.
    fn constants_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let value = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmGRBM_CNTL, ridx!(gc::mmGRBM_CNTL))?;
        let value = set_field(
            value,
            gc::GRBM_CNTL__READ_TIMEOUT__SHIFT,
            gc::GRBM_CNTL__READ_TIMEOUT_MASK,
            0xff,
        );
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmGRBM_CNTL, ridx!(gc::mmGRBM_CNTL), value)?;

        // gfx_v10_0_setup_rb: read the hardware and user disable masks and
        // only record the active RB bitmap. Linux does not write either
        // disable register here.
        let rb_bitmap_width_per_sh = self.max_backends_per_se / self.max_sh_per_se;
        let mut active_rbs = 0u32;
        for se in 0..self.max_shader_engines {
            for sh in 0..self.max_sh_per_se {
                self.select_se_sh(dev, se, sh, u32::MAX)?;
                let hw_disabled = dev
                    .regs
                    .read_ip(HwIp::Gc, 0, gc::mmCC_RB_BACKEND_DISABLE, 0)?;
                let user_disabled =
                    dev.regs
                        .read_ip(HwIp::Gc, 0, gc::mmGC_USER_RB_BACKEND_DISABLE, 0)?;
                let disabled = get_field(
                    hw_disabled | user_disabled,
                    gc::CC_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT,
                    gc::CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK,
                );
                active_rbs |= (!disabled & ((1 << rb_bitmap_width_per_sh) - 1))
                    << ((se * self.max_sh_per_se + sh) * rb_bitmap_width_per_sh);
            }
        }
        self.select_se_sh(dev, u32::MAX, u32::MAX, u32::MAX)?;
        dev.gfx_info.backend_enable_mask = active_rbs;
        dev_info!("astra: GFX active RB mask {:#010x}", active_rbs);

        // Linux gfx_v10_0_get_cu_info: convert the active-WGP mask into
        // the CU bitmap consumed by Mesa. Each enabled RDNA WGP contains
        // two CUs. The discovery table supplies the maximum WGP count;
        // the two shader-array registers apply fuse and VBIOS harvesting.
        let max_wgps = dev.gfx_info.max_cu_per_sh / 2;
        let valid_wgps = if max_wgps >= 32 {
            u32::MAX
        } else if max_wgps == 0 {
            0
        } else {
            (1u32 << max_wgps) - 1
        };
        let mut active_cus = 0u32;
        for se in 0..self.max_shader_engines.min(4) {
            for sh in 0..self.max_sh_per_se.min(4) {
                self.select_se_sh(dev, se, sh, u32::MAX)?;
                let hw = dev.regs.read_ip(
                    HwIp::Gc,
                    0,
                    gc::mmCC_GC_SHADER_ARRAY_CONFIG,
                    ridx!(gc::mmCC_GC_SHADER_ARRAY_CONFIG),
                )?;
                let user = dev.regs.read_ip(
                    HwIp::Gc,
                    0,
                    gc::mmGC_USER_SHADER_ARRAY_CONFIG,
                    ridx!(gc::mmGC_USER_SHADER_ARRAY_CONFIG),
                )?;
                let inactive = get_field(
                    hw | user,
                    gc::CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT,
                    gc::CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK,
                );
                let active_wgps = !inactive & valid_wgps;
                let mut cu_bitmap = 0u32;
                for wgp in 0..max_wgps.min(16) {
                    if active_wgps & (1 << wgp) != 0 {
                        cu_bitmap |= 3 << (wgp * 2);
                    }
                }
                dev.gfx_info.cu_bitmap[se as usize][sh as usize] = cu_bitmap;
                dev.gfx_info.cu_ao_bitmap[se as usize][sh as usize] = cu_bitmap;
                active_cus += cu_bitmap.count_ones();
            }
        }
        self.select_se_sh(dev, u32::MAX, u32::MAX, u32::MAX)?;
        dev.gfx_info.cu_active_number = active_cus;
        dev_info!("astra: GFX active CU count {}", active_cus);

        // DB_RING_CONTROL: split occlusion counters between gfx pipes.
        let value = dev.regs.read_ip(
            HwIp::Gc,
            0,
            gc::mmDB_RING_CONTROL,
            ridx!(gc::mmDB_RING_CONTROL),
        )?;
        let value = set_field(
            value,
            gc::DB_RING_CONTROL__COUNTER_CONTROL__SHIFT,
            gc::DB_RING_CONTROL__COUNTER_CONTROL_MASK,
            (self.num_pipe_per_me <= 1) as u64,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmDB_RING_CONTROL,
            ridx!(gc::mmDB_RING_CONTROL),
            value,
        )?;

        // SH_MEM config per VMID.
        let default_sh_mem_config = set_field(
            0,
            gc::SH_MEM_CONFIG__ADDRESS_MODE__SHIFT,
            gc::SH_MEM_CONFIG__ADDRESS_MODE_MASK,
            2,
        ) | set_field(
            0,
            gc::SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT,
            gc::SH_MEM_CONFIG__ALIGNMENT_MODE_MASK,
            3,
        ) | set_field(
            0,
            gc::SH_MEM_CONFIG__INITIAL_INST_PREFETCH__SHIFT,
            gc::SH_MEM_CONFIG__INITIAL_INST_PREFETCH_MASK,
            3,
        );
        for vmid in 0..16 {
            self.grbm_select(dev, 0, 0, 0, vmid)?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmSH_MEM_CONFIG,
                ridx!(gc::mmSH_MEM_CONFIG),
                default_sh_mem_config,
            )?;
            if vmid != 0 {
                dev.regs.write_ip(
                    HwIp::Gc,
                    0,
                    gc::mmSH_MEM_BASES,
                    ridx!(gc::mmSH_MEM_BASES),
                    0,
                )?;
            }
        }
        self.grbm_select(dev, 0, 0, 0, 0)?;

        // init_compute_vmid (VMIDs 8..16) + init_gds_vmid (1..16).
        for vmid in 1..16 {
            dev.regs
                .write_ip(HwIp::Gc, 0, gc::mmGDS_VMID0_BASE + 2 * vmid, 0, 0)?;
            dev.regs
                .write_ip(HwIp::Gc, 0, gc::mmGDS_VMID0_SIZE + 2 * vmid, 0, 0)?;
            dev.regs
                .write_ip(HwIp::Gc, 0, gc::mmGDS_GWS_VMID0 + vmid, 0, 0)?;
            dev.regs
                .write_ip(HwIp::Gc, 0, gc::mmGDS_OA_VMID0 + vmid, 0, 0)?;
        }
        Ok(())
    }

    /// Linux setup_grbm_cam_remapping.
    fn cam_remapping(&self, dev: &mut Adapter) -> Result<()> {
        // Check whether the remap is already in place.
        let pattern = 0xDEAD_BEEFu32;
        let data = dev.regs.read_ip(
            HwIp::Gc,
            0,
            local_regs::VGT_ESGS_RING_SIZE_SIENNA_CICHLID,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            local_regs::VGT_ESGS_RING_SIZE_SIENNA_CICHLID,
            0,
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmVGT_ESGS_RING_SIZE_UMD,
            local_regs::BASE_RLC,
            pattern,
        )?;
        let remapped = dev.regs.read_ip(
            HwIp::Gc,
            0,
            local_regs::VGT_ESGS_RING_SIZE_SIENNA_CICHLID,
            0,
        )? == pattern;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            local_regs::VGT_ESGS_RING_SIZE_SIENNA_CICHLID,
            0,
            data,
        )?;
        if remapped {
            return Ok(());
        }

        let main_base = dev.regs.base_u32(HwIp::Gc, 0, 0)?;
        let rlc_base = dev.regs.base_u32(HwIp::Gc, 0, local_regs::BASE_RLC)?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGRBM_CAM_INDEX,
            ridx!(gc::mmGRBM_CAM_INDEX),
            0,
        )?;
        for (umd, real) in CAM_PAIRS {
            let value = set_field(
                0,
                gc::GRBM_CAM_DATA__CAM_ADDR__SHIFT,
                gc::GRBM_CAM_DATA__CAM_ADDR_MASK,
                (rlc_base + *umd) as u64,
            ) | set_field(
                0,
                gc::GRBM_CAM_DATA__CAM_REMAPADDR__SHIFT,
                gc::GRBM_CAM_DATA__CAM_REMAPADDR_MASK,
                (main_base + *real) as u64,
            );
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGRBM_CAM_DATA_UPPER,
                ridx!(gc::mmGRBM_CAM_DATA_UPPER),
                0,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmGRBM_CAM_DATA,
                ridx!(gc::mmGRBM_CAM_DATA),
                value,
            )?;
        }
        Ok(())
    }

    /// Linux gfx_v10_0_wait_for_rlc_autoload_complete.
    fn wait_rlc_autoload(&self, dev: &mut Adapter) -> Result<()> {
        let mut last_cp_status = 0;
        let mut last_bootload = 0;
        for _ in 0..1_000_000 {
            let cp_status = dev
                .regs
                .read_ip(HwIp::Gc, 0, gc::mmCP_STAT, ridx!(gc::mmCP_STAT))?;
            let bootload = dev.regs.read_ip(
                HwIp::Gc,
                0,
                gc::mmRLC_RLCS_BOOTLOAD_STATUS,
                ridx!(gc::mmRLC_RLCS_BOOTLOAD_STATUS),
            )?;
            last_cp_status = cp_status;
            last_bootload = bootload;
            let complete = get_field(
                bootload,
                gc::RLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE__SHIFT,
                gc::RLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK,
            );
            if cp_status == 0 && complete == 1 {
                return Ok(());
            }
            time::delay(Duration::from_micros(1));
        }
        dev_info!(
            "astra: rlc autoload: gc ucode autoload timeout (CP_STAT={:#010x}, BOOTLOAD_STATUS={:#010x})",
            last_cp_status,
            last_bootload,
        );
        Err(Error::Io)
    }

    /// Linux gfx_v10_0_init_csb.
    fn init_csb(&mut self, dev: &mut Adapter) -> Result<()> {
        let csb_addr = self.csb.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmRLC_CSIB_ADDR_HI,
            ridx!(gc::mmRLC_CSIB_ADDR_HI),
            (csb_addr >> 32) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmRLC_CSIB_ADDR_LO,
            ridx!(gc::mmRLC_CSIB_ADDR_LO),
            (csb_addr as u32) & 0xffff_fffc,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmRLC_CSIB_LENGTH,
            ridx!(gc::mmRLC_CSIB_LENGTH),
            self.csb_dwords as u32,
        )?;
        Ok(())
    }

    /// Linux gfx_v10_0_rlc_resume (PSP/autoload path).
    fn rlc_resume(&mut self, dev: &mut Adapter) -> Result<()> {
        self.wait_rlc_autoload(dev)?;
        dev_info!("astra: RLC autoload complete");
        self.init_csb(dev)?;

        // update_spm_vmid_internal(0xf)
        let value = dev.regs.read_ip(
            HwIp::Gc,
            0,
            gc::mmRLC_SPM_MC_CNTL,
            ridx!(gc::mmRLC_SPM_MC_CNTL),
        )?;
        let value = (value & !(gc::RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK as u32))
            | set_field(
                0,
                gc::RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT,
                gc::RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK,
                0xf,
            );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmRLC_SPM_MC_CNTL,
            ridx!(gc::mmRLC_SPM_MC_CNTL),
            value,
        )?;

        // rlc_enable_srm
        let value = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmRLC_SRM_CNTL, ridx!(gc::mmRLC_SRM_CNTL))?;
        let value = value
            | gc::RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK as u32
            | gc::RLC_SRM_CNTL__SRM_ENABLE_MASK as u32;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmRLC_SRM_CNTL,
            ridx!(gc::mmRLC_SRM_CNTL),
            value,
        )?;
        Ok(())
    }

    /// Linux gfx_v10_0_kiq_init_register: program the KIQ MQD into the
    /// hardware registers (after grbm_select to MEC 1).
    fn kiq_init_register(&mut self, dev: &mut Adapter) -> Result<()> {
        // Values captured from the MQD buffer (offset table above).
        let mqd = |offset: usize| -> Result<u32> {
            let bo = self
                .kiq
                .as_ref()
                .and_then(|r| r.mqd.as_ref())
                .ok_or(Error::NoDevice)?;
            let cpu = bo.cpu.as_ref().ok_or(Error::NoDevice)?;
            let at = offset * 4;
            cpu.as_slice()
                .get(at..at + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                .ok_or(Error::Range)
        };

        // Disable wptr polling (WREG32_FIELD15(..., EN, 0)).
        let value = dev.regs.read_ip(
            HwIp::Gc,
            0,
            gc::mmCP_PQ_WPTR_POLL_CNTL,
            ridx!(gc::mmCP_PQ_WPTR_POLL_CNTL),
        )?;
        let value = set_field(
            value,
            gc::CP_PQ_WPTR_POLL_CNTL__EN__SHIFT,
            gc::CP_PQ_WPTR_POLL_CNTL__EN_MASK,
            0,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_PQ_WPTR_POLL_CNTL,
            ridx!(gc::mmCP_PQ_WPTR_POLL_CNTL),
            value,
        )?;

        // Dequeue any active queue.
        if dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_HQD_ACTIVE, 0)? & 1 != 0 {
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_HQD_DEQUEUE_REQUEST,
                ridx!(gc::mmCP_HQD_DEQUEUE_REQUEST),
                1,
            )?;
            for _ in 0..1_000_000 {
                if dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_HQD_ACTIVE, 0)? & 1 == 0 {
                    break;
                }
                time::delay(Duration::from_micros(1));
            }
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_HQD_DEQUEUE_REQUEST,
                ridx!(gc::mmCP_HQD_DEQUEUE_REQUEST),
                mqd(MQD_DEQUEUE_REQUEST)?,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_HQD_PQ_RPTR,
                ridx!(gc::mmCP_HQD_PQ_RPTR),
                mqd(MQD_PQ_RPTR)?,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_HQD_PQ_WPTR_LO,
                ridx!(gc::mmCP_HQD_PQ_WPTR_LO),
                mqd(MQD_PQ_WPTR_LO)?,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_HQD_PQ_WPTR_HI,
                ridx!(gc::mmCP_HQD_PQ_WPTR_HI),
                mqd(MQD_PQ_WPTR_HI)?,
            )?;
        }

        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_DOORBELL_CONTROL,
            ridx!(gc::mmCP_HQD_PQ_DOORBELL_CONTROL),
            0,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_EOP_BASE_ADDR,
            ridx!(gc::mmCP_HQD_EOP_BASE_ADDR),
            mqd(MQD_EOP_BASE_ADDR_LO)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_EOP_BASE_ADDR_HI,
            ridx!(gc::mmCP_HQD_EOP_BASE_ADDR_HI),
            mqd(MQD_EOP_BASE_ADDR_HI)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_EOP_CONTROL,
            ridx!(gc::mmCP_HQD_EOP_CONTROL),
            mqd(MQD_EOP_CONTROL)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_MQD_BASE_ADDR,
            ridx!(gc::mmCP_MQD_BASE_ADDR),
            mqd(MQD_MQD_BASE_ADDR_LO)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_MQD_BASE_ADDR_HI,
            ridx!(gc::mmCP_MQD_BASE_ADDR_HI),
            mqd(MQD_MQD_BASE_ADDR_HI)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_MQD_CONTROL,
            ridx!(gc::mmCP_MQD_CONTROL),
            mqd(MQD_MQD_CONTROL)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_BASE,
            ridx!(gc::mmCP_HQD_PQ_BASE),
            mqd(MQD_PQ_BASE_LO)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_BASE_HI,
            ridx!(gc::mmCP_HQD_PQ_BASE_HI),
            mqd(MQD_PQ_BASE_HI)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_CONTROL,
            ridx!(gc::mmCP_HQD_PQ_CONTROL),
            mqd(MQD_PQ_CONTROL)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_RPTR_REPORT_ADDR,
            ridx!(gc::mmCP_HQD_PQ_RPTR_REPORT_ADDR),
            mqd(MQD_PQ_RPTR_REPORT_ADDR_LO)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI,
            ridx!(gc::mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI),
            mqd(MQD_PQ_RPTR_REPORT_ADDR_HI)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_WPTR_POLL_ADDR,
            ridx!(gc::mmCP_HQD_PQ_WPTR_POLL_ADDR),
            mqd(MQD_PQ_WPTR_POLL_ADDR_LO)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_WPTR_POLL_ADDR_HI,
            ridx!(gc::mmCP_HQD_PQ_WPTR_POLL_ADDR_HI),
            mqd(MQD_PQ_WPTR_POLL_ADDR_HI)?,
        )?;

        // Doorbell range for the KIQ (kiq*2<<2 .. userqueue_end*2<<2).
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_MEC_DOORBELL_RANGE_LOWER,
            ridx!(gc::mmCP_MEC_DOORBELL_RANGE_LOWER),
            (doorbell::DOORBELL_KIQ * 2) << 2,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_MEC_DOORBELL_RANGE_UPPER,
            ridx!(gc::mmCP_MEC_DOORBELL_RANGE_UPPER),
            (0x8a * 2) << 2,
        )?;

        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_DOORBELL_CONTROL,
            ridx!(gc::mmCP_HQD_PQ_DOORBELL_CONTROL),
            mqd(MQD_PQ_DOORBELL_CONTROL)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_WPTR_LO,
            ridx!(gc::mmCP_HQD_PQ_WPTR_LO),
            mqd(MQD_PQ_WPTR_LO)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PQ_WPTR_HI,
            ridx!(gc::mmCP_HQD_PQ_WPTR_HI),
            mqd(MQD_PQ_WPTR_HI)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_VMID,
            ridx!(gc::mmCP_HQD_VMID),
            mqd(MQD_VMID)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_PERSISTENT_STATE,
            ridx!(gc::mmCP_HQD_PERSISTENT_STATE),
            mqd(MQD_PERSISTENT_STATE)?,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_HQD_ACTIVE,
            ridx!(gc::mmCP_HQD_ACTIVE),
            mqd(MQD_ACTIVE)?,
        )?;
        let value = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmCP_PQ_STATUS, ridx!(gc::mmCP_PQ_STATUS))?;
        let value = set_field(
            value,
            gc::CP_PQ_STATUS__DOORBELL_ENABLE__SHIFT,
            gc::CP_PQ_STATUS__DOORBELL_ENABLE_MASK,
            1,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_PQ_STATUS,
            ridx!(gc::mmCP_PQ_STATUS),
            value,
        )?;
        Ok(())
    }

    /// Linux gfx_v10_0_kiq_resume.
    fn kiq_resume(&mut self, dev: &mut Adapter) -> Result<()> {
        // kiq_setting: tell RLC which queue is the KIQ.
        let me = self.kiq.as_ref().ok_or(Error::NoDevice)?.me;
        let value = dev.regs.read_ip(
            HwIp::Gc,
            0,
            local_regs::RLC_CP_SCHEDULERS_SIENNA_CICHLID,
            ridx!(local_regs::RLC_CP_SCHEDULERS_SIENNA_CICHLID),
        )?;
        let pipe = self.kiq.as_ref().ok_or(Error::NoDevice)?.pipe;
        let queue = self.kiq.as_ref().ok_or(Error::NoDevice)?.queue;
        let value = (value & 0xffff_ff00) | (me << 5) | (pipe << 3) | queue | 0x80;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            local_regs::RLC_CP_SCHEDULERS_SIENNA_CICHLID,
            ridx!(local_regs::RLC_CP_SCHEDULERS_SIENNA_CICHLID),
            value,
        )?;

        // Build the MQD image in the KIQ MQD buffer (indexed like a
        // compute ring at slot 0 of the MQD list).
        let eop_base = self.hpd_eop.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        self.grbm_select(dev, me, pipe, queue, 0)?;
        self.fill_mqd(dev, 0, true, eop_base)?;
        self.kiq_init_register(dev)?;
        self.grbm_select(dev, 0, 0, 0, 0)?;
        Ok(())
    }

    /// Fills the MQD buffer of ring `index` (0 = KIQ, else compute).
    fn fill_mqd(
        &mut self,
        dev: &mut Adapter,
        index: usize,
        is_kiq: bool,
        eop_base: u64,
    ) -> Result<()> {
        if is_kiq {
            // Build the KIQ MQD into its buffer.
            let mqd_gpu_addr = self
                .kiq
                .as_ref()
                .ok_or(Error::NoDevice)?
                .mqd
                .as_ref()
                .ok_or(Error::NoDevice)?
                .gpu_addr;
            let ring = self.kiq.as_ref().ok_or(Error::NoDevice)?;
            let (gpu_addr, doorbell_index, rptr_wb, wptr_wb, queue_size) = (
                ring.gpu_addr,
                ring.doorbell,
                ring.rptr_wb,
                ring.wptr_wb,
                ring.size,
            );
            self.fill_mqd_buffer(
                dev,
                MqdConfig {
                    is_kiq: true,
                    mqd_gpu_addr,
                    ring_gpu_addr: gpu_addr,
                    doorbell_index,
                    rptr_wb,
                    wptr_wb,
                    queue_size,
                    eop_base,
                },
            )
        } else {
            let ring_idx = index - 1;
            let mqd_gpu_addr = self.compute_rings[ring_idx]
                .mqd
                .as_ref()
                .ok_or(Error::NoDevice)?
                .gpu_addr;
            let ring = &self.compute_rings[ring_idx];
            let (gpu_addr, doorbell_index, rptr_wb, wptr_wb, queue_size) = (
                ring.gpu_addr,
                ring.doorbell,
                ring.rptr_wb,
                ring.wptr_wb,
                ring.size,
            );
            self.fill_mqd_buffer(
                dev,
                MqdConfig {
                    is_kiq: false,
                    mqd_gpu_addr,
                    ring_gpu_addr: gpu_addr,
                    doorbell_index,
                    rptr_wb,
                    wptr_wb,
                    queue_size,
                    eop_base,
                },
            )
        }
    }

    fn fill_mqd_buffer(&mut self, dev: &mut Adapter, config: MqdConfig) -> Result<()> {
        // Locate the MQD CPU buffer by its GPU address.
        let mqd_bo = self
            .find_mqd_bo(config.mqd_gpu_addr)
            .ok_or(Error::NoDevice)?;
        let cpu = mqd_bo.cpu.as_mut().ok_or(Error::NoDevice)?;
        {
            let slice = cpu.as_mut_slice();
            slice.fill(0);
            let mut put = |offset: usize, value: u32| -> Result<()> {
                let at = offset * 4;
                let dst = slice.get_mut(at..at + 4).ok_or(Error::Range)?;
                dst.copy_from_slice(&value.to_le_bytes());
                Ok(())
            };

            put(MQD_HEADER, 0xC031_0800)?;
            put(MQD_PIPELINESTAT_ENABLE, 1)?;
            for offset in [
                MQD_STATIC_THREAD_MGMT_SE0,
                MQD_STATIC_THREAD_MGMT_SE1,
                MQD_STATIC_THREAD_MGMT_SE2,
                MQD_STATIC_THREAD_MGMT_SE3,
            ] {
                put(offset, 0xffff_ffff)?;
            }
            put(MQD_MISC_RESERVED, 3)?;

            let eop_base = config.eop_base >> 8;
            put(MQD_EOP_BASE_ADDR_LO, eop_base as u32)?;
            put(MQD_EOP_BASE_ADDR_HI, (eop_base >> 32) as u32)?;
            let eop_control = dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_HQD_EOP_CONTROL, 0)?;
            put(
                MQD_EOP_CONTROL,
                set_field(
                    eop_control,
                    gc::CP_HQD_EOP_CONTROL__EOP_SIZE__SHIFT,
                    gc::CP_HQD_EOP_CONTROL__EOP_SIZE_MASK,
                    8,
                ),
            )?;

            let doorbell_control =
                dev.regs
                    .read_ip(HwIp::Gc, 0, gc::mmCP_HQD_PQ_DOORBELL_CONTROL, 0)?;
            let mut doorbell_control = set_field(
                doorbell_control,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK,
                config.doorbell_index as u64,
            );
            doorbell_control = set_field(
                doorbell_control,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN__SHIFT,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK,
                1,
            );
            doorbell_control = set_field(
                doorbell_control,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_SOURCE__SHIFT,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_SOURCE_MASK,
                0,
            );
            doorbell_control = set_field(
                doorbell_control,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_HIT__SHIFT,
                gc::CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_HIT_MASK,
                0,
            );
            put(MQD_PQ_DOORBELL_CONTROL, doorbell_control)?;

            put(MQD_DEQUEUE_REQUEST, 0)?;
            put(MQD_PQ_RPTR, 0)?;
            put(MQD_PQ_WPTR_LO, 0)?;
            put(MQD_PQ_WPTR_HI, 0)?;
            put(
                MQD_MQD_BASE_ADDR_LO,
                (config.mqd_gpu_addr as u32) & 0xffff_fffc,
            )?;
            put(MQD_MQD_BASE_ADDR_HI, (config.mqd_gpu_addr >> 32) as u32)?;

            let mqd_control = dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_MQD_CONTROL, 0)?;
            put(
                MQD_MQD_CONTROL,
                set_field(
                    mqd_control,
                    gc::CP_MQD_CONTROL__VMID__SHIFT,
                    gc::CP_MQD_CONTROL__VMID_MASK,
                    0,
                ),
            )?;

            let hqd_base = config.ring_gpu_addr >> 8;
            put(MQD_PQ_BASE_LO, hqd_base as u32)?;
            put(MQD_PQ_BASE_HI, (hqd_base >> 32) as u32)?;

            let pq_control = dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_HQD_PQ_CONTROL, 0)?;
            let mut pq_control = set_field(
                pq_control,
                gc::CP_HQD_PQ_CONTROL__QUEUE_SIZE__SHIFT,
                gc::CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK,
                (config.queue_size / 4).trailing_zeros() as u64 - 1,
            );
            pq_control = set_field(
                pq_control,
                gc::CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE__SHIFT,
                gc::CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE_MASK,
                9,
            );
            pq_control = set_field(
                pq_control,
                gc::CP_HQD_PQ_CONTROL__UNORD_DISPATCH__SHIFT,
                gc::CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK,
                1,
            );
            pq_control = set_field(
                pq_control,
                gc::CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH__SHIFT,
                gc::CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH_MASK,
                0,
            );
            pq_control = set_field(
                pq_control,
                gc::CP_HQD_PQ_CONTROL__PRIV_STATE__SHIFT,
                gc::CP_HQD_PQ_CONTROL__PRIV_STATE_MASK,
                1,
            );
            pq_control = set_field(
                pq_control,
                gc::CP_HQD_PQ_CONTROL__KMD_QUEUE__SHIFT,
                gc::CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK,
                1,
            );
            put(MQD_PQ_CONTROL, pq_control)?;

            put(
                MQD_PQ_RPTR_REPORT_ADDR_LO,
                (config.rptr_wb as u32) & 0xffff_fffc,
            )?;
            put(
                MQD_PQ_RPTR_REPORT_ADDR_HI,
                ((config.rptr_wb >> 32) as u32) & 0xffff,
            )?;
            put(
                MQD_PQ_WPTR_POLL_ADDR_LO,
                (config.wptr_wb as u32) & 0xffff_fffc,
            )?;
            put(
                MQD_PQ_WPTR_POLL_ADDR_HI,
                ((config.wptr_wb >> 32) as u32) & 0xffff,
            )?;

            put(
                MQD_PQ_RPTR,
                dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_HQD_PQ_RPTR, 0)?,
            )?;
            put(MQD_VMID, 0)?;

            let persistent = dev
                .regs
                .read_ip(HwIp::Gc, 0, gc::mmCP_HQD_PERSISTENT_STATE, 0)?;
            put(
                MQD_PERSISTENT_STATE,
                set_field(
                    persistent,
                    gc::CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT,
                    gc::CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK,
                    0x53,
                ),
            )?;

            let ib_control = dev.regs.read_ip(HwIp::Gc, 0, gc::mmCP_HQD_IB_CONTROL, 0)?;
            put(
                MQD_IB_CONTROL,
                set_field(
                    ib_control,
                    gc::CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE__SHIFT,
                    gc::CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE_MASK,
                    3,
                ),
            )?;

            let quantum = set_field(
                0,
                gc::CP_HQD_QUANTUM__QUANTUM_EN__SHIFT,
                gc::CP_HQD_QUANTUM__QUANTUM_EN_MASK,
                1,
            ) | set_field(
                0,
                gc::CP_HQD_QUANTUM__QUANTUM_SCALE__SHIFT,
                gc::CP_HQD_QUANTUM__QUANTUM_SCALE_MASK,
                1,
            ) | set_field(
                0,
                gc::CP_HQD_QUANTUM__QUANTUM_DURATION__SHIFT,
                gc::CP_HQD_QUANTUM__QUANTUM_DURATION_MASK,
                1,
            );
            put(MQD_QUANTUM, quantum)?;
            // amdgpu_ring_to_mqd_prop: normal KGD queues use the zero/default
            // priorities, and MAP_QUEUES activates compute queues. Only the KIQ
            // is activated while its HQD registers are programmed directly.
            put(MQD_PIPE_PRIORITY, 0)?;
            put(MQD_QUEUE_PRIORITY, 0)?;
            put(MQD_ACTIVE, config.is_kiq as u32)?;
        }
        cpu.sync_for_device();
        Ok(())
    }

    fn find_mqd_bo(&mut self, gpu_addr: u64) -> Option<&mut Bo> {
        if let Some(kiq) = self.kiq.as_mut()
            && kiq.mqd.as_ref().map(|m| m.gpu_addr) == Some(gpu_addr)
        {
            return kiq.mqd.as_mut();
        }
        self.compute_rings
            .iter_mut()
            .find_map(|r| r.mqd.as_mut().filter(|m| m.gpu_addr == gpu_addr))
    }

    /// Linux gfx_v10_0_cp_gfx_switch_pipe.
    fn switch_pipe(&self, dev: &mut Adapter, pipe: u32) -> Result<()> {
        let value =
            dev.regs
                .read_ip(HwIp::Gc, 0, gc::mmGRBM_GFX_CNTL, ridx!(gc::mmGRBM_GFX_CNTL))?;
        let value = set_field(
            value,
            gc::GRBM_GFX_CNTL__PIPEID__SHIFT,
            gc::GRBM_GFX_CNTL__PIPEID_MASK,
            pipe as u64,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmGRBM_GFX_CNTL,
            ridx!(gc::mmGRBM_GFX_CNTL),
            value,
        )
    }

    /// Linux gfx_v10_0_cp_gfx_set_doorbell.
    fn cp_gfx_set_doorbell(&self, dev: &mut Adapter, doorbell_index: u32) -> Result<()> {
        let value = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmCP_RB_DOORBELL_CONTROL, 0)?;
        let mut value = set_field(
            value,
            gc::CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT,
            gc::CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK,
            doorbell_index as u64,
        );
        value = set_field(
            value,
            gc::CP_RB_DOORBELL_CONTROL__DOORBELL_EN__SHIFT,
            gc::CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK,
            1,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_RB_DOORBELL_CONTROL,
            ridx!(gc::mmCP_RB_DOORBELL_CONTROL),
            value,
        )?;

        // Doorbell range (10.3.x sienna field names).
        let lower = set_field(
            0,
            local_regs::CP_RB_DOORBELL_RANGE_LOWER_SHIFT,
            local_regs::CP_RB_DOORBELL_RANGE_LOWER_MASK,
            doorbell_index as u64,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_RB_DOORBELL_RANGE_LOWER,
            ridx!(gc::mmCP_RB_DOORBELL_RANGE_LOWER),
            lower,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_RB_DOORBELL_RANGE_UPPER,
            ridx!(gc::mmCP_RB_DOORBELL_RANGE_UPPER),
            local_regs::CP_RB_DOORBELL_RANGE_UPPER_MASK as u32,
        )?;
        Ok(())
    }

    /// Linux gfx_v10_0_cp_gfx_enable.
    fn cp_gfx_enable(&self, dev: &mut Adapter, enable: bool) -> Result<()> {
        let halt = (!enable) as u64;
        let value = dev
            .regs
            .read_ip(HwIp::Gc, 0, gc::mmCP_ME_CNTL, ridx!(gc::mmCP_ME_CNTL))?;
        let value = set_field(
            value,
            gc::CP_ME_CNTL__ME_HALT__SHIFT,
            gc::CP_ME_CNTL__ME_HALT_MASK,
            halt,
        );
        let value = set_field(
            value,
            gc::CP_ME_CNTL__PFP_HALT__SHIFT,
            gc::CP_ME_CNTL__PFP_HALT_MASK,
            halt,
        );
        let value = set_field(
            value,
            gc::CP_ME_CNTL__CE_HALT__SHIFT,
            gc::CP_ME_CNTL__CE_HALT_MASK,
            halt,
        );
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_ME_CNTL,
            ridx!(gc::mmCP_ME_CNTL),
            value,
        )
    }

    /// Linux gfx_v10_0_cp_gfx_start: init CP and stream the clear-state
    /// sequence through the gfx ring.
    fn cp_gfx_start(&mut self, dev: &mut Adapter) -> Result<()> {
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_MAX_CONTEXT,
            ridx!(gc::mmCP_MAX_CONTEXT),
            self.max_hw_contexts - 1,
        )?;
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_DEVICE_ID,
            ridx!(gc::mmCP_DEVICE_ID),
            1,
        )?;
        self.cp_gfx_enable(dev, true)?;

        let ring = &mut self.gfx_rings[0];
        ring.write(packet3(PACKET3_PREAMBLE_CNTL, 0))?;
        ring.write(PACKET3_PREAMBLE_BEGIN_CLEAR_STATE)?;
        ring.write(packet3(PACKET3_CONTEXT_CONTROL, 1))?;
        ring.write(0x8000_0000)?;
        ring.write(0x8000_0000)?;

        for (extent, reg_index) in Self::csb_extents() {
            ring.write(packet3(PACKET3_SET_CONTEXT_REG, extent.len() as u32))?;
            ring.write(reg_index - PACKET3_SET_CONTEXT_REG_START)?;
            for value in extent {
                ring.write(*value)?;
            }
        }

        // SOC15_REG_OFFSET(GC, 0, mmPA_SC_TILE_STEERING_OVERRIDE). Context
        // registers are in GC segment 1 on Navi23.
        let tile_reg = dev
            .regs
            .base_u32(HwIp::Gc, 0, ridx!(gc::mmPA_SC_TILE_STEERING_OVERRIDE))?
            + gc::mmPA_SC_TILE_STEERING_OVERRIDE;
        ring.write(packet3(PACKET3_SET_CONTEXT_REG, 1))?;
        ring.write(tile_reg - PACKET3_SET_CONTEXT_REG_START)?;
        ring.write(self.pa_sc_tile_steering_override)?;

        ring.write(packet3(PACKET3_PREAMBLE_CNTL, 0))?;
        ring.write(PACKET3_PREAMBLE_END_CLEAR_STATE)?;
        ring.write(packet3(PACKET3_CLEAR_STATE, 0))?;
        ring.write(0)?;
        ring.write(packet3(PACKET3_SET_BASE, 2))?;
        ring.write(PACKET3_BASE_INDEX_CE_PARTITION)?;
        ring.write(0x8000)?;
        ring.write(0x8000)?;
        ring.commit(dev)?;

        // With two gfx rings Linux submits CLEAR_STATE on the second ring so
        // it copies state 0 into its next available state as well.
        if self.gfx_rings.len() > 1 {
            let ring = &mut self.gfx_rings[1];
            ring.write(packet3(PACKET3_CLEAR_STATE, 0))?;
            ring.write(0)?;
            ring.commit(dev)?;
        }
        Ok(())
    }

    /// Linux gfx_v10_0_cp_gfx_resume.
    fn cp_gfx_resume(&mut self, dev: &mut Adapter) -> Result<()> {
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            gc::mmCP_RB_WPTR_DELAY,
            ridx!(gc::mmCP_RB_WPTR_DELAY),
            0,
        )?;
        dev.regs
            .write_ip(HwIp::Gc, 0, gc::mmCP_RB_VMID, ridx!(gc::mmCP_RB_VMID), 0)?;

        for pipe in 0..self.gfx_rings.len() {
            self.switch_pipe(dev, pipe as u32)?;
            let (gpu_addr, rptr_wb, wptr_wb, doorbell_index, ring_size) = {
                let ring = &self.gfx_rings[pipe];
                (
                    ring.gpu_addr,
                    ring.rptr_wb,
                    ring.wptr_wb,
                    ring.doorbell,
                    ring.size,
                )
            };

            let bufsz = (ring_size / 8).trailing_zeros() as u64;
            let (cntl, base, base_hi, wptr, wptr_hi, rptr_addr, rptr_addr_hi, active) = if pipe == 0
            {
                (
                    gc::mmCP_RB0_CNTL,
                    gc::mmCP_RB0_BASE,
                    gc::mmCP_RB0_BASE_HI,
                    gc::mmCP_RB0_WPTR,
                    gc::mmCP_RB0_WPTR_HI,
                    gc::mmCP_RB0_RPTR_ADDR,
                    gc::mmCP_RB0_RPTR_ADDR_HI,
                    gc::mmCP_RB_ACTIVE,
                )
            } else {
                (
                    gc::mmCP_RB1_CNTL,
                    gc::mmCP_RB1_BASE,
                    gc::mmCP_RB1_BASE_HI,
                    gc::mmCP_RB1_WPTR,
                    gc::mmCP_RB1_WPTR_HI,
                    gc::mmCP_RB1_RPTR_ADDR,
                    gc::mmCP_RB1_RPTR_ADDR_HI,
                    gc::mmCP_RB1_ACTIVE,
                )
            };

            let mut value = set_field(
                0,
                gc::CP_RB0_CNTL__RB_BUFSZ__SHIFT,
                gc::CP_RB0_CNTL__RB_BUFSZ_MASK,
                bufsz,
            );
            value = set_field(
                value,
                gc::CP_RB0_CNTL__RB_BLKSZ__SHIFT,
                gc::CP_RB0_CNTL__RB_BLKSZ_MASK,
                bufsz - 2,
            );
            dev.regs.write_ip(HwIp::Gc, 0, cntl, 0, value)?;
            dev.regs.write_ip(HwIp::Gc, 0, wptr, 0, 0)?;
            dev.regs.write_ip(HwIp::Gc, 0, wptr_hi, 0, 0)?;
            dev.regs
                .write_ip(HwIp::Gc, 0, rptr_addr, 0, rptr_wb as u32)?;
            dev.regs
                .write_ip(HwIp::Gc, 0, rptr_addr_hi, 0, (rptr_wb >> 32) as u32)?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_RB_WPTR_POLL_ADDR_LO,
                ridx!(gc::mmCP_RB_WPTR_POLL_ADDR_LO),
                wptr_wb as u32,
            )?;
            dev.regs.write_ip(
                HwIp::Gc,
                0,
                gc::mmCP_RB_WPTR_POLL_ADDR_HI,
                ridx!(gc::mmCP_RB_WPTR_POLL_ADDR_HI),
                (wptr_wb >> 32) as u32,
            )?;
            time::delay(Duration::from_millis(1));
            dev.regs.write_ip(HwIp::Gc, 0, cntl, 0, value)?;
            let rb_addr = gpu_addr >> 8;
            dev.regs.write_ip(HwIp::Gc, 0, base, 0, rb_addr as u32)?;
            dev.regs
                .write_ip(HwIp::Gc, 0, base_hi, 0, (rb_addr >> 32) as u32)?;
            dev.regs.write_ip(HwIp::Gc, 0, active, 0, 1)?;
            self.cp_gfx_set_doorbell(dev, doorbell_index)?;
        }
        self.switch_pipe(dev, 0)?;
        self.cp_gfx_start(dev)?;
        Ok(())
    }

    /// Linux gfx10_kiq_set_resources + gfx10_kiq_map_queues +
    /// amdgpu_gfx_enable_kcq: bring up the compute queues over the KIQ.
    fn enable_kcq(&mut self, dev: &mut Adapter) -> Result<()> {
        // cp_compute_enable(true)
        dev.regs.write_ip(
            HwIp::Gc,
            0,
            local_regs::CP_MEC_CNTL_SIENNA_CICHLID,
            ridx!(local_regs::CP_MEC_CNTL_SIENNA_CICHLID),
            0,
        )?;
        time::delay(Duration::from_micros(50));

        // Fill every compute MQD before mapping (kcq_init_queue).
        let eop_base = self.hpd_eop.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        for i in 0..NUM_COMPUTE_RINGS {
            let ring = &self.compute_rings[i];
            let (me, pipe, queue) = (ring.me, ring.pipe, ring.queue);
            self.grbm_select(dev, me, pipe, queue, 0)?;
            self.fill_mqd(
                dev,
                i + 1,
                false,
                eop_base + ((i as u64 + 1) * MEC_HPD_SIZE as u64),
            )?;
            self.grbm_select(dev, 0, 0, 0, 0)?;
        }

        let cleaner = self.cleaner.as_ref().ok_or(Error::NoDevice)?.gpu_addr;

        // Build the queue mask: one bit per compute queue.
        let mut queue_mask: u64 = 0;
        for ring in &self.compute_rings {
            queue_mask |= 1u64 << ((ring.me - 1) * 32 + ring.pipe * 8 + ring.queue);
        }

        // Linux amdgpu_gfx_enable_kcq flushes CPU writes to the MQDs before
        // the KIQ is asked to consume them with MAP_QUEUES.
        Self::flush_hdp(dev)?;

        {
            let kiq = self.kiq.as_mut().ok_or(Error::NoDevice)?;
            let shader_mc_addr = cleaner >> 8;
            kiq.write(packet3(PACKET3_SET_RESOURCES, 6))?;
            kiq.write(0)?; // vmid_mask 0, queue_type 0 (KIQ)
            kiq.write(queue_mask as u32)?;
            kiq.write((queue_mask >> 32) as u32)?;
            kiq.write(shader_mc_addr as u32)?;
            kiq.write((shader_mc_addr >> 32) as u32)?;
            kiq.write(0)?; // oac mask
            kiq.write(0)?; // gds heap
        }

        // One MAP_QUEUES packet per compute ring.
        for ring in &self.compute_rings {
            let mqd_addr = ring.mqd.as_ref().map(|m| m.gpu_addr).unwrap_or(0);
            let wptr_addr = ring.wptr_wb;
            let (queue, pipe, me, doorbell_index) = (ring.queue, ring.pipe, ring.me, ring.doorbell);
            {
                let kiq = self.kiq.as_mut().ok_or(Error::NoDevice)?;
                kiq.write(packet3(PACKET3_MAP_QUEUES, 5))?;
                kiq.write(map_queues_dw1(queue, pipe, if me == 1 { 0 } else { 1 }))?;
                kiq.write(map_queues_doorbell_offset(doorbell_index))?;
                kiq.write(mqd_addr as u32)?;
                kiq.write((mqd_addr >> 32) as u32)?;
                kiq.write(wptr_addr as u32)?;
                kiq.write((wptr_addr >> 32) as u32)?;
            }
        }
        if let Some(kiq) = self.kiq.as_mut() {
            kiq.commit(dev)?;
        }

        // Linux amdgpu_gfx_enable_kcq immediately tests the KIQ so that all
        // preceding SET_RESOURCES/MAP_QUEUES packets have completed.
        // Linux SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0): SCRATCH_REG0 lives
        // in GC segment 1 on GFX10.3, not the main segment 0 used by most CP
        // registers.  Using base[0] produces an invalid SET_UCONFIG_REG
        // offset even though the KIQ still consumes the packet.
        let scratch =
            dev.regs.base_u32(HwIp::Gc, 0, ridx!(gc::mmSCRATCH_REG0))? + gc::mmSCRATCH_REG0;
        dev_info!("astra: testing KIQ after KCQ map");
        self.kiq
            .as_mut()
            .ok_or(Error::NoDevice)?
            .scratch_test(dev, scratch, 1_000_000)?;
        dev_info!("astra: KIQ set-resources/map-queues completed");
        Ok(())
    }

    /// Linux amdgpu_device_flush_hdp(NULL) through the native NBIO window.
    fn flush_hdp(dev: &mut Adapter) -> Result<()> {
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            nbio::regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL,
            ridx!(nbio::regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL),
            0,
        )?;
        let _ = dev.regs.read_ip(
            HwIp::Nbio,
            0,
            nbio::regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE,
            ridx!(nbio::regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE),
        )?;
        Ok(())
    }

    /// The gfx10 clear-state extents (clearstate_gfx10.h:961). The register
    /// count is derived from the generated array length, never hardcoded.
    fn csb_extents() -> [(&'static [u32], u32); 8] {
        [
            (&clearstate::gfx10_SECT_CONTEXT_def_1, 0x0000_a000),
            (&clearstate::gfx10_SECT_CONTEXT_def_2, 0x0000_a0d8),
            (&clearstate::gfx10_SECT_CONTEXT_def_3, 0x0000_a1f5),
            (&clearstate::gfx10_SECT_CONTEXT_def_4, 0x0000_a1ff),
            (&clearstate::gfx10_SECT_CONTEXT_def_5, 0x0000_a2a0),
            (&clearstate::gfx10_SECT_CONTEXT_def_6, 0x0000_a2a3),
            (&clearstate::gfx10_SECT_CONTEXT_def_7, 0x0000_a2a5),
            (&clearstate::gfx10_SECT_CONTEXT_def_8, 0x0000_a2f5),
        ]
    }

    /// Clear-state buffer size in dwords (gfx_v10_0_get_csb_size).
    fn csb_size_dwords() -> usize {
        2 + 3
            + Self::csb_extents()
                .iter()
                .map(|(extent, _)| 2 + extent.len())
                .sum::<usize>()
            + 3
            + 2
            + 2
    }

    /// Builds the clear-state buffer contents (Linux
    /// amdgpu_gfx_csb_preamble_start/data_parser/end + clear state).
    fn build_csb(tile_reg: u32, tile_steering: u32) -> Result<Vec<u32>> {
        let mut buffer = alloc::vec![
            packet3(PACKET3_PREAMBLE_CNTL, 0),
            PACKET3_PREAMBLE_BEGIN_CLEAR_STATE,
            packet3(PACKET3_CONTEXT_CONTROL, 1),
            0x8000_0000,
            0x8000_0000,
        ];
        for (extent, reg_index) in Self::csb_extents() {
            buffer.push(packet3(PACKET3_SET_CONTEXT_REG, extent.len() as u32));
            buffer.push(reg_index - PACKET3_SET_CONTEXT_REG_START);
            buffer.extend_from_slice(extent);
        }
        buffer.push(packet3(PACKET3_SET_CONTEXT_REG, 1));
        buffer.push(tile_reg - PACKET3_SET_CONTEXT_REG_START);
        buffer.push(tile_steering);
        buffer.push(packet3(PACKET3_PREAMBLE_CNTL, 0));
        buffer.push(PACKET3_PREAMBLE_END_CLEAR_STATE);
        buffer.push(packet3(PACKET3_CLEAR_STATE, 0));
        buffer.push(0);
        Ok(buffer)
    }
}

impl IpBlock for GfxV10 {
    fn hw_ip(&self) -> HwIp {
        HwIp::Gc
    }

    fn name(&self) -> &'static str {
        "GFX 10.3"
    }

    /// Linux gfx_v10_0_sw_init essentials: gfx config, RLC buffers,
    /// rings, MQD buffers, EOP and the cleaner shader.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // Chip configuration from the discovery GC table
        // (amdgpu_discovery_get_gfx_info).
        self.gb_addr_config = dev.regs.read_ip(HwIp::Gc, 0, gc::mmGB_ADDR_CONFIG, 0)?;
        dev.gfx_info.gb_addr_config = self.gb_addr_config;
        self.max_shader_engines = dev.gfx_info.max_shader_engines.max(1);
        self.max_sh_per_se = dev.gfx_info.max_sh_per_se.max(1);
        self.max_backends_per_se = dev.gfx_info.max_backends_per_se.max(self.max_sh_per_se);
        self.num_pipe_per_me = 2;
        self.max_hw_contexts = 8;
        self.pa_sc_tile_steering_override = 0;
        dev.gfx_info.max_hw_contexts = self.max_hw_contexts;
        dev.gfx_info.pa_sc_tile_steering_override = self.pa_sc_tile_steering_override;

        dev_info!(
            "astra: gfx config: SE {}, SH/SE {}, RB/SE {}, GB_ADDR_CONFIG {:#x}",
            self.max_shader_engines,
            self.max_sh_per_se,
            self.max_backends_per_se,
            self.gb_addr_config,
        );

        // RLC clear-state buffer + cleaner shader.
        self.csb_dwords = Self::csb_size_dwords();
        let csb = dev.mem.alloc_gart(&mut dev.regs, self.csb_dwords * 4)?;
        let tile_reg = dev
            .regs
            .base_u32(HwIp::Gc, 0, ridx!(gc::mmPA_SC_TILE_STEERING_OVERRIDE))?
            + gc::mmPA_SC_TILE_STEERING_OVERRIDE;
        let buffer = Self::build_csb(tile_reg, self.pa_sc_tile_steering_override)?;
        if buffer.len() != self.csb_dwords {
            return Err(Error::Range);
        }
        {
            let mut csb_mut = csb;
            let cpu = csb_mut.cpu.as_mut().ok_or(Error::NoDevice)?;
            for (i, value) in buffer.iter().enumerate() {
                let at = i * 4;
                let dst = cpu.as_mut_slice().get_mut(at..at + 4).ok_or(Error::Range)?;
                dst.copy_from_slice(&value.to_le_bytes());
            }
            cpu.sync_for_device();
            self.csb = Some(csb_mut);
        }

        let cleaner_size = cleaner_shader::gfx_10_3_0_cleaner_shader_hex.len() * 4;
        let cleaner = dev.mem.alloc_gart(&mut dev.regs, cleaner_size)?;
        {
            let mut cleaner_mut = cleaner;
            let cpu = cleaner_mut.cpu.as_mut().ok_or(Error::NoDevice)?;
            for (i, value) in cleaner_shader::gfx_10_3_0_cleaner_shader_hex
                .iter()
                .enumerate()
            {
                let at = i * 4;
                let dst = cpu.as_mut_slice().get_mut(at..at + 4).ok_or(Error::Range)?;
                dst.copy_from_slice(&value.to_le_bytes());
            }
            cpu.sync_for_device();
            self.cleaner = Some(cleaner_mut);
        }

        // Linux Navi23 layout: KIQ on MEC2 pipe1 queue0, plus two gfx
        // rings and eight compute rings spread over MEC1 pipe0..3/q0..1.
        for i in 0..2u32 {
            let bo = dev.mem.alloc_gart(&mut dev.regs, RING_SIZE)?;
            let rptr_wb = self.wb_slot(dev)?;
            let wptr_wb = self.wb_slot(dev)?;
            let assigned = if i == 0 {
                doorbell::DOORBELL_GFX_RING0
            } else {
                doorbell::DOORBELL_GFX_RING1
            };
            let mut ring = Ring::new(
                bo,
                RingConfig {
                    doorbell: doorbell::ring_doorbell(assigned),
                    rptr_wb,
                    wptr_wb,
                    me: 0,
                    pipe: i,
                    queue: 0,
                    kind: RingKind::Gfx,
                },
            );
            ring.vm_inv_eng = i;
            ring.fence_wb = self.wb_slot(dev)?;
            self.gfx_rings.push(ring);
        }
        {
            let bo = dev.mem.alloc_gart(&mut dev.regs, RING_SIZE)?;
            let rptr_wb = self.wb_slot(dev)?;
            let wptr_wb = self.wb_slot(dev)?;
            let mut kiq = Ring::new(
                bo,
                RingConfig {
                    doorbell: doorbell::ring_doorbell(doorbell::DOORBELL_KIQ),
                    rptr_wb,
                    wptr_wb,
                    me: 2,
                    pipe: 1,
                    queue: 0,
                    kind: RingKind::Gfx,
                },
            );
            // GFX and compute rings consume engines 0, 1, 4..11 in Linux's
            // registration order; KIQ follows them on engine 12.
            kiq.vm_inv_eng = 12;
            kiq.fence_wb = self.wb_slot(dev)?;
            kiq.mqd = Some(dev.mem.alloc_gart(&mut dev.regs, 4096)?);
            self.kiq = Some(kiq);
        }
        for i in 0..NUM_COMPUTE_RINGS {
            let bo = dev.mem.alloc_gart(&mut dev.regs, RING_SIZE)?;
            let rptr_wb = self.wb_slot(dev)?;
            let wptr_wb = self.wb_slot(dev)?;
            let assigned = doorbell::DOORBELL_MEC_RING0 + i as u32;
            let mut ring = Ring::new(
                bo,
                RingConfig {
                    doorbell: doorbell::ring_doorbell(assigned),
                    rptr_wb,
                    wptr_wb,
                    me: 1,
                    pipe: i as u32 % 4,
                    queue: i as u32 / 4,
                    kind: RingKind::Gfx,
                },
            );
            ring.vm_inv_eng = 4 + i as u32;
            ring.fence_wb = self.wb_slot(dev)?;
            ring.mqd = Some(dev.mem.alloc_gart(&mut dev.regs, 4096)?);
            self.compute_rings.push(ring);
        }

        // EOP buffer: 2048 bytes per queue (KIQ + compute).
        self.hpd_eop = Some(
            dev.mem
                .alloc_gart(&mut dev.regs, (NUM_COMPUTE_RINGS + 1) * MEC_HPD_SIZE)?,
        );
        Ok(())
    }

    /// Linux gfx_v10_0_hw_init.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // Golden registers (golden_settings_gc_10_3_4).
        for (reg, base, and, or) in GOLDEN_GC_10_3_4 {
            dev.regs
                .rmw_ip(HwIp::Gc, 0, *reg, *base as usize, *and, *or)?;
        }
        for i in 0..16 {
            dev.regs.rmw_ip(
                HwIp::Gc,
                0,
                gc::mmSQ_PERFCOUNTER0_SELECT + i,
                0,
                0xf0f0_01ff,
                0,
            )?;
        }
        dev_info!("astra: gfx golden registers programmed");

        self.cam_remapping(dev)?;
        self.constants_init(dev)?;
        self.rlc_resume(dev)?;

        // cp_resume: KIQ, KCQ, gfx rings.
        dev_info!("astra: GFX stage: KIQ resume");
        self.kiq_resume(dev)?;
        if let Some(kiq) = self.kiq.as_ref() {
            dev_info!(
                "astra: KIQ initialized on MEC{} pipe{} queue{} doorbell {:#x}",
                kiq.me,
                kiq.pipe,
                kiq.queue,
                kiq.doorbell,
            );
        }
        dev_info!("astra: GFX stage: KCQ resume");
        self.enable_kcq(dev)?;
        dev_info!("astra: KCQ enabled");
        dev_info!("astra: GFX stage: CP gfx resume");
        self.cp_gfx_resume(dev)?;
        dev_info!("astra: CP gfx rings resumed");

        // Linux gfx_v10_0_cp_resume tests gfx rings first, then compute.
        let scratch =
            dev.regs.base_u32(HwIp::Gc, 0, ridx!(gc::mmSCRATCH_REG0))? + gc::mmSCRATCH_REG0;
        for (i, ring) in self.gfx_rings.iter_mut().enumerate() {
            dev_info!("astra: testing gfx_0.{}.0", i);
            ring.scratch_test(dev, scratch, 1_000_000)?;
            dev_info!("astra: ring test on gfx_0.{}.0 succeeded", i);
        }
        for ring in &mut self.compute_rings {
            dev_info!(
                "astra: testing comp_{}.{}.{}",
                ring.me,
                ring.pipe,
                ring.queue,
            );
            ring.scratch_test(dev, scratch, 1_000_000)?;
            dev_info!(
                "astra: ring test on comp_{}.{}.{} succeeded",
                ring.me,
                ring.pipe,
                ring.queue,
            );
        }
        Ok(())
    }

    fn submit_user_ibs(
        &mut self,
        dev: &mut Adapter,
        submission: UserSubmission<'_>,
    ) -> Result<crate::ip::CompletionFence> {
        if submission.ibs.is_empty()
            || submission.vmid == 0
            || submission.vmid > 15
            || submission.root_pde & 1 == 0
        {
            return Err(Error::InvalidArgument);
        }
        if submission
            .ibs
            .iter()
            .any(|ib| ib.flags & crate::uapi::AMDGPU_IB_FLAGS_SECURE != 0)
        {
            // Secure/TMZ submission requires gfx_v10 frame-control packets;
            // never execute it as a normal IB.
            return Err(Error::Unsupported);
        }
        let compute = match submission.ip_type {
            crate::uapi::AMDGPU_HW_IP_GFX => false,
            crate::uapi::AMDGPU_HW_IP_COMPUTE => true,
            _ => return Err(Error::Unsupported),
        };
        let rings = if compute {
            &mut self.compute_rings
        } else {
            &mut self.gfx_rings
        };
        let result = match rings.get_mut(submission.ring as usize) {
            Some(ring) => Self::submit_ring_ibs(ring, dev, compute, submission),
            None => Err(Error::InvalidArgument),
        };
        let (gpu_address, value) = result?;
        Ok(crate::ip::CompletionFence { gpu_address, value })
    }
}
