//! PSP IP block (Linux `psp_v13_0.c` + `amdgpu_psp.c`): secure OS boot,
//! ring creation, firmware delivery via `LOAD_IP_FW` and TA loading.

use alloc::vec::Vec;
use core::time::Duration;

use na_std::time;
use na_std::{Error, Result};

use crate::atom::FIRMWARE_CAP_ENABLE_2STAGE_BIST_TRAINING;
use crate::dev_info;
use crate::device::Adapter;
use crate::firmware::{FirmwareCatalog, UcodeId};
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::mem::Bo;
use crate::regs::Regs;
use crate::regs::hdp5_0_0 as hdp;
use crate::regs::mp13_0_2 as mp;
use crate::ucode::*;

/// PSP_1_MEG (amdgpu_psp.h): primary firmware staging buffer.
const FW_PRI_SIZE: usize = 0x10_0000;
/// Default bare-metal TMR size/alignment (`PSP_TMR_SIZE` and
/// `PSP_TMR_ALIGNMENT`). Navi23 does not have a boot-time TMR, so the
/// driver must reserve and submit this region before loading IP firmware.
const TMR_SIZE: usize = 0x40_0000;
const TMR_ALIGNMENT: usize = 0x10_0000;
/// KM ring size (psp_ring_init).
const KM_RING_SIZE: usize = 0x1000;
/// Fence/cmd buffer sizes.
const FENCE_BUF_SIZE: usize = 0x1000;
const CMD_BUF_SIZE: usize = 0x1000;
/// Fence poll timeout (Linux `adev->psp_timeout` iterations).
const PSP_TIMEOUT: u32 = 2000;
/// Linux `AMDGPU_MAX_USEC_TIMEOUT`: one mailbox poll attempt is 100 ms.
const MBOX_TIMEOUT_US: u32 = 100_000;
/// Memory training data size (GDDR6_MEM_TRAINING_DATA_SIZE_IN_BYTES).
const MEM_TRAIN_DATA_SIZE: u64 = 0x1000;
/// VBIOS-to-driver training data location at the top of VRAM.
const MEM_TRAIN_P2C_OFFSET: u64 = 0x8000;
/// Linux fallback firmware reservation when firmwareinfo does not advertise
/// `fw_reserved_size_in_kb`.
const DISCOVERY_TMR_OFFSET: u64 = 64 << 10;
/// Linux `BIST_MEM_TRAINING_ENCROACHED_SIZE`.
const MEM_TRAIN_ENCROACHED_SIZE: usize = 0x200_0000;
/// Linux `MEM_TRAIN_SEND_MSG_TIMEOUT_US`.
const MEM_TRAIN_SEND_MSG_TIMEOUT_US: u32 = 3_000_000;

/// One loaded TA context.
struct TaContext {
    desc: TaBinDesc,
    shared: Bo,
}

pub struct PspBlock {
    version: IpVersion,
    /// autoload_supported (psp_early_init): RLC autoload + PSP-only
    /// firmware delivery.
    autoload: bool,
    sos_data: Vec<u8>,
    sos_offset: usize,
    sos_size: usize,
    toc_offset: usize,
    toc_size: usize,
    boot_components: Vec<PspBootComponent>,
    mem_train_cache: Vec<u32>,
    ta_data: Vec<u8>,
    ta_descs: Vec<TaBinDesc>,
    tas: Vec<TaContext>,
    fw_pri: Option<Bo>,
    fence_buf: Option<Bo>,
    cmd_buf: Option<Bo>,
    km_ring: Option<Bo>,
    tmr: Option<Bo>,
    ring_wptr: u32,
    fence_value: u32,
    /// Discovery base index used by the MP0 C2PMSG register block.
    mailbox_base_idx: usize,
    /// C2PMSG full registers (MP0 base + reg).
    c2pmsg35: u32,
    c2pmsg36: u32,
    c2pmsg64: u32,
    c2pmsg67: u32,
    c2pmsg69: u32,
    c2pmsg70: u32,
    c2pmsg71: u32,
    c2pmsg81: u32,
}

impl PspBlock {
    pub fn new(version: IpVersion) -> Self {
        Self {
            version,
            autoload: true, // MP0 13.0.x (psp_early_init)
            sos_data: Vec::new(),
            sos_offset: 0,
            sos_size: 0,
            toc_offset: 0,
            toc_size: 0,
            boot_components: Vec::new(),
            mem_train_cache: Vec::new(),
            ta_data: Vec::new(),
            ta_descs: Vec::new(),
            tas: Vec::new(),
            fw_pri: None,
            fence_buf: None,
            cmd_buf: None,
            km_ring: None,
            tmr: None,
            ring_wptr: 0,
            fence_value: 0,
            mailbox_base_idx: 0,
            c2pmsg35: 0,
            c2pmsg36: 0,
            c2pmsg64: 0,
            c2pmsg67: 0,
            c2pmsg69: 0,
            c2pmsg70: 0,
            c2pmsg71: 0,
            c2pmsg81: 0,
        }
    }

    fn init_mailbox(&mut self, regs: &mut Regs) -> Result<()> {
        // MP0 C2PMSG registers are dword offsets relative to MP0 base[0]
        // (`regMP0_SMN_C2PMSG_*_BASE_IDX == 0`); `c2pmsg_read`/`write` add
        // base[0], so store only the block offsets here.
        let base0 = regs.base_u32(HwIp::Mp0, 0, 0).unwrap_or(0);
        let base1 = regs.base_u32(HwIp::Mp0, 0, 1).unwrap_or(0);
        self.c2pmsg35 = mp::regMP0_SMN_C2PMSG_35;
        self.c2pmsg36 = mp::regMP0_SMN_C2PMSG_36;
        self.c2pmsg64 = mp::regMP0_SMN_C2PMSG_64;
        self.c2pmsg67 = mp::regMP0_SMN_C2PMSG_67;
        self.c2pmsg69 = mp::regMP0_SMN_C2PMSG_69;
        self.c2pmsg70 = mp::regMP0_SMN_C2PMSG_70;
        self.c2pmsg71 = mp::regMP0_SMN_C2PMSG_71;
        self.c2pmsg81 = mp::regMP0_SMN_C2PMSG_81;

        // MP0 11.0 normally uses base[0]. Probe both discovery bases so a
        // malformed/variant discovery table cannot silently turn all PSP
        // mailbox reads into zero. C2PMSG_33 is IFWI readiness, 35 is the
        // bootloader handshake and 81 is the sOS sign-of-life register.
        let mut snapshots = [(0u32, 0u32, 0u32); 2];
        for (base_idx, snapshot) in snapshots.iter_mut().enumerate() {
            snapshot.0 = regs
                .read_ip(HwIp::Mp0, 0, mp::regMP0_SMN_C2PMSG_33, base_idx)
                .unwrap_or(u32::MAX);
            snapshot.1 = regs
                .read_ip(HwIp::Mp0, 0, self.c2pmsg35, base_idx)
                .unwrap_or(u32::MAX);
            snapshot.2 = regs
                .read_ip(HwIp::Mp0, 0, self.c2pmsg81, base_idx)
                .unwrap_or(u32::MAX);
        }
        if snapshots[0] == (0, 0, 0) && snapshots[1] != (0, 0, 0) {
            self.mailbox_base_idx = 1;
        }
        dev_info!(
            "astra: MP0 mailbox bases {:#x} {:#x}, using base[{}]",
            base0,
            base1,
            self.mailbox_base_idx
        );
        Ok(())
    }

    fn c2pmsg_read(&self, regs: &mut Regs, reg: u32) -> Result<u32> {
        regs.read_ip(HwIp::Mp0, 0, reg, self.mailbox_base_idx)
    }

    fn c2pmsg_write(&self, regs: &mut Regs, reg: u32, value: u32) -> Result<()> {
        regs.write_ip(HwIp::Mp0, 0, reg, self.mailbox_base_idx, value)
    }

    /// Polls a C2PMSG register until `(value & mask) == expected`, or
    /// until the value changes (Linux `psp_wait_for`).
    fn psp_wait_for(
        &self,
        regs: &mut Regs,
        reg: u32,
        mask: u32,
        expected: u32,
        changed: bool,
    ) -> Result<()> {
        self.psp_wait_for_inner(regs, reg, mask, expected, changed, true)
    }

    fn psp_wait_for_quiet(
        &self,
        regs: &mut Regs,
        reg: u32,
        mask: u32,
        expected: u32,
        changed: bool,
    ) -> Result<()> {
        self.psp_wait_for_inner(regs, reg, mask, expected, changed, false)
    }

    fn psp_wait_for_inner(
        &self,
        regs: &mut Regs,
        reg: u32,
        mask: u32,
        expected: u32,
        changed: bool,
        log_timeout: bool,
    ) -> Result<()> {
        let original = self.c2pmsg_read(regs, reg)?;
        for _ in 0..MBOX_TIMEOUT_US {
            let value = self.c2pmsg_read(regs, reg)?;
            if changed {
                if value != original {
                    return Ok(());
                }
            } else if value & mask == expected {
                return Ok(());
            }
            time::delay(Duration::from_micros(1));
        }
        if log_timeout {
            let value = self.c2pmsg_read(regs, reg).unwrap_or(0xffff_ffff);
            dev_info!(
                "astra: PSP mailbox timeout reg {:#x}: read {:#010x}, mask {:#010x}, expected {:#010x}",
                reg,
                value,
                mask,
                expected
            );
        }
        Err(Error::Io)
    }

    /// Linux `psp_mem_training(PSP_MEM_TRAIN_COLD_BOOT)` and
    /// `psp_v11_0_memory_training`: preserve the 32 MiB region touched by
    /// BIST, run long training with its dedicated 3-second timeout, restore
    /// VRAM, then save the PSP-to-driver training data.
    fn mem_training(&mut self, dev: &mut Adapter) -> Result<()> {
        let capability = dev
            .atom
            .as_ref()
            .and_then(|atom| atom.firmware_info())
            .map(|fw| fw.firmware_capability)
            .unwrap_or(0);
        if capability & FIRMWARE_CAP_ENABLE_2STAGE_BIST_TRAINING == 0 {
            return Ok(());
        }
        if self.c2pmsg_read(&mut dev.regs, self.c2pmsg81)? != 0 {
            dev_info!("astra: PSP sOS alive, skipping DRAM training");
            return Ok(());
        }

        let vram = dev.gmc.mc_vram_size;
        let fw_reserved_size = dev
            .atom
            .as_ref()
            .and_then(|atom| atom.firmware_info())
            .map(|info| info.fw_reserved_size)
            .filter(|size| *size != 0)
            .unwrap_or(DISCOVERY_TMR_OFFSET);
        // Linux amdgpu_ttm_init_mem_train_resv_region(): place the C2P
        // block one MiB below the VBIOS firmware reservation, rounded up to
        // a one-MiB boundary.
        let c2p_offset = vram
            .checked_sub(fw_reserved_size)
            .and_then(|value| value.checked_sub(0x10_0000))
            .and_then(|value| value.checked_add(0x10_0000 - 1))
            .map(|value| value & !(0x10_0000 - 1))
            .ok_or(Error::Range)?;
        let p2c_offset = vram - MEM_TRAIN_P2C_OFFSET;

        // Long training touches the first 32 MiB of VRAM. Linux preserves
        // this region, which now contains the pre-OS scanout and GART table.
        let save_words = MEM_TRAIN_ENCROACHED_SIZE / core::mem::size_of::<u32>();
        let mut saved_vram = Vec::new();
        saved_vram
            .try_reserve_exact(save_words)
            .map_err(|_| Error::OutOfMemory)?;
        saved_vram.resize(save_words, 0);
        for (index, chunk) in saved_vram.chunks_mut(1024).enumerate() {
            dev.regs.vram_read_dwords((index * 4096) as u64, chunk)?;
        }

        dev_info!(
            "astra: running PSP DRAM cold-boot training (c2p=0x{:X}, p2c=0x{:X}, preserving {} bytes)",
            c2p_offset,
            p2c_offset,
            MEM_TRAIN_ENCROACHED_SIZE,
        );
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg36, (c2p_offset >> 20) as u32)?;
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg35, PSP_BL_DRAM_LONG_TRAIN)?;

        let attempts = MEM_TRAIN_SEND_MSG_TIMEOUT_US / MBOX_TIMEOUT_US;
        let mut training_result = Err(Error::Io);
        for _ in 0..attempts {
            training_result = self.psp_wait_for_quiet(
                &mut dev.regs,
                self.c2pmsg35,
                0x8000_0000,
                0x8000_0000,
                false,
            );
            if training_result.is_ok() {
                break;
            }
        }

        // Restore even when the mailbox command failed.
        let mut restore_result = Ok(());
        for (index, chunk) in saved_vram.chunks(1024).enumerate() {
            if let Err(error) = dev.regs.vram_write_dwords((index * 4096) as u64, chunk) {
                restore_result = Err(error);
                break;
            }
        }
        restore_result?;

        if training_result.is_err() {
            dev_info!(
                "astra: PSP DRAM training failed: msg35={:#010x} msg36={:#010x} msg81={:#010x}",
                self.c2pmsg_read(&mut dev.regs, self.c2pmsg35)
                    .unwrap_or(u32::MAX),
                self.c2pmsg_read(&mut dev.regs, self.c2pmsg36)
                    .unwrap_or(u32::MAX),
                self.c2pmsg_read(&mut dev.regs, self.c2pmsg81)
                    .unwrap_or(u32::MAX),
            );
            return training_result;
        }

        let cache_words = (MEM_TRAIN_DATA_SIZE / 4) as usize;
        self.mem_train_cache.clear();
        self.mem_train_cache
            .try_reserve_exact(cache_words)
            .map_err(|_| Error::OutOfMemory)?;
        self.mem_train_cache.resize(cache_words, 0);
        dev.regs
            .vram_read_dwords(p2c_offset, &mut self.mem_train_cache)?;
        dev_info!(
            "astra: PSP DRAM training complete (data signature {:#010x})",
            self.mem_train_cache.first().copied().unwrap_or(0),
        );
        Ok(())
    }

    /// Linux `psp_v11_0_wait_for_bootloader` / `psp_v13_0_wait_for_bootloader`.
    fn wait_for_bootloader(&self, dev: &mut Adapter) -> Result<()> {
        let (mask, retries) = if self.version.major == 11 {
            // psp_v11_0_wait_for_bootloader: bit 31 set, error bits 15:0 clear.
            (MBOX_TOS_READY_MASK, 20)
        } else {
            // psp_v13_0_wait_for_bootloader: all remaining bits clear.
            (u32::MAX, 10)
        };
        for _ in 0..retries {
            if self
                .psp_wait_for_quiet(&mut dev.regs, self.c2pmsg35, mask, 0x8000_0000, false)
                .is_ok()
            {
                return Ok(());
            }
        }
        Err(Error::Io)
    }

    fn bootloader_load_component(
        &mut self,
        dev: &mut Adapter,
        offset: usize,
        size: usize,
        command: u32,
    ) -> Result<()> {
        if let Err(error) = self.wait_for_bootloader(dev) {
            self.log_bootloader_failure(dev, command, "before command");
            return Err(error);
        }
        let fw_pri_mc = self.fw_pri.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let image = self
            .sos_data
            .get(offset..offset.checked_add(size).ok_or(Error::Range)?)
            .ok_or(Error::Range)?
            .to_vec();
        {
            let fw_pri = self.fw_pri.as_mut().ok_or(Error::NoDevice)?;
            let cpu = fw_pri.cpu.as_mut().ok_or(Error::NoDevice)?;
            cpu.as_mut_slice()
                .get_mut(..size)
                .ok_or(Error::Range)?
                .copy_from_slice(&image);
            cpu.sync_for_device();
        }
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg36, (fw_pri_mc >> 20) as u32)?;
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg35, command)?;
        dev_info!(
            "astra: PSP bootloader command {:#x} ({} bytes at fw_pri {:#x})",
            command,
            size,
            fw_pri_mc
        );
        if command != PSP_BL_LOAD_SOSDRV {
            let result = self.wait_for_bootloader(dev);
            if result.is_err() {
                self.log_bootloader_failure(dev, command, "after command");
            }
            return result;
        }
        time::delay(Duration::from_millis(20));
        let result = self.psp_wait_for(&mut dev.regs, self.c2pmsg81, 0, 0, true);
        if result.is_err() {
            self.log_bootloader_failure(dev, command, "waiting for sOS sign-of-life");
        }
        result?;
        dev_info!("astra: sOS loaded via bootloader");
        Ok(())
    }

    fn log_bootloader_failure(&self, dev: &mut Adapter, command: u32, phase: &str) {
        let msg33 = self
            .c2pmsg_read(&mut dev.regs, mp::regMP0_SMN_C2PMSG_33)
            .unwrap_or(u32::MAX);
        let msg35 = self
            .c2pmsg_read(&mut dev.regs, self.c2pmsg35)
            .unwrap_or(u32::MAX);
        let msg36 = self
            .c2pmsg_read(&mut dev.regs, self.c2pmsg36)
            .unwrap_or(u32::MAX);
        let msg81 = self
            .c2pmsg_read(&mut dev.regs, self.c2pmsg81)
            .unwrap_or(u32::MAX);
        dev_info!(
            "astra: PSP bootloader failure {} command {:#x}: msg33={:#010x} msg35={:#010x} msg36={:#010x} msg81={:#010x}",
            phase,
            command,
            msg33,
            msg35,
            msg36,
            msg81,
        );
    }

    /// Linux `psp_hw_start`: load packaged boot components, then the sOS.
    fn bootloader_load_sos(&mut self, dev: &mut Adapter) -> Result<()> {
        if self.c2pmsg_read(&mut dev.regs, self.c2pmsg81)? != 0 {
            dev_info!("astra: sOS already alive, skipping SOS load");
            return Ok(());
        }

        for component in self.boot_components.clone() {
            self.bootloader_load_component(
                dev,
                component.offset_bytes as usize,
                component.size_bytes as usize,
                component.command,
            )?;
        }
        self.bootloader_load_component(dev, self.sos_offset, self.sos_size, PSP_BL_LOAD_SOSDRV)
    }

    /// Linux `psp_v13_0_ring_create` (bare-metal path).
    fn ring_create(&mut self, dev: &mut Adapter) -> Result<()> {
        self.psp_wait_for(
            &mut dev.regs,
            self.c2pmsg64,
            MBOX_TOS_READY_MASK,
            MBOX_TOS_READY_FLAG,
            false,
        )?;

        let ring_mc = self.km_ring.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg69, ring_mc as u32)?;
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg70, (ring_mc >> 32) as u32)?;
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg71, KM_RING_SIZE as u32)?;
        // enum psp_ring_type: KM = 2, encoded in bits 31:16.
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg64, 2 << 16)?;
        time::delay(Duration::from_millis(20));
        self.psp_wait_for(
            &mut dev.regs,
            self.c2pmsg64,
            MBOX_TOS_RESP_MASK,
            MBOX_TOS_RESP_FLAG,
            false,
        )?;
        dev_info!("astra: PSP KM ring created");
        Ok(())
    }

    /// Builds `psp_gfx_cmd_setup_tmr`. Linux prefers a naturally-aligned VRAM
    /// BO and supplies both its MC and VM-walker physical addresses.
    fn put_u32(buffer: &mut [u8], offset: usize, value: u32) {
        if let Some(destination) = buffer.get_mut(offset..offset + 4) {
            destination.copy_from_slice(&value.to_le_bytes());
        }
    }

    fn build_setup_tmr_cmd(tmr_mc: u64, tmr_pa: u64, size: u32) -> [u8; PSP_CMD_RESP_SIZE] {
        let mut cmd = [0u8; PSP_CMD_RESP_SIZE];
        Self::put_u32(&mut cmd, 0, PSP_CMD_RESP_SIZE as u32);
        Self::put_u32(&mut cmd, 4, PSP_GFX_CMD_BUF_VERSION);
        Self::put_u32(&mut cmd, 8, GFX_CMD_ID_SETUP_TMR);
        Self::put_u32(&mut cmd, 28, tmr_mc as u32);
        Self::put_u32(&mut cmd, 32, (tmr_mc >> 32) as u32);
        Self::put_u32(&mut cmd, 36, size);
        Self::put_u32(&mut cmd, 40, 1 << 1); // virt_phy_addr
        Self::put_u32(&mut cmd, 44, tmr_pa as u32);
        Self::put_u32(&mut cmd, 48, (tmr_pa >> 32) as u32);
        cmd
    }

    fn setup_tmr(&mut self, dev: &mut Adapter) -> Result<()> {
        let tmr = self.tmr.as_ref().ok_or(Error::NoDevice)?;
        if tmr.place != crate::mem::Place::Vram {
            return Err(Error::InvalidArgument);
        }
        let tmr_mc = dev
            .gmc
            .fb_start
            .checked_add(tmr.gpu_addr)
            .ok_or(Error::Range)?;
        let tmr_pa = tmr_mc
            .checked_sub(dev.gmc.vram_start)
            .and_then(|offset| offset.checked_add(dev.gmc.vram_base_offset))
            .ok_or(Error::Range)?;
        let cmd = Self::build_setup_tmr_cmd(tmr_mc, tmr_pa, tmr.size as u32);
        dev_info!(
            "astra: reserving PSP TMR: {} bytes at GPU {:#x}, PA {:#x}",
            tmr.size,
            tmr_mc,
            tmr_pa
        );
        dev_info!(
            "astra: PSP SETUP_TMR fields: buf_lo={:#010x} buf_hi={:#010x} size={:#010x} flags={:#010x} sys_lo={:#010x} sys_hi={:#010x}",
            tmr_mc as u32,
            (tmr_mc >> 32) as u32,
            tmr.size as u32,
            1u32 << 1,
            tmr_pa as u32,
            (tmr_pa >> 32) as u32,
        );
        let (status, _, _, _) = self.cmd_submit(dev, &cmd)?;
        if status != 0 {
            // Linux treats a completed bare-metal PSP command as successful
            // even when old PSP firmware leaves a stale non-zero status.
            dev_info!(
                "astra: PSP SETUP_TMR completed with response status {:#x}",
                status
            );
            // A GlobalPlatform TEE error is a real command failure.  Do not
            // submit LOAD_IP_FW commands without a valid TMR: the PSP may DMA
            // firmware to an undefined destination and corrupt host memory.
            if status & 0xffff_0000 == 0xffff_0000 {
                return Err(Error::Io);
            }
        }
        Ok(())
    }

    /// Linux `psp_check_pmfw_centralized_cstate_management`.
    fn pmfw_centralized_cstate_management(&self) -> bool {
        matches!(
            (
                self.version.major,
                self.version.minor,
                self.version.revision
            ),
            (11, 0, 0)
                | (11, 0, 4)
                | (11, 0, 5)
                | (11, 0, 7)
                | (11, 0, 9)
                | (11, 0, 11)
                | (11, 0, 12)
                | (11, 0, 13)
                | (13, 0, 0)
                | (13, 0, 2)
                | (13, 0, 7)
        )
    }

    /// Linux `psp_load_smu_fw`: PMFW must precede TMR setup on Navi23.
    fn load_smu_fw(&mut self, dev: &mut Adapter) -> Result<()> {
        let (mc_addr, size) = dev
            .firmware(UcodeId::Smc)
            .map(|firmware| (firmware.mc_addr, firmware.size))
            .ok_or(Error::NoDevice)?;
        let cmd = Self::build_load_ip_fw_cmd(mc_addr, size as u32, UcodeId::Smc.psp_fw_type());
        let (status, fw_addr_lo, fw_addr_hi, _) = self.cmd_submit(dev, &cmd)?;
        if status != 0 {
            dev_info!(
                "astra: PSP PMFW load completed with response status {:#x}",
                status
            );
            if status & 0xffff_0000 == 0xffff_0000 {
                return Err(Error::Io);
            }
        } else {
            dev_info!("astra: PMFW loaded via PSP");
        }
        let tmr_addr = ((fw_addr_hi as u64) << 32) | fw_addr_lo as u64;
        if let Some(firmware) = dev.firmware_mut(UcodeId::Smc) {
            firmware.tmr_addr = Some(tmr_addr);
        }
        Ok(())
    }

    /// Builds the 1024-byte command/response buffer for `LOAD_IP_FW`.
    fn build_load_ip_fw_cmd(fw_mc: u64, size: u32, fw_type: u32) -> [u8; PSP_CMD_RESP_SIZE] {
        let mut cmd = [0u8; PSP_CMD_RESP_SIZE];
        Self::put_u32(&mut cmd, 0, PSP_CMD_RESP_SIZE as u32);
        Self::put_u32(&mut cmd, 4, PSP_GFX_CMD_BUF_VERSION);
        Self::put_u32(&mut cmd, 8, GFX_CMD_ID_LOAD_IP_FW);
        Self::put_u32(&mut cmd, 28, fw_mc as u32);
        Self::put_u32(&mut cmd, 32, (fw_mc >> 32) as u32);
        Self::put_u32(&mut cmd, 36, size);
        Self::put_u32(&mut cmd, 40, fw_type);
        cmd
    }

    fn build_load_toc_cmd(fw_pri_mc: u64, size: u32) -> [u8; PSP_CMD_RESP_SIZE] {
        let mut cmd = [0u8; PSP_CMD_RESP_SIZE];
        Self::put_u32(&mut cmd, 0, PSP_CMD_RESP_SIZE as u32);
        Self::put_u32(&mut cmd, 4, PSP_GFX_CMD_BUF_VERSION);
        Self::put_u32(&mut cmd, 8, GFX_CMD_ID_LOAD_TOC);
        Self::put_u32(&mut cmd, 28, fw_pri_mc as u32);
        Self::put_u32(&mut cmd, 32, (fw_pri_mc >> 32) as u32);
        Self::put_u32(&mut cmd, 36, size);
        cmd
    }

    /// Linux `psp_load_toc`: the PSP parses the RLC autoload table and
    /// returns the exact TMR size required for the staged IP firmware.
    fn load_toc(&mut self, dev: &mut Adapter) -> Result<usize> {
        if self.toc_size == 0 {
            return Ok(TMR_SIZE);
        }
        let toc_end = self
            .toc_offset
            .checked_add(self.toc_size)
            .ok_or(Error::Range)?;
        let toc = self
            .sos_data
            .get(self.toc_offset..toc_end)
            .ok_or(Error::Range)?
            .to_vec();
        let fw_pri_mc = self.fw_pri.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        {
            let fw_pri = self.fw_pri.as_mut().ok_or(Error::NoDevice)?;
            let cpu = fw_pri.cpu.as_mut().ok_or(Error::NoDevice)?;
            cpu.as_mut_slice()
                .get_mut(..self.toc_size)
                .ok_or(Error::Range)?
                .copy_from_slice(&toc);
            cpu.sync_for_device();
        }
        dev_info!(
            "astra: PSP loading TOC ({} bytes at fw_pri {:#x})",
            self.toc_size,
            fw_pri_mc,
        );
        let cmd = Self::build_load_toc_cmd(fw_pri_mc, self.toc_size as u32);
        let (status, _, _, tmr_size) = self.cmd_submit(dev, &cmd)?;
        if status != 0 {
            dev_info!(
                "astra: PSP LOAD_TOC completed with response status {:#x}",
                status,
            );
            return Err(Error::Io);
        }
        let tmr_size = usize::try_from(tmr_size)
            .ok()
            .filter(|size| *size != 0)
            .ok_or(Error::Range)?;
        dev_info!("astra: PSP LOAD_TOC requested TMR size {} bytes", tmr_size);
        Ok(tmr_size)
    }

    /// Builds the command buffer for a TA load (LOAD_TA or LOAD_ASD).
    fn build_load_ta_cmd(
        cmd_id: u32,
        fw_pri_mc: u64,
        size: u32,
        shared_mc: u64,
        shared_size: u32,
    ) -> [u8; PSP_CMD_RESP_SIZE] {
        let mut cmd = [0u8; PSP_CMD_RESP_SIZE];
        Self::put_u32(&mut cmd, 0, PSP_CMD_RESP_SIZE as u32);
        Self::put_u32(&mut cmd, 4, PSP_GFX_CMD_BUF_VERSION);
        Self::put_u32(&mut cmd, 8, cmd_id);
        Self::put_u32(&mut cmd, 28, fw_pri_mc as u32);
        Self::put_u32(&mut cmd, 32, (fw_pri_mc >> 32) as u32);
        Self::put_u32(&mut cmd, 36, size);
        Self::put_u32(&mut cmd, 40, shared_mc as u32);
        Self::put_u32(&mut cmd, 44, (shared_mc >> 32) as u32);
        Self::put_u32(&mut cmd, 48, shared_size);
        cmd
    }

    /// Linux `psp_cmd_submit_buf` + `psp_ring_cmd_submit` + fence poll.
    fn cmd_submit(
        &mut self,
        dev: &mut Adapter,
        cmd: &[u8; PSP_CMD_RESP_SIZE],
    ) -> Result<(u32, u32, u32, u32)> {
        let (cmd_mc, fence_mc) = {
            (
                self.cmd_buf.as_ref().ok_or(Error::NoDevice)?.gpu_addr,
                self.fence_buf.as_ref().ok_or(Error::NoDevice)?.gpu_addr,
            )
        };
        self.fence_value = self.fence_value.wrapping_add(1);
        let index = self.fence_value;

        {
            let cmd_buf = self.cmd_buf.as_mut().ok_or(Error::NoDevice)?;
            let fence = self.fence_buf.as_mut().ok_or(Error::NoDevice)?;
            let ring = self.km_ring.as_mut().ok_or(Error::NoDevice)?;
            let cmd_cpu = cmd_buf.cpu.as_mut().ok_or(Error::NoDevice)?;
            cmd_cpu
                .as_mut_slice()
                .get_mut(..PSP_CMD_RESP_SIZE)
                .ok_or(Error::Range)?
                .copy_from_slice(cmd);
            cmd_cpu.sync_for_device();
            let fence_cpu = fence.cpu.as_mut().ok_or(Error::NoDevice)?;
            fence_cpu
                .as_mut_slice()
                .get_mut(..4)
                .ok_or(Error::Range)?
                .fill(0);
            fence_cpu.sync_for_device();

            // psp_gfx_rb_frame is 64 bytes / 16 dwords.
            let ring_cpu = ring.cpu.as_mut().ok_or(Error::NoDevice)?;
            let frame_at = (self.ring_wptr * 4) as usize;
            let frame = ring_cpu
                .as_mut_slice()
                .get_mut(frame_at..frame_at + 64)
                .ok_or(Error::Range)?;
            frame.fill(0);
            Self::put_u32(frame, 0, cmd_mc as u32);
            Self::put_u32(frame, 4, (cmd_mc >> 32) as u32);
            Self::put_u32(frame, 8, PSP_CMD_RESP_SIZE as u32);
            Self::put_u32(frame, 12, fence_mc as u32);
            Self::put_u32(frame, 16, (fence_mc >> 32) as u32);
            Self::put_u32(frame, 20, index);
            ring_cpu.sync_for_device();

            self.ring_wptr = (self.ring_wptr + 16) % (KM_RING_SIZE as u32 / 4);
        }

        self.flush_hdp(dev)?;
        self.c2pmsg_write(&mut dev.regs, self.c2pmsg67, self.ring_wptr)?;

        // Poll the fence (invalidate HDP + small delay each iteration).
        for _ in 0..PSP_TIMEOUT {
            let fence = self.fence_buf.as_ref().ok_or(Error::NoDevice)?;
            let fence_cpu = fence.cpu.as_ref().ok_or(Error::NoDevice)?;
            fence_cpu.sync_for_cpu();
            let value = fence_cpu
                .as_slice()
                .get(..4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                .unwrap_or(0);
            if value == index {
                let cmd_buf = self.cmd_buf.as_ref().ok_or(Error::NoDevice)?;
                let cmd_cpu = cmd_buf.cpu.as_ref().ok_or(Error::NoDevice)?;
                cmd_cpu.sync_for_cpu();
                let word = |at: usize| {
                    cmd_cpu
                        .as_slice()
                        .get(at..at + 4)
                        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                        .unwrap_or(0)
                };
                return Ok((word(864), word(872), word(876), word(880)));
            }
            self.invalidate_hdp(dev)?;
            time::delay(Duration::from_micros(60));
        }
        let mailbox_wptr = self
            .c2pmsg_read(&mut dev.regs, self.c2pmsg67)
            .unwrap_or(0xffff_ffff);
        dev_info!(
            "astra: PSP command timeout index {}, cmd {:#x}, ring wptr {} (mailbox {}), cmd_mc {:#x}, fence_mc {:#x}",
            index,
            u32::from_le_bytes([cmd[8], cmd[9], cmd[10], cmd[11]]),
            self.ring_wptr,
            mailbox_wptr,
            cmd_mc,
            fence_mc
        );
        Err(Error::Io)
    }

    /// `LOAD_IP_FW` + optional RLC autoload (Linux `psp_load_non_psp_fw`).
    fn load_non_psp_fw(&mut self, dev: &mut Adapter) -> Result<()> {
        let staged: Vec<(UcodeId, u64, usize)> = dev
            .firmwares()
            .map(|fw| (fw.id, fw.mc_addr, fw.size))
            .collect();

        for (id, mc_addr, size) in &staged {
            // With RLC autoload Linux's `fw_load_skip_check` skips PMFW. On
            // MP0 11.0.12 all SDMA instances share one image and PSP accepts
            // only SDMA0.
            if *id == UcodeId::Smc
                || (self.version.major == 11
                    && self.version.minor == 0
                    && self.version.revision == 12
                    && matches!(*id, UcodeId::Sdma1 | UcodeId::Sdma2 | UcodeId::Sdma3))
            {
                continue;
            }
            let cmd = Self::build_load_ip_fw_cmd(*mc_addr, *size as u32, id.psp_fw_type());
            let (status, fw_addr_lo, fw_addr_hi, _) = self.cmd_submit(dev, &cmd)?;
            if status != 0 {
                dev_info!(
                    "astra: PSP LOAD_IP_FW for {} completed with response status {:#x}",
                    id.index(),
                    status
                );
            }
            let tmr_addr = ((fw_addr_hi as u64) << 32) | fw_addr_lo as u64;
            if let Some(firmware) = dev.firmware_mut(*id) {
                firmware.tmr_addr = Some(tmr_addr);
            }
            let name = dev
                .firmware(*id)
                .map(|firmware| firmware.name.as_str())
                .unwrap_or("?");
            dev_info!("astra: firmware {} loaded via PSP", name);

            if self.autoload && *id == UcodeId::RlcG {
                let cmd = Self::build_simple_cmd(GFX_CMD_ID_AUTOLOAD_RLC);
                let (status, _, _, _) = self.cmd_submit(dev, &cmd)?;
                if status != 0 {
                    dev_info!(
                        "astra: PSP AUTOLOAD_RLC completed with response status {:#x}",
                        status
                    );
                }
                dev_info!("astra: RLC autoload started");
            }
        }
        Ok(())
    }

    /// Builds a command buffer carrying only the command id.
    fn build_simple_cmd(cmd_id: u32) -> [u8; PSP_CMD_RESP_SIZE] {
        let mut cmd = [0u8; PSP_CMD_RESP_SIZE];
        Self::put_u32(&mut cmd, 0, PSP_CMD_RESP_SIZE as u32);
        Self::put_u32(&mut cmd, 4, PSP_GFX_CMD_BUF_VERSION);
        Self::put_u32(&mut cmd, 8, cmd_id);
        cmd
    }

    /// Loads one TA from ta.bin (Linux `psp_ta_load`).
    fn ta_load(&mut self, dev: &mut Adapter, index: usize) -> Result<()> {
        let desc = self.tas[index].desc;
        let fw_pri_mc = self.fw_pri.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let shared_mc = self.tas[index].shared.gpu_addr;

        let bin = self
            .ta_data
            .get(desc.offset_bytes as usize..(desc.offset_bytes + desc.size_bytes) as usize)
            .ok_or(Error::Range)?
            .to_vec();
        {
            let fw_pri = self.fw_pri.as_mut().ok_or(Error::NoDevice)?;
            let cpu = fw_pri.cpu.as_mut().ok_or(Error::NoDevice)?;
            cpu.as_mut_slice()
                .get_mut(..desc.size_bytes as usize)
                .ok_or(Error::Range)?
                .copy_from_slice(&bin);
            cpu.sync_for_device();
        }

        let cmd = Self::build_load_ta_cmd(
            GFX_CMD_ID_LOAD_TA,
            fw_pri_mc,
            desc.size_bytes,
            shared_mc,
            TA_SHARED_MEM_SIZE as u32,
        );
        let (status, _, _, _) = self.cmd_submit(dev, &cmd)?;
        if status != 0 {
            dev_info!(
                "astra: PSP LOAD_TA type {} completed with response status {:#x}",
                desc.fw_type,
                status
            );
        }
        Ok(())
    }

    /// ASD is the only mandatory TA (Linux `psp_asd_initialize`).
    fn asd_initialize(&mut self, dev: &mut Adapter) -> Result<()> {
        let Some(index) = self
            .tas
            .iter()
            .position(|ta| ta.desc.fw_type == TA_TYPE_ASD)
        else {
            dev_info!("astra: no ASD TA found in ta.bin");
            return Err(Error::Io);
        };
        let desc = self.tas[index].desc;
        let fw_pri_mc = self.fw_pri.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let shared_mc = self.tas[index].shared.gpu_addr;

        let bin = self
            .ta_data
            .get(desc.offset_bytes as usize..(desc.offset_bytes + desc.size_bytes) as usize)
            .ok_or(Error::Range)?
            .to_vec();
        {
            let fw_pri = self.fw_pri.as_mut().ok_or(Error::NoDevice)?;
            let cpu = fw_pri.cpu.as_mut().ok_or(Error::NoDevice)?;
            cpu.as_mut_slice()
                .get_mut(..desc.size_bytes as usize)
                .ok_or(Error::Range)?
                .copy_from_slice(&bin);
            cpu.sync_for_device();
        }

        let cmd = Self::build_load_ta_cmd(
            GFX_CMD_ID_LOAD_ASD,
            fw_pri_mc,
            desc.size_bytes,
            shared_mc,
            ASD_SHARED_MEM_SIZE as u32,
        );
        let (status, _, _, _) = self.cmd_submit(dev, &cmd)?;
        if status != 0 {
            dev_info!(
                "astra: PSP LOAD_ASD completed with response status {:#x}",
                status
            );
        }
        dev_info!("astra: ASD TA loaded");
        Ok(())
    }

    /// Linux `amdgpu_device_flush_hdp` (CPU path).
    fn flush_hdp(&self, dev: &mut Adapter) -> Result<()> {
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            crate::regs::nbio4_3_0::regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL,
            2,
            0,
        )
    }

    /// Linux `amdgpu_device_invalidate_hdp` (CPU path).
    fn invalidate_hdp(&self, dev: &mut Adapter) -> Result<()> {
        dev.regs
            .write_ip(HwIp::Hdp, 0, hdp::mmHDP_READ_CACHE_INVALIDATE, 0, 1)
    }
}

impl IpBlock for PspBlock {
    fn hw_ip(&self) -> HwIp {
        HwIp::Mp0
    }

    fn name(&self) -> &'static str {
        if self.version.major == 11 {
            "PSP 11.0"
        } else {
            "PSP 13.0"
        }
    }

    /// Linux `psp_sw_init`: mailbox registers, SOS/TA firmware, memory
    /// training, command/fence/ring buffers.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.init_mailbox(&mut dev.regs)?;

        // SOS + TA microcode (psp_v13_0_init_microcode). Firmware images
        // are named by chip codename, not IP version (Linux `chip_name`).
        let firmware = FirmwareCatalog::for_adapter(dev);
        self.sos_data = firmware.load_suffix("sos")?;
        let sos_header = SosHeader::parse(&self.sos_data).ok_or(Error::Io)?;
        self.sos_offset = sos_header.sos_offset_bytes as usize;
        self.sos_size = sos_header.sos_size_bytes as usize;
        self.toc_offset = sos_header.toc_offset_bytes as usize;
        self.toc_size = sos_header.toc_size_bytes as usize;
        self.boot_components = sos_header.boot_components;
        if self.sos_size == 0 {
            return Err(Error::Io);
        }
        dev_info!(
            "astra: PSP SOS header v{}.{}: offset {:#x}, size {}, TOC offset {:#x} size {}, {} boot components",
            sos_header.header_version_major,
            sos_header.header_version_minor,
            self.sos_offset,
            self.sos_size,
            self.toc_offset,
            self.toc_size,
            self.boot_components.len()
        );

        self.ta_data = firmware.load_suffix("ta")?;
        let ta_header = TaHeader::parse(&self.ta_data).ok_or(Error::Io)?;
        dev_info!(
            "astra: PSP TA header v{}.{}: {} binaries",
            ta_header.header_version_major,
            ta_header.header_version_minor,
            ta_header.ta_fw_bin_count
        );
        for desc in ta_header.descriptors {
            dev_info!(
                "astra: TA firmware type {} version {} ({} bytes)",
                desc.fw_type,
                desc.fw_version,
                desc.size_bytes
            );
            self.ta_descs.push(desc);
        }

        // Cold-boot DRAM training before the SOS is up.
        self.mem_training(dev)?;

        // Command, fence and ring buffers.
        self.fw_pri = Some(
            dev.mem
                .alloc_gart_aligned(&mut dev.regs, FW_PRI_SIZE, FW_PRI_SIZE)?,
        );
        self.fence_buf = Some(dev.mem.alloc_gart(&mut dev.regs, FENCE_BUF_SIZE)?);
        self.cmd_buf = Some(dev.mem.alloc_gart(&mut dev.regs, CMD_BUF_SIZE)?);
        self.km_ring = Some(dev.mem.alloc_gart(&mut dev.regs, KM_RING_SIZE)?);

        // TA shared buffers.
        for desc in self.ta_descs.drain(..) {
            let shared = dev.mem.alloc_gart(&mut dev.regs, TA_SHARED_MEM_SIZE)?;
            self.tas.push(TaContext { desc, shared });
        }

        dev.psp_autoload = self.autoload;
        Ok(())
    }

    /// Linux `psp_hw_init`: ucode staging, SOS boot, ring, firmware
    /// delivery, TA loading.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // Stage every firmware image (amdgpu_ucode_init_bo).
        let staged = FirmwareCatalog::for_adapter(dev).stage(dev)?;
        // The staging BO was just bound into the already-live GART. Linux's
        // BO bind path synchronizes this mapping before PSP can consume it.
        dev.flush_gart()?;
        for fw in staged.iter() {
            dev_info!(
                "astra: staging firmware {} at 0x{:016x}",
                fw.name,
                fw.mc_addr
            );
        }
        dev.install_firmware(staged);

        let (fw_pri_mc, fw_pri_pa) = {
            let fw_pri = self.fw_pri.as_ref().ok_or(Error::NoDevice)?;
            let pa = fw_pri
                .cpu
                .as_ref()
                .ok_or(Error::NoDevice)?
                .physical_address()
                .get();
            (fw_pri.gpu_addr, pa)
        };
        let fw_pri_pte = dev.mem.read_pte(&mut dev.regs, fw_pri_mc)?;
        let expected_pte = dev.mem.expected_system_pte(fw_pri_pa);
        let pte_index = (fw_pri_mc - dev.gmc.gart_start) >> 12;
        dev_info!(
            "astra: PSP fw_pri GART: va=0x{:016X}, pa=0x{:016X}, index=0x{:X}, pte=0x{:016X}, expected=0x{:016X}",
            fw_pri_mc,
            fw_pri_pa,
            pte_index,
            fw_pri_pte,
            expected_pte,
        );

        dev_info!(
            "astra: PSP pre-boot mailbox: msg33={:#010x} msg35={:#010x} msg81={:#010x}",
            self.c2pmsg_read(&mut dev.regs, mp::regMP0_SMN_C2PMSG_33)?,
            self.c2pmsg_read(&mut dev.regs, self.c2pmsg35)?,
            self.c2pmsg_read(&mut dev.regs, self.c2pmsg81)?
        );
        dev_info!("astra: PSP starting bootloader firmware sequence");
        self.bootloader_load_sos(dev)?;
        dev_info!("astra: PSP creating KM ring");
        self.ring_create(dev)?;
        let tmr_size = self.load_toc(dev)?;
        self.tmr = Some(dev.mem.alloc_vram_top_down_aligned(
            &mut dev.regs,
            tmr_size,
            TMR_ALIGNMENT,
        )?);
        let centralized_pmfw = self.pmfw_centralized_cstate_management();
        if centralized_pmfw {
            dev_info!("astra: PSP loading PMFW before TMR");
            self.load_smu_fw(dev)?;
        }
        dev_info!("astra: PSP setting up TMR");
        self.setup_tmr(dev)?;
        if self.autoload && !centralized_pmfw {
            dev_info!("astra: PSP loading PMFW before other firmware");
            self.load_smu_fw(dev)?;
        }
        dev_info!("astra: PSP loading non-PSP firmware");
        self.load_non_psp_fw(dev)?;

        // ASD is mandatory; the remaining TAs are best-effort.
        self.asd_initialize(dev)?;
        let count = self.tas.len();
        for index in 0..count {
            if self.tas[index].desc.fw_type == TA_TYPE_ASD {
                continue;
            }
            match self.ta_load(dev, index) {
                Ok(()) => dev_info!("astra: TA type {} loaded", self.tas[index].desc.fw_type),
                Err(_) => dev_info!(
                    "astra: TA type {} failed to load (non-fatal)",
                    self.tas[index].desc.fw_type
                ),
            }
        }

        dev_info!("astra: PSP firmware loading complete");
        Ok(())
    }
}
