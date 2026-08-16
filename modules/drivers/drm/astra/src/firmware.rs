//! Firmware registry and staging (Linux `amdgpu_ucode.c`).

use alloc::ffi::CString;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use na_std::firmware::{FirmwareProvider, KernelFirmwareProvider};
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;
use crate::ip::{HwIp, IpVersion};
use crate::ucode::CommonFirmwareHeader;

/// Firmware types carried over the PSP `LOAD_IP_FW` command
/// (Linux `GFX_FW_TYPE_*`).
pub const FW_TYPE_SMU: u32 = 18;
pub const FW_TYPE_SDMA0: u32 = 9;
pub const FW_TYPE_SDMA1: u32 = 10;
pub const FW_TYPE_SDMA2: u32 = 52;
pub const FW_TYPE_SDMA3: u32 = 53;
pub const FW_TYPE_CP_CE: u32 = 3;
pub const FW_TYPE_CP_PFP: u32 = 2;
pub const FW_TYPE_CP_ME: u32 = 1;
pub const FW_TYPE_CP_MEC: u32 = 4;
pub const FW_TYPE_RLC_G: u32 = 8;
pub const FW_TYPE_VCN: u32 = 13;
pub const FW_TYPE_DMUB: u32 = 51;

/// Firmware slots staged for the PSP (Linux `AMDGPU_UCODE_ID` subset
/// used on sienna cichlid, in load order).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UcodeId {
    Sdma0,
    Sdma1,
    Sdma2,
    Sdma3,
    CpCe,
    CpPfp,
    CpMe,
    CpMec1,
    CpMec2,
    RlcG,
    Smc,
    Vcn,
    Dmcub,
}

pub const UCODE_COUNT: usize = 13;

impl UcodeId {
    pub const ALL: [UcodeId; UCODE_COUNT] = [
        UcodeId::Sdma0,
        UcodeId::Sdma1,
        UcodeId::Sdma2,
        UcodeId::Sdma3,
        UcodeId::CpCe,
        UcodeId::CpPfp,
        UcodeId::CpMe,
        UcodeId::CpMec1,
        UcodeId::CpMec2,
        UcodeId::RlcG,
        UcodeId::Smc,
        UcodeId::Vcn,
        UcodeId::Dmcub,
    ];

    pub const fn index(self) -> usize {
        self as usize
    }

    /// PSP firmware type for `LOAD_IP_FW` (Linux `amdgpu_psp_get_fw_type`).
    pub const fn psp_fw_type(self) -> u32 {
        match self {
            UcodeId::Smc => FW_TYPE_SMU,
            UcodeId::Sdma0 => FW_TYPE_SDMA0,
            UcodeId::Sdma1 => FW_TYPE_SDMA1,
            UcodeId::Sdma2 => FW_TYPE_SDMA2,
            UcodeId::Sdma3 => FW_TYPE_SDMA3,
            UcodeId::CpCe => FW_TYPE_CP_CE,
            UcodeId::CpPfp => FW_TYPE_CP_PFP,
            UcodeId::CpMe => FW_TYPE_CP_ME,
            UcodeId::CpMec1 | UcodeId::CpMec2 => FW_TYPE_CP_MEC,
            UcodeId::RlcG => FW_TYPE_RLC_G,
            UcodeId::Vcn => FW_TYPE_VCN,
            UcodeId::Dmcub => FW_TYPE_DMUB,
        }
    }
}

/// One staged firmware image: the VFS blob plus its location inside the
/// contiguous staging buffer.
pub struct StagedFw {
    pub id: UcodeId,
    pub name: String,
    pub size: usize,
    /// GPU address inside the staging BO (page-aligned).
    pub mc_addr: u64,
    /// Firmware version from `common_firmware_header.ucode_version`.
    pub fw_version: u32,
    /// Data kept for header parsing (SOS/TA live outside the staging).
    pub data: Vec<u8>,
    /// TMR address reported by the PSP after LOAD_IP_FW.
    pub tmr_addr: Option<u64>,
}

/// Requests the firmware images for the discovered IP versions and
/// stages them into one contiguous GART buffer (Linux
/// `amdgpu_ucode_init_bo` + `amdgpu_ucode_init_single_fw`).
pub fn stage_all(dev: &mut Adapter) -> Result<Vec<StagedFw>> {
    let provider = KernelFirmwareProvider;

    let mut entries = Vec::new();
    for id in UcodeId::ALL {
        let discovered = match id {
            UcodeId::Sdma0 => dev.versions[HwIp::Sdma0.index()][0] != 0,
            UcodeId::Sdma1 => dev.versions[HwIp::Sdma1.index()][0] != 0,
            UcodeId::Sdma2 => dev.versions[HwIp::Sdma2.index()][0] != 0,
            UcodeId::Sdma3 => dev.versions[HwIp::Sdma3.index()][0] != 0,
            _ => true,
        };
        if !discovered {
            continue;
        }
        let name = firmware_name(dev, id)?;
        let data = request(&provider, &name)?;
        if data.is_empty() {
            return Err(Error::Io);
        }
        let header = CommonFirmwareHeader::parse(&data).ok_or(Error::Io)?;
        let payload_size = match id {
            // Linux `amdgpu_ucode_init_single_fw` submits the MEC program
            // without the jump table.  The latter occupies the final
            // `jt_size * 4` bytes of a v1 GFX firmware payload and is skipped
            // as a separate PSP command when RLC autoload is enabled.
            UcodeId::CpMec1 | UcodeId::CpMec2 => {
                let jt_size_dw = data
                    .get(40..44)
                    .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                    .ok_or(Error::Io)?;
                (header.ucode_size_bytes as usize)
                    .checked_sub(jt_size_dw as usize * 4)
                    .ok_or(Error::Io)?
            }
            // Linux uses `dmcub_firmware_header_v1_0.inst_const_bytes`
            // for the PSP submission.  This includes the 256-byte PSP
            // header at the start of the firmware payload.
            UcodeId::Dmcub => data
                .get(32..36)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize)
                .ok_or(Error::Io)?,
            _ => header.ucode_size_bytes as usize,
        };
        let payload_start = header.ucode_array_offset_bytes as usize;
        let payload_end = payload_start.checked_add(payload_size).ok_or(Error::Io)?;
        data.get(payload_start..payload_end).ok_or(Error::Io)?;
        let fw_version = header.ucode_version;
        dev_info!("astra: found firmware {} ({} bytes)", name, data.len());
        entries.push((id, name, data, payload_size, fw_version));
    }

    // Stage into one contiguous, page-aligned GART buffer.
    let total: usize = entries
        .iter()
        .map(|(_, _, _, payload_size, _)| payload_size.next_multiple_of(4096))
        .sum();
    let mut staging = dev.mem.alloc_gart(&mut dev.regs, total)?;
    let staging_base = staging.gpu_addr;

    let mut staged = Vec::new();
    let mut offset = 0usize;
    for (id, name, data, payload_size, fw_version) in entries {
        let mc_addr = staging_base + offset as u64;
        let header = CommonFirmwareHeader::parse(&data).ok_or(Error::Io)?;
        let payload_start = header.ucode_array_offset_bytes as usize;
        let payload = data
            .get(payload_start..payload_start + payload_size)
            .ok_or(Error::Io)?;
        let size = payload_size;
        let cpu = staging.cpu.as_mut().ok_or(Error::NoDevice)?;
        cpu.as_mut_slice()
            .get_mut(offset..offset + size)
            .ok_or(Error::Range)?
            .copy_from_slice(payload);
        staged.push(StagedFw {
            id,
            name,
            size,
            mc_addr,
            fw_version,
            data,
            tmr_addr: None,
        });
        offset += size.next_multiple_of(4096);
    }
    staging.cpu.as_ref().map(|cpu| cpu.sync_for_device());
    dev.fw_staging = Some(staging);
    Ok(staged)
}

/// Requests a single firmware image (SOS/TA) and returns its bytes.
pub fn request(provider: &KernelFirmwareProvider, name: &str) -> Result<Vec<u8>> {
    let cname = CString::new(name).map_err(|_| Error::InvalidArgument)?;
    Ok(provider.request(&cname)?.into_data())
}

/// Returns the chip codename derived from the GC version (Linux
/// `chip_name`). All firmware images (SOS/TA/SMC/SDMA/VCN/CP/RLC) share
/// this prefix, so it is derived from the stable GC version rather than
/// the MP0/MP1 revisions.
pub fn chip_name(dev: &Adapter) -> &'static str {
    let gc = IpVersion::from_full(dev.versions[HwIp::Gc.index()][0]);
    match (gc.major, gc.minor, gc.revision) {
        (10, 3, 4) => "dimgrey_cavefish",
        (10, 3, 0) => "sienna_cichlid",
        (10, 3, 7) => "gc_10_3_7",
        _ => "gc_unknown",
    }
}

/// Computes the firmware file name (Linux `amdgpu_ucode.c` legacy naming).
pub fn firmware_name(dev: &Adapter, id: UcodeId) -> Result<String> {
    let chip = chip_name(dev);
    let suffix = match id {
        UcodeId::Smc => "smc",
        UcodeId::Vcn => "vcn",
        UcodeId::Dmcub => "dmcub",
        UcodeId::Sdma0 | UcodeId::Sdma1 | UcodeId::Sdma2 | UcodeId::Sdma3 => "sdma",
        UcodeId::CpCe => "ce",
        UcodeId::CpPfp => "pfp",
        UcodeId::CpMe => "me",
        UcodeId::CpMec1 => "mec",
        UcodeId::CpMec2 => "mec2",
        UcodeId::RlcG => "rlc",
    };
    Ok(format!("amdgpu/{}_{}.bin", chip, suffix))
}
