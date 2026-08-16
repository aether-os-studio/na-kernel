//! SMU IP block: swsmu v11 with the sienna_cichlid ppt (Linux
//! `smu_v11_0.c` + `sienna_cichlid_ppt.c`).

use alloc::vec::Vec;
use core::time::Duration;

use na_std::time;
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::mem::Bo;
use crate::regs::Regs;

/// MP1 mailbox registers: dword offsets relative to the MP1 discovery
/// base[0] (mp_11_0_offset.h: `mmMP1_SMN_C2PMSG_*_BASE_IDX == 0`).
const MM_MP1_SMN_C2PMSG_66: u32 = 0x0282;
const MM_MP1_SMN_C2PMSG_82: u32 = 0x0292;
const MM_MP1_SMN_C2PMSG_90: u32 = 0x029a;

/// SMN address of the MP1 firmware flags register
/// (MP1_Public | smnMP1_FIRMWARE_FLAGS).
const SMN_MP1_FIRMWARE_FLAGS: u32 = 0x03B1_0024;
/// INTERRUPTS_ENABLED (bit 0, mp_11_0_sh_mask.h).
const FIRMWARE_FLAGS_INTERRUPTS_ENABLED: u32 = 0x1;

/// PPSMC message ids (smu_v11_0_7_ppsmc.h).
const MSG_GET_SMU_VERSION: u32 = 0x2;
const MSG_GET_DRIVER_IF_VERSION: u32 = 0x3;
const MSG_SET_ALLOWED_FEATURES_MASK_LOW: u32 = 0x4;
const MSG_SET_ALLOWED_FEATURES_MASK_HIGH: u32 = 0x5;
const MSG_ENABLE_ALL_SMU_FEATURES: u32 = 0x6;
const MSG_GET_RUNNING_SMU_FEATURES_LOW: u32 = 0xC;
const MSG_GET_RUNNING_SMU_FEATURES_HIGH: u32 = 0xD;
const MSG_SET_DRIVER_DRAM_ADDR_HIGH: u32 = 0xE;
const MSG_SET_DRIVER_DRAM_ADDR_LOW: u32 = 0xF;
const MSG_SET_TOOLS_DRAM_ADDR_HIGH: u32 = 0x10;
const MSG_SET_TOOLS_DRAM_ADDR_LOW: u32 = 0x11;
const MSG_TRANSFER_TABLE_DRAM2SMU: u32 = 0x13;
const MSG_GET_MAX_DPM_FREQ: u32 = 0x1E;
const MSG_RUN_DC_BTC: u32 = 0x36;

/// SMU table ids (sienna_cichlid_table_map: PPTABLE = 0).
const TABLE_PPTABLE: u32 = 0;

/// Linux `SMU11_DRIVER_IF_VERSION_Dimgrey_Cavefish`.
const DIMGREY_CAVEFISH_DRIVER_IF_VERSION: u32 = 0xF;

/// Linux uses `adev->usec_timeout * 20` for the SMU v1 mailbox.  The
/// default amdgpu timeout is 100 ms, hence a two-second mailbox timeout.
const SMU_MSG_TIMEOUT_US: usize = 2_000_000;

/// PMSTATUSLOG buffer size (SMU11_TOOL_SIZE).
const TOOL_TABLE_SIZE: usize = 0x19000;

pub struct SmuV11 {
    _version: IpVersion,
    msg_reg: u32,
    arg_reg: u32,
    resp_reg: u32,
    /// Driver table (VRAM): holds the powerplay table handed to the SMU.
    driver_table: Option<Bo>,
    /// Tool table (VRAM): PM status log.
    tool_table: Option<Bo>,
    /// Linux-style Sienna Cichlid `PPTable_t` bytes.
    ppt_bytes: Vec<u8>,
    /// Linux skips the response pre-poll only for the first message after
    /// transitioning the SMC firmware from INIT to RUNTIME.
    mailbox_started: bool,
}

impl SmuV11 {
    pub fn new(version: IpVersion) -> Self {
        Self {
            _version: version,
            msg_reg: 0,
            arg_reg: 0,
            resp_reg: 0,
            driver_table: None,
            tool_table: None,
            ppt_bytes: Vec::new(),
            mailbox_started: false,
        }
    }

    fn init_msg_ctl(&mut self, regs: &Regs) -> Result<()> {
        // MP1 mailbox C2PMSG registers are dword offsets relative to the MP1
        // discovery base[0] (`mmMP1_SMN_C2PMSG_*_BASE_IDX == 0`). `read_ip` /
        // `write_ip` add base[0], so store only the block offsets here.
        let base0 = regs.base_u32(HwIp::Mp1, 0, 0).unwrap_or(0);
        let base1 = regs.base_u32(HwIp::Mp1, 0, 1).unwrap_or(0);
        dev_info!("astra: MP1 mailbox bases {:#x} {:#x}", base0, base1);
        self.msg_reg = MM_MP1_SMN_C2PMSG_66;
        self.arg_reg = MM_MP1_SMN_C2PMSG_82;
        self.resp_reg = MM_MP1_SMN_C2PMSG_90;
        Ok(())
    }

    /// Publishes the mailbox registers for other IP blocks (VCN).
    pub fn mailbox(&self) -> Option<(u32, u32, u32)> {
        if self.msg_reg == 0 {
            None
        } else {
            Some((self.msg_reg, self.arg_reg, self.resp_reg))
        }
    }

    fn msg_name(msg: u32) -> &'static str {
        match msg {
            MSG_GET_SMU_VERSION => "GetSmuVersion",
            MSG_GET_DRIVER_IF_VERSION => "GetDriverIfVersion",
            MSG_SET_ALLOWED_FEATURES_MASK_LOW => "SetAllowedFeaturesMaskLow",
            MSG_SET_ALLOWED_FEATURES_MASK_HIGH => "SetAllowedFeaturesMaskHigh",
            MSG_ENABLE_ALL_SMU_FEATURES => "EnableAllSmuFeatures",
            MSG_GET_RUNNING_SMU_FEATURES_LOW => "GetRunningSmuFeaturesLow",
            MSG_GET_RUNNING_SMU_FEATURES_HIGH => "GetRunningSmuFeaturesHigh",
            MSG_SET_DRIVER_DRAM_ADDR_HIGH => "SetDriverDramAddrHigh",
            MSG_SET_DRIVER_DRAM_ADDR_LOW => "SetDriverDramAddrLow",
            MSG_SET_TOOLS_DRAM_ADDR_HIGH => "SetToolsDramAddrHigh",
            MSG_SET_TOOLS_DRAM_ADDR_LOW => "SetToolsDramAddrLow",
            MSG_TRANSFER_TABLE_DRAM2SMU => "TransferTableDram2Smu",
            MSG_GET_MAX_DPM_FREQ => "GetMaxDpmFreq",
            MSG_RUN_DC_BTC => "RunDcBtc",
            _ => "unknown",
        }
    }

    fn poll_response(&mut self, dev: &mut Adapter) -> Result<Option<u32>> {
        for _ in 0..SMU_MSG_TIMEOUT_US {
            let response = dev.regs.read_ip(HwIp::Mp1, 0, self.resp_reg, 0)?;
            if response != 0 {
                return Ok(Some(response));
            }
            time::delay(Duration::from_micros(1));
        }
        Ok(None)
    }

    /// Linux `smu_msg_v1_send_msg` / `smu_cmn_send_smc_msg_with_param`.
    /// The response register is command status; the return value is always
    /// read from C2PMSG_82 after a successful command.
    fn send_msg(&mut self, dev: &mut Adapter, msg: u32, param: u32) -> Result<u32> {
        // Linux pre-polls the response register before every message except
        // the first one after SMC startup.  This prevents clearing a pending
        // completion while the firmware is still transitioning to runtime.
        if self.mailbox_started && self.poll_response(dev)?.is_none() {
            dev_info!(
                "astra: SMU mailbox pre-poll timeout for {}({:#x}) param={:#010x}",
                Self::msg_name(msg),
                msg,
                param,
            );
            return Err(Error::Io);
        }

        dev_info!(
            "astra: SMU sending {}({:#x}) param={:#010x}",
            Self::msg_name(msg),
            msg,
            param,
        );
        dev.regs.write_ip(HwIp::Mp1, 0, self.resp_reg, 0, 0)?;
        dev.regs.write_ip(HwIp::Mp1, 0, self.arg_reg, 0, param)?;
        dev.regs.write_ip(HwIp::Mp1, 0, self.msg_reg, 0, msg)?;
        self.mailbox_started = true;

        let Some(response) = self.poll_response(dev)? else {
            dev_info!(
                "astra: SMU mailbox post-poll timeout for {}({:#x}) param={:#010x}",
                Self::msg_name(msg),
                msg,
                param,
            );
            return Err(Error::Io);
        };
        let argument = dev.regs.read_ip(HwIp::Mp1, 0, self.arg_reg, 0)?;
        dev_info!(
            "astra: SMU msg {}({:#x}) param={:#010x} response={:#010x} arg={:#010x}",
            Self::msg_name(msg),
            msg,
            param,
            response,
            argument,
        );

        match response {
            0x1 => Ok(argument),    // SMU_RESP_OK
            0xFF => Err(Error::Io), // SMU_RESP_CMD_FAIL
            0xFE => Err(Error::Io), // SMU_RESP_CMD_UNKNOWN
            0xFD => Err(Error::Io), // SMU_RESP_CMD_BAD_PREREQ
            0xFC => Err(Error::Io), // SMU_RESP_BUSY_OTHER
            _ => Err(Error::Io),    // Linux: unknown/debug response
        }
    }

    /// Linux `smu_v11_0_check_fw_status` (SMN read of MP1_FIRMWARE_FLAGS).
    fn check_fw_status(&mut self, dev: &mut Adapter) -> Result<()> {
        let flags = dev.regs.smn_read(SMN_MP1_FIRMWARE_FLAGS)?;
        if flags & FIRMWARE_FLAGS_INTERRUPTS_ENABLED != 0 {
            Ok(())
        } else {
            dev_info!("astra: SMU firmware not running (flags {:#x})", flags);
            Err(Error::Io)
        }
    }

    /// Linux `smu_cmn_write_pptable` + `smu_cmn_update_table`.
    fn write_pptable(&mut self, dev: &mut Adapter) -> Result<()> {
        let table_addr = self.driver_table.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let bytes = self.ppt_bytes.clone();
        // Pack bytes into dwords, zero-padding a trailing partial dword.
        let dwords: Vec<u32> = bytes
            .chunks(4)
            .map(|c| {
                let mut word = [0u8; 4];
                word[..c.len()].copy_from_slice(c);
                u32::from_le_bytes(word)
            })
            .collect();
        dev.regs.vram_write_dwords(table_addr, &dwords)?;
        // HDP flush so the SMU sees the table (smu_cmn_update_table).
        dev.regs.write_ip(
            HwIp::Nbio,
            0,
            crate::regs::nbio4_3_0::regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL,
            2,
            0,
        )?;
        let param = TABLE_PPTABLE & 0xffff;
        self.send_msg(dev, MSG_TRANSFER_TABLE_DRAM2SMU, param)?;
        Ok(())
    }

    /// Linux `smu_smc_hw_setup` (sienna cichlid path).
    fn smc_hw_setup(&mut self, dev: &mut Adapter) -> Result<()> {
        // Driver table location (VRAM BO: GPU address is fb_start + offset).
        let driver_offset = self.driver_table.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let driver_addr = dev
            .gmc
            .fb_start
            .checked_add(driver_offset)
            .ok_or(Error::Range)?;
        dev_info!(
            "astra: SMU driver table at MC {:#018x} (VRAM offset {:#x})",
            driver_addr,
            driver_offset,
        );
        self.send_msg(
            dev,
            MSG_SET_DRIVER_DRAM_ADDR_HIGH,
            (driver_addr >> 32) as u32,
        )?;
        self.send_msg(dev, MSG_SET_DRIVER_DRAM_ADDR_LOW, driver_addr as u32)?;

        // Tool table location (PM status log).
        let tool_offset = self.tool_table.as_ref().ok_or(Error::NoDevice)?.gpu_addr;
        let tool_addr = dev
            .gmc
            .fb_start
            .checked_add(tool_offset)
            .ok_or(Error::Range)?;
        dev_info!(
            "astra: SMU tool table at MC {:#018x} (VRAM offset {:#x})",
            tool_addr,
            tool_offset,
        );
        self.send_msg(dev, MSG_SET_TOOLS_DRAM_ADDR_HIGH, (tool_addr >> 32) as u32)?;
        self.send_msg(dev, MSG_SET_TOOLS_DRAM_ADDR_LOW, tool_addr as u32)?;

        // Powerplay table: VBIOS-provided bytes → VRAM → SMU.
        self.write_pptable(dev)?;

        // RunDcBtc + EnableAllSmuFeatures are deferred until the DCN display
        // block is up: RunDcBtc calibrates the display clock and
        // EnableAllSmuFeatures turns on DPM, both of which disturb the boot
        // display while DCN is uninitialized. Re-enable once DCN scans out.
        dev_info!("astra: skipping RunDcBtc / EnableAllSmuFeatures (DCN pending)");
        Ok(())
    }
}

impl IpBlock for SmuV11 {
    fn hw_ip(&self) -> HwIp {
        HwIp::Mp1
    }

    fn name(&self) -> &'static str {
        "SMU 11.0"
    }

    /// Linux `smu_sw_init` essentials: mailbox, boot values, pptable
    /// bytes and the driver/tool tables.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.init_msg_ctl(&dev.regs)?;
        dev.smu_mailbox = self.mailbox();

        if let Some(fw) = dev.atom.as_ref().and_then(|atom| atom.firmware_info()) {
            dev_info!(
                "astra: VBIOS bootup vddc {} mV, vddci {} mV, vddgfx {} mV",
                fw.bootup_vddc_mv,
                fw.bootup_vddci_mv,
                fw.bootup_vddgfx_mv,
            );
        }

        if let Some(pp) = dev.atom.as_ref().and_then(|atom| atom.powerplay_info()) {
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
            if let (Some(first), Some(last)) =
                (pp.bytes.first_chunk::<4>(), pp.bytes.last_chunk::<4>())
            {
                dev_info!(
                    "astra: SMU PPTable first dword {:#010x}, last dword {:#010x}",
                    u32::from_le_bytes(*first),
                    u32::from_le_bytes(*last),
                );
            }
            self.ppt_bytes = pp.bytes;
        } else {
            return Err(Error::Io);
        }

        // Driver table (VRAM) sized to the pptable; tool table (VRAM).
        let table_size = self.ppt_bytes.len().next_multiple_of(4096);
        self.driver_table = Some(dev.mem.alloc_vram(&mut dev.regs, table_size)?);
        self.tool_table = Some(dev.mem.alloc_vram(&mut dev.regs, TOOL_TABLE_SIZE)?);
        Ok(())
    }

    /// Linux `smu_hw_init`: start the SMC engine, verify the firmware,
    /// hand over the tables and enable the features.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        // smu_start_smc_engine: the firmware was delivered by the PSP.
        self.check_fw_status(dev)?;
        dev_info!("astra: SMC engine up");

        // Linux `smu_cmn_check_fw_version` sends both version messages in
        // this order.  A driver/firmware interface mismatch is diagnostic,
        // not fatal.
        let if_version = self.send_msg(dev, MSG_GET_DRIVER_IF_VERSION, 0)?;
        let smu_version = self.send_msg(dev, MSG_GET_SMU_VERSION, 0)?;
        dev_info!(
            "astra: SMU driver if {:#x}, firmware if {:#x}, firmware version {:#010x}",
            DIMGREY_CAVEFISH_DRIVER_IF_VERSION,
            if_version,
            smu_version,
        );

        self.smc_hw_setup(dev)?;

        // smu_init_max_sustainable_clocks: report the max frequencies.
        let mut freqs = [0u32; 4];
        for (i, freq) in freqs.iter_mut().enumerate() {
            if let Ok(value) = self.send_msg(dev, MSG_GET_MAX_DPM_FREQ, i as u32) {
                *freq = value;
            }
        }
        if freqs[0] != 0 {
            dev.clocks.max_engine_clock_khz = freqs[0] as u64 * 1000;
        }
        if freqs[2] != 0 {
            dev.clocks.max_memory_clock_khz = freqs[2] as u64 * 1000;
        }
        dev_info!(
            "astra: max clocks: gfx {} MHz, soc {} MHz, uclk {} MHz, fclk {} MHz",
            freqs[0],
            freqs[1],
            freqs[2],
            freqs[3],
        );

        dev_info!("astra: SMU is initialized successfully!");
        Ok(())
    }
}
