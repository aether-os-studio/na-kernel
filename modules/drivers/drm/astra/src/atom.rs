//! ATOM BIOS / VBIOS access (Linux `amdgpu_bios.c` + `amdgpu_atomfirmware.c`).
//!
//! The video BIOS is read through three sources, mirroring Linux
//! `amdgpu_get_bios_dgpu` order for a discrete GPU: the VRAM shadow (the
//! GPU posts the VBIOS at the start of VRAM when it has been posted), the
//! SMUIO ROM window (register-based, no PCI ROM BAR needed) and the PCI
//! expansion-ROM BAR. The ATOM master data table is then parsed for the
//! tables needed during init: firmware info, powerplay (SMU pptable) and
//! VRAM info.

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
    pub bootup_vddgfx_mv: u16,
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

/// One SMUIO ROM-window register layout (index/data/clock-gate and the
/// SMUIO base-address index the window lives in).
#[derive(Clone, Copy)]
struct RomLayout {
    rom_index: u32,
    rom_data: u32,
    cgtt: u32,
    base_idx: usize,
}

impl AtomBios {
    /// Locates a valid ATOM BIOS image inside `bytes` and returns the byte
    /// offset of its start. Mirrors Linux `check_atom_bios`: a 0x55 0xaa
    /// signature, a non-zero header pointer at `+0x48`, and the 4-byte
    /// `"ATOM"` / `"MOTA"` signature at `header + 4`.
    fn locate(bytes: &[u8]) -> Option<usize> {
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

    /// Builds an `AtomBios` from a raw image by locating the ATOM header
    /// (used by the ATRM and PCI ROM BAR sources, which return the whole
    /// image from offset 0).
    pub fn from_bytes(mut bytes: Vec<u8>) -> Option<Self> {
        let offset = Self::locate(&bytes)?;
        let bios = bytes.split_off(offset);
        Some(Self { bios })
    }

    /// Reads the video BIOS. VRAM shadow first, then the SMUIO ROM window
    /// (Linux `amdgpu_soc15_read_bios_from_rom`). Returns `Err` so the
    /// caller can fall back to the PCI ROM BAR.
    pub fn read(regs: &mut Regs, _smuio_version: IpVersion) -> Result<Self> {
        if let Some(bios) = Self::read_from_vram(regs) {
            dev_info!("astra: VBIOS loaded ({} bytes) from VRAM", bios.bios.len());
            return Ok(bios);
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
        let offset = Self::locate(&bytes)?;
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
        let rom_offset = get_field(
            rom_ctrl,
            crate::regs::nbio4_3_0::REGS_ROM_OFFSET_CTRL__ROM_OFFSET__SHIFT,
            crate::regs::nbio4_3_0::REGS_ROM_OFFSET_CTRL__ROM_OFFSET_MASK,
        ) << 17;

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

        if let Some(offset) = Self::locate(&bytes) {
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
        // atom_firmware_info_v3_1: firmware_revision(+4),
        // bootup_sclk/mclk_in10khz(+8/+12), firmware_capability(+16).
        // bootup voltages (+28..+34), mem_module_id(+36),
        // coolingsolution_id(+37).
        let mut info = FirmwareInfo {
            bootup_sclk_khz: self.u32(offset + 8)?.wrapping_mul(10),
            bootup_mclk_khz: self.u32(offset + 12)?.wrapping_mul(10),
            firmware_capability: self.u32(offset + 16)?,
            bootup_vddc_mv: self.u16(offset + 28)?,
            bootup_vddci_mv: self.u16(offset + 30)?,
            bootup_vddgfx_mv: self.u16(offset + 34)?,
            ..FirmwareInfo::default()
        };
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
}
