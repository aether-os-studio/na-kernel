//! IP discovery table parsing (Linux `amdgpu_discovery.c`).
//!
//! The table lives in the VRAM "top of memory" region programmed by the
//! video BIOS: poll `mmMP0_SMN_C2PMSG_33` for IFWI readiness, read the
//! VRAM size from `mmRCC_CONFIG_MEMSIZE`, then fetch the packed binary
//! from VRAM and decode the per-IP versions and register base addresses.

use alloc::vec;
use alloc::vec::Vec;
use core::time::Duration;

use na_std::time;
use na_std::{Error, Result};

use crate::dev_info;
use crate::ip::{HWIP_COUNT, HwIp, IpVersion, MAX_BASE_ADDR, MAX_INSTANCE};
use crate::regs::Regs;

/// Default discovery TMR size and its distance from the end of VRAM
/// (Linux `DISCOVERY_TMR_SIZE` / `DISCOVERY_TMR_OFFSET`).
const TMR_SIZE: u64 = 10 << 10;
const TMR_OFFSET: u64 = 64 << 10;

/// Raw (base-0) registers used before the table itself is parsed.
const MM_MP0_SMN_C2PMSG_33: u32 = 0x16061;
const MM_RCC_CONFIG_MEMSIZE: u32 = 0xde3;
const MM_DRIVER_SCRATCH_0: u32 = 0x94;
const MM_DRIVER_SCRATCH_1: u32 = 0x95;
const MM_DRIVER_SCRATCH_2: u32 = 0x96;

const BINARY_SIGNATURE: u32 = 0x2821_1407;
const IP_DISCOVERY_SIGNATURE: u32 = 0x5344_5049;
const TABLE_IP_DISCOVERY: usize = 0;
const TABLE_GC: usize = 1;
const TABLE_HARVEST: usize = 2;
const HARVEST_TABLE_SIGNATURE: u32 = 0x5652_4148;

/// Bounds-checked little-endian reader over the raw discovery binary.
struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Self { buf, pos: 0 }
    }

    fn u8(&mut self) -> Result<u8> {
        let value = *self.buf.get(self.pos).ok_or(Error::Range)?;
        self.pos += 1;
        Ok(value)
    }

    fn u16(&mut self) -> Result<u16> {
        let bytes = self.buf.get(self.pos..self.pos + 2).ok_or(Error::Range)?;
        self.pos += 2;
        Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
    }

    fn u32(&mut self) -> Result<u32> {
        let bytes = self.buf.get(self.pos..self.pos + 4).ok_or(Error::Range)?;
        self.pos += 4;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    fn skip(&mut self, count: usize) -> Result<()> {
        self.pos = self.pos.checked_add(count).ok_or(Error::Range)?;
        if self.pos > self.buf.len() {
            return Err(Error::Range);
        }
        Ok(())
    }

    fn seek(&mut self, pos: usize) -> Result<()> {
        if pos > self.buf.len() {
            return Err(Error::Range);
        }
        self.pos = pos;
        Ok(())
    }
}

/// The discovery table result: per-IP versions and register base
/// addresses (indexed `[ip][instance][base_idx]`).
pub struct Discovery {
    pub ip_versions: [[u32; MAX_INSTANCE]; HWIP_COUNT],
    /// GC config from the GC info table (amdgpu_discovery_get_gfx_info).
    pub gfx_info: GfxInfo,
}

/// GC config fields (discovery GC table, struct gc_info_v1_x) plus the
/// runtime masks populated by GFX initialization.  These are the same
/// values Linux publishes in `drm_amdgpu_info_device`.
#[derive(Clone, Copy, Debug, Default)]
pub struct GfxInfo {
    /// Linux `adev->gfx.config.gb_addr_config`, cached for the whitelisted
    /// AMDGPU_INFO_READ_MMR_REG userspace query.
    pub gb_addr_config: u32,
    pub max_shader_engines: u32,
    pub max_sh_per_se: u32,
    pub max_backends_per_se: u32,
    pub max_cu_per_sh: u32,
    pub max_texture_channel_caches: u32,
    pub max_gprs: u32,
    pub max_gs_threads: u32,
    pub gs_vgt_table_depth: u32,
    pub gs_prim_buffer_depth: u32,
    pub double_offchip_lds_buf: u32,
    pub wave_front_size: u32,
    pub max_hw_contexts: u32,
    pub backend_enable_mask: u32,
    pub cu_active_number: u32,
    pub cu_bitmap: [[u32; 4]; 4],
    pub cu_ao_bitmap: [[u32; 4]; 4],
    pub pa_sc_tile_steering_override: u32,
    pub gc_tcp_l1_size: u32,
    pub gc_num_sqc_per_wgp: u32,
    pub gc_l1_instruction_cache_size_per_sqc: u32,
    pub gc_l1_data_cache_size_per_sqc: u32,
    pub gc_gl1c_per_sa: u32,
    pub gc_gl1c_size_per_instance: u32,
    pub gc_gl2c_per_gpu: u32,
}

impl Discovery {
    /// Byte-wise sum truncated to 16 bits (Linux
    /// `amdgpu_discovery_calculate_checksum`).
    fn checksum(data: &[u8]) -> u32 {
        data.iter()
            .fold(0u16, |acc, byte| acc.wrapping_add(*byte as u16)) as u32
    }
}

impl Discovery {
    /// Reads and parses the IP discovery table, filling the register base
    /// offsets in `regs` and returning the per-IP versions.
    pub fn read(regs: &mut Regs) -> Result<Self> {
        // Wait for IFWI init (can take up to 2 s on some dGPUs).
        for _ in 0..2000 {
            let msg = regs.read_raw(MM_MP0_SMN_C2PMSG_33)?;
            if msg & 0x8000_0000 != 0 {
                break;
            }
            time::delay(Duration::from_millis(1));
        }

        let vram_size = regs.read_raw(MM_RCC_CONFIG_MEMSIZE)?;
        if vram_size == u32::MAX {
            return Err(Error::NoDevice);
        }

        // Preferred: exact location from the driver scratch registers.
        let mut size = TMR_SIZE;
        let mut offset = ((vram_size as u64) << 20).saturating_sub(TMR_OFFSET);
        let scratch_size = regs.read_raw(MM_DRIVER_SCRATCH_2)?;
        if scratch_size != 0 {
            size = scratch_size as u64;
            let lo = regs.read_raw(MM_DRIVER_SCRATCH_0)? as u64;
            let hi = regs.read_raw(MM_DRIVER_SCRATCH_1)? as u64;
            offset = hi << 32 | lo;
        }

        let mut dwords = vec![0u32; (size as usize) / 4];
        regs.vram_read_dwords(offset, &mut dwords)?;
        let mut binary = Vec::with_capacity(size as usize);
        for dword in &dwords {
            binary.extend_from_slice(&dword.to_le_bytes());
        }
        let mut reader = Reader::new(&binary);

        let signature = reader.u32()?;
        if signature != BINARY_SIGNATURE {
            dev_info!(
                "astra: invalid ip discovery binary signature {:#x}",
                signature
            );
            return Err(Error::Io);
        }
        let version_major = reader.u16()?;
        let version_minor = reader.u16()?;
        let _binary_checksum = reader.u16()?;
        let binary_size = reader.u16()?;
        dev_info!(
            "astra: ip discovery binary v{}.{}, {} bytes",
            version_major,
            version_minor,
            binary_size,
        );
        let mut table_list = [(0u16, 0u16, 0u16); 6];
        for entry in &mut table_list {
            entry.0 = reader.u16()?;
            entry.1 = reader.u16()?;
            entry.2 = reader.u16()?;
            reader.skip(2)?;
        }

        let mut discovery = Discovery {
            ip_versions: [[0; MAX_INSTANCE]; HWIP_COUNT],
            gfx_info: GfxInfo::default(),
        };

        for (id, offset, checksum, table_size) in table_list
            .into_iter()
            .enumerate()
            .map(|(id, e)| (id, e.0, e.1, e.2))
        {
            if table_size == 0 {
                continue;
            }
            let start = offset as usize;
            let end = start
                .checked_add(table_size as usize)
                .filter(|e| *e <= binary_size as usize && *e <= binary.len())
                .ok_or(Error::Range)?;
            let table = &binary[start..end];
            let calculated = Self::checksum(table);
            dev_info!(
                "astra: ip discovery table {} at {:#x}, {} bytes, checksum {:#06x} (calculated {:#06x})",
                id,
                start,
                table_size,
                checksum,
                calculated,
            );
            if calculated != checksum as u32 {
                dev_info!("astra: ip discovery table {} checksum mismatch", id);
                return Err(Error::Io);
            }
            match id {
                TABLE_IP_DISCOVERY => Self::parse_ip_table(table, start, regs, &mut discovery)?,
                TABLE_GC => discovery.gfx_info = GfxInfo::read(table),
                TABLE_HARVEST => Self::parse_harvest_table(table),
                // MALL / VCN / NPS info tables are only needed for feature
                // masks; parsed lazily by the consumers.
                _ => {}
            }
        }

        let mut found = 0;
        for ip in 0..HWIP_COUNT {
            for inst in 0..MAX_INSTANCE {
                if discovery.ip_versions[ip][inst] != 0 {
                    found += 1;
                    let version = IpVersion::from_full(discovery.ip_versions[ip][inst]);
                    dev_info!(
                        "astra: ip block {} version {}.{}.{}.{}.{}",
                        HwIp::from_index(ip).name(),
                        version.major,
                        version.minor,
                        version.revision,
                        version.variant,
                        version.subrev,
                    );
                }
            }
        }
        dev_info!("astra: {} ip versions discovered", found);

        Ok(discovery)
    }
}

impl Discovery {
    fn parse_ip_table(
        table: &[u8],
        table_offset: usize,
        regs: &mut Regs,
        discovery: &mut Self,
    ) -> Result<()> {
        let mut reader = Reader::new(table);
        let signature = reader.u32()?;
        if signature != IP_DISCOVERY_SIGNATURE {
            dev_info!(
                "astra: ip discovery table signature {:#x} (expected {:#x})",
                signature,
                IP_DISCOVERY_SIGNATURE,
            );
            return Err(Error::Io);
        }
        let version = reader.u16()?;
        let _size = reader.u16()?;

        reader.skip(4)?;
        let num_dies = reader.u16()?;
        let mut dies = [(0u16, 0u16); 16];
        for die in &mut dies {
            die.0 = reader.u16()?;
            die.1 = reader.u16()?;
        }
        dev_info!(
            "astra: ip discovery header v{}, size {}, num_dies {}, die0 = {{id {}, offset {:#x}}}",
            version,
            _size,
            num_dies,
            dies[0].0,
            dies[0].1,
        );
        let base_addr_64_bit = if version >= 4 {
            reader.u16()? & 0x1 != 0
        } else {
            false
        };

        let num_dies = usize::from(num_dies);
        if num_dies > dies.len() {
            return Err(Error::Range);
        }
        for (i, die) in dies.iter().take(num_dies).enumerate() {
            // Die offsets are relative to the binary start, not the table
            // (Linux: discovery_bin + die_offset).
            let die_offset = die.1 as usize;
            let die_pos = die_offset.checked_sub(table_offset).ok_or(Error::Range)?;
            reader.seek(die_pos)?;
            let die_id = reader.u16()?;
            let num_ips = reader.u16()?;
            dev_info!(
                "astra: die {} at {:#x}: die_id {}, num_ips {}",
                i,
                die_offset,
                die_id,
                num_ips,
            );
            if die_id as usize != i {
                dev_info!("astra: invalid die id {}, expected {}", die_id, i);
                return Err(Error::Io);
            }
            for _ in 0..num_ips {
                let hw_id = reader.u16()?;
                let instance = reader.u8()?;
                let num_base = reader.u8()?;
                let major = reader.u8()?;
                let minor = reader.u8()?;
                let revision = reader.u8()?;
                // `ip_v4` always carries one sub_revision:4/variant:4 byte at
                // offset 7; it is only meaningful for discovery version >= 3
                // (Linux amdgpu_discovery.c:1655). Reading it (rather than
                // skipping) keeps the base-address stream aligned for v4.
                let sv = reader.u8()?;
                let (subrev, variant) = if version >= 3 {
                    (sv & 0xf, sv >> 4)
                } else {
                    (0, 0)
                };

                let mut bases = [0u32; MAX_BASE_ADDR];
                for base in bases.iter_mut().take(num_base as usize) {
                    *base = if base_addr_64_bit {
                        // Truncate the 64-bit base: bits > 32 follow an
                        // ASIC-specific format; the low 30 bits are the
                        // dword base address (Linux discovery.c:1625).
                        let lo = reader.u32()?;
                        let _hi = reader.u32()?;
                        lo & 0x3FFF_FFFF
                    } else {
                        reader.u32()?
                    };
                }

                dev_info!(
                    "astra: ip entry hw_id {} ({}) inst {} v{}.{}.{} bases {}",
                    hw_id,
                    HwIp::from_hardware_id(hw_id)
                        .map(|ip| ip.name())
                        .unwrap_or("?"),
                    instance,
                    major,
                    minor,
                    revision,
                    num_base,
                );
                let Some(ip) = HwIp::from_hardware_id(hw_id) else {
                    continue;
                };
                let inst = instance as usize;
                if inst >= MAX_INSTANCE {
                    continue;
                }
                discovery.ip_versions[ip.index()][inst] = IpVersion {
                    major,
                    minor,
                    revision,
                    variant,
                    subrev,
                }
                .full();
                for (base_idx, base) in bases.iter().enumerate() {
                    regs.set_reg_offset(ip, inst, base_idx, *base);
                }
            }
        }
        Ok(())
    }
}

/// Parses the GC info table exactly like Linux
/// `amdgpu_discovery_get_gfx_info()` for the v1.x layout used by Navi 23.
impl GfxInfo {
    fn read(table: &[u8]) -> Self {
        let mut info = Self::default();
        let u16 = |offset: usize| {
            table
                .get(offset..offset + 2)
                .map(|b| u16::from_le_bytes([b[0], b[1]]))
        };
        let u32 = |offset: usize| {
            table
                .get(offset..offset + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        };
        if u16(4) != Some(1) {
            return info;
        }
        let minor = u16(6).unwrap_or(0);
        info.max_shader_engines = u32(12).unwrap_or(0);
        info.max_cu_per_sh = 2 * (u32(16).unwrap_or(0) + u32(20).unwrap_or(0));
        info.max_sh_per_se = u32(76).unwrap_or(0);
        info.max_backends_per_se = u32(24).unwrap_or(0);
        info.max_texture_channel_caches = u32(28).unwrap_or(0);
        info.max_gprs = u32(32).unwrap_or(0);
        info.max_gs_threads = u32(36).unwrap_or(0);
        info.gs_vgt_table_depth = u32(40).unwrap_or(0);
        info.gs_prim_buffer_depth = u32(44).unwrap_or(0);
        info.double_offchip_lds_buf = u32(52).unwrap_or(0);
        info.wave_front_size = u32(56).unwrap_or(0);
        if minor >= 2 {
            info.gc_tcp_l1_size = u32(104).unwrap_or(0);
            info.gc_num_sqc_per_wgp = u32(108).unwrap_or(0);
            info.gc_l1_instruction_cache_size_per_sqc = u32(112).unwrap_or(0);
            info.gc_l1_data_cache_size_per_sqc = u32(116).unwrap_or(0);
            info.gc_gl1c_per_sa = u32(120).unwrap_or(0);
            info.gc_gl1c_size_per_instance = u32(124).unwrap_or(0);
            info.gc_gl2c_per_gpu = u32(128).unwrap_or(0);
        }
        info
    }
}

impl Discovery {
    fn parse_harvest_table(table: &[u8]) {
        let mut reader = Reader::new(table);
        let Ok(signature) = reader.u32() else {
            return;
        };
        if signature != HARVEST_TABLE_SIGNATURE {
            return;
        }
        let _version = reader.u32();
        let mut count = 0;
        while let (Ok(hw_id), Ok(instance)) = (reader.u16(), reader.u8()) {
            let _ = reader.u8();
            let _ = HwIp::from_hardware_id(hw_id);
            let _ = instance;
            count += 1;
        }
        if count != 0 {
            dev_info!("astra: discovery harvest table: {} entries", count);
        }
    }
}
