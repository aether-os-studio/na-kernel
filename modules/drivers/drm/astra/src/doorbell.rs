//! Doorbell index assignments for the navi10-class aperture (Linux
//! `enum AMDGPU_NAVI10_DOORBELL_ASSIGNMENT` in amdgpu_doorbell.h).
//! All values are dword indexes into the BAR2 aperture.

pub const DOORBELL_KIQ: u32 = 0x000;
pub const DOORBELL_MEC_RING0: u32 = 0x003;
pub const DOORBELL_GFX_RING0: u32 = 0x08b;
pub const DOORBELL_GFX_RING1: u32 = 0x08c;
pub const DOORBELL_SDMA_ENGINE0: u32 = 0x100;
pub const DOORBELL_SDMA_ENGINE1: u32 = 0x10a;
pub const DOORBELL_SDMA_ENGINE2: u32 = 0x114;
pub const DOORBELL_SDMA_ENGINE3: u32 = 0x11e;
pub const DOORBELL_IH: u32 = 0x178;
pub const DOORBELL_VCN_0_1: u32 = 0x188;

/// Linux doorbell dword index for a ring (assigned index << 1, matching
/// `ring->doorbell_index = doorbell_index.xxx << 1`).
pub const fn ring_doorbell(assigned: u32) -> u32 {
    assigned << 1
}
