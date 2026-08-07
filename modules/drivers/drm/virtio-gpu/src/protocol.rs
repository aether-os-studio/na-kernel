use na_std::{Error, Result, bindings};

pub const QUEUE_CONTROL: u16 = 0;
pub const QUEUE_CURSOR: u16 = 1;
pub const MAX_SCANOUTS: usize = 64;

pub const FLAG_FENCE: u32 = 1;
pub const F_VIRGL: u64 = 1 << 0;
pub const F_EDID: u64 = 1 << 1;
pub const F_CONTEXT_INIT: u64 = 1 << 4;
pub const F_SUPPORTED_CAPSET_IDS: u64 = 1 << 5;
pub const F_RING_INDIRECT_DESC: u64 = 1 << 28;
pub const F_VERSION_1: u64 = 1 << 32;
pub const SUPPORTED_FEATURES: u64 =
    F_VIRGL | F_EDID | F_CONTEXT_INIT | F_SUPPORTED_CAPSET_IDS | F_RING_INDIRECT_DESC | F_VERSION_1;

pub const FORMAT_B8G8R8A8_UNORM: u32 = 1;
pub const FORMAT_B8G8R8X8_UNORM: u32 = 2;
pub const FORMAT_A8R8G8B8_UNORM: u32 = 3;
pub const FORMAT_X8R8G8B8_UNORM: u32 = 4;

pub const CMD_GET_DISPLAY_INFO: u32 = 0x0100;
pub const CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
pub const CMD_RESOURCE_UNREF: u32 = 0x0102;
pub const CMD_SET_SCANOUT: u32 = 0x0103;
pub const CMD_RESOURCE_FLUSH: u32 = 0x0104;
pub const CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
pub const CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;
pub const CMD_RESOURCE_DETACH_BACKING: u32 = 0x0107;
pub const CMD_GET_CAPSET_INFO: u32 = 0x0108;
pub const CMD_GET_CAPSET: u32 = 0x0109;
pub const CMD_CTX_CREATE: u32 = 0x0200;
pub const CMD_CTX_DESTROY: u32 = 0x0201;
pub const CMD_CTX_ATTACH_RESOURCE: u32 = 0x0202;
pub const CMD_CTX_DETACH_RESOURCE: u32 = 0x0203;
pub const CMD_RESOURCE_CREATE_3D: u32 = 0x0204;
pub const CMD_TRANSFER_TO_HOST_3D: u32 = 0x0205;
pub const CMD_TRANSFER_FROM_HOST_3D: u32 = 0x0206;
pub const CMD_SUBMIT_3D: u32 = 0x0207;

pub const RESP_OK_NODATA: u32 = 0x1100;
pub const RESP_OK_DISPLAY_INFO: u32 = 0x1101;
pub const RESP_OK_CAPSET_INFO: u32 = 0x1102;
pub const RESP_OK_CAPSET: u32 = 0x1103;

pub struct Command<const N: usize> {
    bytes: [u8; N],
}

impl<const N: usize> Command<N> {
    pub fn new(kind: u32) -> Self {
        let mut command = Self { bytes: [0; N] };
        command.put_u32(0, kind);
        command
    }

    pub fn put_u32(&mut self, offset: usize, value: u32) {
        self.bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    pub fn put_u64(&mut self, offset: usize, value: u64) {
        self.bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn bytes_mut(&mut self) -> &mut [u8] {
        &mut self.bytes
    }
}

pub fn response_type(response: &[u8]) -> Result<u32> {
    let bytes = response.get(..4).ok_or(Error::InvalidArgument)?;
    Ok(u32::from_le_bytes(bytes.try_into().unwrap()))
}

pub fn check_response(response: &[u8], expected: u32) -> Result<()> {
    let kind = response_type(response)?;
    if kind >= 0x1200 {
        return Err(Error::Kernel(-(kind as i32)));
    }
    (kind == expected)
        .then_some(())
        .ok_or(Error::Kernel(-(bindings::EIO as i32)))
}

pub fn rect(command: &mut [u8], offset: usize, x: u32, y: u32, w: u32, h: u32) {
    command[offset..offset + 4].copy_from_slice(&x.to_le_bytes());
    command[offset + 4..offset + 8].copy_from_slice(&y.to_le_bytes());
    command[offset + 8..offset + 12].copy_from_slice(&w.to_le_bytes());
    command[offset + 12..offset + 16].copy_from_slice(&h.to_le_bytes());
}

pub fn box3(command: &mut [u8], offset: usize, x: u32, y: u32, z: u32, w: u32, h: u32, d: u32) {
    for (index, value) in [x, y, z, w, h, d].into_iter().enumerate() {
        command[offset + index * 4..offset + index * 4 + 4].copy_from_slice(&value.to_le_bytes());
    }
}
