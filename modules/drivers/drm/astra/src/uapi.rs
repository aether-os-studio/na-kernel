//! Safe definitions for the subset of Linux `amdgpu_drm.h` implemented by
//! ASTRA.  The top-level ioctl payload and every reply are encoded manually;
//! no userspace pointer is ever dereferenced by Rust.

use alloc::vec::Vec;

use na_std::user::UserAddress;
use na_std::{Error, Result};

const DRM_IOCTL_BASE: u32 = b'd' as u32;
const DRM_COMMAND_BASE: u32 = 0x40;
const DRM_IOCTL_GEM_CLOSE_NR: u32 = 0x09;
const DRM_AMDGPU_GEM_CREATE: u32 = 0x00;
const DRM_AMDGPU_GEM_MMAP: u32 = 0x01;
const DRM_AMDGPU_CTX: u32 = 0x02;
const DRM_AMDGPU_BO_LIST: u32 = 0x03;
const DRM_AMDGPU_CS: u32 = 0x04;
const DRM_AMDGPU_INFO: u32 = 0x05;
const DRM_AMDGPU_GEM_METADATA: u32 = 0x06;
const DRM_AMDGPU_GEM_WAIT_IDLE: u32 = 0x07;
const DRM_AMDGPU_GEM_VA: u32 = 0x08;
const DRM_AMDGPU_WAIT_CS: u32 = 0x09;
const DRM_AMDGPU_GEM_OP: u32 = 0x10;

pub const AMDGPU_GEM_DOMAIN_CPU: u64 = 0x1;
pub const AMDGPU_GEM_DOMAIN_GTT: u64 = 0x2;
pub const AMDGPU_GEM_DOMAIN_VRAM: u64 = 0x4;
pub const AMDGPU_GEM_DOMAIN_GDS: u64 = 0x8;
pub const AMDGPU_GEM_DOMAIN_GWS: u64 = 0x10;
pub const AMDGPU_GEM_DOMAIN_OA: u64 = 0x20;
pub const AMDGPU_GEM_DOMAIN_DOORBELL: u64 = 0x40;
pub const AMDGPU_GEM_DOMAIN_MASK: u64 = AMDGPU_GEM_DOMAIN_CPU
    | AMDGPU_GEM_DOMAIN_GTT
    | AMDGPU_GEM_DOMAIN_VRAM
    | AMDGPU_GEM_DOMAIN_GDS
    | AMDGPU_GEM_DOMAIN_GWS
    | AMDGPU_GEM_DOMAIN_OA
    | AMDGPU_GEM_DOMAIN_DOORBELL;

pub const AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED: u64 = 1 << 0;
pub const AMDGPU_GEM_CREATE_NO_CPU_ACCESS: u64 = 1 << 1;
pub const AMDGPU_GEM_CREATE_CPU_GTT_USWC: u64 = 1 << 2;
pub const AMDGPU_GEM_CREATE_VRAM_CLEARED: u64 = 1 << 3;
pub const AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS: u64 = 1 << 5;
pub const AMDGPU_GEM_CREATE_VM_ALWAYS_VALID: u64 = 1 << 6;
pub const AMDGPU_GEM_CREATE_EXPLICIT_SYNC: u64 = 1 << 7;
pub const AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE: u64 = 1 << 9;
pub const AMDGPU_GEM_CREATE_ENCRYPTED: u64 = 1 << 10;
pub const AMDGPU_GEM_CREATE_DISCARDABLE: u64 = 1 << 12;
pub const AMDGPU_GEM_CREATE_COHERENT: u64 = 1 << 13;
pub const AMDGPU_GEM_CREATE_UNCACHED: u64 = 1 << 14;
pub const AMDGPU_GEM_CREATE_EXT_COHERENT: u64 = 1 << 15;
pub const AMDGPU_GEM_CREATE_SETTABLE_MASK: u64 = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED
    | AMDGPU_GEM_CREATE_NO_CPU_ACCESS
    | AMDGPU_GEM_CREATE_CPU_GTT_USWC
    | AMDGPU_GEM_CREATE_VRAM_CLEARED
    | AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS
    | AMDGPU_GEM_CREATE_VM_ALWAYS_VALID
    | AMDGPU_GEM_CREATE_EXPLICIT_SYNC
    | AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE
    | AMDGPU_GEM_CREATE_ENCRYPTED
    | AMDGPU_GEM_CREATE_DISCARDABLE
    | AMDGPU_GEM_CREATE_COHERENT
    | AMDGPU_GEM_CREATE_UNCACHED
    | AMDGPU_GEM_CREATE_EXT_COHERENT;

pub const AMDGPU_GEM_METADATA_OP_SET_METADATA: u32 = 1;
pub const AMDGPU_GEM_METADATA_OP_GET_METADATA: u32 = 2;
pub const AMDGPU_GEM_OP_GET_GEM_CREATE_INFO: u32 = 0;
pub const AMDGPU_GEM_OP_SET_PLACEMENT: u32 = 1;

pub const AMDGPU_CTX_OP_ALLOC_CTX: u32 = 1;
pub const AMDGPU_CTX_OP_FREE_CTX: u32 = 2;
pub const AMDGPU_CTX_OP_QUERY_STATE: u32 = 3;
pub const AMDGPU_CTX_OP_QUERY_STATE2: u32 = 4;
pub const AMDGPU_CTX_OP_GET_STABLE_PSTATE: u32 = 5;
pub const AMDGPU_CTX_OP_SET_STABLE_PSTATE: u32 = 6;

pub const AMDGPU_CTX_PRIORITY_VERY_LOW: i32 = -1023;
pub const AMDGPU_CTX_PRIORITY_LOW: i32 = -512;
pub const AMDGPU_CTX_PRIORITY_NORMAL: i32 = 0;
pub const AMDGPU_CTX_PRIORITY_HIGH: i32 = 512;
pub const AMDGPU_CTX_PRIORITY_VERY_HIGH: i32 = 1023;

pub const AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK: u32 = 0xf;
pub const AMDGPU_CTX_STABLE_PSTATE_NONE: u32 = 0;
pub const AMDGPU_CTX_STABLE_PSTATE_PEAK: u32 = 4;

pub const AMDGPU_BO_LIST_OP_CREATE: u32 = 0;
pub const AMDGPU_BO_LIST_OP_DESTROY: u32 = 1;
pub const AMDGPU_BO_LIST_OP_UPDATE: u32 = 2;

pub const AMDGPU_CHUNK_ID_IB: u32 = 0x01;
pub const AMDGPU_CHUNK_ID_FENCE: u32 = 0x02;
pub const AMDGPU_CHUNK_ID_DEPENDENCIES: u32 = 0x03;
pub const AMDGPU_CHUNK_ID_SYNCOBJ_IN: u32 = 0x04;
pub const AMDGPU_CHUNK_ID_SYNCOBJ_OUT: u32 = 0x05;
pub const AMDGPU_CHUNK_ID_BO_HANDLES: u32 = 0x06;
pub const AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES: u32 = 0x07;
pub const AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: u32 = 0x08;
pub const AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: u32 = 0x09;

pub const AMDGPU_IB_FLAG_CE: u32 = 1 << 0;
pub const AMDGPU_IB_FLAG_PREAMBLE: u32 = 1 << 1;
pub const AMDGPU_IB_FLAG_PREEMPT: u32 = 1 << 2;
pub const AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE: u32 = 1 << 3;
pub const AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID: u32 = 1 << 4;
pub const AMDGPU_IB_FLAGS_SECURE: u32 = 1 << 5;
pub const AMDGPU_IB_FLAG_EMIT_MEM_SYNC: u32 = 1 << 6;
pub const AMDGPU_IB_FLAGS_MASK: u32 = AMDGPU_IB_FLAG_CE
    | AMDGPU_IB_FLAG_PREAMBLE
    | AMDGPU_IB_FLAG_PREEMPT
    | AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE
    | AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID
    | AMDGPU_IB_FLAGS_SECURE
    | AMDGPU_IB_FLAG_EMIT_MEM_SYNC;

pub const AMDGPU_VA_OP_MAP: u32 = 1;
pub const AMDGPU_VA_OP_UNMAP: u32 = 2;
pub const AMDGPU_VA_OP_CLEAR: u32 = 3;
pub const AMDGPU_VA_OP_REPLACE: u32 = 4;
pub const AMDGPU_VM_DELAY_UPDATE: u32 = 1 << 0;
pub const AMDGPU_VM_PAGE_READABLE: u32 = 1 << 1;
pub const AMDGPU_VM_PAGE_WRITEABLE: u32 = 1 << 2;
pub const AMDGPU_VM_PAGE_EXECUTABLE: u32 = 1 << 3;
pub const AMDGPU_VM_PAGE_PRT: u32 = 1 << 4;
pub const AMDGPU_VM_MTYPE_MASK: u32 = 0xf << 5;
pub const AMDGPU_VM_MTYPE_WC: u32 = 2 << 5;
pub const AMDGPU_VM_MTYPE_CC: u32 = 3 << 5;
pub const AMDGPU_VM_MTYPE_UC: u32 = 4 << 5;
pub const AMDGPU_VM_PAGE_NOALLOC: u32 = 1 << 9;

pub const AMDGPU_INFO_ACCEL_WORKING: u32 = 0x00;
pub const AMDGPU_INFO_HW_IP_INFO: u32 = 0x02;
pub const AMDGPU_INFO_HW_IP_COUNT: u32 = 0x03;
pub const AMDGPU_INFO_TIMESTAMP: u32 = 0x05;
pub const AMDGPU_INFO_FW_VERSION: u32 = 0x0e;
pub const AMDGPU_INFO_NUM_BYTES_MOVED: u32 = 0x0f;
pub const AMDGPU_INFO_VRAM_USAGE: u32 = 0x10;
pub const AMDGPU_INFO_GTT_USAGE: u32 = 0x11;
pub const AMDGPU_INFO_GDS_CONFIG: u32 = 0x13;
pub const AMDGPU_INFO_VRAM_GTT: u32 = 0x14;
pub const AMDGPU_INFO_READ_MMR_REG: u32 = 0x15;
pub const AMDGPU_INFO_DEV_INFO: u32 = 0x16;
pub const AMDGPU_INFO_VIS_VRAM_USAGE: u32 = 0x17;
pub const AMDGPU_INFO_NUM_EVICTIONS: u32 = 0x18;
pub const AMDGPU_INFO_MEMORY: u32 = 0x19;
pub const AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS: u32 = 0x1e;
pub const AMDGPU_INFO_VRAM_LOST_COUNTER: u32 = 0x1f;
pub const AMDGPU_INFO_RAS_ENABLED_FEATURES: u32 = 0x20;
pub const AMDGPU_INFO_MAX_IBS: u32 = 0x22;

pub const AMDGPU_HW_IP_GFX: u32 = 0;
pub const AMDGPU_HW_IP_COMPUTE: u32 = 1;
pub const AMDGPU_HW_IP_DMA: u32 = 2;
pub const AMDGPU_HW_IP_UVD: u32 = 3;
pub const AMDGPU_HW_IP_VCE: u32 = 4;
pub const AMDGPU_HW_IP_UVD_ENC: u32 = 5;
pub const AMDGPU_HW_IP_VCN_DEC: u32 = 6;
pub const AMDGPU_HW_IP_VCN_ENC: u32 = 7;
pub const AMDGPU_HW_IP_VCN_JPEG: u32 = 8;
pub const AMDGPU_HW_IP_VPE: u32 = 9;
pub const AMDGPU_HW_IP_NUM: usize = 10;

pub const AMDGPU_INFO_FW_GFX_ME: u32 = 0x04;
pub const AMDGPU_INFO_FW_GFX_PFP: u32 = 0x05;
pub const AMDGPU_INFO_FW_GFX_CE: u32 = 0x06;
pub const AMDGPU_INFO_FW_GFX_RLC: u32 = 0x07;
pub const AMDGPU_INFO_FW_GFX_MEC: u32 = 0x08;
pub const AMDGPU_INFO_FW_SMC: u32 = 0x0a;
pub const AMDGPU_INFO_FW_SDMA: u32 = 0x0b;
pub const AMDGPU_INFO_FW_VCN: u32 = 0x0e;
pub const AMDGPU_INFO_FW_DMCUB: u32 = 0x14;

pub const AMDGPU_FAMILY_NV: u32 = 143;

const INFO_SIZE: usize = 32;
const HW_IP_INFO_SIZE: usize = 32;
const DEVICE_INFO_SIZE: usize = 448;
pub const GEM_CREATE_SIZE: usize = 32;
pub const GEM_MMAP_SIZE: usize = 8;
pub const CTX_SIZE: usize = 16;
pub const BO_LIST_SIZE: usize = 24;
pub const CS_SIZE: usize = 24;
pub const GEM_METADATA_SIZE: usize = 288;
pub const GEM_WAIT_IDLE_SIZE: usize = 16;
pub const GEM_VA_SIZE: usize = 64;
pub const WAIT_CS_SIZE: usize = 32;
pub const GEM_OP_SIZE: usize = 16;
pub const GEM_CLOSE_SIZE: usize = 8;

pub fn read_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    let value = bytes
        .get(offset..offset + 4)
        .ok_or(Error::InvalidArgument)?;
    Ok(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

pub fn read_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    let value = bytes
        .get(offset..offset + 8)
        .ok_or(Error::InvalidArgument)?;
    Ok(u64::from_le_bytes([
        value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7],
    ]))
}

pub fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

pub fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn ioctl_number(command: u32) -> Option<u32> {
    (((command >> 8) & 0xff) == DRM_IOCTL_BASE).then_some(command & 0xff)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Command {
    GemCreate,
    GemMmap,
    Ctx,
    BoList,
    Cs,
    Info,
    GemMetadata,
    GemWaitIdle,
    GemVa,
    WaitCs,
    GemOp,
    GemClose,
}

pub fn command(command: u32) -> Option<Command> {
    match ioctl_number(command)? {
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_GEM_CREATE => Some(Command::GemCreate),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_GEM_MMAP => Some(Command::GemMmap),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_CTX => Some(Command::Ctx),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_BO_LIST => Some(Command::BoList),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_CS => Some(Command::Cs),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_INFO => Some(Command::Info),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_GEM_METADATA => Some(Command::GemMetadata),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_GEM_WAIT_IDLE => Some(Command::GemWaitIdle),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_GEM_VA => Some(Command::GemVa),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_WAIT_CS => Some(Command::WaitCs),
        nr if nr == DRM_COMMAND_BASE + DRM_AMDGPU_GEM_OP => Some(Command::GemOp),
        DRM_IOCTL_GEM_CLOSE_NR => Some(Command::GemClose),
        _ => None,
    }
}

#[derive(Clone, Copy, Debug)]
pub struct BoListRequest {
    pub operation: u32,
    pub list_handle: u32,
    pub bo_number: u32,
    pub bo_info_size: u32,
    pub bo_info_ptr: u64,
}

impl BoListRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != BO_LIST_SIZE {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            operation: read_u32(bytes, 0)?,
            list_handle: read_u32(bytes, 4)?,
            bo_number: read_u32(bytes, 8)?,
            bo_info_size: read_u32(bytes, 12)?,
            bo_info_ptr: read_u64(bytes, 16)?,
        })
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CsRequest {
    pub context_id: u32,
    pub bo_list_handle: u32,
    pub num_chunks: u32,
    pub flags: u32,
    pub chunks: u64,
}

impl CsRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != CS_SIZE {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            context_id: read_u32(bytes, 0)?,
            bo_list_handle: read_u32(bytes, 4)?,
            num_chunks: read_u32(bytes, 8)?,
            flags: read_u32(bytes, 12)?,
            chunks: read_u64(bytes, 16)?,
        })
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CsChunk {
    pub id: u32,
    pub length_dw: u32,
    pub data: u64,
}

impl CsChunk {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != 16 {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            id: read_u32(bytes, 0)?,
            length_dw: read_u32(bytes, 4)?,
            data: read_u64(bytes, 8)?,
        })
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CsIb {
    pub flags: u32,
    pub va_start: u64,
    pub ib_bytes: u32,
    pub ip_type: u32,
    pub ip_instance: u32,
    pub ring: u32,
}

impl CsIb {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != 32 {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            flags: read_u32(bytes, 4)?,
            va_start: read_u64(bytes, 8)?,
            ib_bytes: read_u32(bytes, 16)?,
            ip_type: read_u32(bytes, 20)?,
            ip_instance: read_u32(bytes, 24)?,
            ring: read_u32(bytes, 28)?,
        })
    }
}

#[derive(Clone, Copy, Debug)]
pub struct WaitCsRequest {
    pub handle: u64,
    pub timeout: u64,
    pub ip_type: u32,
    pub ip_instance: u32,
    pub ring: u32,
    pub context_id: u32,
}

impl WaitCsRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != WAIT_CS_SIZE {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            handle: read_u64(bytes, 0)?,
            timeout: read_u64(bytes, 8)?,
            ip_type: read_u32(bytes, 16)?,
            ip_instance: read_u32(bytes, 20)?,
            ring: read_u32(bytes, 24)?,
            context_id: read_u32(bytes, 28)?,
        })
    }
}

/// Decoded `union drm_amdgpu_ctx`. The input and output layouts overlap, so
/// callers must parse the request before clearing the reply buffer.
#[derive(Clone, Copy, Debug)]
pub struct ContextRequest {
    pub operation: u32,
    pub flags: u32,
    pub context_id: u32,
    pub priority: i32,
}

impl ContextRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != CTX_SIZE {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            operation: read_u32(bytes, 0)?,
            flags: read_u32(bytes, 4)?,
            context_id: read_u32(bytes, 8)?,
            priority: read_u32(bytes, 12)? as i32,
        })
    }
}

#[derive(Clone, Copy, Debug)]
pub struct GemCreateRequest {
    pub bo_size: u64,
    pub alignment: u64,
    pub domains: u64,
    pub domain_flags: u64,
}

impl GemCreateRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != GEM_CREATE_SIZE {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            bo_size: read_u64(bytes, 0)?,
            alignment: read_u64(bytes, 8)?,
            domains: read_u64(bytes, 16)?,
            domain_flags: read_u64(bytes, 24)?,
        })
    }

    pub fn write_reply(bytes: &mut [u8], handle: u32) -> Result<()> {
        if bytes.len() != GEM_CREATE_SIZE {
            return Err(Error::InvalidArgument);
        }
        bytes.fill(0);
        put_u32(bytes, 0, handle);
        Ok(())
    }
}

#[derive(Clone, Copy, Debug)]
pub struct GemVaRequest {
    pub handle: u32,
    pub operation: u32,
    pub flags: u32,
    pub va_address: u64,
    pub offset_in_bo: u64,
    pub map_size: u64,
    pub vm_timeline_point: u64,
    pub vm_timeline_syncobj_out: u32,
    pub num_syncobj_handles: u32,
    pub input_fence_syncobj_handles: u64,
}

impl GemVaRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        // Linux extended this write-only structure from 40 to 64 bytes with
        // VM timeline synchronization. Accept both layouts so the ABI stays
        // compatible with the Void libdrm/Mesa build and newer userspace.
        if bytes.len() != 40 && bytes.len() != GEM_VA_SIZE {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            handle: read_u32(bytes, 0)?,
            operation: read_u32(bytes, 8)?,
            flags: read_u32(bytes, 12)?,
            va_address: read_u64(bytes, 16)?,
            offset_in_bo: read_u64(bytes, 24)?,
            map_size: read_u64(bytes, 32)?,
            vm_timeline_point: if bytes.len() == GEM_VA_SIZE {
                read_u64(bytes, 40)?
            } else {
                0
            },
            vm_timeline_syncobj_out: if bytes.len() == GEM_VA_SIZE {
                read_u32(bytes, 48)?
            } else {
                0
            },
            num_syncobj_handles: if bytes.len() == GEM_VA_SIZE {
                read_u32(bytes, 52)?
            } else {
                0
            },
            input_fence_syncobj_handles: if bytes.len() == GEM_VA_SIZE {
                read_u64(bytes, 56)?
            } else {
                0
            },
        })
    }
}

/// Decoded `struct drm_amdgpu_info` (32 bytes on every supported ABI).
pub struct InfoRequest {
    return_pointer: u64,
    return_size: usize,
    pub query: u32,
    pub data: [u32; 4],
}

impl InfoRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != INFO_SIZE {
            return Err(Error::InvalidArgument);
        }
        let return_pointer = read_u64(bytes, 0)?;
        let return_size = read_u32(bytes, 8)? as usize;
        if return_pointer == 0 || return_size == 0 {
            return Err(Error::InvalidArgument);
        }
        Ok(Self {
            return_pointer,
            return_size,
            query: read_u32(bytes, 12)?,
            data: [
                read_u32(bytes, 16)?,
                read_u32(bytes, 20)?,
                read_u32(bytes, 24)?,
                read_u32(bytes, 28)?,
            ],
        })
    }

    pub fn write_reply(&self, reply: &[u8]) -> Result<()> {
        let length = self.return_size.min(reply.len());
        UserAddress::new(self.return_pointer).write(&reply[..length])
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct HwIpInfo {
    pub major: u32,
    pub minor: u32,
    pub capabilities: u64,
    pub ib_start_alignment: u32,
    pub ib_size_alignment: u32,
    pub available_rings: u32,
    pub discovery_version: u32,
}

impl HwIpInfo {
    pub fn encode(self) -> Vec<u8> {
        let mut bytes = [0u8; HW_IP_INFO_SIZE];
        put_u32(&mut bytes, 0, self.major);
        put_u32(&mut bytes, 4, self.minor);
        put_u64(&mut bytes, 8, self.capabilities);
        put_u32(&mut bytes, 16, self.ib_start_alignment);
        put_u32(&mut bytes, 20, self.ib_size_alignment);
        put_u32(&mut bytes, 24, self.available_rings);
        put_u32(&mut bytes, 28, self.discovery_version);
        bytes.to_vec()
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct HeapInfo {
    pub total: u64,
    pub usable: u64,
    pub usage: u64,
    pub max_allocation: u64,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct MemoryInfo {
    pub vram: HeapInfo,
    pub visible_vram: HeapInfo,
    pub gtt: HeapInfo,
}

impl MemoryInfo {
    pub fn encode(self) -> Vec<u8> {
        let mut bytes = [0u8; 96];
        for (offset, heap) in [(0, self.vram), (32, self.visible_vram), (64, self.gtt)] {
            put_u64(&mut bytes, offset, heap.total);
            put_u64(&mut bytes, offset + 8, heap.usable);
            put_u64(&mut bytes, offset + 16, heap.usage);
            put_u64(&mut bytes, offset + 24, heap.max_allocation);
        }
        bytes.to_vec()
    }
}

/// Known fields of `struct drm_amdgpu_info_device`. Unknown or unsupported
/// fields remain zero, as required by the extensible Linux ABI.
#[derive(Clone, Copy, Debug, Default)]
pub struct DeviceInfo {
    pub device_id: u32,
    pub chip_rev: u32,
    pub external_rev: u32,
    pub pci_rev: u32,
    pub family: u32,
    pub num_shader_engines: u32,
    pub num_shader_arrays_per_engine: u32,
    pub gpu_counter_freq: u32,
    pub max_engine_clock: u64,
    pub max_memory_clock: u64,
    pub cu_active_number: u32,
    pub cu_bitmap: [[u32; 4]; 4],
    pub enabled_rb_pipes_mask: u32,
    pub num_rb_pipes: u32,
    pub num_hw_gfx_contexts: u32,
    pub virtual_address_offset: u64,
    pub virtual_address_max: u64,
    pub virtual_address_alignment: u32,
    pub pte_fragment_size: u32,
    pub gart_page_size: u32,
    pub vram_type: u32,
    pub vram_bit_width: u32,
    pub gc_double_offchip_lds_buf: u32,
    pub wave_front_size: u32,
    pub num_shader_visible_vgprs: u32,
    pub num_cu_per_sh: u32,
    pub num_tcc_blocks: u32,
    pub gs_vgt_table_depth: u32,
    pub gs_prim_buffer_depth: u32,
    pub max_gs_waves_per_vgt: u32,
    pub cu_ao_bitmap: [[u32; 4]; 4],
    pub pa_sc_tile_steering_override: u32,
    pub min_engine_clock: u64,
    pub min_memory_clock: u64,
    pub tcp_cache_size: u32,
    pub num_sqc_per_wgp: u32,
    pub sqc_data_cache_size: u32,
    pub sqc_inst_cache_size: u32,
    pub gl1c_cache_size: u32,
    pub gl2c_cache_size: u32,
}

impl DeviceInfo {
    pub fn encode(self) -> Vec<u8> {
        let mut bytes = [0u8; DEVICE_INFO_SIZE];
        for (offset, value) in [
            (0, self.device_id),
            (4, self.chip_rev),
            (8, self.external_rev),
            (12, self.pci_rev),
            (16, self.family),
            (20, self.num_shader_engines),
            (24, self.num_shader_arrays_per_engine),
            (28, self.gpu_counter_freq),
            (48, self.cu_active_number),
            (120, self.enabled_rb_pipes_mask),
            (124, self.num_rb_pipes),
            (128, self.num_hw_gfx_contexts),
            (160, self.virtual_address_alignment),
            (164, self.pte_fragment_size),
            (168, self.gart_page_size),
            (176, self.vram_type),
            (180, self.vram_bit_width),
            (188, self.gc_double_offchip_lds_buf),
            (240, self.wave_front_size),
            (244, self.num_shader_visible_vgprs),
            (248, self.num_cu_per_sh),
            (252, self.num_tcc_blocks),
            (256, self.gs_vgt_table_depth),
            (260, self.gs_prim_buffer_depth),
            (264, self.max_gs_waves_per_vgt),
            (352, self.pa_sc_tile_steering_override),
            (384, self.tcp_cache_size),
            (388, self.num_sqc_per_wgp),
            (392, self.sqc_data_cache_size),
            (396, self.sqc_inst_cache_size),
            (400, self.gl1c_cache_size),
            (404, self.gl2c_cache_size),
        ] {
            put_u32(&mut bytes, offset, value);
        }
        for (offset, value) in [
            (32, self.max_engine_clock),
            (40, self.max_memory_clock),
            (144, self.virtual_address_offset),
            (152, self.virtual_address_max),
            (368, self.min_engine_clock),
            (376, self.min_memory_clock),
        ] {
            put_u64(&mut bytes, offset, value);
        }
        for (index, value) in self.cu_bitmap.into_iter().flatten().enumerate() {
            put_u32(&mut bytes, 56 + index * 4, value);
        }
        for (index, value) in self.cu_ao_bitmap.into_iter().flatten().enumerate() {
            put_u32(&mut bytes, 272 + index * 4, value);
        }
        bytes.to_vec()
    }
}
