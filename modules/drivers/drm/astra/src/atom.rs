//! ATOM BIOS / VBIOS access (Linux `amdgpu_bios.c` + `amdgpu_atomfirmware.c`).
//!
//! The video BIOS is read through three sources, mirroring Linux
//! `amdgpu_get_bios_dgpu` order for a discrete GPU: the VRAM shadow (the
//! GPU posts the VBIOS at the start of VRAM when it has been posted), the
//! SMUIO ROM window (register-based, no PCI ROM BAR needed) and the PCI
//! expansion-ROM BAR. The ATOM master data table is then parsed for the
//! tables needed during init: firmware info, powerplay (SMU pptable) and
//! VRAM info.

// The command-table interpreter is kept complete for the remaining Linux
// ATOM command paths even though DCN302 currently routes display commands
// through DMCUB.  Keep that staged interpreter from producing dead-code
// warnings without suppressing warnings in the rest of the driver.
#![allow(dead_code)]

use alloc::vec;
use alloc::vec::Vec;

use na_std::pci::Bar;
use na_std::{Error, Result};

use crate::dev_info;
use crate::ip::{HwIp, IpVersion};
use crate::regs::{Regs, get_field};

/// ATOM ROM header offsets.
const ATOM_ROM_DATA_PTR: usize = 0x20;
const ATOM_ROM_TABLE_PTR: usize = 0x48;

/// Data-table indexes into `atom_master_list_of_data_tables_v2_1`
/// (atomfirmware.h:390), i.e. `offsetof(field) / sizeof(u16)`.
const INDEX_FIRMWARE_INFO: usize = 4;
const INDEX_SMC_DPM_INFO: usize = 2;
const INDEX_POWERPLAY_INFO: usize = 15;
const INDEX_VRAM_INFO: usize = 28;
const INDEX_GPIO_PIN_LUT: usize = 12;
const INDEX_DISPLAY_OBJECT_INFO: usize = 22;

// BIOS graphics-object IDs come from amdgpu/ObjectID.h. Do not use the
// similarly named SOC15 displayobject.h values: Navi VBIOS object tables
// retain the legacy IDs consumed by Linux bios_parser_common.c.
pub const CONNECTOR_OBJECT_ID_HDMI_TYPE_A: u8 = 0x0c;
const ENCODER_OBJECT_ID_INTERNAL_UNIPHY: u8 = 0x1e;
const ENCODER_OBJECT_ID_INTERNAL_UNIPHY1: u8 = 0x20;
const ENCODER_OBJECT_ID_INTERNAL_UNIPHY2: u8 = 0x21;

/// Navi23 / Sienna Cichlid uses the SMU 11.0.7 driver interface.  These
/// sizes and offsets are the packed Linux structures from
/// `smu_v11_0_7_pptable.h`,
/// `smu11_driver_if_sienna_cichlid.h` and `atomfirmware.h`.
const SIENNA_CICHLID_PPTABLE_SIZE: usize = 1668;
const PPTABLE_I2C_CONTROLLERS_OFFSET: usize = 1344;
const SMC_DPM_V4_9_SIZE: usize = 296;
const SMC_DPM_I2C_CONTROLLERS_OFFSET: usize = 4;
const SMC_DPM_BOARD_DATA_SIZE: usize = 292;

/// ATOM firmware capability bits (atombios_firmware_capability).
pub const FIRMWARE_CAP_ENABLE_2STAGE_BIST_TRAINING: u32 = 0x0000_0400;

/// Number of dwords read through the ROM BAR while looking for the ATOM
/// signature (512 KiB, Linux `AMD_VBIOS_LENGTH`).
const ROM_READ_BYTES: usize = 512 << 10;

/// The SMUIO ROM window scans the whole SPI flash from index 0: the VBIOS
/// image may sit anywhere inside a 4 MiB flash.
const ROM_SCAN_BYTES: usize = 4 << 20;
const ROM_SCAN_DWORDS: usize = ROM_SCAN_BYTES / 4;

/// Boot values parsed from the `firmwareinfo` data table.
#[derive(Clone, Copy, Debug, Default)]
pub struct FirmwareInfo {
    pub bootup_sclk_khz: u32,
    pub bootup_mclk_khz: u32,
    pub firmware_capability: u32,
    pub bootup_vddc_mv: u16,
    pub bootup_vddci_mv: u16,
    pub bootup_mvddc_mv: u16,
    pub bootup_vddgfx_mv: u16,
    pub cooling_solution_id: u8,
    pub pplib_pptable_id: u32,
    /// VBIOS-owned region at the top of VRAM.  Present in firmwareinfo
    /// v3.4+ as `fw_reserved_size_in_kb`.
    pub fw_reserved_size: u64,
}

/// The Sienna Cichlid driver powerplay table constructed from the ATOM
/// PowerPlay container and the separate SMC DPM board-info table.
pub struct PowerplayInfo {
    pub bytes: Vec<u8>,
    pub format_revision: u8,
    pub content_revision: u8,
    pub atom_table_size: usize,
    pub smc_pptable_offset: usize,
    pub smc_dpm_format_revision: u8,
    pub smc_dpm_content_revision: u8,
}

/// VRAM characteristics from the `vram_info` data table (v2.3).
#[derive(Clone, Copy, Debug, Default)]
pub struct VramInfo {
    pub memory_type: u8,
    pub channel_num: u8,
    pub channel_width: u8,
}

/// One physical display path from `displayobjectinfo` v1.4/v1.5.
/// Object IDs retain their VBIOS low-byte IDs while `phy_id` is the
/// zero-based UNIPHY A..F index used by the DCN link encoder.
#[derive(Clone, Copy, Debug)]
pub struct DisplayPath {
    pub connector_obj_id: u8,
    pub connector_enum_id: u8,
    pub encoder_obj_id: u8,
    pub encoder_enum_id: u8,
    pub phy_id: u8,
    /// ATOM transmitter HPD selection (1..6, or 0 when unassigned).
    pub hpd_sel: u8,
    /// GPIO ID referenced by the connector HPD record.
    pub hpd_pin_id: u8,
}

pub struct AtomBios {
    bios: Vec<u8>,
}

/// Locates a valid ATOM BIOS image inside `bytes` and returns the byte
/// offset of its start. Mirrors Linux `check_atom_bios`: a 0x55 0xaa
/// signature, a non-zero header pointer at `+0x48`, and the 4-byte
/// `"ATOM"` / `"MOTA"` signature at `header + 4`.
fn locate_atom(bytes: &[u8]) -> Option<usize> {
    if bytes.len() < 0x49 {
        return None;
    }
    for offset in (0..=bytes.len() - 0x49).step_by(2) {
        if bytes[offset] != 0x55 || bytes[offset + 1] != 0xaa {
            continue;
        }
        let header = u16::from_le_bytes([bytes[offset + 0x48], bytes[offset + 0x49]]) as usize;
        if header == 0 {
            continue;
        }
        let atom = offset + header + 4;
        if atom + 4 > bytes.len() {
            continue;
        }
        if &bytes[atom..atom + 4] == b"ATOM" || &bytes[atom..atom + 4] == b"MOTA" {
            return Some(offset);
        }
    }
    None
}

impl AtomBios {
    /// Builds an `AtomBios` from a raw image by locating the ATOM header
    /// (used by the ATRM and PCI ROM BAR sources, which return the whole
    /// image from offset 0).
    pub fn from_bytes(mut bytes: Vec<u8>) -> Option<Self> {
        let offset = locate_atom(&bytes)?;
        let bios = bytes.split_off(offset);
        Some(Self { bios })
    }

    /// Reads the video BIOS. VRAM shadow first, then the SMUIO ROM window
    /// (Linux `amdgpu_soc15_read_bios_from_rom`). Returns `Err` so the
    /// caller can fall back to the PCI ROM BAR.
    pub fn read(regs: &mut Regs, _smuio_version: IpVersion) -> Result<Self> {
        match Self::read_from_vram(regs) {
            Some(bios) => {
                dev_info!("astra: VBIOS loaded ({} bytes) from VRAM", bios.bios.len());
                return Ok(bios);
            }
            None => {}
        }

        dev_info!("astra: VBIOS not in VRAM, trying SMUIO ROM window");
        let base0 = regs.base_u32(HwIp::Smuio, 0, 0).unwrap_or(0);
        let base1 = regs.base_u32(HwIp::Smuio, 0, 1).unwrap_or(0);
        dev_info!("astra: SMUIO bases {:#x} {:#x}", base0, base1);
        // SMUIO 11.0.10 uses the 11.0.6 register layout; the ROM window
        // lives in base index 0. base index 1 is tried as a fallback.
        let candidates: [RomLayout; 4] = [
            RomLayout {
                rom_index: crate::regs::smuio11_0_6::mmROM_INDEX,
                rom_data: crate::regs::smuio11_0_6::mmROM_DATA,
                cgtt: crate::regs::smuio11_0_6::mmCGTT_ROM_CLK_CTRL0,
                base_idx: 0,
            },
            RomLayout {
                rom_index: crate::regs::smuio11_0_6::mmROM_INDEX,
                rom_data: crate::regs::smuio11_0_6::mmROM_DATA,
                cgtt: crate::regs::smuio11_0_6::mmCGTT_ROM_CLK_CTRL0,
                base_idx: 1,
            },
            RomLayout {
                rom_index: crate::regs::smuio11_0_0::mmROM_INDEX,
                rom_data: crate::regs::smuio11_0_0::mmROM_DATA,
                cgtt: crate::regs::smuio11_0_0::mmCGTT_ROM_CLK_CTRL0,
                base_idx: 0,
            },
            RomLayout {
                rom_index: crate::regs::smuio11_0_0::mmROM_INDEX,
                rom_data: crate::regs::smuio11_0_0::mmROM_DATA,
                cgtt: crate::regs::smuio11_0_0::mmCGTT_ROM_CLK_CTRL0,
                base_idx: 1,
            },
        ];

        for layout in &candidates {
            match Self::read_from_rom_window(regs, *layout) {
                Ok(bios) => {
                    dev_info!(
                        "astra: VBIOS loaded ({} bytes) from SMUIO ROM window",
                        bios.bios.len()
                    );
                    return Ok(bios);
                }
                Err(_) => dev_info!(
                    "astra: ROM window (base_idx {}, index {:#x}) failed",
                    layout.base_idx,
                    layout.rom_index
                ),
            }
        }

        Err(Error::Io)
    }

    /// Reads the VBIOS from the PCI expansion-ROM BAR (Linux
    /// `amdgpu_read_bios`, via `pci_map_rom`). The ROM BAR is assigned and
    /// enabled by the PCI enumeration.
    pub fn read_from_rom_bar(bar: Bar) -> Result<Self> {
        let range = match bar {
            Bar::Memory { range, .. } => range,
            Bar::Port { .. } => return Err(Error::Unsupported),
        };
        let region = range.map_mmio().map_err(|_| Error::Io)?;
        let len = region.length().min(ROM_READ_BYTES);
        let mut bytes = vec![0u8; len];
        region.read_into(0, &mut bytes).map_err(|_| Error::Io)?;
        let bios = Self::from_bytes(bytes).ok_or(Error::Io)?;
        dev_info!(
            "astra: VBIOS loaded ({} bytes) from PCI ROM BAR",
            bios.bios.len()
        );
        Ok(bios)
    }

    /// Reads the 256 KiB VBIOS shadow from the start of VRAM
    /// (Linux `amdgpu_read_bios_from_vram`).
    fn read_from_vram(regs: &mut Regs) -> Option<Self> {
        const VRAM_VBIOS_SIZE: usize = 256 << 10;
        let mut dwords = vec![0u32; VRAM_VBIOS_SIZE / 4];
        regs.vram_read_dwords(0, &mut dwords).ok()?;

        let first = dwords[0];
        dev_info!("astra: VRAM shadow first dword {:#x}", first);

        let mut bytes = Vec::with_capacity(VRAM_VBIOS_SIZE);
        for dword in &dwords {
            bytes.extend_from_slice(&dword.to_le_bytes());
        }
        let offset = locate_atom(&bytes)?;
        let bios = bytes.split_off(offset);
        Some(Self { bios })
    }

    /// Reads the VBIOS through the SMUIO ROM window: force the ROM clock
    /// on, write index 0, then read the whole 4 MiB SPI flash through the
    /// auto-incrementing data register and scan for the ATOM header (the
    /// NBIO `ROM_OFFSET` field is meaningless on an un-posted card).
    fn read_from_rom_window(regs: &mut Regs, layout: RomLayout) -> Result<Self> {
        // The NBIO ROM_OFFSET strapping points at the VBIOS location inside
        // the SPI flash (units of 128 KiB, so `<< 17` converts to bytes).
        let rom_ctrl = regs.read_ip(
            HwIp::Nbio,
            0,
            crate::regs::nbio4_3_0::regREGS_ROM_OFFSET_CTRL,
            5,
        )?;
        let rom_offset = (get_field(
            rom_ctrl,
            crate::regs::nbio4_3_0::REGS_ROM_OFFSET_CTRL__ROM_OFFSET__SHIFT,
            crate::regs::nbio4_3_0::REGS_ROM_OFFSET_CTRL__ROM_OFFSET_MASK,
        ) as u32)
            << 17;

        // Force the ROM clock on (SOFT_OVERRIDE0|SOFT_OVERRIDE1), reporting
        // the before/after value to confirm the register is writable.
        let cgtt = regs.read_ip(HwIp::Smuio, 0, layout.cgtt, layout.base_idx)?;
        regs.write_ip(
            HwIp::Smuio,
            0,
            layout.cgtt,
            layout.base_idx,
            cgtt | 0x8000_0000 | 0x4000_0000,
        )?;
        let cgtt_after = regs.read_ip(HwIp::Smuio, 0, layout.cgtt, layout.base_idx)?;
        dev_info!(
            "astra: ROM window (idx {}): CGTT {:#x} -> {:#x}, rom_offset {:#x}",
            layout.base_idx,
            cgtt,
            cgtt_after,
            rom_offset,
        );

        // Write the index explicitly before every data read: some SMUIO
        // revisions do not auto-increment the data register.
        let mut dwords = vec![0u32; ROM_SCAN_DWORDS];
        for (i, dword) in dwords.iter_mut().enumerate() {
            regs.write_ip(
                HwIp::Smuio,
                0,
                layout.rom_index,
                layout.base_idx,
                (i as u32) * 4,
            )?;
            *dword = regs.read_ip(HwIp::Smuio, 0, layout.rom_data, layout.base_idx)?;
        }

        let mut bytes = Vec::with_capacity(ROM_SCAN_BYTES);
        for dword in &dwords {
            bytes.extend_from_slice(&dword.to_le_bytes());
        }

        if let Some(offset) = locate_atom(&bytes) {
            let bios = bytes.split_off(offset);
            dev_info!(
                "astra: ATOM header at ROM window offset {:#x} (idx {})",
                offset,
                layout.base_idx,
            );
            return Ok(Self { bios });
        }

        Err(Error::Io)
    }

    /// Locates a data table: returns `(table_offset, frev, crev)` where
    /// `table_offset` is absolute from the ROM start (Linux
    /// `amdgpu_atom_parse_data_header`). The master data table lives at
    /// `usMasterDataTableOffset` (absolute); each `ListOfDataTables` entry
    /// at `+4 + index*2` is itself an absolute table offset.
    pub fn data_table(&self, index: usize) -> Option<(usize, u8, u8)> {
        // `base` = ATOM ROM header offset (value at +0x48).
        let base = self.u16(ATOM_ROM_TABLE_PTR)? as usize;
        // `data_table` = master data table offset (usMasterDataTableOffset,
        // absolute from ROM start).
        let data_table = self.u16(base + ATOM_ROM_DATA_PTR)? as usize;
        // ListOfDataTables[index] = u16 at data_table + 4 + index*2.
        let entry = data_table
            .checked_add(4)?
            .checked_add(index.checked_mul(2)?)?;
        let table_off = self.u16(entry)? as usize;
        if table_off == 0 {
            return None;
        }
        let frev = *self.bios.get(table_off + 2)?;
        let crev = *self.bios.get(table_off + 3)?;
        Some((table_off, frev, crev))
    }

    /// Raw posted VBIOS image copied into DMCUB Window 3, matching
    /// `dm_dmub_hw_init()`.
    pub fn bytes(&self) -> &[u8] {
        &self.bios
    }

    /// Parses the connector/encoder paths used by DC's BIOS parser.  Navi
    /// VBIOSes use displayobjectinfo v1.4 or v1.5; both keep the connector,
    /// record offset and encoder object in the first six bytes of each
    /// 16-byte path.
    pub fn display_paths(&self) -> Vec<DisplayPath> {
        let Some((offset, frev, crev)) = self.data_table(INDEX_DISPLAY_OBJECT_INFO) else {
            dev_info!("astra: ATOM displayobjectinfo table is absent");
            return Vec::new();
        };
        let table_size = self.u16(offset).unwrap_or(0) as usize;
        let count = self.bios.get(offset + 6).copied().unwrap_or(0) as usize;
        dev_info!(
            "astra: ATOM displayobjectinfo v{}.{} at {:#x}, size {}, paths {}",
            frev,
            crev,
            offset,
            table_size,
            count,
        );
        if frev != 1 || !matches!(crev, 4 | 5) {
            dev_info!("astra: unsupported ATOM displayobjectinfo revision");
            return Vec::new();
        }
        let mut paths = Vec::new();
        for index in 0..count {
            let Some(path) = (offset + 8).checked_add(index * 16) else {
                break;
            };
            // Linux indexes display_path[] from number_of_path and follows
            // record offsets relative to the object table. Those records can
            // sit beyond the table header's structuresize, so only enforce
            // the actual BIOS image boundary here.
            let Some(path_end) = path.checked_add(16) else {
                break;
            };
            if path_end > self.bios.len() {
                break;
            }
            let Some(display_objid) = self.u16(path) else {
                continue;
            };
            let Some(record_offset) = self.u16(path + 2) else {
                continue;
            };
            let Some(encoder_objid) = self.u16(path + 4) else {
                continue;
            };
            if display_objid == 0 || encoder_objid == 0 {
                continue;
            }

            let connector_obj_id = display_objid as u8;
            let connector_enum_id = ((display_objid >> 8) & 0x0f) as u8;
            let encoder_obj_id = encoder_objid as u8;
            let encoder_enum_id = ((encoder_objid >> 8) & 0x0f) as u8;
            dev_info!(
                "astra: ATOM ObjectID v2 path {} raw connector={:#06x} (id={:#04x}, enum={}), encoder={:#06x} (id={:#04x}, enum={}), records={:#x}",
                index,
                display_objid,
                connector_obj_id,
                connector_enum_id,
                encoder_objid,
                encoder_obj_id,
                encoder_enum_id,
                record_offset,
            );
            // Linux link_factory.c translates ObjectID.h UNIPHY, UNIPHY1
            // and UNIPHY2 objects to transmitters A/B, C/D and E/F.
            let phy_base = match encoder_obj_id {
                ENCODER_OBJECT_ID_INTERNAL_UNIPHY => 0,
                ENCODER_OBJECT_ID_INTERNAL_UNIPHY1 => 2,
                ENCODER_OBJECT_ID_INTERNAL_UNIPHY2 => 4,
                _ => {
                    dev_info!(
                        "astra: ATOM display path {} has unsupported encoder id {:#04x} (raw {:#06x})",
                        index,
                        encoder_obj_id,
                        encoder_objid,
                    );
                    continue;
                }
            };
            if !(1..=2).contains(&encoder_enum_id) {
                dev_info!(
                    "astra: ATOM display path {} has unsupported encoder enum {} (raw {:#06x})",
                    index,
                    encoder_enum_id,
                    encoder_objid,
                );
                continue;
            }
            let phy_id = phy_base + encoder_enum_id - 1;
            let hpd_pin_id = self
                .hpd_pin_from_records(offset, record_offset as usize)
                .unwrap_or(0);
            let hpd_sel = self.hpd_sel_from_gpio(hpd_pin_id).unwrap_or(0);
            paths.push(DisplayPath {
                connector_obj_id,
                connector_enum_id,
                encoder_obj_id,
                encoder_enum_id,
                phy_id,
                hpd_sel,
                hpd_pin_id,
            });
        }
        paths
    }

    fn hpd_pin_from_records(&self, object_table: usize, record_offset: usize) -> Option<u8> {
        let mut at = object_table.checked_add(record_offset)?;
        for _ in 0..256 {
            if at + 2 > self.bios.len() {
                return None;
            }
            let record_type = self.bios[at];
            let record_size = self.bios[at + 1] as usize;
            if record_type == 0xff || record_size == 0 {
                return None;
            }
            let end = at.checked_add(record_size)?;
            if end > self.bios.len() {
                return None;
            }
            if record_type == 2 && record_size >= 4 {
                return self.bios.get(at + 2).copied();
            }
            at = end;
        }
        None
    }

    fn hpd_sel_from_gpio(&self, pin_id: u8) -> Option<u8> {
        if pin_id == 0 {
            return None;
        }
        let (offset, frev, crev) = self.data_table(INDEX_GPIO_PIN_LUT)?;
        if frev != 2 || crev != 1 {
            return None;
        }
        let size = self.u16(offset)? as usize;
        let end = offset.checked_add(size)?;
        if end > self.bios.len() || size < 12 {
            return None;
        }
        let hpd_a = 0x34c0u32 + crate::regs::dcn3_0_2::mmDC_GPIO_HPD_A;
        for at in (offset + 4..end).step_by(8) {
            if at + 8 > end || self.bios[at + 6] != pin_id {
                continue;
            }
            let data_a = self.u32(at)?;
            let bit = self.bios[at + 4];
            if data_a != hpd_a || bit >= 32 {
                return None;
            }
            return match 1u32 << bit {
                0x0000_0001 => Some(1),
                0x0000_0100 => Some(2),
                0x0001_0000 => Some(3),
                0x0100_0000 => Some(4),
                0x0400_0000 => Some(5),
                0x1000_0000 => Some(6),
                _ => None,
            };
        }
        None
    }

    /// Parses the `firmwareinfo` table (v3.1 layout; the prefix is
    /// identical through v3.4).
    pub fn firmware_info(&self) -> Option<FirmwareInfo> {
        let (offset, frev, crev) = self.data_table(INDEX_FIRMWARE_INFO)?;
        if frev != 3 || !(1..=4).contains(&crev) {
            return None;
        }
        let mut info = FirmwareInfo::default();
        // atom_firmware_info_v3_1: firmware_revision(+4),
        // bootup_sclk/mclk_in10khz(+8/+12), firmware_capability(+16).
        info.bootup_sclk_khz = self.u32(offset + 8)?.wrapping_mul(10);
        info.bootup_mclk_khz = self.u32(offset + 12)?.wrapping_mul(10);
        info.firmware_capability = self.u32(offset + 16)?;
        // bootup voltages (+28..+34), mem_module_id(+36),
        // coolingsolution_id(+37).
        info.bootup_vddc_mv = self.u16(offset + 28)?;
        info.bootup_vddci_mv = self.u16(offset + 30)?;
        info.bootup_mvddc_mv = self.u16(offset + 32)?;
        info.bootup_vddgfx_mv = self.u16(offset + 34)?;
        info.cooling_solution_id = *self.bios.get(offset + 37)?;
        if crev >= 3 {
            // v3.3 appends pplib_pptable_id at +60.
            info.pplib_pptable_id = self.u32(offset + 60)?;
        }
        if crev >= 4 {
            // v3.4 keeps this field at +84.  Linux reserves it before TTM
            // allocates the PSP TMR or any other top-down VRAM BO.
            info.fw_reserved_size = (self.u32(offset + 84)? as u64) << 10;
        }
        Some(info)
    }

    /// Builds the SMU 11.0.7 `PPTable_t` exactly as Linux's Sienna Cichlid
    /// path does in `sienna_cichlid_store_powerplay_table()` followed by
    /// `sienna_cichlid_append_powerplay_table()`.
    ///
    /// The ATOM PowerPlay table is an outer driver container; only its
    /// trailing `smc_pptable` member is sent to PMFW.  Board-specific I2C,
    /// telemetry, GPIO and spread-spectrum fields then come from the
    /// separate `atom_smc_dpm_info_v4_9` table.
    pub fn powerplay_info(&self) -> Option<PowerplayInfo> {
        let (offset, frev, crev) = self.data_table(INDEX_POWERPLAY_INFO)?;
        let atom_table_size = self.u16(offset)? as usize;

        // `struct smu_11_0_7_powerplay_table::table_size` is at +5 in the
        // packed structure.  Despite its name it is the byte offset from
        // the table header to `smc_pptable` (Linux header comment).
        let smc_pptable_offset = self.u16(offset + 5)? as usize;
        let smc_start = offset.checked_add(smc_pptable_offset)?;
        let smc_end = smc_start.checked_add(SIENNA_CICHLID_PPTABLE_SIZE)?;
        if smc_pptable_offset.checked_add(SIENNA_CICHLID_PPTABLE_SIZE)? > atom_table_size {
            return None;
        }
        let mut bytes = self.bios.get(smc_start..smc_end)?.to_vec();

        let (dpm_offset, dpm_frev, dpm_crev) = self.data_table(INDEX_SMC_DPM_INFO)?;
        let dpm_size = self.u16(dpm_offset)? as usize;
        if dpm_size < SMC_DPM_V4_9_SIZE {
            return None;
        }
        let dpm_board_start = dpm_offset.checked_add(SMC_DPM_I2C_CONTROLLERS_OFFSET)?;
        let dpm_board_end = dpm_board_start.checked_add(SMC_DPM_BOARD_DATA_SIZE)?;
        let dpm_board = self.bios.get(dpm_board_start..dpm_board_end)?;
        let pp_board_end = PPTABLE_I2C_CONTROLLERS_OFFSET.checked_add(SMC_DPM_BOARD_DATA_SIZE)?;
        bytes
            .get_mut(PPTABLE_I2C_CONTROLLERS_OFFSET..pp_board_end)?
            .copy_from_slice(dpm_board);

        Some(PowerplayInfo {
            bytes,
            format_revision: frev,
            content_revision: crev,
            atom_table_size,
            smc_pptable_offset,
            smc_dpm_format_revision: dpm_frev,
            smc_dpm_content_revision: dpm_crev,
        })
    }

    /// Parses the `vram_info` table (v2.3, module 0).
    pub fn vram_info(&self) -> Option<VramInfo> {
        let (offset, frev, crev) = self.data_table(INDEX_VRAM_INFO)?;
        if frev != 2 || crev != 3 {
            return None;
        }
        // v2.3 header: table_header(4) + 8*u16 offsets + module_num.
        let module_num = *self.bios.get(offset + 20)?;
        if module_num == 0 {
            return None;
        }
        // atom_vram_module_v9 at offset 24.
        let module = offset + 24;
        Some(VramInfo {
            memory_type: *self.bios.get(module + 12)?,
            channel_num: *self.bios.get(module + 13)?,
            channel_width: *self.bios.get(module + 14)?,
        })
    }

    fn u16(&self, offset: usize) -> Option<u16> {
        let bytes = self.bios.get(offset..offset + 2)?;
        Some(u16::from_le_bytes([bytes[0], bytes[1]]))
    }

    fn u32(&self, offset: usize) -> Option<u32> {
        let bytes = self.bios.get(offset..offset + 4)?;
        Some(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// Builds an ATOM command-table interpreter for this VBIOS.
    pub fn cmd_ctx(&self) -> Option<CmdCtx> {
        CmdCtx::new(&self.bios)
    }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ATOM command-table interpreter (Linux `amdgpu/atom.c`).
//
// The VBIOS encodes display bring-up (HDMI/DP PHY, encoders) as bytecode
// command tables. Linux runs them through `amdgpu_atom_execute_table`; this
// mirrors that interpreter so `digxencodercontrol` / `dig1transmittercontrol`
// can enable the physical output without re-implementing the register dance.
// ---------------------------------------------------------------------------

use core::time::Duration;

use na_std::time;

/// ATOM ROM header pointers (amdgpu/atom.h).
const ROM_TABLE_PTR: usize = 0x48;
const ROM_CMD_PTR: usize = 0x1e;
const ROM_DATA_PTR: usize = 0x20;
const DATA_IIO_PTR: usize = 0x32;

/// Command-table header layout.
const CT_SIZE_PTR: usize = 0;
const CT_WS_PTR: usize = 4;
const CT_PS_PTR: usize = 5;
const CT_PS_MASK: u32 = 0x7f;
const CT_CODE_PTR: usize = 6;

/// Argument kinds (atom.h).
const ARG_REG: u32 = 0;
const ARG_PS: u32 = 1;
const ARG_WS: u32 = 2;
const ARG_FB: u32 = 3;
const ARG_ID: u32 = 4;
const ARG_IMM: u32 = 5;
const ARG_PLL: u32 = 6;
const ARG_MC: u32 = 7;

const SRC_DWORD: u32 = 0;
const SRC_WORD0: u32 = 1;
const SRC_WORD8: u32 = 2;
const SRC_WORD16: u32 = 3;
const SRC_BYTE0: u32 = 4;
const SRC_BYTE8: u32 = 5;
const SRC_BYTE16: u32 = 6;
const SRC_BYTE24: u32 = 7;

const IO_MM: u32 = 0;
const IO_IIO: u32 = 0x80;

const WS_QUOTIENT: u32 = 0x40;
const WS_REMAINDER: u32 = 0x41;
const WS_DATAPTR: u32 = 0x42;
const WS_SHIFT: u32 = 0x43;
const WS_OR_MASK: u32 = 0x44;
const WS_AND_MASK: u32 = 0x45;
const WS_FB_WINDOW: u32 = 0x46;
const WS_ATTRIBUTES: u32 = 0x47;
const WS_REGPTR: u32 = 0x48;

const COND_ABOVE: u32 = 0;
const COND_ABOVEOREQUAL: u32 = 1;
const COND_ALWAYS: u32 = 2;
const COND_BELOW: u32 = 3;
const COND_BELOWOREQUAL: u32 = 4;
const COND_EQUAL: u32 = 5;
const COND_NOTEQUAL: u32 = 6;

const PORT_ATI: u32 = 0;
const PORT_PCI: u32 = 1;
const PORT_SYSIO: u32 = 2;

const UNIT_MICROSEC: u32 = 0;
const UNIT_MILLISEC: u32 = 1;

const CASE_MAGIC: u8 = 0x63;
const CASE_END: u16 = 0x5a5a;

const IIO_NOP: u8 = 0;
const IIO_START: u8 = 1;
const IIO_READ: u8 = 2;
const IIO_WRITE: u8 = 3;
const IIO_CLEAR: u8 = 4;
const IIO_SET: u8 = 5;
const IIO_MOVE_INDEX: u8 = 6;
const IIO_MOVE_ATTR: u8 = 7;
const IIO_MOVE_DATA: u8 = 8;
const IIO_END: u8 = 9;
const IIO_LEN: [usize; 10] = [1, 2, 3, 3, 3, 3, 4, 4, 4, 3];

const ARG_MASK: [u32; 8] = [
    0xffff_ffff,
    0xffff,
    0xffff_00,
    0xffff_0000,
    0xff,
    0xff00,
    0xff_0000,
    0xff00_0000,
];
const ARG_SHIFT: [u32; 8] = [0, 0, 8, 16, 0, 8, 16, 24];
const DST_TO_SRC: [[u32; 4]; 8] = [
    [0, 0, 0, 0],
    [1, 2, 3, 0],
    [1, 2, 3, 0],
    [1, 2, 3, 0],
    [4, 5, 6, 7],
    [4, 5, 6, 7],
    [4, 5, 6, 7],
    [4, 5, 6, 7],
];
const DEF_DST: [u32; 8] = [0, 0, 1, 2, 0, 1, 2, 3];

const EXECUTE_MAX_DEPTH: usize = 32;
/// Guard against infinite jump loops (Linux aborts after a timeout).
const MAX_JUMPS: usize = 1_000_000;

/// Command-table interpreter state (Linux `atom_context` + `atom_exec_context`).
pub struct CmdCtx {
    bios: Vec<u8>,
    cmd_table: u32,
    data_table: u32,
    iio: [u32; 256],
    /// FB-window scratch region.
    scratch: Vec<u32>,
    /// Parameter stack (dword array passed to the outermost table).
    ps: Vec<u32>,
    /// Working-stack dwords.
    ws: Vec<u32>,
    reg_block: u32,
    fb_base: u32,
    divmul: [u32; 2],
    data_block: u32,
    io_attr: u32,
    shift: u32,
    cs_equal: bool,
    cs_above: bool,
    io_mode: u32,
    depth: usize,
}

impl CmdCtx {
    /// Builds the interpreter from the VBIOS (Linux `amdgpu_atom_parse`).
    pub fn new(bios: &[u8]) -> Option<Self> {
        let u16 = |off: usize| -> Option<u16> {
            let b = bios.get(off..off + 2)?;
            Some(u16::from_le_bytes([b[0], b[1]]))
        };
        let base = u16(ROM_TABLE_PTR)? as usize;
        if bios.get(base + 4..base + 8)? != b"ATOM" {
            return None;
        }
        let cmd_table = u16(base + ROM_CMD_PTR)? as u32;
        let data_table = u16(base + ROM_DATA_PTR)? as u32;
        let mut iio = [0u32; 256];
        let mut iio_base = u16(data_table as usize + DATA_IIO_PTR)? as usize + 4;
        while bios.get(iio_base).copied() == Some(IIO_START) {
            iio[bios[iio_base + 1] as usize] = (iio_base + 2) as u32;
            iio_base += 2;
            while bios.get(iio_base).copied() != Some(IIO_END) {
                let op = bios[iio_base] as usize;
                iio_base += IIO_LEN.get(op).copied().unwrap_or(1);
            }
            iio_base += 3;
        }
        Some(Self {
            bios: bios.to_vec(),
            cmd_table,
            data_table,
            iio,
            scratch: alloc::vec![0u32; 4096],
            ps: Vec::new(),
            ws: Vec::new(),
            reg_block: 0,
            fb_base: 0,
            divmul: [0; 2],
            data_block: 0,
            io_attr: 0,
            shift: 0,
            cs_equal: false,
            cs_above: false,
            io_mode: IO_MM,
            depth: 0,
        })
    }

    /// Command indexes into `atom_master_list_of_command_functions_v2_1`
    /// (atomfirmware.h), i.e. `offsetof(field) / 2`.
    pub const fn cmd_digx_encoder_control() -> usize {
        4
    }
    pub const fn cmd_dig1_transmitter_control() -> usize {
        76
    }

    fn u8(&self, off: usize) -> u8 {
        self.bios.get(off).copied().unwrap_or(0)
    }
    fn u16(&self, off: usize) -> u16 {
        let b = self.bios.get(off..off + 2).unwrap_or(&[0, 0]);
        u16::from_le_bytes([b[0], b[1]])
    }
    fn u32(&self, off: usize) -> u32 {
        let b = self.bios.get(off..off + 4).unwrap_or(&[0, 0, 0, 0]);
        u32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    /// IIO interpreter (Linux `atom_iio_execute`).
    fn iio_execute(&mut self, regs: &mut Regs, base: usize, index: u32, data: u32) -> u32 {
        let mut base = base;
        let mut temp = 0xCDCD_CDCDu32;
        loop {
            match self.u8(base) {
                IIO_NOP => base += 1,
                IIO_READ => {
                    temp = regs.read_full(self.u16(base + 1) as u32).unwrap_or(0);
                    base += 3;
                }
                IIO_WRITE => {
                    let _ = regs.write_full(self.u16(base + 1) as u32, temp);
                    base += 3;
                }
                IIO_CLEAR => {
                    let width = self.u8(base + 1) as u32;
                    let shift = self.u8(base + 2) as u32;
                    temp &= !((0xffff_ffffu32 >> (32 - width)) << shift);
                    base += 3;
                }
                IIO_SET => {
                    let width = self.u8(base + 1) as u32;
                    let shift = self.u8(base + 2) as u32;
                    temp |= (0xffff_ffffu32 >> (32 - width)) << shift;
                    base += 3;
                }
                IIO_MOVE_INDEX | IIO_MOVE_ATTR | IIO_MOVE_DATA => {
                    let width = self.u8(base + 1) as u32;
                    let src_shift = self.u8(base + 2) as u32;
                    let dst_shift = self.u8(base + 3) as u32;
                    let value = match self.u8(base) {
                        IIO_MOVE_INDEX => index,
                        IIO_MOVE_ATTR => self.io_attr,
                        _ => data,
                    };
                    temp &= !((0xffff_ffffu32 >> (32 - width)) << dst_shift);
                    temp |= ((value >> src_shift) & (0xffff_ffffu32 >> (32 - width))) << dst_shift;
                    base += 4;
                }
                IIO_END => return temp,
                _ => return 0,
            }
        }
    }

    /// Linux `atom_get_src_int`: resolve one argument, apply align mask/shift.
    fn get_src(
        &mut self,
        regs: &mut Regs,
        attr: u8,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) -> u32 {
        let arg = (attr & 7) as u32;
        let align = ((attr >> 3) & 7) as u32;
        let mut val = 0xCDCD_CDCDu32;
        match arg {
            ARG_REG => {
                let idx = self.u16(*ptr) as u32;
                *ptr += 2;
                match self.io_mode {
                    IO_MM => {
                        val = regs
                            .read_full(idx.wrapping_add(self.reg_block))
                            .unwrap_or(0);
                    }
                    _ => {
                        let iio = self
                            .iio
                            .get((self.io_mode & 0x7f) as usize)
                            .copied()
                            .unwrap_or(0);
                        if iio != 0 {
                            val = self.iio_execute(
                                regs,
                                iio as usize,
                                idx.wrapping_add(self.reg_block),
                                0,
                            );
                        }
                    }
                }
            }
            ARG_PS => {
                let idx = self.u8(*ptr) as usize;
                *ptr += 1;
                if idx < ps_len {
                    val = self.ps.get(ps_base + idx).copied().unwrap_or(0);
                }
            }
            ARG_WS => {
                let idx = self.u8(*ptr) as u32;
                *ptr += 1;
                val = match idx {
                    WS_QUOTIENT => self.divmul[0],
                    WS_REMAINDER => self.divmul[1],
                    WS_DATAPTR => self.data_block,
                    WS_SHIFT => self.shift,
                    WS_OR_MASK => 1u32 << self.shift,
                    WS_AND_MASK => !(1u32 << self.shift),
                    WS_FB_WINDOW => self.fb_base,
                    WS_ATTRIBUTES => self.io_attr,
                    WS_REGPTR => self.reg_block,
                    _ => self.ws.get(idx as usize).copied().unwrap_or(0),
                };
            }
            ARG_ID => {
                let idx = self.u16(*ptr) as u32;
                *ptr += 2;
                val = self.u32(idx.wrapping_add(self.data_block) as usize);
            }
            ARG_FB => {
                let idx = self.u8(*ptr) as usize;
                *ptr += 1;
                val = self
                    .scratch
                    .get((self.fb_base as usize / 4) + idx)
                    .copied()
                    .unwrap_or(0);
            }
            ARG_IMM => {
                match align {
                    SRC_DWORD => {
                        val = self.u32(*ptr);
                        *ptr += 4;
                    }
                    SRC_WORD0 | SRC_WORD8 | SRC_WORD16 => {
                        val = self.u16(*ptr) as u32;
                        *ptr += 2;
                    }
                    _ => {
                        val = self.u8(*ptr) as u32;
                        *ptr += 1;
                    }
                }
                return val;
            }
            ARG_PLL | ARG_MC => {
                *ptr += 1;
            }
            _ => {}
        }
        val &= ARG_MASK[align as usize];
        val >>= ARG_SHIFT[align as usize];
        val
    }

    /// Linux `atom_put_dst`.
    fn put_dst(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        attr: u8,
        ptr: &mut usize,
        val: u32,
        saved: u32,
        ps_base: usize,
        ps_len: usize,
    ) {
        let align = DST_TO_SRC[((attr >> 3) & 7) as usize][((attr >> 6) & 3) as usize];
        let mut val = val;
        val <<= ARG_SHIFT[align as usize];
        val &= ARG_MASK[align as usize];
        val |= saved & !ARG_MASK[align as usize];
        match arg {
            ARG_REG => {
                let idx = self.u16(*ptr) as u32;
                *ptr += 2;
                match self.io_mode {
                    IO_MM => {
                        let addr = idx.wrapping_add(self.reg_block);
                        // Register 0 is the MMIO window index (<<2).
                        let value = if addr == 0 { val << 2 } else { val };
                        let _ = regs.write_full(addr, value);
                    }
                    _ => {
                        let iio = self
                            .iio
                            .get((self.io_mode & 0x7f) as usize)
                            .copied()
                            .unwrap_or(0);
                        if iio != 0 {
                            self.iio_execute(
                                regs,
                                iio as usize,
                                idx.wrapping_add(self.reg_block),
                                val,
                            );
                        }
                    }
                }
            }
            ARG_PS => {
                let idx = self.u8(*ptr) as usize;
                *ptr += 1;
                if idx < ps_len
                    && let Some(slot) = self.ps.get_mut(ps_base + idx)
                {
                    *slot = val;
                }
            }
            ARG_WS => {
                let idx = self.u8(*ptr) as u32;
                *ptr += 1;
                match idx {
                    WS_QUOTIENT => self.divmul[0] = val,
                    WS_REMAINDER => self.divmul[1] = val,
                    WS_DATAPTR => self.data_block = val,
                    WS_SHIFT => self.shift = val,
                    WS_FB_WINDOW => self.fb_base = val,
                    WS_ATTRIBUTES => self.io_attr = val,
                    WS_REGPTR => self.reg_block = val,
                    _ => {
                        if let Some(slot) = self.ws.get_mut(idx as usize) {
                            *slot = val;
                        }
                    }
                }
            }
            ARG_FB => {
                let idx = self.u8(*ptr) as usize;
                *ptr += 1;
                if let Some(slot) = self.scratch.get_mut((self.fb_base as usize / 4) + idx) {
                    *slot = val;
                }
            }
            _ => {
                // PLL/MC writes are not used by the display command tables.
                let _ = ptr;
            }
        }
    }

    /// Linux `atom_skip_src_int` (destination-encoded).
    fn skip_dst(&self, attr: u8, ptr: &mut usize) {
        let arg = (attr & 7) as u32;
        match arg {
            ARG_REG | ARG_ID => *ptr += 2,
            ARG_IMM => {
                *ptr += match ((attr >> 3) & 7) as u32 {
                    SRC_DWORD => 4,
                    SRC_WORD0 | SRC_WORD8 | SRC_WORD16 => 2,
                    _ => 1,
                };
            }
            _ => *ptr += 1,
        }
    }

    /// Linux `atom_get_dst` (routes through get_src with the dst alignment).
    fn get_dst(
        &mut self,
        regs: &mut Regs,
        _arg: u32,
        attr: u8,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) -> u32 {
        let align = DST_TO_SRC[((attr >> 3) & 7) as usize][((attr >> 6) & 3) as usize];
        let src_attr = (attr & 0xc7) | ((align as u8) << 3);
        self.get_src(regs, src_attr, ptr, ps_base, ps_len)
    }

    /// Executes command table `index` (Linux `amdgpu_atom_execute_table`).
    pub fn execute(&mut self, regs: &mut Regs, index: usize, params: &mut [u32]) -> Result<()> {
        self.ps.clear();
        self.ps.extend_from_slice(params);
        let len = self.ps.len();
        self.execute_at(regs, index, 0, len)
    }

    fn execute_at(
        &mut self,
        regs: &mut Regs,
        index: usize,
        ps_base: usize,
        ps_len: usize,
    ) -> Result<()> {
        if self.depth >= EXECUTE_MAX_DEPTH {
            return Err(Error::Io);
        }
        let base = self.u16(self.cmd_table as usize + 4 + 2 * index) as usize;
        if base == 0 {
            return Err(Error::Io);
        }
        let ws_size = self.u8(base + CT_WS_PTR) as usize;
        let ps_shift = (self.u8(base + CT_PS_PTR) & CT_PS_MASK as u8) as usize / 4;
        let mut ptr = base + CT_CODE_PTR;
        let start = base as u32;
        let saved_ws = core::mem::take(&mut self.ws);
        self.ws = alloc::vec![0u32; ws_size];

        self.depth += 1;
        let mut jumps = 0usize;
        loop {
            let op = self.u8(ptr);
            ptr += 1;
            if op == 0 {
                break;
            }
            let arg = ((op - 1) % 6) as u32;
            match op {
                1..=6 => self.op_move(regs, arg, &mut ptr, ps_base, ps_len),
                7..=12 => self.op_bin(regs, arg, &mut ptr, ps_base, ps_len, |a, b| a & b),
                13..=18 => self.op_bin(regs, arg, &mut ptr, ps_base, ps_len, |a, b| a | b),
                19..=24 => self.op_shift_lr(regs, arg, &mut ptr, ps_base, ps_len, true),
                25..=30 => self.op_shift_lr(regs, arg, &mut ptr, ps_base, ps_len, false),
                31..=36 => self.op_divmul(regs, arg, &mut ptr, ps_base, ps_len, true, false),
                37..=42 => self.op_divmul(regs, arg, &mut ptr, ps_base, ps_len, false, false),
                43..=48 => self.op_bin(regs, arg, &mut ptr, ps_base, ps_len, |a, b| {
                    a.wrapping_add(b)
                }),
                49..=54 => self.op_bin(regs, arg, &mut ptr, ps_base, ps_len, |a, b| {
                    a.wrapping_sub(b)
                }),
                55..=57 => self.op_setport((op - 55) as u32, &mut ptr),
                58 => self.op_setregblock(&mut ptr),
                59 => self.op_setfbbase(regs, &mut ptr, ps_base, ps_len),
                60..=65 => self.op_compare(regs, arg, &mut ptr, ps_base, ps_len),
                66 => self.op_switch(regs, &mut ptr, ps_base, ps_len, start),
                67..=73 => {
                    let cond = (op - 67) as u32;
                    let taken = self.op_jump(cond, &mut ptr, start);
                    if taken {
                        jumps += 1;
                        if jumps > MAX_JUMPS {
                            self.depth -= 1;
                            self.ws = saved_ws;
                            return Err(Error::Io);
                        }
                    }
                }
                74..=79 => self.op_test(regs, arg, &mut ptr, ps_base, ps_len),
                80..=81 => self.op_delay((op - 80) as u32, &mut ptr),
                82 => {
                    self.op_calltable(regs, &mut ptr, ps_base, ps_len, ps_shift)?;
                }
                84..=89 => self.op_clear(regs, arg, &mut ptr, ps_base, ps_len),
                90 | 91 | 99 | 121 | 122 => {}
                92..=97 => self.op_mask(regs, arg, &mut ptr, ps_base, ps_len),
                98 => {
                    ptr += 1;
                }
                100..=101 => {}
                102 => self.op_setdatablock(&mut ptr, start),
                103..=108 => self.op_bin(regs, arg, &mut ptr, ps_base, ps_len, |a, b| a ^ b),
                109..=114 => self.op_shl_shr(regs, arg, &mut ptr, ps_base, ps_len, true),
                115..=120 => self.op_shl_shr(regs, arg, &mut ptr, ps_base, ps_len, false),
                123..=126 => self.op_mul32_div32(regs, op, &mut ptr, ps_base, ps_len),
                _ => break,
            }
            if op == 91 {
                break;
            }
        }
        self.depth -= 1;
        self.ws = saved_ws;
        Ok(())
    }

    fn op_move(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let saved;
        let dptr = *ptr;
        if (((attr >> 3) & 7) as u32) != SRC_DWORD {
            saved = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        } else {
            self.skip_dst(attr, ptr);
            saved = 0xCDCD_CDCD;
        }
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        self.put_dst(regs, arg, attr, &mut { dptr }, src, saved, ps_base, ps_len);
    }

    fn op_bin<F: Fn(u32, u32) -> u32>(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
        f: F,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dptr = *ptr;
        let dst = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        self.put_dst(
            regs,
            arg,
            attr,
            &mut { dptr },
            f(dst, src),
            0,
            ps_base,
            ps_len,
        );
    }

    fn op_shift_lr(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
        left: bool,
    ) {
        let mut attr = self.u8(*ptr);
        *ptr += 1;
        attr &= 0x38;
        attr |= (DEF_DST[(attr >> 3) as usize] as u8) << 6;
        let dptr = *ptr;
        let saved = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let shift = self.u8(*ptr);
        *ptr += 1;
        let dst = if left { saved << shift } else { saved >> shift };
        self.put_dst(regs, arg, attr, &mut { dptr }, dst, saved, ps_base, ps_len);
    }

    fn op_shl_shr(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
        left: bool,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dptr = *ptr;
        let align = DST_TO_SRC[((attr >> 3) & 7) as usize][((attr >> 6) & 3) as usize];
        let saved = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let mut dst = saved;
        let shift = self.get_src(regs, attr, ptr, ps_base, ps_len);
        if left {
            dst = dst.wrapping_shl(shift);
        } else {
            dst = dst.wrapping_shr(shift);
        }
        dst &= ARG_MASK[align as usize];
        dst >>= ARG_SHIFT[align as usize];
        self.put_dst(regs, arg, attr, &mut { dptr }, dst, saved, ps_base, ps_len);
    }

    fn op_divmul(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
        mul: bool,
        _div32: bool,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dst = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        if mul {
            self.divmul[0] = dst.wrapping_mul(src);
        } else if src != 0 {
            self.divmul[0] = dst / src;
            self.divmul[1] = dst % src;
        } else {
            self.divmul = [0, 0];
        }
    }

    fn op_mul32_div32(
        &mut self,
        regs: &mut Regs,
        op: u8,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dst = self.get_dst(regs, op as u32 - 122, attr, ptr, ps_base, ps_len);
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        let value = (dst as u64).wrapping_mul(src as u64);
        if op <= 124 {
            // mul32
            self.divmul[0] = value as u32;
            self.divmul[1] = (value >> 32) as u32;
        } else if src != 0 {
            // div32: (dst | rem<<32) / src
            let wide = (dst as u64) | ((self.divmul[1] as u64) << 32);
            let q = wide / src as u64;
            self.divmul[0] = q as u32;
            self.divmul[1] = (q >> 32) as u32;
        } else {
            self.divmul = [0, 0];
        }
    }

    fn op_compare(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dst = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        self.cs_equal = dst == src;
        self.cs_above = dst > src;
    }

    fn op_test(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dst = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        self.cs_equal = (dst & src) == 0;
    }

    fn op_mask(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let dptr = *ptr;
        let saved = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        let align = ((attr >> 3) & 7) as u32;
        let mask = match align {
            SRC_DWORD => self.u32(*ptr),
            SRC_WORD0 | SRC_WORD8 | SRC_WORD16 => self.u16(*ptr) as u32,
            _ => self.u8(*ptr) as u32,
        };
        *ptr += match align {
            SRC_DWORD => 4,
            SRC_WORD0 | SRC_WORD8 | SRC_WORD16 => 2,
            _ => 1,
        };
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        self.put_dst(
            regs,
            arg,
            attr,
            &mut { dptr },
            (saved & mask) | src,
            saved,
            ps_base,
            ps_len,
        );
    }

    fn op_clear(
        &mut self,
        regs: &mut Regs,
        arg: u32,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
    ) {
        let mut attr = self.u8(*ptr);
        *ptr += 1;
        attr &= 0x38;
        attr |= (DEF_DST[(attr >> 3) as usize] as u8) << 6;
        let dptr = *ptr;
        let saved = self.get_dst(regs, arg, attr, ptr, ps_base, ps_len);
        self.put_dst(regs, arg, attr, &mut { dptr }, 0, saved, ps_base, ps_len);
    }

    fn op_jump(&mut self, cond: u32, ptr: &mut usize, start: u32) -> bool {
        let target = self.u16(*ptr);
        *ptr += 2;
        let execute = match cond {
            COND_ABOVE => self.cs_above,
            COND_ABOVEOREQUAL => self.cs_above || self.cs_equal,
            COND_ALWAYS => true,
            COND_BELOW => !(self.cs_above || self.cs_equal),
            COND_BELOWOREQUAL => !self.cs_above,
            COND_EQUAL => self.cs_equal,
            COND_NOTEQUAL => !self.cs_equal,
            _ => false,
        };
        if execute {
            *ptr = start as usize + target as usize;
            true
        } else {
            false
        }
    }

    fn op_calltable(
        &mut self,
        regs: &mut Regs,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
        ps_shift: usize,
    ) -> Result<()> {
        let idx = self.u8(*ptr) as usize;
        *ptr += 1;
        if self.u16(self.cmd_table as usize + 4 + 2 * idx) != 0 {
            let next_base = ps_base + ps_shift;
            let next_len = ps_len.saturating_sub(ps_shift);
            self.execute_at(regs, idx, next_base, next_len)?;
        }
        Ok(())
    }

    fn op_delay(&mut self, unit: u32, ptr: &mut usize) {
        let count = self.u8(*ptr) as u64;
        *ptr += 1;
        if unit == UNIT_MICROSEC {
            time::delay(Duration::from_micros(count));
        } else {
            time::delay(Duration::from_millis(count));
        }
    }

    fn op_switch(
        &mut self,
        regs: &mut Regs,
        ptr: &mut usize,
        ps_base: usize,
        ps_len: usize,
        start: u32,
    ) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        let src = self.get_src(regs, attr, ptr, ps_base, ps_len);
        while self.u16(*ptr) != CASE_END {
            if self.u8(*ptr) == CASE_MAGIC {
                *ptr += 1;
                let val = self.get_src(regs, (attr & 0x38) | (ARG_IMM as u8), ptr, ps_base, ps_len);
                let target = self.u16(*ptr);
                if val == src {
                    *ptr = start as usize + target as usize;
                    return;
                }
                *ptr += 2;
            } else {
                return;
            }
        }
        *ptr += 2;
    }

    fn op_setport(&mut self, port: u32, ptr: &mut usize) {
        match port {
            PORT_ATI => {
                let port = self.u16(*ptr);
                *ptr += 2;
                if port == 0 {
                    self.io_mode = IO_MM;
                } else {
                    self.io_mode = IO_IIO | port as u32;
                }
            }
            PORT_PCI | PORT_SYSIO => {
                // Unsupported IO modes; consume the byte and keep MMIO.
                *ptr += 1;
            }
            _ => {}
        }
    }

    fn op_setregblock(&mut self, ptr: &mut usize) {
        self.reg_block = self.u16(*ptr) as u32;
        *ptr += 2;
    }

    fn op_setfbbase(&mut self, regs: &mut Regs, ptr: &mut usize, ps_base: usize, ps_len: usize) {
        let attr = self.u8(*ptr);
        *ptr += 1;
        self.fb_base = self.get_src(regs, attr, ptr, ps_base, ps_len);
    }

    fn op_setdatablock(&mut self, ptr: &mut usize, start: u32) {
        let idx = self.u8(*ptr);
        *ptr += 1;
        if idx == 0 {
            self.data_block = 0;
        } else if idx == 255 {
            self.data_block = start;
        } else {
            self.data_block = self.u16(self.data_table as usize + 4 + 2 * idx as usize) as u32;
        }
    }
}

/// One SMUIO ROM-window register layout (index/data/clock-gate and the
/// SMUIO base-address index the window lives in).
#[derive(Clone, Copy)]
struct RomLayout {
    rom_index: u32,
    rom_data: u32,
    cgtt: u32,
    base_idx: usize,
}
