//! IP block construction from the discovery table — the single version
//! dispatch point, mirroring Linux `amdgpu_discovery_set_ip_blocks`.

use alloc::boxed::Box;
use alloc::vec::Vec;

use crate::dev_info;
use crate::device::Adapter;
use crate::ip::{
    CompletionFence, HWIP_COUNT, HwIp, IpBlock, IpVersion, MAX_INSTANCE, UserFence, UserIb,
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
    CursorAttributes, CursorPosition, PrimarySurfaceConfig, disable_cursor,
    program_cursor_attributes, program_cursor_position, program_primary_address,
    program_primary_geometry, program_primary_surface,
};
use gfx::GfxV10;
use gmc::GmcBlock;
pub(crate) use gmc::flush_pending_gart;
use ih::IhV6;
use psp::PspV13;
use sdma::SdmaV52;
use smu::SmuV11;
use vcn::{JpegV30, VcnV30};

pub(crate) fn submit_user_ibs(
    dev: &mut Adapter,
    ip_type: u32,
    ring: u32,
    vmid: u32,
    root_pde: u64,
    context_id: u32,
    ibs: &[UserIb],
    user_fence: Option<UserFence>,
) -> Result<CompletionFence> {
    let mut blocks = core::mem::take(&mut dev.blocks);
    let result = blocks
        .iter_mut()
        .find(|block| block.hw_ip() == HwIp::Gc)
        .ok_or(Error::NoDevice)
        .and_then(|block| {
            block.submit_user_ibs(
                dev, ip_type, ring, vmid, root_pde, context_id, ibs, user_fence,
            )
        });
    dev.blocks = blocks;
    result
}

pub(crate) fn update_vm_table(
    dev: &mut Adapter,
    dst: u64,
    addr: u64,
    count: u32,
    incr: u32,
    flags: u64,
) -> Result<()> {
    let mut blocks = core::mem::take(&mut dev.blocks);
    let result = blocks
        .iter_mut()
        .find(|block| block.hw_ip() == HwIp::Sdma0)
        .ok_or(Error::NoDevice)
        .and_then(|block| block.update_vm_table(dev, dst, addr, count, incr, flags));
    dev.blocks = blocks;
    result
}

fn version(versions: &[[u32; MAX_INSTANCE]; HWIP_COUNT], ip: HwIp) -> Option<IpVersion> {
    let full = versions[ip.index()][0];
    if full == 0 {
        return None;
    }
    Some(IpVersion::from_full(full))
}

/// Linux `amdgpu_discovery_set_common_ip_blocks` selects `nbio_v2_3_funcs`
/// for NBIO 2.1.x, 2.3.x and 3.3.x (including Navi23's 3.3.2).
fn uses_nbio_v2_3(dev: &Adapter) -> bool {
    let version = IpVersion::from_full(dev.versions[HwIp::Nbio.index()][0]);
    matches!((version.major, version.minor), (2, 1) | (2, 3) | (3, 3))
}

/// Builds the IP block list in Linux's init order:
/// COMMON → GMC → IH → PSP → SMU → [DM] → GC → SDMA → VCN → JPEG.
pub fn build(versions: &[[u32; MAX_INSTANCE]; HWIP_COUNT]) -> Vec<Box<dyn IpBlock>> {
    let mut blocks: Vec<Box<dyn IpBlock>> = Vec::new();

    // COMMON is always present; its non-block funcs are selected by the
    // NBIO/HDP/SMUIO/THM/DF versions (nv_common_early_init equivalent).
    if let Some(nbio_version) = version(versions, HwIp::Nbio) {
        blocks.push(Box::new(CommonBlock::new(
            nbio_version,
            version(versions, HwIp::Hdp).unwrap_or_default(),
            version(versions, HwIp::Smuio).unwrap_or_default(),
            version(versions, HwIp::Thm).unwrap_or_default(),
            version(versions, HwIp::Df).unwrap_or_default(),
        )));
    }

    // GMC: GC 10.3.x → gmc_v10_0 (gfxhub_v2_1 / mmhub_v2_0).
    if let Some(gc_version) = version(versions, HwIp::Gc) {
        let mmhub_version = version(versions, HwIp::Mmhub).unwrap_or_default();
        if gc_version.major == 10 && gc_version.minor == 3 {
            blocks.push(Box::new(GmcBlock::new(gc_version, mmhub_version)));
        }
    }

    // IH: OSSSYS 6.0.x → ih_v6_0.
    if let Some(osssys_version) = version(versions, HwIp::OssSys) {
        if osssys_version.major == 6 {
            blocks.push(Box::new(IhV6::new(osssys_version)));
        }
    }

    // PSP: Navi 2x uses MP0 11.0.x; newer parts use 13.0.x. The shared block
    // selects the small mailbox-handshake differences from the discovered IP.
    if let Some(mp0_version) = version(versions, HwIp::Mp0) {
        dev_info!(
            "astra: MP0 (PSP) version {}.{}.{}",
            mp0_version.major,
            mp0_version.minor,
            mp0_version.revision,
        );
        if mp0_version.major == 11 || mp0_version.major == 13 {
            blocks.push(Box::new(PspV13::new(mp0_version)));
        }
    } else {
        dev_info!("astra: MP0 (PSP) absent from discovery");
    }

    // SMU: MP1 11.0.x → smu_v11_0 (sienna cichlid ppt).
    if let Some(mp1_version) = version(versions, HwIp::Mp1) {
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
    if let Some(dmu_version) = version(versions, HwIp::Dmu) {
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
    if let Some(gc_version) = version(versions, HwIp::Gc) {
        if gc_version.major == 10 && gc_version.minor == 3 {
            blocks.push(Box::new(GfxV10::new(gc_version)));
        }
    }

    // VCN + JPEG: UVD 3.0.x → vcn_v3_0 / jpeg_v3_0.
    if let Some(uvd_version) = version(versions, HwIp::Uvd) {
        if uvd_version.major == 3 {
            blocks.push(Box::new(VcnV30::new(uvd_version)));
            blocks.push(Box::new(JpegV30::new(uvd_version)));
        }
    }

    // SDMA: 5.2.x → sdma_v5_2 (instance count from the discovery table).
    let sdma_instances = [HwIp::Sdma0, HwIp::Sdma1, HwIp::Sdma2, HwIp::Sdma3]
        .iter()
        .filter(|ip| version(versions, **ip).is_some())
        .count() as u32;
    if sdma_instances != 0 {
        if let Some(sdma_version) = version(versions, HwIp::Sdma0) {
            if sdma_version.major == 5 && sdma_version.minor == 2 {
                blocks.push(Box::new(SdmaV52::new(sdma_version, sdma_instances)));
            }
        }
    }

    // Remaining blocks land in later milestones; log what the table
    // reported so bring-up gaps are obvious.
    for ip in [
        HwIp::Uvd,
        HwIp::Sdma0,
        HwIp::Sdma1,
        HwIp::Sdma2,
        HwIp::Sdma3,
    ] {
        if let Some(v) = version(versions, ip) {
            dev_info!(
                "astra: ip block {} {}.{}.{} not yet initialized",
                ip.name(),
                v.major,
                v.minor,
                v.revision,
            );
        }
    }

    blocks
}
