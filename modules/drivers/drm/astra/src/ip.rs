//! Hardware IP block taxonomy and the per-block init trait, mirroring
//! Linux `amdgpu.h` HWIP enums and `amdgpu_ip_block_funcs`.

use na_std::Result;

use crate::device::Adapter;

pub const MAX_INSTANCE: usize = 8;
pub const MAX_BASE_ADDR: usize = 6;

/// Hardware IP block identifiers. Register spaces are addressed through
/// these; the enum doubles as the index into the per-IP register base /
/// version arrays filled from the IP discovery table.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum HwIp {
    Common,
    Gc,
    Uvd,
    Vce,
    Mmhub,
    Athub,
    OssSys,
    Hdp,
    Sdma0,
    Sdma1,
    Sdma2,
    Sdma3,
    Lsdma,
    Df,
    Nbio,
    Umc,
    Dmu,
    Mp0,
    Mp1,
    Thm,
    Smuio,
    Clk,
    Pwr,
    Nbif,
    Xgmi,
    Pcie,
    /// Driver-level GMC block (gmc_v10_0); shares the GC register space
    /// with `Gc` but is dispatched separately during init so its
    /// `hw_init` runs inline before the interrupt handler, ahead of GFX.
    Gmc,
    /// Driver-level display block (DCN 3.0.2); dispatched in phase2 between
    /// SMU and GC (Linux `amdgpu_device_ip_hw_init_phase2`).
    Dm,
}

pub const HWIP_COUNT: usize = 28;

impl HwIp {
    pub const fn index(self) -> usize {
        self as usize
    }

    pub const fn name(self) -> &'static str {
        const NAMES: [&str; HWIP_COUNT] = [
            "COMMON", "GC", "UVD", "VCE", "MMHUB", "ATHUB", "OSSSYS", "HDP", "SDMA0", "SDMA1",
            "SDMA2", "SDMA3", "LSDMA", "DF", "NBIO", "UMC", "DMU", "MP0", "MP1", "THM", "SMUIO",
            "CLK", "PWR", "NBIF", "XGMI", "PCIE", "GMC", "DM",
        ];
        NAMES[self as usize]
    }

    pub const fn from_index(index: usize) -> Self {
        const IPS: [HwIp; HWIP_COUNT] = [
            HwIp::Common,
            HwIp::Gc,
            HwIp::Uvd,
            HwIp::Vce,
            HwIp::Mmhub,
            HwIp::Athub,
            HwIp::OssSys,
            HwIp::Hdp,
            HwIp::Sdma0,
            HwIp::Sdma1,
            HwIp::Sdma2,
            HwIp::Sdma3,
            HwIp::Lsdma,
            HwIp::Df,
            HwIp::Nbio,
            HwIp::Umc,
            HwIp::Dmu,
            HwIp::Mp0,
            HwIp::Mp1,
            HwIp::Thm,
            HwIp::Smuio,
            HwIp::Clk,
            HwIp::Pwr,
            HwIp::Nbif,
            HwIp::Xgmi,
            HwIp::Pcie,
            HwIp::Gmc,
            HwIp::Dm,
        ];
        IPS[index]
    }

    /// Resolves a binary IP-discovery hardware id to the driver's logical
    /// block, matching Linux `hw_id_map[]`.
    pub fn from_hardware_id(id: u16) -> Option<Self> {
        HW_ID_MAP
            .iter()
            .find(|(_, hardware_id)| *hardware_id == id)
            .map(|(ip, _)| *ip)
    }
}

/// Full IP version as packed by the discovery table:
/// `(major << 24) | (minor << 16) | (rev << 8) | (variant << 4) | subrev`
/// (Linux `IP_VERSION_FULL`).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct IpVersion {
    pub major: u8,
    pub minor: u8,
    pub revision: u8,
    pub variant: u8,
    pub subrev: u8,
}

#[derive(Clone, Copy, Debug)]
pub struct UserIb {
    pub va_start: u64,
    pub length_dw: u32,
    pub flags: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct UserFence {
    pub gpu_addr: u64,
    pub sequence: u64,
}

/// Complete scheduler input for one userspace CS job.  Keeping the ring,
/// VM and fence state together prevents the independent scalar arguments
/// from drifting out of sync as more IP blocks gain submission support.
#[derive(Clone, Copy, Debug)]
pub struct UserSubmission<'a> {
    pub ip_type: u32,
    pub ring: u32,
    pub vmid: u32,
    pub root_pde: u64,
    pub context_id: u32,
    pub ibs: &'a [UserIb],
    pub user_fence: Option<UserFence>,
}

/// Hardware completion fence returned by an asynchronous ring submission.
#[derive(Clone, Copy, Eq, PartialEq)]
pub struct CompletionFence {
    pub gpu_address: u64,
    pub value: u64,
}

#[derive(Clone, Copy)]
pub enum InitStage {
    Early,
    Software,
    Hardware,
    Late,
}

impl InitStage {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Early => "early_init",
            Self::Software => "sw_init",
            Self::Hardware => "hw_init",
            Self::Late => "late_init",
        }
    }
}

impl IpVersion {
    pub const fn from_full(full: u32) -> Self {
        Self {
            major: (full >> 24) as u8,
            minor: ((full >> 16) & 0xff) as u8,
            revision: ((full >> 8) & 0xff) as u8,
            variant: ((full >> 4) & 0xf) as u8,
            subrev: (full & 0xf) as u8,
        }
    }

    pub const fn full(self) -> u32 {
        ((self.major as u32) << 24)
            | ((self.minor as u32) << 16)
            | ((self.revision as u32) << 8)
            | ((self.variant as u32) << 4)
            | self.subrev as u32
    }
}

/// A driver IP block, mirroring Linux `amdgpu_ip_block_funcs`.
///
/// The init sequence follows `amdgpu_device_init`: `early_init` for all
/// blocks, then a single `sw_init` walk (with COMMON and GMC `hw_init`
/// running inline when encountered), the interrupt handler phase, firmware
/// loading, the remaining `hw_init` phases, and finally `late_init`.
pub trait IpBlock: Send {
    fn hw_ip(&self) -> HwIp;

    /// Human-readable name, e.g. `"GMC 10.3.4"`.
    fn name(&self) -> &'static str;

    fn early_init(&mut self, _dev: &mut Adapter) -> Result<()> {
        Ok(())
    }

    fn sw_init(&mut self, _dev: &mut Adapter) -> Result<()> {
        Ok(())
    }

    fn hw_init(&mut self, _dev: &mut Adapter) -> Result<()> {
        Ok(())
    }

    fn late_init(&mut self, _dev: &mut Adapter) -> Result<()> {
        Ok(())
    }

    fn init(&mut self, dev: &mut Adapter, stage: InitStage) -> Result<()> {
        match stage {
            InitStage::Early => self.early_init(dev),
            InitStage::Software => self.sw_init(dev),
            InitStage::Hardware => self.hw_init(dev),
            InitStage::Late => self.late_init(dev),
        }
    }

    fn submit_user_ibs(
        &mut self,
        _dev: &mut Adapter,
        _submission: UserSubmission<'_>,
    ) -> Result<CompletionFence> {
        Err(na_std::Error::Unsupported)
    }

    /// Linux `amdgpu_vm_sdma_update` entry point. `dst` is the MC address of
    /// the first PDE/PTE, and the SDMA backend selects WRITE for fewer than
    /// three entries or PTEPDE_GEN otherwise.
    fn update_vm_table(
        &mut self,
        _dev: &mut Adapter,
        _dst: u64,
        _addr: u64,
        _count: u32,
        _incr: u32,
        _flags: u64,
    ) -> Result<()> {
        Err(na_std::Error::Unsupported)
    }
}

/// Hardware-id to driver-ip map used by the IP discovery table
/// (Linux `hw_id_map[]` in `amdgpu_discovery.c`).
const HW_ID_MAP: &[(HwIp, u16)] = &[
    (HwIp::Gc, 11),
    (HwIp::Uvd, 12),
    (HwIp::Vce, 32),
    (HwIp::Mmhub, 34),
    (HwIp::Athub, 35),
    (HwIp::OssSys, 40),
    (HwIp::Hdp, 41),
    (HwIp::Sdma0, 42),
    (HwIp::Sdma1, 43),
    (HwIp::Df, 46),
    (HwIp::Sdma2, 68),
    (HwIp::Sdma3, 69),
    (HwIp::Pcie, 70),
    (HwIp::Nbio, 108),
    (HwIp::Umc, 150),
    (HwIp::Xgmi, 200),
    (HwIp::Dmu, 271),
    (HwIp::Mp0, 255),
    (HwIp::Mp1, 1),
    (HwIp::Thm, 3),
    (HwIp::Smuio, 4),
    (HwIp::Clk, 6),
    (HwIp::Pwr, 10),
    (HwIp::Lsdma, 2),
];
