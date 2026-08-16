//! COMMON IP block: NBIO/asic-level init (Linux `nv.c` nv_common_*).

use na_std::Result;

use crate::device::Adapter;
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::regs::nbio4_3_0 as nbio;
use crate::regs::{get_field, set_field};

/// NBIO register base address indexes from the discovery table (the
/// NBIO block exposes several windows).
const NBIO_BASE_RSMU: usize = 1;
const NBIO_BASE_MAIN: usize = 2;
const NBIO_BASE_CFG: usize = 5;

/// HDP flush register aliased at the MMIO register hole for user space
/// (Linux `MMIO_REG_HOLE_OFFSET`).
const MMIO_REG_HOLE_OFFSET: u32 = 0x80000;

pub struct CommonBlock {
    _nbio_version: IpVersion,
    _hdp_version: IpVersion,
    _smuio_version: IpVersion,
    _thm_version: IpVersion,
    _df_version: IpVersion,
    pub rev_id: u32,
}

impl CommonBlock {
    pub fn new(
        nbio_version: IpVersion,
        hdp_version: IpVersion,
        smuio_version: IpVersion,
        thm_version: IpVersion,
        df_version: IpVersion,
    ) -> Self {
        Self {
            _nbio_version: nbio_version,
            _hdp_version: hdp_version,
            _smuio_version: smuio_version,
            _thm_version: thm_version,
            _df_version: df_version,
            rev_id: 0,
        }
    }
}

impl IpBlock for CommonBlock {
    fn hw_ip(&self) -> HwIp {
        HwIp::Common
    }

    fn name(&self) -> &'static str {
        "COMMON"
    }

    /// Linux `nv_common_early_init`: revision id, the RSMU indirect
    /// register window and the HDP flush remap.
    fn early_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // nbio_v4_3_get_rev_id
        let strap = dev.regs.read_ip(
            HwIp::Nbio,
            0,
            nbio::regRCC_STRAP0_RCC_DEV0_EPF0_STRAP0,
            NBIO_BASE_MAIN,
        )?;
        self.rev_id = get_field(
            strap,
            nbio::RCC_STRAP0_RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0__SHIFT,
            nbio::RCC_STRAP0_RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0_MASK,
        );

        // The RSMU index/data window serves all indirect register and
        // SMN access (amdgpu_reg_access.c indirect_rreg/wreg).
        dev.regs.set_rsmu_window(
            dev.regs
                .base_u32(HwIp::Nbio, 0, NBIO_BASE_RSMU as usize)?
                .wrapping_add(nbio::regBIF_BX_PF0_RSMU_INDEX),
            dev.regs
                .base_u32(HwIp::Nbio, 0, NBIO_BASE_RSMU as usize)?
                .wrapping_add(nbio::regBIF_BX_PF0_RSMU_DATA),
        );
        Ok(())
    }

    /// Linux `nv_common_hw_init`: STRAP soft-reset unlock, HDP flush
    /// remap and the doorbell aperture.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // nbio_v4_3_init_registers
        dev.regs.rmw_ip(
            HwIp::Nbio,
            0,
            nbio::regRCC_DEV0_EPF2_STRAP2,
            NBIO_BASE_CFG,
            !(nbio::RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK as u32),
            0,
        )?;

        // nbio_v4_3_remap_hdp_registers: alias the HDP flush registers at
        // the MMIO register hole (needed by future user-space clients).
        let hdp_mem_flush = MMIO_REG_HOLE_OFFSET + nbio::regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL * 4;
        let hdp_reg_flush = MMIO_REG_HOLE_OFFSET + nbio::regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL * 4;
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            nbio::regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL,
            NBIO_BASE_MAIN,
            hdp_mem_flush,
        )?;
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            nbio::regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL,
            NBIO_BASE_MAIN,
            hdp_reg_flush,
        )?;

        // nbio_v4_3_enable_doorbell_aperture
        dev.regs.rmw_ip(
            HwIp::Nbio,
            0,
            nbio::regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN,
            NBIO_BASE_MAIN,
            u32::MAX,
            set_field(
                0,
                nbio::RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN__SHIFT,
                nbio::RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN_MASK,
                1,
            ),
        )?;
        Ok(())
    }
}
