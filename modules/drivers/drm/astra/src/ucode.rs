//! Firmware binary header formats (Linux `amdgpu_ucode.h`): common AMD
//! firmware headers plus PSP SOS/TA packaging.

// Firmware headers intentionally model the complete upstream ABI, including
// fields and TA identifiers not needed by the current Navi23 boot path.
#![allow(dead_code)]

use alloc::vec::Vec;

/// `struct common_firmware_header` (32 bytes).
#[derive(Clone, Copy, Debug)]
pub struct CommonFirmwareHeader {
    pub size_bytes: u32,
    pub header_size_bytes: u32,
    pub header_version_major: u16,
    pub header_version_minor: u16,
    pub ucode_version: u32,
    pub ucode_size_bytes: u32,
    pub ucode_array_offset_bytes: u32,
}

impl CommonFirmwareHeader {
    pub fn parse(data: &[u8]) -> Option<Self> {
        let u16_at = |offset: usize| {
            data.get(offset..offset + 2)
                .map(|b| u16::from_le_bytes([b[0], b[1]]))
        };
        let u32_at = |offset: usize| {
            data.get(offset..offset + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        };
        let header = Self {
            size_bytes: u32_at(0)?,
            header_size_bytes: u32_at(4)?,
            header_version_major: u16_at(8)?,
            header_version_minor: u16_at(10)?,
            ucode_version: u32_at(16)?,
            ucode_size_bytes: u32_at(20)?,
            ucode_array_offset_bytes: u32_at(24)?,
        };
        let declared = header.size_bytes as usize;
        let payload_end = (header.ucode_array_offset_bytes as usize)
            .checked_add(header.ucode_size_bytes as usize)?;
        if header.header_size_bytes < 32 || declared > data.len() || payload_end > data.len() {
            return None;
        }
        Some(header)
    }

    pub fn payload<'a>(&self, data: &'a [u8]) -> Option<&'a [u8]> {
        let start = self.ucode_array_offset_bytes as usize;
        let end = start.checked_add(self.ucode_size_bytes as usize)?;
        data.get(start..end)
    }
}

/// Common firmware header prefix: `fw_version` sits at byte offset 12.
pub fn fw_version(data: &[u8]) -> Option<u32> {
    CommonFirmwareHeader::parse(data).map(|header| header.ucode_version)
}

#[derive(Clone, Copy, Debug)]
pub struct PspBootComponent {
    pub command: u32,
    pub offset_bytes: u32,
    pub size_bytes: u32,
}

/// Parsed SOS package. `sos_offset_bytes` and component offsets are absolute
/// offsets within the firmware file, after applying `ucode_array_offset`.
pub struct SosHeader {
    pub header_size_bytes: u32,
    pub header_version_major: u16,
    pub header_version_minor: u16,
    pub sos_offset_bytes: u32,
    pub sos_size_bytes: u32,
    pub toc_offset_bytes: u32,
    pub toc_size_bytes: u32,
    pub boot_components: Vec<PspBootComponent>,
}

impl SosHeader {
    pub fn parse(data: &[u8]) -> Option<Self> {
        let common = CommonFirmwareHeader::parse(data)?;
        let u32_at = |offset: usize| {
            data.get(offset..offset + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        };
        let array = common.ucode_array_offset_bytes;
        let mut components = Vec::new();
        let mut toc = None;
        let (sos_offset, sos_size) = match common.header_version_major {
            1 => {
                // psp_firmware_header_v1_0: common + legacy SOS descriptor.
                let relative = u32_at(36)?;
                let size = u32_at(40)?;
                if relative != 0 {
                    components.push(PspBootComponent {
                        command: PSP_BL_LOAD_SYSDRV,
                        offset_bytes: array,
                        size_bytes: relative,
                    });
                }
                // v1.1/v1.2/v1.3 all place KDB at byte 56.
                if (1..=3).contains(&common.header_version_minor) {
                    let kdb_offset = u32_at(60)?;
                    let kdb_size = u32_at(64)?;
                    if kdb_size != 0 {
                        components.push(PspBootComponent {
                            command: PSP_BL_LOAD_KDB,
                            offset_bytes: array.checked_add(kdb_offset)?,
                            size_bytes: kdb_size,
                        });
                    }
                }
                if matches!(common.header_version_minor, 1 | 3) {
                    let toc_offset = u32_at(48)?;
                    let toc_size = u32_at(52)?;
                    if toc_size != 0 {
                        toc = Some((array.checked_add(toc_offset)?, toc_size));
                    }
                }
                if common.header_version_minor == 3 {
                    let spl_offset = u32_at(72)?;
                    let spl_size = u32_at(76)?;
                    if spl_size != 0 {
                        components.push(PspBootComponent {
                            command: PSP_BL_LOAD_SPL,
                            offset_bytes: array.checked_add(spl_offset)?,
                            size_bytes: spl_size,
                        });
                    }
                }
                (array.checked_add(relative)?, size)
            }
            2 => {
                let count = u32_at(32)? as usize;
                let (desc_at, standard_count) = if common.header_version_minor == 1 {
                    let aux_index = u32_at(36)? as usize;
                    (40usize, count.checked_sub(aux_index)?)
                } else {
                    (36usize, count)
                };
                let mut sos = None;
                for index in 0..standard_count {
                    let at = desc_at.checked_add(index.checked_mul(16)?)?;
                    let fw_type = u32_at(at)?;
                    let relative = u32_at(at + 8)?;
                    let size = u32_at(at + 12)?;
                    let offset = array.checked_add(relative)?;
                    if fw_type == PSP_FW_TYPE_SOS {
                        sos = Some((offset, size));
                    } else if fw_type == PSP_FW_TYPE_TOC {
                        if size != 0 {
                            toc = Some((offset, size));
                        }
                    } else if let Some(command) = boot_command(fw_type) {
                        if size != 0 {
                            components.push(PspBootComponent {
                                command,
                                offset_bytes: offset,
                                size_bytes: size,
                            });
                        }
                    }
                }
                sos?
            }
            _ => return None,
        };
        let sos_end = (sos_offset as usize).checked_add(sos_size as usize)?;
        if sos_size == 0 || sos_end > data.len() {
            return None;
        }
        components.sort_by_key(|component| boot_priority(component.command));
        for component in &components {
            let end =
                (component.offset_bytes as usize).checked_add(component.size_bytes as usize)?;
            if end > data.len() {
                return None;
            }
        }
        let (toc_offset_bytes, toc_size_bytes) = toc.unwrap_or((0, 0));
        let toc_end = (toc_offset_bytes as usize).checked_add(toc_size_bytes as usize)?;
        if toc_size_bytes != 0 && toc_end > data.len() {
            return None;
        }
        Some(Self {
            header_size_bytes: common.header_size_bytes,
            header_version_major: common.header_version_major,
            header_version_minor: common.header_version_minor,
            sos_offset_bytes: sos_offset,
            sos_size_bytes: sos_size,
            toc_offset_bytes,
            toc_size_bytes,
            boot_components: components,
        })
    }
}

pub struct TaHeader {
    pub header_size_bytes: u32,
    pub header_version_major: u16,
    pub header_version_minor: u16,
    pub ta_fw_bin_count: u32,
    pub descriptors: Vec<TaBinDesc>,
}

impl TaHeader {
    pub fn parse(data: &[u8]) -> Option<Self> {
        let common = CommonFirmwareHeader::parse(data)?;
        let u32_at = |offset: usize| {
            data.get(offset..offset + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        };
        let array = common.ucode_array_offset_bytes;
        let mut descriptors = Vec::new();
        match common.header_version_major {
            1 => {
                // Legacy TA package: XGMI, RAS, HDCP, DTM, SecureDisplay.
                for (index, fw_type) in [2u32, 3, 4, 5, 7].into_iter().enumerate() {
                    let at = 32 + index * 12;
                    let fw_version = u32_at(at)?;
                    let relative = u32_at(at + 4)?;
                    let size = u32_at(at + 8)?;
                    if size != 0 {
                        descriptors.push(TaBinDesc {
                            offset_bytes: array.checked_add(relative)?,
                            size_bytes: size,
                            fw_type,
                            fw_version,
                        });
                    }
                }
            }
            2 => {
                let count = u32_at(32)? as usize;
                for index in 0..count {
                    let at = 36usize.checked_add(index.checked_mul(16)?)?;
                    let fw_type = u32_at(at)?;
                    let fw_version = u32_at(at + 4)?;
                    let relative = u32_at(at + 8)?;
                    let size = u32_at(at + 12)?;
                    if size != 0 {
                        descriptors.push(TaBinDesc {
                            offset_bytes: array.checked_add(relative)?,
                            size_bytes: size,
                            fw_type,
                            fw_version,
                        });
                    }
                }
            }
            _ => return None,
        }
        for descriptor in &descriptors {
            let end =
                (descriptor.offset_bytes as usize).checked_add(descriptor.size_bytes as usize)?;
            if end > data.len() {
                return None;
            }
        }
        Some(Self {
            header_size_bytes: common.header_size_bytes,
            header_version_major: common.header_version_major,
            header_version_minor: common.header_version_minor,
            ta_fw_bin_count: descriptors.len() as u32,
            descriptors,
        })
    }
}

#[derive(Clone, Copy, Debug)]
pub struct TaBinDesc {
    pub offset_bytes: u32,
    pub size_bytes: u32,
    pub fw_type: u32,
    pub fw_version: u32,
}

/// TA firmware types (Linux `enum ta_fw_type` in amdgpu_ucode.h).
pub const TA_TYPE_ASD: u32 = 1;
pub const TA_TYPE_RAS: u32 = 3;
pub const TA_TYPE_HDCP: u32 = 4;
pub const TA_TYPE_DTM: u32 = 5;
pub const TA_TYPE_RAP: u32 = 6;
pub const TA_TYPE_SECUREDISPLAY: u32 = 7;

/// `GFX_CMD_ID_*` (psp_gfx_if.h).
pub const GFX_CMD_ID_LOAD_TA: u32 = 0x1;
pub const GFX_CMD_ID_LOAD_ASD: u32 = 0x4;
pub const GFX_CMD_ID_SETUP_TMR: u32 = 0x5;
pub const GFX_CMD_ID_LOAD_IP_FW: u32 = 0x6;
pub const GFX_CMD_ID_LOAD_TOC: u32 = 0x20;
pub const GFX_CMD_ID_AUTOLOAD_RLC: u32 = 0x21;

/// `PSP_BL__LOAD_SOSDRV` and memory-training command.
pub const PSP_BL_LOAD_SOSDRV: u32 = 0x2_0000;
pub const PSP_BL_DRAM_LONG_TRAIN: u32 = 0x10_0000;
pub const PSP_BL_LOAD_SYSDRV: u32 = 0x1_0000;
pub const PSP_BL_LOAD_KDB: u32 = 0x8_0000;
pub const PSP_BL_LOAD_SPL: u32 = 0x1000_0000;
pub const PSP_BL_LOAD_SOCDRV: u32 = 0xB_0000;
pub const PSP_BL_LOAD_DBGDRV: u32 = 0xC_0000;
pub const PSP_BL_LOAD_INTFDRV: u32 = 0xD_0000;
pub const PSP_BL_LOAD_RASDRV: u32 = 0xE_0000;
pub const PSP_BL_LOAD_IPKEYMGRDRV: u32 = 0xF_0000;
pub const PSP_BL_LOAD_SPDMDRV: u32 = 0x2000_0000;

const PSP_FW_TYPE_SOS: u32 = 1;
const PSP_FW_TYPE_SYS: u32 = 2;
const PSP_FW_TYPE_KDB: u32 = 3;
const PSP_FW_TYPE_TOC: u32 = 4;
const PSP_FW_TYPE_SPL: u32 = 5;
const PSP_FW_TYPE_SOC: u32 = 7;
const PSP_FW_TYPE_INTF: u32 = 8;
const PSP_FW_TYPE_DBG: u32 = 9;
const PSP_FW_TYPE_RAS: u32 = 10;
const PSP_FW_TYPE_IPKEYMGR: u32 = 11;
const PSP_FW_TYPE_SPDM: u32 = 12;

fn boot_command(fw_type: u32) -> Option<u32> {
    Some(match fw_type {
        PSP_FW_TYPE_SYS => PSP_BL_LOAD_SYSDRV,
        PSP_FW_TYPE_KDB => PSP_BL_LOAD_KDB,
        PSP_FW_TYPE_SPL => PSP_BL_LOAD_SPL,
        PSP_FW_TYPE_SOC => PSP_BL_LOAD_SOCDRV,
        PSP_FW_TYPE_INTF => PSP_BL_LOAD_INTFDRV,
        PSP_FW_TYPE_DBG => PSP_BL_LOAD_DBGDRV,
        PSP_FW_TYPE_RAS => PSP_BL_LOAD_RASDRV,
        PSP_FW_TYPE_IPKEYMGR => PSP_BL_LOAD_IPKEYMGRDRV,
        PSP_FW_TYPE_SPDM => PSP_BL_LOAD_SPDMDRV,
        _ => return None,
    })
}

fn boot_priority(command: u32) -> u8 {
    match command {
        PSP_BL_LOAD_KDB => 0,
        PSP_BL_LOAD_SPL => 1,
        PSP_BL_LOAD_SYSDRV => 2,
        PSP_BL_LOAD_SOCDRV => 3,
        PSP_BL_LOAD_INTFDRV => 4,
        PSP_BL_LOAD_DBGDRV => 5,
        PSP_BL_LOAD_RASDRV => 6,
        PSP_BL_LOAD_IPKEYMGRDRV => 7,
        PSP_BL_LOAD_SPDMDRV => 8,
        _ => u8::MAX,
    }
}

/// PSP command buffer version (PSP_GFX_CMD_BUF_VERSION).
pub const PSP_GFX_CMD_BUF_VERSION: u32 = 0x1;
/// Command/response buffer total size.
pub const PSP_CMD_RESP_SIZE: usize = 1024;
/// TA shared memory size (PSP_TA_SHARED_MEM_SIZE).
pub const TA_SHARED_MEM_SIZE: usize = 0x4000;
/// ASD shared memory size (PSP_ASD_SHARED_MEM_SIZE).
pub const ASD_SHARED_MEM_SIZE: usize = 0x4000;

/// PSP mailbox masks (amdgpu_psp.h).
pub const MBOX_TOS_READY_MASK: u32 = 0x8000_FFFF;
pub const MBOX_TOS_READY_FLAG: u32 = 0x8000_0000;
pub const MBOX_TOS_RESP_MASK: u32 = 0x8000_FFFF;
pub const MBOX_TOS_RESP_FLAG: u32 = 0x8000_0000;
