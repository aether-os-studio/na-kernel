//! DCN 3.0.2 Display Microcontroller (DMCUB) service.
//!
//! This is the PSP-load path from Linux `dm_dmub_sw_init()` /
//! `dm_dmub_hw_init()` and `dmub_srv_hw_init()`: PSP owns CW0 firmware
//! loading, while the display driver allocates the remaining regions,
//! programs CW2-CW6 plus the mailboxes, releases reset and communicates
//! through the 64-byte framebuffer inbox.

use alloc::vec::Vec;

use na_std::arch::fence;
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;
use crate::firmware::{FirmwareCatalog, UcodeId};
use crate::ip::HwIp;
use crate::mem::Bo;
use crate::regs::dcn3_0_2 as dcn;
use crate::regs::mp13_0_2 as mp;
use crate::regs::{Regs, get_field, set_field};
use crate::ucode::CommonFirmwareHeader;

const PSP_HEADER_BYTES: usize = 256;
const PSP_FOOTER_BYTES: [usize; 2] = [256, 512];
const DMUB_FW_META_MAGIC: u32 = 0x444d_5542;

const WINDOW_COUNT: usize = 12;
const WINDOW_INST_CONST: usize = 0;
const WINDOW_BSS_DATA: usize = 2;
const WINDOW_VBIOS: usize = 3;
const WINDOW_MAILBOX: usize = 4;
const WINDOW_TRACE: usize = 5;
const WINDOW_FW_STATE: usize = 6;
const WINDOW_SHARED_STATE: usize = 9;

const DMUB_STACK_SIZE: usize = 128 * 1024;
const DMUB_CONTEXT_SIZE: usize = 512 * 1024;
const DMUB_RB_CMD_SIZE: u32 = 64;
const DMUB_RB_SIZE: u32 = 64 * 128;
const DMUB_MAILBOX_SIZE: usize = (DMUB_RB_SIZE as usize) * 2;
const DMUB_DEFAULT_STATE_SIZE: usize = 64 * 1024;
const DMUB_DEFAULT_TRACE_SIZE: usize = 64 * 1024;
const DMUB_SCRATCH_SIZE: usize = 1024;
const DMUB_LSDMA_SIZE: usize = 64 * 1024;
const DMUB_SHARED_STATE_MIN: usize = 6 * 256;

// These two windows are not mapped into DMCUB's CW2-CW6 address space on
// DCN302. Conservative 64 KiB allocations preserve Linux's separate region
// layout and leave room for the current FAMS2/cursor structures.
const DMUB_IB_SIZE: usize = 64 * 1024;
const DMUB_CURSOR_SIZE: usize = 64 * 1024;

const DMUB_CW0_BASE: u32 = 0x6000_0000;
const DMUB_CW3_BASE: u32 = 0x6300_0000;
const DMUB_CW4_BASE: u32 = 0x6400_0000;
const DMUB_CW5_BASE: u32 = 0x6500_0000;
const DMUB_CW6_BASE: u32 = 0x6600_0000;
const DMUB_REGION5_BASE: u32 = 0xa000_0000;
const TRACE_BUFFER_ENTRY_OFFSET: u32 = 16;

const DMUB_CMD_VBIOS: u32 = 128;
const DMUB_BOOT_READY: u32 = 0x3;
const DMUB_GPINT_STOP_FW: u32 = 2;
const DMUB_GPINT_STOP_FW_RESPONSE: u32 = 0xdead_dead;

#[derive(Clone, Copy, Default)]
struct Region {
    base: u32,
    size: u32,
}

#[derive(Clone, Copy, Default)]
struct MetaInfo {
    fw_region_size: u32,
    trace_buffer_size: u32,
    fw_version: u32,
    shared_state_size: u32,
}

#[derive(Clone, Copy)]
struct CacheWindowRegisters {
    offset: u32,
    offset_high: u32,
    base: u32,
    top: u32,
}

#[derive(Clone, Copy)]
struct CacheWindow {
    offset: u64,
    base: u32,
    top: u32,
}

impl CacheWindow {
    const fn new(offset: u64, base: u32, size: u32) -> Self {
        Self {
            offset,
            base,
            top: base + size,
        }
    }
}

const CW2_REGISTERS: CacheWindowRegisters = CacheWindowRegisters {
    offset: dcn::mmDMCUB_REGION3_CW2_OFFSET,
    offset_high: dcn::mmDMCUB_REGION3_CW2_OFFSET_HIGH,
    base: dcn::mmDMCUB_REGION3_CW2_BASE_ADDRESS,
    top: dcn::mmDMCUB_REGION3_CW2_TOP_ADDRESS,
};
const CW3_REGISTERS: CacheWindowRegisters = CacheWindowRegisters {
    offset: dcn::mmDMCUB_REGION3_CW3_OFFSET,
    offset_high: dcn::mmDMCUB_REGION3_CW3_OFFSET_HIGH,
    base: dcn::mmDMCUB_REGION3_CW3_BASE_ADDRESS,
    top: dcn::mmDMCUB_REGION3_CW3_TOP_ADDRESS,
};
const CW4_REGISTERS: CacheWindowRegisters = CacheWindowRegisters {
    offset: dcn::mmDMCUB_REGION3_CW4_OFFSET,
    offset_high: dcn::mmDMCUB_REGION3_CW4_OFFSET_HIGH,
    base: dcn::mmDMCUB_REGION3_CW4_BASE_ADDRESS,
    top: dcn::mmDMCUB_REGION3_CW4_TOP_ADDRESS,
};
const CW5_REGISTERS: CacheWindowRegisters = CacheWindowRegisters {
    offset: dcn::mmDMCUB_REGION3_CW5_OFFSET,
    offset_high: dcn::mmDMCUB_REGION3_CW5_OFFSET_HIGH,
    base: dcn::mmDMCUB_REGION3_CW5_BASE_ADDRESS,
    top: dcn::mmDMCUB_REGION3_CW5_TOP_ADDRESS,
};
const CW6_REGISTERS: CacheWindowRegisters = CacheWindowRegisters {
    offset: dcn::mmDMCUB_REGION3_CW6_OFFSET,
    offset_high: dcn::mmDMCUB_REGION3_CW6_OFFSET_HIGH,
    base: dcn::mmDMCUB_REGION3_CW6_BASE_ADDRESS,
    top: dcn::mmDMCUB_REGION3_CW6_TOP_ADDRESS,
};

pub struct Dmub {
    fw: Vec<u8>,
    payload_offset: usize,
    inst_const_bytes: usize,
    bss_data_bytes: usize,
    fw_version: u32,
    regions: [Region; WINDOW_COUNT],
    bo: Option<Bo>,
    inbox_wptr: u32,
    initialized: bool,
}

impl Dmub {
    pub fn new() -> Self {
        Self {
            fw: Vec::new(),
            payload_offset: 0,
            inst_const_bytes: 0,
            bss_data_bytes: 0,
            fw_version: 0,
            regions: [Region::default(); WINDOW_COUNT],
            bo: None,
            inbox_wptr: 0,
            initialized: false,
        }
    }

    /// Linux `dm_dmub_sw_init`: request and parse the DMCUB firmware,
    /// calculate the region layout, then allocate one VRAM BO for all FB
    /// windows. The PSP staging copy is made later by
    /// [`FirmwareCatalog::stage`].
    pub fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let firmware = FirmwareCatalog::for_adapter(dev);
        let name = firmware.name(UcodeId::Dmcub);
        self.fw = firmware.load(UcodeId::Dmcub)?;
        let header = CommonFirmwareHeader::parse(&self.fw).ok_or(Error::Io)?;
        if header.header_version_major != 1 || self.fw.len() < 40 {
            return Err(Error::Unsupported);
        }
        self.payload_offset = header.ucode_array_offset_bytes as usize;
        self.inst_const_bytes = Self::u32_at(&self.fw, 32).ok_or(Error::Io)? as usize;
        self.bss_data_bytes = Self::u32_at(&self.fw, 36).ok_or(Error::Io)? as usize;
        if self.inst_const_bytes < PSP_HEADER_BYTES {
            return Err(Error::Io);
        }
        let payload_end = self
            .payload_offset
            .checked_add(self.inst_const_bytes)
            .and_then(|end| end.checked_add(self.bss_data_bytes))
            .ok_or(Error::Range)?;
        if payload_end > self.fw.len() {
            return Err(Error::Io);
        }

        let logical_inst_size = self.inst_const_bytes - PSP_HEADER_BYTES;
        let meta = self.find_meta(logical_inst_size);
        self.fw_version = meta
            .map(|info| info.fw_version)
            .filter(|version| *version != 0)
            .unwrap_or(header.ucode_version);

        let vbios_size = dev.atom.as_ref().ok_or(Error::NoDevice)?.bytes().len();
        let window_sizes = [
            logical_inst_size,
            DMUB_STACK_SIZE + DMUB_CONTEXT_SIZE,
            self.bss_data_bytes,
            vbios_size,
            DMUB_MAILBOX_SIZE,
            meta.map(|info| info.trace_buffer_size as usize)
                .filter(|size| *size != 0)
                .unwrap_or(DMUB_DEFAULT_TRACE_SIZE),
            meta.map(|info| info.fw_region_size as usize)
                .filter(|size| *size != 0)
                .unwrap_or(DMUB_DEFAULT_STATE_SIZE),
            DMUB_SCRATCH_SIZE,
            DMUB_IB_SIZE,
            meta.map(|info| info.shared_state_size as usize)
                .unwrap_or(0)
                .max(DMUB_SHARED_STATE_MIN),
            DMUB_LSDMA_SIZE,
            DMUB_CURSOR_SIZE,
        ];

        let mut top = 0usize;
        for (region, size) in self.regions.iter_mut().zip(window_sizes) {
            let base = top.next_multiple_of(256);
            let size = size.next_multiple_of(64);
            let end = base.checked_add(size).ok_or(Error::OutOfMemory)?;
            region.base = u32::try_from(base).map_err(|_| Error::Range)?;
            region.size = u32::try_from(size).map_err(|_| Error::Range)?;
            top = end;
        }
        let bo_size = top.next_multiple_of(4096);
        self.bo = Some(dev.mem.alloc_vram(&mut dev.regs, bo_size)?);

        dev_info!(
            "astra: DMCUB firmware {} common {:#010x}, DMUB {:#010x}, inst {}, bss {}, BO {} bytes at VRAM {:#x}",
            name,
            header.ucode_version,
            self.fw_version,
            logical_inst_size,
            self.bss_data_bytes,
            bo_size,
            self.bo.as_ref().map(|bo| bo.gpu_addr).unwrap_or(0),
        );
        if let Some(meta) = meta {
            dev_info!(
                "astra: DMCUB metadata: state {}, trace {}, shared {} bytes",
                meta.fw_region_size,
                meta.trace_buffer_size,
                meta.shared_state_size,
            );
        } else {
            dev_info!("astra: DMCUB metadata not found; using Linux default region sizes");
        }
        Ok(())
    }

    /// Linux `dm_dmub_hw_init` + `dmub_srv_hw_init`, PSP-load branch.
    pub fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        let supported = dev.regs.read_dcn(
            dcn::mmCC_DC_PIPE_DIS,
            dcn::mmCC_DC_PIPE_DIS_BASE_IDX as usize,
        )?;
        if get_field(
            supported,
            dcn::CC_DC_PIPE_DIS__DC_DMCUB_ENABLE__SHIFT,
            dcn::CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK,
        ) == 0
        {
            dev_info!(
                "astra: DMCUB is fused off (CC_DC_PIPE_DIS={:#010x})",
                supported
            );
            return Err(Error::Unsupported);
        }

        let staged = dev.firmware(UcodeId::Dmcub).ok_or(Error::NoDevice)?;
        dev_info!(
            "astra: DMCUB PSP image staged at {:#x}, TMR {:#x}",
            staged.mc_addr,
            staged.tmr_addr.unwrap_or(0),
        );

        self.reset(&mut dev.regs)?;

        // PSP already loaded/configured CW0. Driver-owned BSS and VBIOS
        // still live in the DMUB BO exactly as in Linux's PSP branch.
        if self.bss_data_bytes != 0 {
            let start = self
                .payload_offset
                .checked_add(self.inst_const_bytes)
                .ok_or(Error::Range)?;
            let end = start.checked_add(self.bss_data_bytes).ok_or(Error::Range)?;
            let bytes = self.fw.get(start..end).ok_or(Error::Range)?;
            self.write_region(&mut dev.regs, WINDOW_BSS_DATA, bytes)?;
        }
        let vbios = dev.atom.as_ref().ok_or(Error::NoDevice)?.bytes();
        self.write_region(&mut dev.regs, WINDOW_VBIOS, vbios)?;
        for window in [
            WINDOW_MAILBOX,
            WINDOW_TRACE,
            WINDOW_FW_STATE,
            WINDOW_SHARED_STATE,
        ] {
            self.zero_region(&mut dev.regs, window)?;
        }

        self.setup_windows(dev)?;
        // Linux dcn21_dmcu_construct reads MP0 C2PMSG_58 and passes that
        // PSP version to dmub_dcn20_reset_release through SCRATCH15.
        let psp_version = dev.regs.read_ip(
            HwIp::Mp0,
            0,
            mp::regMP0_SMN_C2PMSG_58,
            mp::regMP0_SMN_C2PMSG_58_BASE_IDX as usize,
        )?;
        self.release_reset(&mut dev.regs, psp_version)?;

        for _ in 0..1000 {
            let status = dev.regs.read_dcn(
                dcn::mmDMCUB_SCRATCH0,
                dcn::mmDMCUB_SCRATCH0_BASE_IDX as usize,
            )?;
            if status & DMUB_BOOT_READY == DMUB_BOOT_READY {
                self.inbox_wptr = 0;
                self.initialized = true;
                dev_info!(
                    "astra: DMCUB hardware initialized: version={:#010x}, SCRATCH0={:#010x}",
                    self.fw_version,
                    status,
                );
                return Ok(());
            }
            na_std::time::delay(core::time::Duration::from_micros(100));
        }

        dev_info!("astra: timeout waiting for DMCUB firmware auto-load");
        self.diagnostics(&mut dev.regs);
        Err(Error::Io)
    }

    /// Queues one `DMUB_CMD__VBIOS` command and waits until firmware has
    /// consumed it, matching `dmub_srv_fb_cmd_queue/execute/wait_for_pending`.
    pub fn execute_vbios(&mut self, regs: &mut Regs, subtype: u8, payload: &[u8]) -> Result<()> {
        if !self.initialized || payload.len() > 60 {
            return Err(Error::InvalidArgument);
        }
        self.wait_for_rptr(regs, self.inbox_wptr)?;

        let mut command = [0u8; DMUB_RB_CMD_SIZE as usize];
        let header = DMUB_CMD_VBIOS | ((subtype as u32) << 8) | ((payload.len() as u32) << 24);
        command[..4].copy_from_slice(&header.to_le_bytes());
        command[4..4 + payload.len()].copy_from_slice(payload);

        let bo = self.bo.as_ref().ok_or(Error::NoDevice)?;
        let mailbox = self.regions[WINDOW_MAILBOX];
        let pos = bo
            .gpu_addr
            .checked_add(mailbox.base as u64)
            .and_then(|value| value.checked_add(self.inbox_wptr as u64))
            .ok_or(Error::Range)?;
        let mut dwords = [0u32; 16];
        for (word, bytes) in dwords.iter_mut().zip(command.as_chunks::<4>().0) {
            *word = u32::from_le_bytes(*bytes);
        }
        regs.vram_write_dwords(pos, &dwords)?;
        fence::sfence();

        let next = (self.inbox_wptr + DMUB_RB_CMD_SIZE) % DMUB_RB_SIZE;
        regs.write_dcn(
            dcn::mmDMCUB_INBOX1_WPTR,
            dcn::mmDMCUB_INBOX1_WPTR_BASE_IDX as usize,
            next,
        )?;
        self.inbox_wptr = next;
        if let Err(error) = self.wait_for_rptr(regs, next) {
            dev_info!(
                "astra: DMCUB VBIOS command subtype {} ({} bytes) timed out",
                subtype,
                payload.len(),
            );
            self.diagnostics(regs);
            return Err(error);
        }
        Ok(())
    }

    fn wait_for_rptr(&self, regs: &mut Regs, expected: u32) -> Result<()> {
        for _ in 0..100_000 {
            let rptr = regs.read_dcn(
                dcn::mmDMCUB_INBOX1_RPTR,
                dcn::mmDMCUB_INBOX1_RPTR_BASE_IDX as usize,
            )?;
            if rptr == expected {
                return Ok(());
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        Err(Error::Io)
    }

    fn reset(&self, regs: &mut Regs) -> Result<()> {
        let mut cntl = regs.read_dcn(dcn::mmDMCUB_CNTL, dcn::mmDMCUB_CNTL_BASE_IDX as usize)?;
        let in_reset = get_field(
            cntl,
            dcn::DMCUB_CNTL__DMCUB_SOFT_RESET__SHIFT,
            dcn::DMCUB_CNTL__DMCUB_SOFT_RESET_MASK,
        );
        if in_reset == 0 {
            let command = (1u32 << 28) | (DMUB_GPINT_STOP_FW << 16);
            regs.write_dcn(
                dcn::mmDMCUB_GPINT_DATAIN1,
                dcn::mmDMCUB_GPINT_DATAIN1_BASE_IDX as usize,
                command,
            )?;
            let ack = command & !(0xf << 28);
            for _ in 0..30 {
                if regs.read_dcn(
                    dcn::mmDMCUB_GPINT_DATAIN1,
                    dcn::mmDMCUB_GPINT_DATAIN1_BASE_IDX as usize,
                )? == ack
                {
                    break;
                }
            }
            for _ in 0..30 {
                if regs.read_dcn(
                    dcn::mmDMCUB_SCRATCH7,
                    dcn::mmDMCUB_SCRATCH7_BASE_IDX as usize,
                )? == DMUB_GPINT_STOP_FW_RESPONSE
                {
                    break;
                }
            }
            regs.write_dcn(
                dcn::mmDMCUB_GPINT_DATAIN1,
                dcn::mmDMCUB_GPINT_DATAIN1_BASE_IDX as usize,
                0,
            )?;
        }

        cntl = set_field(
            cntl,
            dcn::DMCUB_CNTL__DMCUB_SOFT_RESET__SHIFT,
            dcn::DMCUB_CNTL__DMCUB_SOFT_RESET_MASK,
            1,
        );
        cntl = set_field(
            cntl,
            dcn::DMCUB_CNTL__DMCUB_ENABLE__SHIFT,
            dcn::DMCUB_CNTL__DMCUB_ENABLE_MASK,
            0,
        );
        regs.write_dcn(dcn::mmDMCUB_CNTL, dcn::mmDMCUB_CNTL_BASE_IDX as usize, cntl)?;
        Self::rmw(
            regs,
            dcn::mmMMHUBBUB_SOFT_RESET,
            dcn::mmMMHUBBUB_SOFT_RESET_BASE_IDX as usize,
            dcn::MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET__SHIFT,
            dcn::MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK,
            1,
        )?;
        for (reg, base) in [
            (dcn::mmDMCUB_INBOX1_RPTR, dcn::mmDMCUB_INBOX1_RPTR_BASE_IDX),
            (dcn::mmDMCUB_INBOX1_WPTR, dcn::mmDMCUB_INBOX1_WPTR_BASE_IDX),
            (
                dcn::mmDMCUB_OUTBOX1_RPTR,
                dcn::mmDMCUB_OUTBOX1_RPTR_BASE_IDX,
            ),
            (
                dcn::mmDMCUB_OUTBOX1_WPTR,
                dcn::mmDMCUB_OUTBOX1_WPTR_BASE_IDX,
            ),
            (dcn::mmDMCUB_SCRATCH0, dcn::mmDMCUB_SCRATCH0_BASE_IDX),
        ] {
            regs.write_dcn(reg, base as usize, 0)?;
        }
        Ok(())
    }

    fn setup_windows(&self, dev: &mut Adapter) -> Result<()> {
        let bo = self.bo.as_ref().ok_or(Error::NoDevice)?;
        let gpu = |window: usize| -> Result<u64> {
            dev.gmc
                .fb_start
                .checked_add(bo.gpu_addr)
                .and_then(|value| value.checked_add(self.regions[window].base as u64))
                .ok_or(Error::Range)
        };

        let cw2_base = DMUB_CW0_BASE
            .checked_add(self.regions[WINDOW_INST_CONST].size)
            .ok_or(Error::Range)?;
        self.program_cw2(&mut dev.regs, gpu(WINDOW_BSS_DATA)?, cw2_base)?;
        Self::program_cw(
            &mut dev.regs,
            CW3_REGISTERS,
            CacheWindow::new(
                gpu(WINDOW_VBIOS)?,
                DMUB_CW3_BASE,
                self.regions[WINDOW_VBIOS].size,
            ),
        )?;

        let cached_inbox = !(self.fw_version >= Self::fw_version(1, 0, 0)
            && self.fw_version <= Self::fw_version(1, 10, 0));
        if cached_inbox {
            Self::program_cw(
                &mut dev.regs,
                CW4_REGISTERS,
                CacheWindow::new(
                    gpu(WINDOW_MAILBOX)?,
                    DMUB_CW4_BASE,
                    self.regions[WINDOW_MAILBOX].size,
                ),
            )?;
        } else {
            let offset = gpu(WINDOW_MAILBOX)?;
            dev.regs.write_dcn(
                dcn::mmDMCUB_REGION4_OFFSET,
                dcn::mmDMCUB_REGION4_OFFSET_BASE_IDX as usize,
                offset as u32,
            )?;
            dev.regs.write_dcn(
                dcn::mmDMCUB_REGION4_OFFSET_HIGH,
                dcn::mmDMCUB_REGION4_OFFSET_HIGH_BASE_IDX as usize,
                (offset >> 32) as u32,
            )?;
            dev.regs.write_dcn(
                dcn::mmDMCUB_REGION4_TOP_ADDRESS,
                dcn::mmDMCUB_REGION4_TOP_ADDRESS_BASE_IDX as usize,
                set_field(
                    set_field(
                        0,
                        dcn::DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_TOP_ADDRESS__SHIFT,
                        dcn::DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_TOP_ADDRESS_MASK,
                        (self.regions[WINDOW_MAILBOX].size - 1) as u64,
                    ),
                    dcn::DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE__SHIFT,
                    dcn::DMCUB_REGION4_TOP_ADDRESS__DMCUB_REGION4_ENABLE_MASK,
                    1,
                ),
            )?;
        }

        Self::program_cw(
            &mut dev.regs,
            CW5_REGISTERS,
            CacheWindow::new(
                gpu(WINDOW_TRACE)?,
                DMUB_CW5_BASE,
                self.regions[WINDOW_TRACE].size,
            ),
        )?;
        let trace_offset = gpu(WINDOW_TRACE)?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_REGION5_OFFSET,
            dcn::mmDMCUB_REGION5_OFFSET_BASE_IDX as usize,
            trace_offset as u32,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_REGION5_OFFSET_HIGH,
            dcn::mmDMCUB_REGION5_OFFSET_HIGH_BASE_IDX as usize,
            (trace_offset >> 32) as u32,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_REGION5_TOP_ADDRESS,
            dcn::mmDMCUB_REGION5_TOP_ADDRESS_BASE_IDX as usize,
            set_field(
                set_field(
                    0,
                    dcn::DMCUB_REGION5_TOP_ADDRESS__DMCUB_REGION5_TOP_ADDRESS__SHIFT,
                    dcn::DMCUB_REGION5_TOP_ADDRESS__DMCUB_REGION5_TOP_ADDRESS_MASK,
                    (self.regions[WINDOW_TRACE].size - 1) as u64,
                ),
                dcn::DMCUB_REGION5_TOP_ADDRESS__DMCUB_REGION5_ENABLE__SHIFT,
                dcn::DMCUB_REGION5_TOP_ADDRESS__DMCUB_REGION5_ENABLE_MASK,
                1,
            ),
        )?;

        Self::program_cw(
            &mut dev.regs,
            CW6_REGISTERS,
            CacheWindow::new(
                gpu(WINDOW_FW_STATE)?,
                DMUB_CW6_BASE,
                self.regions[WINDOW_FW_STATE].size,
            ),
        )?;

        let inbox_base = if cached_inbox {
            DMUB_CW4_BASE
        } else {
            0x8000_0000
        };
        let outbox_base = if cached_inbox {
            DMUB_CW4_BASE + DMUB_RB_SIZE
        } else {
            0x8000_2000
        };
        dev.regs.write_dcn(
            dcn::mmDMCUB_INBOX1_BASE_ADDRESS,
            dcn::mmDMCUB_INBOX1_BASE_ADDRESS_BASE_IDX as usize,
            inbox_base,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_INBOX1_SIZE,
            dcn::mmDMCUB_INBOX1_SIZE_BASE_IDX as usize,
            DMUB_RB_SIZE,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_OUTBOX1_BASE_ADDRESS,
            dcn::mmDMCUB_OUTBOX1_BASE_ADDRESS_BASE_IDX as usize,
            outbox_base,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_OUTBOX1_SIZE,
            dcn::mmDMCUB_OUTBOX1_SIZE_BASE_IDX as usize,
            DMUB_RB_SIZE,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_OUTBOX0_BASE_ADDRESS,
            dcn::mmDMCUB_OUTBOX0_BASE_ADDRESS_BASE_IDX as usize,
            DMUB_REGION5_BASE + TRACE_BUFFER_ENTRY_OFFSET,
        )?;
        dev.regs.write_dcn(
            dcn::mmDMCUB_OUTBOX0_SIZE,
            dcn::mmDMCUB_OUTBOX0_SIZE_BASE_IDX as usize,
            self.regions[WINDOW_TRACE].size - TRACE_BUFFER_ENTRY_OFFSET,
        )?;

        dev_info!(
            "astra: DMCUB windows: CW2 {:#x}->{:#x}, CW3 {:#x}, CW4 {:#x}, CW5 {:#x}, CW6 {:#x}",
            cw2_base,
            gpu(WINDOW_BSS_DATA)?,
            gpu(WINDOW_VBIOS)?,
            gpu(WINDOW_MAILBOX)?,
            gpu(WINDOW_TRACE)?,
            gpu(WINDOW_FW_STATE)?,
        );
        Ok(())
    }

    fn program_cw2(&self, regs: &mut Regs, offset: u64, base: u32) -> Result<()> {
        if self.regions[WINDOW_BSS_DATA].size == 0 {
            for (reg, base_idx) in [
                (
                    dcn::mmDMCUB_REGION3_CW2_OFFSET,
                    dcn::mmDMCUB_REGION3_CW2_OFFSET_BASE_IDX,
                ),
                (
                    dcn::mmDMCUB_REGION3_CW2_OFFSET_HIGH,
                    dcn::mmDMCUB_REGION3_CW2_OFFSET_HIGH_BASE_IDX,
                ),
                (
                    dcn::mmDMCUB_REGION3_CW2_BASE_ADDRESS,
                    dcn::mmDMCUB_REGION3_CW2_BASE_ADDRESS_BASE_IDX,
                ),
                (
                    dcn::mmDMCUB_REGION3_CW2_TOP_ADDRESS,
                    dcn::mmDMCUB_REGION3_CW2_TOP_ADDRESS_BASE_IDX,
                ),
            ] {
                regs.write_dcn(reg, base_idx as usize, 0)?;
            }
            return Ok(());
        }
        Self::program_cw(
            regs,
            CW2_REGISTERS,
            CacheWindow::new(offset, base, self.regions[WINDOW_BSS_DATA].size),
        )
    }

    fn program_cw(
        regs: &mut Regs,
        registers: CacheWindowRegisters,
        window: CacheWindow,
    ) -> Result<()> {
        let idx = dcn::mmDMCUB_REGION3_CW2_OFFSET_BASE_IDX as usize;
        regs.write_dcn(registers.offset, idx, window.offset as u32)?;
        regs.write_dcn(registers.offset_high, idx, (window.offset >> 32) as u32)?;
        regs.write_dcn(registers.base, idx, window.base)?;
        // All CWx top-address registers share the DCN3 bit layout.
        regs.write_dcn(registers.top, idx, (window.top & 0x1fff_ffff) | 0x8000_0000)
    }

    fn release_reset(&self, regs: &mut Regs, psp_version: u32) -> Result<()> {
        Self::rmw(
            regs,
            dcn::mmMMHUBBUB_SOFT_RESET,
            dcn::mmMMHUBBUB_SOFT_RESET_BASE_IDX as usize,
            dcn::MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET__SHIFT,
            dcn::MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK,
            0,
        )?;
        regs.write_dcn(
            dcn::mmDMCUB_SCRATCH15,
            dcn::mmDMCUB_SCRATCH15_BASE_IDX as usize,
            psp_version & 0x0011_00ff,
        )?;
        regs.write_dcn(
            dcn::mmDMCUB_SCRATCH14,
            dcn::mmDMCUB_SCRATCH14_BASE_IDX as usize,
            0,
        )?;
        let mut cntl = regs.read_dcn(dcn::mmDMCUB_CNTL, dcn::mmDMCUB_CNTL_BASE_IDX as usize)?;
        cntl = set_field(
            cntl,
            dcn::DMCUB_CNTL__DMCUB_ENABLE__SHIFT,
            dcn::DMCUB_CNTL__DMCUB_ENABLE_MASK,
            1,
        );
        cntl = set_field(
            cntl,
            dcn::DMCUB_CNTL__DMCUB_TRACEPORT_EN__SHIFT,
            dcn::DMCUB_CNTL__DMCUB_TRACEPORT_EN_MASK,
            1,
        );
        regs.write_dcn(dcn::mmDMCUB_CNTL, dcn::mmDMCUB_CNTL_BASE_IDX as usize, cntl)?;
        Self::rmw(
            regs,
            dcn::mmDMCUB_CNTL,
            dcn::mmDMCUB_CNTL_BASE_IDX as usize,
            dcn::DMCUB_CNTL__DMCUB_SOFT_RESET__SHIFT,
            dcn::DMCUB_CNTL__DMCUB_SOFT_RESET_MASK,
            0,
        )
    }

    fn write_region(&self, regs: &mut Regs, window: usize, bytes: &[u8]) -> Result<()> {
        let region = self.regions[window];
        if bytes.len() > region.size as usize {
            return Err(Error::Range);
        }
        let bo = self.bo.as_ref().ok_or(Error::NoDevice)?;
        let pos = bo
            .gpu_addr
            .checked_add(region.base as u64)
            .ok_or(Error::Range)?;
        let mut dwords = Vec::with_capacity(bytes.len().div_ceil(4));
        for chunk in bytes.chunks(4) {
            let mut word = [0u8; 4];
            word[..chunk.len()].copy_from_slice(chunk);
            dwords.push(u32::from_le_bytes(word));
        }
        regs.vram_write_dwords(pos, &dwords)
    }

    fn zero_region(&self, regs: &mut Regs, window: usize) -> Result<()> {
        let region = self.regions[window];
        let bo = self.bo.as_ref().ok_or(Error::NoDevice)?;
        let mut pos = bo
            .gpu_addr
            .checked_add(region.base as u64)
            .ok_or(Error::Range)?;
        let zeros = [0u32; 1024];
        let mut left = region.size as usize / 4;
        while left != 0 {
            let count = left.min(zeros.len());
            regs.vram_write_dwords(pos, &zeros[..count])?;
            pos += (count * 4) as u64;
            left -= count;
        }
        Ok(())
    }

    fn find_meta(&self, logical_inst_size: usize) -> Option<MetaInfo> {
        let blob_start = self.payload_offset.checked_add(PSP_HEADER_BYTES)?;
        for footer in PSP_FOOTER_BYTES {
            let blob_size = logical_inst_size.checked_sub(footer)?;
            for padding in 0..16usize {
                let at = blob_start
                    .checked_add(blob_size)?
                    .checked_sub(padding.checked_add(64)?)?;
                if Self::u32_at(&self.fw, at)? != DMUB_FW_META_MAGIC {
                    continue;
                }
                return Some(MetaInfo {
                    fw_region_size: Self::u32_at(&self.fw, at + 4)?,
                    trace_buffer_size: Self::u32_at(&self.fw, at + 8)?,
                    fw_version: Self::u32_at(&self.fw, at + 12)?,
                    shared_state_size: Self::u32_at(&self.fw, at + 20)?,
                });
            }
        }
        None
    }

    fn diagnostics(&self, regs: &mut Regs) {
        let read =
            |regs: &mut Regs, reg, idx| regs.read_dcn(reg, idx as usize).unwrap_or(0xffff_ffff);
        dev_info!(
            "astra: DMCUB diag CNTL={:#010x} RESET={:#010x} SCRATCH0={:#010x} S7={:#010x} S14={:#010x} S15={:#010x}",
            read(regs, dcn::mmDMCUB_CNTL, dcn::mmDMCUB_CNTL_BASE_IDX),
            read(
                regs,
                dcn::mmMMHUBBUB_SOFT_RESET,
                dcn::mmMMHUBBUB_SOFT_RESET_BASE_IDX
            ),
            read(regs, dcn::mmDMCUB_SCRATCH0, dcn::mmDMCUB_SCRATCH0_BASE_IDX),
            read(regs, dcn::mmDMCUB_SCRATCH7, dcn::mmDMCUB_SCRATCH7_BASE_IDX),
            read(
                regs,
                dcn::mmDMCUB_SCRATCH14,
                dcn::mmDMCUB_SCRATCH14_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_SCRATCH15,
                dcn::mmDMCUB_SCRATCH15_BASE_IDX
            ),
        );
        dev_info!(
            "astra: DMCUB diag CW2={:#010x}/{:#010x} CW3={:#010x}/{:#010x} CW4={:#010x}/{:#010x}",
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW2_OFFSET,
                dcn::mmDMCUB_REGION3_CW2_OFFSET_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW2_TOP_ADDRESS,
                dcn::mmDMCUB_REGION3_CW2_TOP_ADDRESS_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW3_OFFSET,
                dcn::mmDMCUB_REGION3_CW3_OFFSET_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW3_TOP_ADDRESS,
                dcn::mmDMCUB_REGION3_CW3_TOP_ADDRESS_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW4_OFFSET,
                dcn::mmDMCUB_REGION3_CW4_OFFSET_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW4_TOP_ADDRESS,
                dcn::mmDMCUB_REGION3_CW4_TOP_ADDRESS_BASE_IDX
            ),
        );
        dev_info!(
            "astra: DMCUB diag CW5={:#010x}/{:#010x} CW6={:#010x}/{:#010x} inbox base/size/r/w={:#x}/{:#x}/{:#x}/{:#x}",
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW5_OFFSET,
                dcn::mmDMCUB_REGION3_CW5_OFFSET_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW5_TOP_ADDRESS,
                dcn::mmDMCUB_REGION3_CW5_TOP_ADDRESS_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW6_OFFSET,
                dcn::mmDMCUB_REGION3_CW6_OFFSET_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_REGION3_CW6_TOP_ADDRESS,
                dcn::mmDMCUB_REGION3_CW6_TOP_ADDRESS_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_INBOX1_BASE_ADDRESS,
                dcn::mmDMCUB_INBOX1_BASE_ADDRESS_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_INBOX1_SIZE,
                dcn::mmDMCUB_INBOX1_SIZE_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_INBOX1_RPTR,
                dcn::mmDMCUB_INBOX1_RPTR_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_INBOX1_WPTR,
                dcn::mmDMCUB_INBOX1_WPTR_BASE_IDX
            ),
        );
        dev_info!(
            "astra: DMCUB faults undefined={:#010x} ifetch={:#010x} write={:#010x}",
            read(
                regs,
                dcn::mmDMCUB_UNDEFINED_ADDRESS_FAULT_ADDR,
                dcn::mmDMCUB_UNDEFINED_ADDRESS_FAULT_ADDR_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_INST_FETCH_FAULT_ADDR,
                dcn::mmDMCUB_INST_FETCH_FAULT_ADDR_BASE_IDX
            ),
            read(
                regs,
                dcn::mmDMCUB_DATA_WRITE_FAULT_ADDR,
                dcn::mmDMCUB_DATA_WRITE_FAULT_ADDR_BASE_IDX
            ),
        );
    }

    fn rmw(
        regs: &mut Regs,
        reg: u32,
        base_idx: usize,
        shift: u64,
        mask: u64,
        value: u64,
    ) -> Result<()> {
        let current = regs.read_dcn(reg, base_idx)?;
        regs.write_dcn(reg, base_idx, set_field(current, shift, mask, value))
    }

    const fn fw_version(major: u32, minor: u32, revision: u32) -> u32 {
        ((major & 0xff) << 24) | ((minor & 0xff) << 16) | ((revision & 0xff) << 8)
    }

    fn u32_at(data: &[u8], offset: usize) -> Option<u32> {
        let bytes = data.get(offset..offset + 4)?;
        Some(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }
}
