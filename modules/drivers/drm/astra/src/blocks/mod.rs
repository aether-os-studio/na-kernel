//! IP block construction from the discovery table — the single version
//! dispatch point, mirroring Linux `amdgpu_discovery_set_ip_blocks`.

use alloc::boxed::Box;
use alloc::vec::Vec;

use crate::dev_info;
use crate::device::Adapter;
use crate::ip::{
    CompletionFence, HWIP_COUNT, HwIp, InitStage, IpBlock, IpVersion, MAX_INSTANCE, UserSubmission,
};
use na_std::{Error, Result};

mod common;
mod dcn;
mod dmub;
mod gfx;
mod gmc;
mod ih;
mod psp;
mod sdma;
mod smu;
mod vcn;

use common::CommonBlock;
use dcn::DcnBlock;
pub(crate) use dcn::{
    CursorAttributes, CursorPosition, DcnCursor, DcnDisplayPipe, PrimarySurfaceConfig,
};
use gfx::GfxV10;
use gmc::GmcBlock;
use ih::IhV6;
use psp::PspBlock;
use sdma::SdmaV52;
use smu::SmuV11;
use vcn::{JpegV30, VcnV30};

/// Driver IP blocks owned independently from the shared adapter state.
/// Splitting these fields lets Rust borrow one engine and the device state at
/// the same time without moving the complete block list out of its owner.
pub struct IpBlocks {
    entries: Vec<Box<dyn IpBlock>>,
}

impl IpBlocks {
    fn version(versions: &[[u32; MAX_INSTANCE]; HWIP_COUNT], ip: HwIp) -> Option<IpVersion> {
        let full = versions[ip.index()][0];
        (full != 0).then(|| IpVersion::from_full(full))
    }

    fn init_block(dev: &mut Adapter, block: &mut dyn IpBlock, stage: InitStage) -> Result<()> {
        let name = block.name();
        block.init(dev, stage).inspect_err(|error| {
            dev_info!("astra: {} {} failed: {:?}", name, stage.name(), error);
        })
    }

    fn init_hardware(&mut self, dev: &mut Adapter, ip: HwIp) -> Result<()> {
        for block in &mut self.entries {
            if block.hw_ip() == ip {
                Self::init_block(dev, block.as_mut(), InitStage::Hardware)?;
            }
        }
        Ok(())
    }

    /// Runs Linux's amdgpu_device_ip_init / hw_init / late_init sequence over
    /// the discovered block objects. Keeping phase selection beside the block
    /// collection avoids exposing its representation to the device owner.
    pub(crate) fn initialize(&mut self, dev: &mut Adapter) -> Result<()> {
        for block in &mut self.entries {
            Self::init_block(dev, block.as_mut(), InitStage::Early)?;
        }

        // amdgpu_device_ip_init(): COMMON and GMC hardware setup is performed
        // inline with the software walk so subsequent GTT allocations can bind.
        for block in &mut self.entries {
            Self::init_block(dev, block.as_mut(), InitStage::Software)?;
            if matches!(block.hw_ip(), HwIp::Common | HwIp::Gmc) {
                Self::init_block(dev, block.as_mut(), InitStage::Hardware)?;
            }
            dev.flush_gart().inspect_err(|error| {
                dev_info!("astra: GART bind flush failed: {:?}", error);
            })?;
        }

        // amdgpu_device_ip_hw_init_phase1(), firmware loading, then phase2.
        self.init_hardware(dev, HwIp::OssSys)?;
        self.init_hardware(dev, HwIp::Mp0)?;
        for ip in [HwIp::Mp1, HwIp::Dm, HwIp::Gc, HwIp::Sdma0, HwIp::Uvd] {
            self.init_hardware(dev, ip)?;
        }

        for block in &mut self.entries {
            Self::init_block(dev, block.as_mut(), InitStage::Late)?;
        }
        Ok(())
    }

    pub(crate) fn submit_user_ibs(
        &mut self,
        dev: &mut Adapter,
        submission: UserSubmission<'_>,
    ) -> Result<CompletionFence> {
        self.entries
            .iter_mut()
            .find(|block| block.hw_ip() == HwIp::Gc)
            .ok_or(Error::NoDevice)?
            .submit_user_ibs(dev, submission)
    }

    pub(crate) fn update_vm_table(
        &mut self,
        dev: &mut Adapter,
        dst: u64,
        addr: u64,
        count: u32,
        incr: u32,
        flags: u64,
    ) -> Result<()> {
        self.entries
            .iter_mut()
            .find(|block| block.hw_ip() == HwIp::Sdma0)
            .ok_or(Error::NoDevice)?
            .update_vm_table(dev, dst, addr, count, incr, flags)
    }

    /// Discovers the IP block list in Linux's init order:
    /// COMMON → GMC → IH → PSP → SMU → [DM] → GC → SDMA → VCN → JPEG.
    pub fn discover(versions: &[[u32; MAX_INSTANCE]; HWIP_COUNT]) -> Self {
        let mut blocks: Vec<Box<dyn IpBlock>> = Vec::new();

        // COMMON is always present; its non-block funcs are selected by the
        // NBIO/HDP/SMUIO/THM/DF versions (nv_common_early_init equivalent).
        if let Some(nbio_version) = Self::version(versions, HwIp::Nbio) {
            blocks.push(Box::new(CommonBlock::new(
                nbio_version,
                Self::version(versions, HwIp::Hdp).unwrap_or_default(),
                Self::version(versions, HwIp::Smuio).unwrap_or_default(),
                Self::version(versions, HwIp::Thm).unwrap_or_default(),
                Self::version(versions, HwIp::Df).unwrap_or_default(),
            )));
        }

        // GMC: GC 10.3.x → gmc_v10_0 (gfxhub_v2_1 / mmhub_v2_0).
        if let Some(gc_version) = Self::version(versions, HwIp::Gc) {
            let mmhub_version = Self::version(versions, HwIp::Mmhub).unwrap_or_default();
            if gc_version.major == 10 && gc_version.minor == 3 {
                blocks.push(Box::new(GmcBlock::new(gc_version, mmhub_version)));
            }
        }

        // IH: OSSSYS 6.0.x → ih_v6_0.
        if let Some(osssys_version) = Self::version(versions, HwIp::OssSys)
            && osssys_version.major == 6
        {
            blocks.push(Box::new(IhV6::new(osssys_version)));
        }

        // PSP: Navi 2x uses MP0 11.0.x; newer parts use 13.0.x. The shared block
        // selects the small mailbox-handshake differences from the discovered IP.
        if let Some(mp0_version) = Self::version(versions, HwIp::Mp0) {
            dev_info!(
                "astra: MP0 (PSP) version {}.{}.{}",
                mp0_version.major,
                mp0_version.minor,
                mp0_version.revision,
            );
            if mp0_version.major == 11 || mp0_version.major == 13 {
                blocks.push(Box::new(PspBlock::new(mp0_version)));
            }
        } else {
            dev_info!("astra: MP0 (PSP) absent from discovery");
        }

        // SMU: MP1 11.0.x → smu_v11_0 (sienna cichlid ppt).
        if let Some(mp1_version) = Self::version(versions, HwIp::Mp1) {
            dev_info!(
                "astra: MP1 (SMU) version {}.{}.{}",
                mp1_version.major,
                mp1_version.minor,
                mp1_version.revision,
            );
            if mp1_version.major == 11 {
                blocks.push(Box::new(SmuV11::new(mp1_version)));
            }
        } else {
            dev_info!("astra: MP1 (SMU) absent from discovery");
        }

        // DCN: DMU (DCE) 3.0.2 → dcn302 display block.
        if let Some(dmu_version) = Self::version(versions, HwIp::Dmu) {
            dev_info!(
                "astra: DMU (DCN) version {}.{}.{}",
                dmu_version.major,
                dmu_version.minor,
                dmu_version.revision,
            );
            if dmu_version.major == 3 {
                blocks.push(Box::new(DcnBlock::new(dmu_version)));
            }
        } else {
            dev_info!("astra: DMU (DCN) absent from discovery");
        }

        // GC: GC 10.3.x → gfx_v10_0.
        if let Some(gc_version) = Self::version(versions, HwIp::Gc)
            && gc_version.major == 10
            && gc_version.minor == 3
        {
            blocks.push(Box::new(GfxV10::new(gc_version)));
        }

        // SDMA: 5.2.x → sdma_v5_2 (instance count from the discovery table).
        let sdma_instances = [HwIp::Sdma0, HwIp::Sdma1, HwIp::Sdma2, HwIp::Sdma3]
            .iter()
            .filter(|ip| Self::version(versions, **ip).is_some())
            .count() as u32;
        if sdma_instances != 0
            && let Some(sdma_version) = Self::version(versions, HwIp::Sdma0)
            && sdma_version.major == 5
            && sdma_version.minor == 2
        {
            blocks.push(Box::new(SdmaV52::new(sdma_version, sdma_instances)));
        }

        // MM blocks are appended after SDMA in amdgpu_discovery_set_ip_blocks.
        if let Some(uvd_version) = Self::version(versions, HwIp::Uvd)
            && uvd_version.major == 3
        {
            blocks.push(Box::new(VcnV30::new(uvd_version)));
            blocks.push(Box::new(JpegV30::new(uvd_version)));
        }

        Self { entries: blocks }
    }
}
