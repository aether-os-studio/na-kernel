//! IH (interrupt handler) IP block (Linux `ih_v6_0.c`).

use na_std::pci::MsiIrq;
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;
use crate::doorbell;
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::irq::{IhConfig, IhHandler};
use crate::mem::Bo;
use crate::regs::nbio2_3 as nbio23;
use crate::regs::nbio4_3_0 as nbio;
use crate::regs::osssys6_0_0 as oss;
use crate::regs::set_field;

/// 256 KiB per IH ring (Linux `IH_RING_SIZE`).
const IH_RING_SIZE: usize = 256 << 10;
/// NBIO base indexes (see blocks/common.rs).
const NBIO_BASE_MAIN: usize = 2;
const NBIO_BASE_S2A: usize = 3;

/// Register set of one IH ring.
struct IhRingRegs {
    rb_base: u32,
    rb_base_hi: u32,
    rb_cntl: u32,
    rb_wptr: u32,
    rb_rptr: u32,
    doorbell_rptr: u32,
    wptr_addr_lo: u32,
    wptr_addr_hi: u32,
}

pub struct IhV6 {
    _version: IpVersion,
    ih: IhRingRegs,
    ih1: IhRingRegs,
    ih_bo: Option<Bo>,
    ih1_bo: Option<Bo>,
    doorbell: u32,
    doorbell1: u32,
    msi_enabled: bool,
}

impl IhV6 {
    pub fn new(version: IpVersion) -> Self {
        Self {
            _version: version,
            ih: IhRingRegs {
                rb_base: 0,
                rb_base_hi: 0,
                rb_cntl: 0,
                rb_wptr: 0,
                rb_rptr: 0,
                doorbell_rptr: 0,
                wptr_addr_lo: 0,
                wptr_addr_hi: 0,
            },
            ih1: IhRingRegs {
                rb_base: 0,
                rb_base_hi: 0,
                rb_cntl: 0,
                rb_wptr: 0,
                rb_rptr: 0,
                doorbell_rptr: 0,
                wptr_addr_lo: 0,
                wptr_addr_hi: 0,
            },
            ih_bo: None,
            ih1_bo: None,
            doorbell: doorbell::ring_doorbell(doorbell::DOORBELL_IH),
            doorbell1: doorbell::ring_doorbell(doorbell::DOORBELL_IH + 1),
            msi_enabled: false,
        }
    }

    /// Linux ih_v6_0_init_register_offset.
    fn init_register_offset(&mut self) {
        // These remain IP-relative because all init-time accesses go through
        // `Regs::read_ip`/`write_ip`, which add the OSSSYS discovery base.
        self.ih.rb_base = oss::regIH_RB_BASE;
        self.ih.rb_base_hi = oss::regIH_RB_BASE_HI;
        self.ih.rb_cntl = oss::regIH_RB_CNTL;
        self.ih.rb_wptr = oss::regIH_RB_WPTR;
        self.ih.rb_rptr = oss::regIH_RB_RPTR;
        self.ih.doorbell_rptr = oss::regIH_DOORBELL_RPTR;
        self.ih.wptr_addr_lo = oss::regIH_RB_WPTR_ADDR_LO;
        self.ih.wptr_addr_hi = oss::regIH_RB_WPTR_ADDR_HI;
        self.ih1.rb_base = oss::regIH_RB_BASE_RING1;
        self.ih1.rb_base_hi = oss::regIH_RB_BASE_HI_RING1;
        self.ih1.rb_cntl = oss::regIH_RB_CNTL_RING1;
        self.ih1.rb_wptr = oss::regIH_RB_WPTR_RING1;
        self.ih1.rb_rptr = oss::regIH_RB_RPTR_RING1;
        self.ih1.doorbell_rptr = oss::regIH_DOORBELL_RPTR_RING1;
    }

    /// Linux ih_v6_0_enable_ring.
    fn enable_ring(&mut self, dev: &mut Adapter, ring1: bool) -> Result<()> {
        let (regs, bo, is_ring0) = if ring1 {
            (
                &self.ih1,
                self.ih1_bo.as_ref().ok_or(Error::NoDevice)?,
                false,
            )
        } else {
            (&self.ih, self.ih_bo.as_ref().ok_or(Error::NoDevice)?, true)
        };
        let gpu_addr = bo.gpu_addr;

        dev.regs
            .write_ip(HwIp::OssSys, 0, regs.rb_base, 0, (gpu_addr >> 8) as u32)?;
        dev.regs.write_ip(
            HwIp::OssSys,
            0,
            regs.rb_base_hi,
            0,
            (gpu_addr >> 40) as u32 & 0xff,
        )?;

        let value = dev.regs.read_ip(HwIp::OssSys, 0, regs.rb_cntl, 0)?;
        let mut value = set_field(
            value,
            oss::IH_RB_CNTL__MC_SPACE__SHIFT,
            oss::IH_RB_CNTL__MC_SPACE_MASK,
            4,
        );
        value = set_field(
            value,
            oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT,
            oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK,
            1,
        );
        value = set_field(
            value,
            oss::IH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT,
            oss::IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK,
            1,
        );
        // RB_SIZE = log2(ring size in dwords) = log2(256K/4) = 16
        value = set_field(
            value,
            oss::IH_RB_CNTL__RB_SIZE__SHIFT,
            oss::IH_RB_CNTL__RB_SIZE_MASK,
            16,
        );
        value = set_field(
            value,
            oss::IH_RB_CNTL__WPTR_WRITEBACK_ENABLE__SHIFT,
            oss::IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK,
            1,
        );
        value = set_field(
            value,
            oss::IH_RB_CNTL__MC_SNOOP__SHIFT,
            oss::IH_RB_CNTL__MC_SNOOP_MASK,
            1,
        );
        value = set_field(
            value,
            oss::IH_RB_CNTL__MC_RO__SHIFT,
            oss::IH_RB_CNTL__MC_RO_MASK,
            0,
        );
        value = set_field(
            value,
            oss::IH_RB_CNTL__MC_VMID__SHIFT,
            oss::IH_RB_CNTL__MC_VMID_MASK,
            0,
        );
        if is_ring0 {
            value = set_field(
                value,
                oss::IH_RB_CNTL__RPTR_REARM__SHIFT,
                oss::IH_RB_CNTL__RPTR_REARM_MASK,
                self.msi_enabled as u64,
            );
        } else {
            value = set_field(
                value,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK,
                0,
            );
            value = set_field(
                value,
                oss::IH_RB_CNTL__RB_FULL_DRAIN_ENABLE__SHIFT,
                oss::IH_RB_CNTL__RB_FULL_DRAIN_ENABLE_MASK,
                1,
            );
        }
        dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_cntl, 0, value)?;

        if is_ring0 {
            let wb_addr = self.handler_wb_gpu_addr(dev);
            dev.regs
                .write_ip(HwIp::OssSys, 0, regs.wptr_addr_lo, 0, wb_addr as u32)?;
            dev.regs.write_ip(
                HwIp::OssSys,
                0,
                regs.wptr_addr_hi,
                0,
                (wb_addr >> 32) as u32 & 0xffff,
            )?;
        }

        dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_wptr, 0, 0)?;
        dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_rptr, 0, 0)?;

        let doorbell_index = if is_ring0 {
            self.doorbell
        } else {
            self.doorbell1
        };
        let doorbell_rptr = set_field(
            0,
            oss::IH_DOORBELL_RPTR__OFFSET__SHIFT,
            oss::IH_DOORBELL_RPTR__OFFSET_MASK,
            doorbell_index as u64,
        ) | set_field(
            0,
            oss::IH_DOORBELL_RPTR__ENABLE__SHIFT,
            oss::IH_DOORBELL_RPTR__ENABLE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, regs.doorbell_rptr, 0, doorbell_rptr)?;
        Ok(())
    }

    fn handler_wb_gpu_addr(&self, dev: &Adapter) -> u64 {
        dev.msi
            .as_ref()
            .map(|irq| irq.callback().wb_gpu_addr())
            .unwrap_or(0)
    }

    /// Linux ih_v6_0_toggle_ring_interrupts.
    fn toggle_ring(&mut self, dev: &mut Adapter, ring1: bool, enable: bool) -> Result<()> {
        let regs = if ring1 { &self.ih1 } else { &self.ih };
        let value = dev.regs.read_ip(HwIp::OssSys, 0, regs.rb_cntl, 0)?;
        let mut value = set_field(
            value,
            oss::IH_RB_CNTL__RB_ENABLE__SHIFT,
            oss::IH_RB_CNTL__RB_ENABLE_MASK,
            enable as u64,
        );
        if enable {
            // Clear the overflow bit with a 0 -> 1 -> 0 pulse.
            value = set_field(
                value,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK,
                0,
            );
            dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_cntl, 0, value)?;
            value = set_field(
                value,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK,
                1,
            );
            dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_cntl, 0, value)?;
            value = set_field(
                value,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT,
                oss::IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK,
                0,
            );
        }
        if !ring1 {
            value = set_field(
                value,
                oss::IH_RB_CNTL__ENABLE_INTR__SHIFT,
                oss::IH_RB_CNTL__ENABLE_INTR_MASK,
                enable as u64,
            );
        }
        dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_cntl, 0, value)?;

        if !enable {
            dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_rptr, 0, 0)?;
            dev.regs.write_ip(HwIp::OssSys, 0, regs.rb_wptr, 0, 0)?;
        }
        Ok(())
    }

    /// Linux ih_v6_0_irq_init.
    fn irq_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.toggle_ring(dev, false, false)?;
        self.toggle_ring(dev, true, false)?;

        // nbio_v4_3_ih_control
        let dummy = dev.gmc.dummy_page_addr;
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            nbio::regBIF_BX0_INTERRUPT_CNTL2,
            NBIO_BASE_MAIN,
            (dummy >> 8) as u32,
        )?;
        dev.regs.rmw_ip(
            HwIp::Nbio,
            0,
            nbio::regBIF_BX0_INTERRUPT_CNTL,
            NBIO_BASE_MAIN,
            u32::MAX,
            set_field(
                0,
                nbio::BIF_BX0_INTERRUPT_CNTL__IH_DUMMY_RD_OVERRIDE__SHIFT,
                nbio::BIF_BX0_INTERRUPT_CNTL__IH_DUMMY_RD_OVERRIDE_MASK,
                0,
            ) | set_field(
                0,
                nbio::BIF_BX0_INTERRUPT_CNTL__IH_REQ_NONSNOOP_EN__SHIFT,
                nbio::BIF_BX0_INTERRUPT_CNTL__IH_REQ_NONSNOOP_EN_MASK,
                0,
            ),
        )?;

        self.enable_ring(dev, false)?;
        self.enable_ring(dev, true)?;

        if dev.uses_nbio_v2_3() {
            // Linux nbio_v2_3_ih_doorbell_range.
            let value = dev.regs.read_ip(
                HwIp::Nbio,
                0,
                nbio23::mmBIF_IH_DOORBELL_RANGE,
                NBIO_BASE_MAIN,
            )?;
            let value = set_field(
                value,
                nbio23::BIF_IH_DOORBELL_RANGE__OFFSET__SHIFT,
                nbio23::BIF_IH_DOORBELL_RANGE__OFFSET_MASK,
                self.doorbell as u64,
            );
            let value = set_field(
                value,
                nbio23::BIF_IH_DOORBELL_RANGE__SIZE__SHIFT,
                nbio23::BIF_IH_DOORBELL_RANGE__SIZE_MASK,
                2,
            );
            dev.regs.write_ip(
                HwIp::Nbio,
                0,
                nbio23::mmBIF_IH_DOORBELL_RANGE,
                NBIO_BASE_MAIN,
                value,
            )?;
        } else {
            // Linux nbio_v4_3_ih_doorbell_range (S2A entry 1).
            let value = dev.regs.read_ip(
                HwIp::Nbio,
                0,
                nbio::regS2A_DOORBELL_ENTRY_1_CTRL,
                NBIO_BASE_S2A,
            )?;
            let mut value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_ENABLE__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_ENABLE_MASK,
                1,
            );
            value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_AWID__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_AWID_MASK,
                0,
            );
            value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_OFFSET__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_OFFSET_MASK,
                self.doorbell as u64,
            );
            value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_SIZE__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_RANGE_SIZE_MASK,
                2,
            );
            value = set_field(
                value,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_AWADDR_31_28_VALUE__SHIFT,
                nbio::S2A_DOORBELL_ENTRY_1_CTRL__S2A_DOORBELL_PORT1_AWADDR_31_28_VALUE_MASK,
                0,
            );
            dev.regs.write_ip(
                HwIp::Nbio,
                0,
                nbio::regS2A_DOORBELL_ENTRY_1_CTRL,
                NBIO_BASE_S2A,
                value,
            )?;
        }

        // Storm / flood / MSI-storm controls.
        let value = dev
            .regs
            .read_ip(HwIp::OssSys, 0, oss::regIH_STORM_CLIENT_LIST_CNTL, 0)?;
        let value = set_field(
            value,
            oss::IH_STORM_CLIENT_LIST_CNTL__CLIENT18_IS_STORM_CLIENT__SHIFT,
            oss::IH_STORM_CLIENT_LIST_CNTL__CLIENT18_IS_STORM_CLIENT_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, oss::regIH_STORM_CLIENT_LIST_CNTL, 0, value)?;

        let value = dev
            .regs
            .read_ip(HwIp::OssSys, 0, oss::regIH_INT_FLOOD_CNTL, 0)?;
        let value = set_field(
            value,
            oss::IH_INT_FLOOD_CNTL__FLOOD_CNTL_ENABLE__SHIFT,
            oss::IH_INT_FLOOD_CNTL__FLOOD_CNTL_ENABLE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, oss::regIH_INT_FLOOD_CNTL, 0, value)?;

        let value = dev
            .regs
            .read_ip(HwIp::OssSys, 0, oss::regIH_MSI_STORM_CTRL, 0)?;
        let value = set_field(
            value,
            oss::IH_MSI_STORM_CTRL__DELAY__SHIFT,
            oss::IH_MSI_STORM_CTRL__DELAY_MASK,
            3,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, oss::regIH_MSI_STORM_CTRL, 0, value)?;

        // Redirect dGPU interrupts to IH ring 1.
        let value = dev
            .regs
            .read_ip(HwIp::OssSys, 0, oss::regIH_RING1_CLIENT_CFG_INDEX, 0)?;
        let value = set_field(
            value,
            oss::IH_RING1_CLIENT_CFG_INDEX__INDEX__SHIFT,
            oss::IH_RING1_CLIENT_CFG_INDEX__INDEX_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, oss::regIH_RING1_CLIENT_CFG_INDEX, 0, value)?;

        let value = dev
            .regs
            .read_ip(HwIp::OssSys, 0, oss::regIH_RING1_CLIENT_CFG_DATA, 0)?;
        let mut value = set_field(
            value,
            oss::IH_RING1_CLIENT_CFG_DATA__CLIENT_ID__SHIFT,
            oss::IH_RING1_CLIENT_CFG_DATA__CLIENT_ID_MASK,
            0xa,
        );
        value = set_field(
            value,
            oss::IH_RING1_CLIENT_CFG_DATA__SOURCE_ID__SHIFT,
            oss::IH_RING1_CLIENT_CFG_DATA__SOURCE_ID_MASK,
            0,
        );
        value = set_field(
            value,
            oss::IH_RING1_CLIENT_CFG_DATA__SOURCE_ID_MATCH_ENABLE__SHIFT,
            oss::IH_RING1_CLIENT_CFG_DATA__SOURCE_ID_MATCH_ENABLE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, oss::regIH_RING1_CLIENT_CFG_DATA, 0, value)?;

        self.toggle_ring(dev, false, true)?;
        self.toggle_ring(dev, true, true)?;

        // force_update_wptr_for_self_int(0, 8, true)
        let value = dev.regs.read_ip(HwIp::OssSys, 0, oss::regIH_CNTL2, 0)?;
        let mut value = set_field(
            value,
            oss::IH_CNTL2__SELF_IV_FORCE_WPTR_UPDATE_TIMEOUT__SHIFT,
            oss::IH_CNTL2__SELF_IV_FORCE_WPTR_UPDATE_TIMEOUT_MASK,
            8,
        );
        value = set_field(
            value,
            oss::IH_CNTL2__SELF_IV_FORCE_WPTR_UPDATE_ENABLE__SHIFT,
            oss::IH_CNTL2__SELF_IV_FORCE_WPTR_UPDATE_ENABLE_MASK,
            1,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, oss::regIH_CNTL2, 0, value)?;

        let value = dev.regs.read_ip(HwIp::OssSys, 0, self.ih1.rb_cntl, 0)?;
        let value = set_field(
            value,
            oss::IH_RB_CNTL_RING1__RB_USED_INT_THRESHOLD__SHIFT,
            oss::IH_RB_CNTL_RING1__RB_USED_INT_THRESHOLD_MASK,
            0,
        );
        dev.regs
            .write_ip(HwIp::OssSys, 0, self.ih1.rb_cntl, 0, value)?;

        dev_info!("astra: IH initialized (2 rings, MSI)");
        Ok(())
    }
}

impl IpBlock for IhV6 {
    fn hw_ip(&self) -> HwIp {
        HwIp::OssSys
    }

    fn name(&self) -> &'static str {
        "IH 6.0"
    }

    /// Linux ih_v6_0_sw_init: rings, register offsets, MSI setup.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.init_register_offset();

        // IH ring 0 + ring 1 (dGPU): 256 KiB each in GART.
        let ih_bo = dev.mem.alloc_gart(&mut dev.regs, IH_RING_SIZE)?;
        let ih1_bo = dev.mem.alloc_gart(&mut dev.regs, IH_RING_SIZE)?;

        // Dedicated writeback buffer + IRQ handler for the MSI callback.
        let wb_bo = dev.mem.alloc_gart(&mut dev.regs, 4096)?;
        let device = dev.pci.as_device();
        let bar5 = device.bar(5).and_then(Self::map_bar_range)?.map_mmio()?;
        let bar2 = device.bar(2).and_then(Self::map_bar_range)?.map_mmio()?;
        let oss_base = dev.regs.base_u32(HwIp::OssSys, 0, 0)?;
        let handler = IhHandler::new(IhConfig {
            bar5,
            bar2,
            wb: wb_bo,
            ih_rb_rptr: oss_base + self.ih.rb_rptr,
            ih_rb_wptr1: oss_base + self.ih1.rb_wptr,
            ih_rb_rptr1: oss_base + self.ih1.rb_rptr,
            doorbell: self.doorbell,
            doorbell1: self.doorbell1,
            ring_size_dw: (IH_RING_SIZE / 4) as u32,
        });
        let msi = MsiIrq::setup(&device, true, handler, c"astra")?;
        self.msi_enabled = true;
        dev.msi = Some(msi);

        self.ih_bo = Some(ih_bo);
        self.ih1_bo = Some(ih1_bo);
        Ok(())
    }

    /// Linux ih_v6_0_hw_init.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.irq_init(dev)
    }
}

impl IhV6 {
    fn map_bar_range(bar: na_std::pci::Bar) -> Result<na_std::memory::PhysicalRange> {
        match bar {
            na_std::pci::Bar::Memory { range, .. } => Ok(range),
            na_std::pci::Bar::Port { .. } => Err(Error::Unsupported),
        }
    }
}
