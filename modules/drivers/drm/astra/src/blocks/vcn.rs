//! VCN and JPEG IP blocks (Linux `vcn_v3_0.c` / `jpeg_v3_0.c`).

use alloc::vec::Vec;

use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;
use crate::doorbell;
use crate::firmware::UcodeId;
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::mem::Bo;
use crate::regs::nbio2_3 as nbio23;
use crate::regs::nbio4_3_0 as nbio;
use crate::regs::set_field;
use crate::regs::vcn3_0_0 as vcn;
use crate::ring::{Ring, RingConfig, RingKind};

/// VCN ring size in dwords (vcn_v3_0_sw_init).
const VCN_RING_DWORDS: usize = 512;
/// FW stack / context region sizes.
const VCN_STACK_SIZE: u32 = 128 << 10;
const VCN_CONTEXT_SIZE: u32 = 512 << 10;
/// NBIO main / S2A base indexes (see blocks/common.rs).
const NBIO_BASE_MAIN: usize = 2;
const NBIO_BASE_S2A: usize = 3;
/// VCN decode command packet start (vcn_v3_0.c).
const VCN_DEC_CMD_PACKET_START: u32 = 0x81FF_0000;
/// UVD_STATUS__UVD_BUSY (amdgpu_vcn.h enum; not in the generated masks).
const UVD_BUSY: u32 = 0x0000_0004;
const PACKET0_REGISTER_OFFSET: u32 = 0x2000;
/// VCN instance count from the discovery table.
const VCN_INSTANCES: usize = 1;

pub struct VcnV30 {
    _version: IpVersion,
    /// fw BO (fw + stack + context), GART.
    fw_bo: Option<Bo>,
    /// Firmware shared memory (GART).
    fw_shared: Option<Bo>,
    dec_ring: Option<Ring>,
    enc_rings: Vec<Ring>,
}

impl VcnV30 {
    pub fn new(version: IpVersion) -> Self {
        Self {
            _version: version,
            fw_bo: None,
            fw_shared: None,
            dec_ring: None,
            enc_rings: Vec::new(),
        }
    }

    /// Linux vcn_v3_0_disable_static_power_gating (bare-metal path).
    fn disable_static_power_gating(&self, dev: &mut Adapter, inst: usize) -> Result<()> {
        let _ = inst;
        // UVD_STATUS BUSY keeps the block out of PG.
        let value = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmUVD_STATUS, 0)?;
        let value = value | UVD_BUSY;
        dev.regs.write_ip(HwIp::Uvd, 0, vcn::mmUVD_STATUS, 0, value)
    }

    /// Linux vcn_v3_0_disable_clock_gating: clear DYN_CLOCK_MODE.
    fn disable_clock_gating(&self, dev: &mut Adapter) -> Result<()> {
        let value = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmUVD_CGC_CTRL, 0)?;
        let value = value & !(vcn::UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK as u32);
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_CGC_CTRL, 0, value)
    }

    /// Linux vcn_v3_0_mc_resume: cache windows over the PSP TMR address,
    /// the stack, the context and the shared memory.
    fn mc_resume(&mut self, dev: &mut Adapter, tmr_addr: u64) -> Result<()> {
        let fw_addr = self.fw_bo.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let shared_addr = self.fw_shared.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let size = Self::tmr_size(dev)?;

        // Window 0: firmware (PSP TMR).
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW,
            0,
            tmr_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
            0,
            (tmr_addr >> 32) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_VCPU_CACHE_OFFSET0, 0, 0)?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_VCPU_CACHE_SIZE0, 0, size)?;

        // Window 1: stack; window 2: context.
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW,
            0,
            fw_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH,
            0,
            (fw_addr >> 32) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_VCPU_CACHE_OFFSET1, 0, 0)?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_VCPU_CACHE_SIZE1, 0, VCN_STACK_SIZE)?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW,
            0,
            (fw_addr + VCN_STACK_SIZE as u64) as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH,
            0,
            ((fw_addr + VCN_STACK_SIZE as u64) >> 32) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_VCPU_CACHE_OFFSET2, 0, 0)?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_VCPU_CACHE_SIZE2,
            0,
            VCN_CONTEXT_SIZE,
        )?;

        // Non-cache window: firmware shared memory.
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_NC0_64BIT_BAR_LOW,
            0,
            shared_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_VCPU_NC0_64BIT_BAR_HIGH,
            0,
            (shared_addr >> 32) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_VCPU_NONCACHE_OFFSET0, 0, 0)?;
        Ok(())
    }

    /// Linux vcn_v3_0_start (bare-metal, non-DPG path).
    fn start(&mut self, dev: &mut Adapter, tmr_addr: u64) -> Result<()> {
        self.disable_static_power_gating(dev, 0)?;
        self.disable_clock_gating(dev)?;

        // Enable VCPU clock.
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_VCPU_CNTL,
            0,
            !(vcn::UVD_VCPU_CNTL__CLK_EN_MASK as u32),
            vcn::UVD_VCPU_CNTL__CLK_EN_MASK as u32,
        )?;
        // Disable master interrupt.
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_MASTINT_EN,
            0,
            !(vcn::UVD_MASTINT_EN__VCPU_EN_MASK as u32),
            0,
        )?;
        // Enable LMI MC and UMC channels.
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_CTRL2,
            0,
            !(vcn::UVD_LMI_CTRL2__STALL_ARB_UMC_MASK as u32),
            0,
        )?;
        // Clear the LMI soft resets.
        let value = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmUVD_SOFT_RESET, 0)?;
        let value = value
            & !(vcn::UVD_SOFT_RESET__LMI_SOFT_RESET_MASK as u32)
            & !(vcn::UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK as u32);
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_SOFT_RESET, 0, value)?;

        // LMI control.
        let value = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmUVD_LMI_CTRL, 0)?;
        let value = value
            | vcn::UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK as u32
            | vcn::UVD_LMI_CTRL__MASK_MC_URGENT_MASK as u32
            | vcn::UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK as u32
            | vcn::UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK as u32;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_LMI_CTRL, 0, value)?;

        // MPC setup.
        let value = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmUVD_MPC_CNTL, 0)?;
        let value = set_field(
            value,
            vcn::UVD_MPC_CNTL__REPLACEMENT_MODE__SHIFT,
            vcn::UVD_MPC_CNTL__REPLACEMENT_MODE_MASK,
            2,
        );
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_MPC_CNTL, 0, value)?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_MPC_SET_MUXA0,
            0,
            (1 << vcn::UVD_MPC_SET_MUXA0__VARA_1__SHIFT)
                | (2 << vcn::UVD_MPC_SET_MUXA0__VARA_2__SHIFT)
                | (3 << vcn::UVD_MPC_SET_MUXA0__VARA_3__SHIFT)
                | (4 << vcn::UVD_MPC_SET_MUXA0__VARA_4__SHIFT),
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_MPC_SET_MUXB0,
            0,
            (1 << vcn::UVD_MPC_SET_MUXB0__VARB_1__SHIFT)
                | (2 << vcn::UVD_MPC_SET_MUXB0__VARB_2__SHIFT)
                | (3 << vcn::UVD_MPC_SET_MUXB0__VARB_3__SHIFT)
                | (4 << vcn::UVD_MPC_SET_MUXB0__VARB_4__SHIFT),
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_MPC_SET_MUX,
            0,
            (0 << vcn::UVD_MPC_SET_MUX__SET_0__SHIFT)
                | (1 << vcn::UVD_MPC_SET_MUX__SET_1__SHIFT)
                | (2 << vcn::UVD_MPC_SET_MUX__SET_2__SHIFT),
        )?;

        self.mc_resume(dev, tmr_addr)?;

        // VCN global tiling registers.
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_GFX10_ADDR_CONFIG, 0, 0)?;

        // Unblock VCPU register access, release VCPU reset.
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_RB_ARB_CTRL,
            0,
            !(vcn::UVD_RB_ARB_CTRL__VCPU_DIS_MASK as u32),
            0,
        )?;
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_VCPU_CNTL,
            0,
            !(vcn::UVD_VCPU_CNTL__BLK_RST_MASK as u32),
            0,
        )?;

        // Wait for the VCPU to boot.
        let mut booted = false;
        for _ in 0..10 {
            for _ in 0..100 {
                let status = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmUVD_STATUS, 0)?;
                if status & 2 != 0 {
                    booted = true;
                    break;
                }
                na_std::time::delay(core::time::Duration::from_millis(10));
            }
            if booted {
                break;
            }
            dev_info!("astra: VCN decode not responding, retrying reset");
            dev.regs.rmw_ip(
                HwIp::Uvd,
                0,
                vcn::mmUVD_VCPU_CNTL,
                0,
                !(vcn::UVD_VCPU_CNTL__BLK_RST_MASK as u32),
                vcn::UVD_VCPU_CNTL__BLK_RST_MASK as u32,
            )?;
            na_std::time::delay(core::time::Duration::from_millis(10));
            dev.regs.rmw_ip(
                HwIp::Uvd,
                0,
                vcn::mmUVD_VCPU_CNTL,
                0,
                !(vcn::UVD_VCPU_CNTL__BLK_RST_MASK as u32),
                0,
            )?;
            na_std::time::delay(core::time::Duration::from_millis(10));
        }
        if !booted {
            return Err(Error::Io);
        }

        // Enable the master interrupt, clear busy.
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_MASTINT_EN,
            0,
            !(vcn::UVD_MASTINT_EN__VCPU_EN_MASK as u32),
            vcn::UVD_MASTINT_EN__VCPU_EN_MASK as u32,
        )?;
        dev.regs.rmw_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_STATUS,
            0,
            !(2 << vcn::UVD_STATUS__VCPU_REPORT__SHIFT),
            0,
        )?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_LMI_RBC_RB_VMID, 0, 0)?;

        // Decode ring.
        let ring = self.dec_ring.as_ref().ok_or(Error::NoDevice)?;
        let gpu_addr = ring.gpu_addr;
        let rb_bufsz = (VCN_RING_DWORDS as u32).trailing_zeros() as u64;
        let tmp = set_field(
            0,
            vcn::UVD_RBC_RB_CNTL__RB_BUFSZ__SHIFT,
            vcn::UVD_RBC_RB_CNTL__RB_BUFSZ_MASK,
            rb_bufsz,
        ) | set_field(
            0,
            vcn::UVD_RBC_RB_CNTL__RB_BLKSZ__SHIFT,
            vcn::UVD_RBC_RB_CNTL__RB_BLKSZ_MASK,
            1,
        ) | set_field(
            0,
            vcn::UVD_RBC_RB_CNTL__RB_NO_FETCH__SHIFT,
            vcn::UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK,
            1,
        ) | set_field(
            0,
            vcn::UVD_RBC_RB_CNTL__RB_NO_UPDATE__SHIFT,
            vcn::UVD_RBC_RB_CNTL__RB_NO_UPDATE_MASK,
            1,
        ) | set_field(
            0,
            vcn::UVD_RBC_RB_CNTL__RB_RPTR_WR_EN__SHIFT,
            vcn::UVD_RBC_RB_CNTL__RB_RPTR_WR_EN_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_RBC_RB_CNTL, 0, tmp)?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_RBC_RB_64BIT_BAR_LOW,
            0,
            gpu_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH,
            0,
            (gpu_addr >> 32) as u32,
        )?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_RBC_RB_RPTR, 0, 0)?;
        dev.regs.write_ip(HwIp::Uvd, 0, vcn::mmUVD_SCRATCH2, 0, 0)?;
        Ok(())
    }

    /// VCN decode ring test: PACKET0 write to mmUVD_CONTEXT_ID.
    fn dec_ring_test(&mut self, dev: &mut Adapter) -> Result<()> {
        let context_id = vcn::mmUVD_CONTEXT_ID;
        let ring = self.dec_ring.as_mut().ok_or(Error::NoDevice)?;
        ring.reset();
        ring.write(VCN_DEC_CMD_PACKET_START)?;
        ring.write((context_id - PACKET0_REGISTER_OFFSET) & 0xffff)?;
        ring.write(0xDEAD_BEEF)?;
        ring.commit(dev)?;
        for _ in 0..1_000_000 {
            if dev.regs.read_ip(HwIp::Uvd, 0, context_id, 0)? == 0xDEAD_BEEF {
                return Ok(());
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        Err(Error::Io)
    }

    fn tmr_size(dev: &Adapter) -> Result<u32> {
        let firmware = dev.firmware(UcodeId::Vcn).ok_or(Error::NoDevice)?;
        Ok((firmware.size as u32).next_multiple_of(4096) + 4)
    }
}

impl IpBlock for VcnV30 {
    fn hw_ip(&self) -> HwIp {
        HwIp::Uvd
    }

    fn name(&self) -> &'static str {
        "VCN 3.0"
    }

    /// Linux vcn_v3_0_sw_init: fw/stack/context BO, shared BO, rings.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let fw_size = dev
            .firmware(UcodeId::Vcn)
            .map(|firmware| firmware.size)
            .unwrap_or(0);
        let total = fw_size + VCN_STACK_SIZE as usize + VCN_CONTEXT_SIZE as usize;
        self.fw_bo = Some(dev.mem.alloc_gart(&mut dev.regs, total)?);
        self.fw_shared = Some(dev.mem.alloc_gart(&mut dev.regs, 4096)?);

        for _ in 0..VCN_INSTANCES {
            let bo = dev.mem.alloc_gart(&mut dev.regs, VCN_RING_DWORDS * 4)?;
            let rptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
            let wptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
            self.dec_ring = Some(Ring::new(
                bo,
                RingConfig {
                    doorbell: doorbell::ring_doorbell(doorbell::DOORBELL_VCN_0_1),
                    rptr_wb,
                    wptr_wb,
                    me: 0,
                    pipe: 0,
                    queue: 0,
                    kind: RingKind::Vcn { align_mask: 0x3f },
                },
            ));
            for enc in 0..2 {
                let bo = dev.mem.alloc_gart(&mut dev.regs, VCN_RING_DWORDS * 4)?;
                let rptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
                let wptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
                self.enc_rings.push(Ring::new(
                    bo,
                    RingConfig {
                        doorbell: doorbell::ring_doorbell(doorbell::DOORBELL_VCN_0_1) + 2 + enc,
                        rptr_wb,
                        wptr_wb,
                        me: 0,
                        pipe: 0,
                        queue: 0,
                        kind: RingKind::Vcn { align_mask: 0x3f },
                    },
                ));
            }
        }
        Ok(())
    }

    /// Linux vcn_v3_0_hw_init: doorbell range, start, ring tests.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let doorbell_index = doorbell::ring_doorbell(doorbell::DOORBELL_VCN_0_1);
        if dev.uses_nbio_v2_3() {
            // Linux nbio_v2_3_vcn_doorbell_range, instance 0.
            let value = dev.regs.read_ip(
                HwIp::Nbio,
                0,
                nbio23::mmBIF_MMSCH0_DOORBELL_RANGE,
                NBIO_BASE_MAIN,
            )?;
            let value = set_field(
                value,
                nbio23::BIF_MMSCH0_DOORBELL_RANGE__OFFSET__SHIFT,
                nbio23::BIF_MMSCH0_DOORBELL_RANGE__OFFSET_MASK,
                doorbell_index as u64,
            );
            let value = set_field(
                value,
                nbio23::BIF_MMSCH0_DOORBELL_RANGE__SIZE__SHIFT,
                nbio23::BIF_MMSCH0_DOORBELL_RANGE__SIZE_MASK,
                8,
            );
            dev.regs.write_ip(
                HwIp::Nbio,
                0,
                nbio23::mmBIF_MMSCH0_DOORBELL_RANGE,
                NBIO_BASE_MAIN,
                value,
            )?;
        } else {
            // Linux nbio_v4_3_vcn_doorbell_range (S2A entry 4, instance 0).
            let value = dev.regs.read_ip(
                HwIp::Nbio,
                0,
                nbio::regS2A_DOORBELL_ENTRY_4_CTRL,
                NBIO_BASE_S2A,
            )?;
            let mut value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_ENABLE__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_ENABLE_MASK,
                1,
            );
            value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_OFFSET__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_OFFSET_MASK,
                doorbell_index as u64,
            );
            value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_SIZE__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_4_CTRL__S2A_DOORBELL_PORT4_RANGE_SIZE_MASK,
                8,
            );
            dev.regs.write_ip(
                HwIp::Nbio,
                0,
                nbio::regS2A_DOORBELL_ENTRY_4_CTRL,
                NBIO_BASE_S2A,
                value,
            )?;
        }

        // PSP TMR address of the VCN firmware (LOAD_IP_FW response).
        let tmr_addr = dev
            .firmware(UcodeId::Vcn)
            .and_then(|fw| fw.tmr_addr)
            .ok_or(Error::NoDevice)?;

        self.start(dev, tmr_addr)?;
        dev_info!("astra: VCN decode started");
        self.dec_ring_test(dev)?;
        dev_info!("astra: ring test on vcn_dec_0 succeeded");
        Ok(())
    }
}

/// JPEG IP block (Linux `jpeg_v3_0.c`) — minimal start + ring test.
pub struct JpegV30 {
    _version: IpVersion,
    ring: Option<Ring>,
}

impl JpegV30 {
    pub fn new(version: IpVersion) -> Self {
        Self {
            _version: version,
            ring: None,
        }
    }

    fn ring_test(&mut self, dev: &mut Adapter) -> Result<()> {
        let context_id = vcn::mmUVD_CONTEXT_ID;
        let ring = self.ring.as_mut().ok_or(Error::NoDevice)?;
        ring.reset();
        ring.write(VCN_DEC_CMD_PACKET_START)?;
        ring.write((context_id - PACKET0_REGISTER_OFFSET) & 0xffff)?;
        ring.write(0xDEAD_BEEF)?;
        ring.commit(dev)?;
        for _ in 0..1_000_000 {
            if dev.regs.read_ip(HwIp::Uvd, 0, context_id, 0)? == 0xDEAD_BEEF {
                return Ok(());
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        Err(Error::Io)
    }
}

impl IpBlock for JpegV30 {
    fn hw_ip(&self) -> HwIp {
        HwIp::Uvd
    }

    fn name(&self) -> &'static str {
        "JPEG 3.0"
    }

    /// Linux jpeg_v3_0_sw_init: decode ring.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let bo = dev.mem.alloc_gart(&mut dev.regs, VCN_RING_DWORDS * 4)?;
        let rptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
        let wptr_wb = dev.wb.as_mut().ok_or(Error::NoDevice)?.get()?;
        self.ring = Some(Ring::new(
            bo,
            RingConfig {
                doorbell: doorbell::ring_doorbell(doorbell::DOORBELL_VCN_0_1) + 1,
                rptr_wb,
                wptr_wb,
                me: 0,
                pipe: 0,
                queue: 0,
                kind: RingKind::Vcn { align_mask: 0x0f },
            },
        ));
        Ok(())
    }

    /// Linux jpeg_v3_0_hw_init: doorbell range + ring test.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // Disable power/clock gating and set the tiling config.
        // Static PG off (jpeg_v3_0_disable_static_power_gating).
        let pgfsm = set_field(
            0,
            vcn::UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG__SHIFT,
            vcn::UVD_PGFSM_CONFIG__UVDJ_PWR_CONFIG_MASK,
            2,
        );
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_PGFSM_CONFIG, 0, pgfsm)?;
        // CGC off (jpeg_v3_0_disable_clock_gating).
        let cgc = dev.regs.read_ip(HwIp::Uvd, 0, vcn::mmJPEG_CGC_CTRL, 0)?;
        let cgc = cgc & !(vcn::JPEG_CGC_CTRL__DYN_CLOCK_MODE_MASK as u32);
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmJPEG_CGC_CTRL, 0, cgc)?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmJPEG_DEC_GFX10_ADDR_CONFIG, 0, 0)?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmJPEG_ENC_GFX10_ADDR_CONFIG, 0, 0)?;

        let ring = self.ring.as_ref().ok_or(Error::NoDevice)?;
        let gpu_addr = ring.gpu_addr;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_JRBC_RB_64BIT_BAR_LOW,
            0,
            gpu_addr as u32,
        )?;
        dev.regs.write_ip(
            HwIp::Uvd,
            0,
            vcn::mmUVD_LMI_JRBC_RB_64BIT_BAR_HIGH,
            0,
            (gpu_addr >> 32) as u32,
        )?;
        let value = set_field(
            0,
            vcn::UVD_JRBC_RB_CNTL__RB_NO_FETCH__SHIFT,
            vcn::UVD_JRBC_RB_CNTL__RB_NO_FETCH_MASK,
            1,
        ) | set_field(
            0,
            vcn::UVD_JRBC_RB_CNTL__RB_RPTR_WR_EN__SHIFT,
            vcn::UVD_JRBC_RB_CNTL__RB_RPTR_WR_EN_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_JRBC_RB_CNTL, 0, value)?;
        dev.regs
            .write_ip(HwIp::Uvd, 0, vcn::mmUVD_JRBC_RB_RPTR, 0, 0)?;

        self.ring_test(dev)?;
        dev_info!("astra: ring test on jpeg_dec succeeded");
        Ok(())
    }
}
