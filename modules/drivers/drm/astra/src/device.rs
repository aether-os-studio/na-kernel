//! Device adapter: PCI probe, BAR mapping and the init orchestration
//! mirroring Linux `amdgpu_device_init`.

use alloc::vec::Vec;
use core::ops::{Deref, DerefMut};

use na_std::pci::{self, Bar, DeviceHandle, MsiIrq};
use na_std::{Error, Result};

use crate::atom::AtomBios;
use crate::blocks::IpBlocks;
use crate::dev_info;
use crate::discovery::{self, GfxInfo};
use crate::firmware::{FirmwareStore, StagedFw, UcodeId};
use crate::ip::{CompletionFence, HWIP_COUNT, HwIp, MAX_INSTANCE, UserSubmission};
use crate::irq::IhHandler;
use crate::mem::{Bo, BoAllocator, Wb};
use crate::regs::Regs;

/// BAR layout used by all dGPUs since CIK: 0 = VRAM aperture,
/// 2 = doorbell, 5 = MMIO registers.
const BAR_VRAM: u8 = 0;
const BAR_DOORBELL: u8 = 2;
const BAR_MMIO: u8 = 5;

/// GMC-derived device state shared across IP blocks (Linux `adev->gmc`).
#[derive(Clone, Copy, Debug, Default)]
pub struct GmcInfo {
    pub mc_vram_size: u64,
    pub real_vram_size: u64,
    pub visible_vram_size: u64,
    pub aper_size: u64,
    pub vram_start: u64,
    pub vram_end: u64,
    pub fb_start: u64,
    pub fb_end: u64,
    /// VRAM reserved for the pre-OS/VBIOS scanout before driver BOs.
    pub vram_reserved_size: u64,
    /// Physical VRAM base seen by VM page-table walkers. Linux obtains
    /// this from `gfxhub_v2_1_get_mc_fb_offset()`.
    pub vram_base_offset: u64,
    pub gart_start: u64,
    pub gart_end: u64,
    pub gart_size: u64,
    pub vram_type: u32,
    pub vram_width: u32,
    pub vram_vendor: u32,
    pub dummy_page_addr: u64,
    pub mem_scratch_gpu_addr: u64,
}

/// Driver-owned linear scanout published by the DCN block after it has
/// successfully enabled the display pipe.
#[derive(Clone, Copy, Debug)]
pub struct ScanoutInfo {
    /// Offset inside BAR0 / the visible VRAM aperture.
    pub vram_offset: u64,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
}

/// Clock values exposed through the AMDGPU userspace ABI.  Linux reports
/// these fields in KHz, even though the SMU mailbox uses MHz.
#[derive(Clone, Copy, Debug, Default)]
pub struct ClockInfo {
    pub max_engine_clock_khz: u64,
    pub max_memory_clock_khz: u64,
    pub min_engine_clock_khz: u64,
    pub min_memory_clock_khz: u64,
}

pub struct Adapter {
    pub info: pci::DeviceInfo,
    /// Register access layer (owns the BAR5/BAR0/BAR2 apertures).
    pub regs: Regs,
    /// Per-IP versions from the discovery table.
    pub versions: [[u32; MAX_INSTANCE]; HWIP_COUNT],
    /// Parsed video BIOS.
    pub atom: Option<AtomBios>,
    /// GMC state.
    pub gmc: GmcInfo,
    /// GC config from the discovery GC table.
    pub gfx_info: GfxInfo,
    /// Boot/SMU clock information used by AMDGPU_INFO_DEV_INFO.
    pub clocks: ClockInfo,
    /// GART/VRAM space manager.
    pub mem: BoAllocator,
    /// Host physical base of the BAR0 VRAM aperture (for dumb-buffer mmap).
    pub vram_base: u64,
    /// Active DCN scanout, available only after display hw_init succeeds.
    pub scanout: Option<ScanoutInfo>,
    /// Writeback scratch.
    pub wb: Option<Wb>,
    /// Device scratch page (VRAM).
    pub mem_scratch: Option<Bo>,
    /// Long-lived PCI device handle (MSI setup, config space).
    pub pci: DeviceHandle,
    /// MSI registration for the IH (keeps the handler alive).
    pub msi: Option<MsiIrq<IhHandler>>,
    /// PSP firmware metadata together with its staging BO.
    firmware: Option<FirmwareStore>,
    /// RLC autoload + PSP-only firmware delivery (psp_early_init).
    pub psp_autoload: bool,
    /// SMU mailbox registers (published by the SMU block for VCN).
    pub smu_mailbox: Option<(u32, u32, u32)>,
    /// Keeps the kernel-side BAR claims alive (RAII release).
    _resources: Vec<pci::BarResource>,
}

/// Fully initialized GPU. Device state and executable IP blocks are separate
/// owned fields so command dispatch can borrow both directly without moving
/// either one out of its owner.
pub struct Gpu {
    adapter: Adapter,
    blocks: IpBlocks,
}

impl Deref for Gpu {
    type Target = Adapter;

    fn deref(&self) -> &Self::Target {
        &self.adapter
    }
}

impl DerefMut for Gpu {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.adapter
    }
}

impl Gpu {
    pub fn probe(device: pci::Device<'_>) -> Result<Self> {
        Adapter::probe(device)?.initialize()
    }

    pub fn submit_user_ibs(&mut self, submission: UserSubmission<'_>) -> Result<CompletionFence> {
        self.blocks.submit_user_ibs(&mut self.adapter, submission)
    }

    pub fn update_vm_table(
        &mut self,
        dst: u64,
        addr: u64,
        count: u32,
        incr: u32,
        flags: u64,
    ) -> Result<()> {
        self.blocks
            .update_vm_table(&mut self.adapter, dst, addr, count, incr, flags)
    }
}

impl Adapter {
    pub(crate) fn firmware(&self, id: UcodeId) -> Option<&StagedFw> {
        self.firmware.as_ref()?.get(id)
    }

    pub(crate) fn firmware_mut(&mut self, id: UcodeId) -> Option<&mut StagedFw> {
        self.firmware.as_mut()?.get_mut(id)
    }

    pub(crate) fn firmwares(&self) -> impl Iterator<Item = &StagedFw> {
        self.firmware.iter().flat_map(FirmwareStore::iter)
    }

    pub(crate) fn install_firmware(&mut self, firmware: FirmwareStore) {
        self.firmware = Some(firmware);
    }

    fn probe(mut device: pci::Device<'_>) -> Result<Self> {
        let info = device.info()?;
        dev_info!(
            "astra: probing {:04x}:{:02x}:{:02x}.{}",
            info.address.segment,
            info.address.bus,
            info.address.device,
            info.address.function,
        );
        device.enable_memory_and_bus_master()?;

        let mut resources = Vec::new();
        let mut bars: [Option<na_std::io::MmioRegion>; 6] = core::array::from_fn(|_| None);
        let mut vram_base = 0u64;
        for index in [BAR_VRAM, BAR_DOORBELL, BAR_MMIO] {
            let resource = device.claim_bar(index)?;
            let range = match resource.bar() {
                Bar::Memory { range, .. } => range,
                Bar::Port { .. } => return Err(Error::Unsupported),
            };
            if index == BAR_VRAM {
                vram_base = range.start().get();
            }
            dev_info!(
                "astra: BAR {} at phys {:#x}, {:#x} bytes",
                index,
                range.start().get(),
                range.length(),
            );
            let region = range.map_mmio().inspect_err(|_error| {
                dev_info!(
                    "astra: failed to map BAR {} (phys {:#x}, {:#x} bytes)",
                    index,
                    range.start().get(),
                    range.length(),
                );
            })?;
            bars[index as usize] = Some(region);
            resources.push(resource);
        }

        let regs = Regs::new(
            bars[BAR_MMIO as usize].take().ok_or(Error::NoDevice)?,
            bars[BAR_VRAM as usize].take().ok_or(Error::NoDevice)?,
            bars[BAR_DOORBELL as usize].take().ok_or(Error::NoDevice)?,
        );

        Ok(Self {
            info,
            regs,
            versions: [[0; MAX_INSTANCE]; HWIP_COUNT],
            atom: None,
            gmc: GmcInfo::default(),
            gfx_info: GfxInfo::default(),
            clocks: ClockInfo::default(),
            mem: BoAllocator::new(),
            vram_base,
            scanout: None,
            wb: None,
            mem_scratch: None,
            pci: device.retain(),
            msi: None,
            firmware: None,
            psp_autoload: false,
            smu_mailbox: None,
            _resources: resources,
        })
    }

    /// Runs the amdgpu-style init chain (grows with each milestone):
    /// early_init → sw_init (COMMON/GMC hw inline) → [IH] → fw loading
    /// → remaining hw_init → late_init.
    fn initialize(mut self) -> Result<Gpu> {
        let info = self.info;
        dev_info!(
            "astra {:#06x}:{:02x}:{:02x}.{}: {:04x}:{:04x} rev {:02x}",
            info.address.segment,
            info.address.bus,
            info.address.device,
            info.address.function,
            info.vendor_id,
            info.device_id,
            info.revision_id,
        );

        // IP discovery (fills reg bases + versions).
        let discovery = discovery::Discovery::read(&mut self.regs)?;
        self.versions = discovery.ip_versions;
        self.gfx_info = discovery.gfx_info;

        // Video BIOS + ATOM tables, Linux `amdgpu_get_bios_dgpu` order:
        // ATRM → VRAM shadow → SMUIO ROM window → PCI ROM BAR.
        let smuio_version =
            crate::ip::IpVersion::from_full(self.versions[crate::ip::HwIp::Smuio.index()][0]);
        self.atom = {
            let mut bios = None;

            // 1. ATRM (the plaintext VBIOS the board firmware stashes for
            //    the OS; the SPI flash itself is not readable pre-POST).
            let mut buf = alloc::vec![0u8; 256 << 10];
            match na_std::acpi::read_atrm(&mut buf) {
                Ok(len) => {
                    dev_info!("astra: ATRM returned {} bytes", len);
                    buf.truncate(len);
                    bios = AtomBios::from_bytes(buf);
                    if bios.is_some() {
                        dev_info!("astra: VBIOS loaded from ATRM");
                    } else {
                        dev_info!("astra: ATRM data is not a valid ATOM image");
                    }
                }
                Err(error) => dev_info!("astra: ATRM unavailable: {:?}", error),
            }

            // 2. VRAM shadow → SMUIO ROM window.
            if bios.is_none() {
                bios = AtomBios::read(&mut self.regs, smuio_version).ok();
            }

            // 3. PCI ROM BAR.
            if bios.is_none() {
                dev_info!("astra: SMUIO ROM window failed, trying PCI ROM BAR");
                match self.pci.as_device().rom_bar().ok() {
                    Some(bar) => bios = AtomBios::read_from_rom_bar(bar).ok(),
                    None => dev_info!("astra: no PCI ROM BAR available"),
                }
            }

            match bios {
                Some(atom) => Some(atom),
                None => {
                    dev_info!("astra: no VBIOS source available");
                    return Err(Error::Io);
                }
            }
        };
        if let Some(fw) = self.atom.as_ref().and_then(|atom| atom.firmware_info()) {
            self.clocks.max_engine_clock_khz = fw.bootup_sclk_khz as u64;
            self.clocks.min_engine_clock_khz = fw.bootup_sclk_khz as u64;
            self.clocks.max_memory_clock_khz = fw.bootup_mclk_khz as u64;
            self.clocks.min_memory_clock_khz = fw.bootup_mclk_khz as u64;
            dev_info!(
                "astra: VBIOS bootup sclk {} MHz, mclk {} MHz, capability {:#x}, pptable_id {}",
                fw.bootup_sclk_khz / 1000,
                fw.bootup_mclk_khz / 1000,
                fw.firmware_capability,
                fw.pplib_pptable_id,
            );
        }
        if let Some(pp) = self.atom.as_ref().and_then(|atom| atom.powerplay_info()) {
            dev_info!(
                "astra: ATOM powerplay v{}.{}: {} bytes, smc_pptable offset {} size {} (SMC DPM v{}.{})",
                pp.format_revision,
                pp.content_revision,
                pp.atom_table_size,
                pp.smc_pptable_offset,
                pp.bytes.len(),
                pp.smc_dpm_format_revision,
                pp.smc_dpm_content_revision,
            );
        }

        let mut blocks = IpBlocks::discover(&self.versions);
        blocks.initialize(&mut self)?;

        dev_info!("astra: init succeeded");
        Ok(Gpu {
            adapter: self,
            blocks,
        })
    }

    /// Linux `amdgpu_discovery_set_common_ip_blocks` selects
    /// `nbio_v2_3_funcs` for NBIO 2.1.x, 2.3.x and 3.3.x.
    pub(crate) fn uses_nbio_v2_3(&self) -> bool {
        let version = crate::ip::IpVersion::from_full(self.versions[HwIp::Nbio.index()][0]);
        matches!((version.major, version.minor), (2, 1) | (2, 3) | (3, 3))
    }
}
