use alloc::vec::Vec;
use core::ops::Range;

use na_std::{Error, Result};

pub const PACKET_HEADER_SIZE: usize = 44;
pub const MESSAGE_PACKET: u32 = 0x0000_0001;
pub const MESSAGE_INDICATE_STATUS: u32 = 0x0000_0007;

const MESSAGE_INITIALIZE: u32 = 0x0000_0002;
const MESSAGE_QUERY: u32 = 0x0000_0004;
const MESSAGE_SET: u32 = 0x0000_0005;
const COMPLETE_INITIALIZE: u32 = 0x8000_0002;
const COMPLETE_QUERY: u32 = 0x8000_0004;
const COMPLETE_SET: u32 = 0x8000_0005;
const STATUS_SUCCESS: u32 = 0;

#[repr(u32)]
#[derive(Clone, Copy)]
pub enum Oid {
    MaximumFrameSize = 0x0001_0106,
    PacketFilter = 0x0001_010e,
    PermanentAddress = 0x0101_0101,
    CurrentAddress = 0x0101_0102,
}

#[derive(Clone, Copy)]
pub enum Request {
    Initialize { max_transfer_size: u32 },
    Query { oid: Oid },
    SetU32 { oid: Oid, value: u32 },
}

pub struct Command {
    request_id: u32,
    completion_type: u32,
    bytes: Vec<u8>,
}

impl Request {
    pub fn encode(self, request_id: u32) -> Command {
        let (length, message_type, completion_type) = match self {
            Self::Initialize { .. } => (24, MESSAGE_INITIALIZE, COMPLETE_INITIALIZE),
            Self::Query { .. } => (28, MESSAGE_QUERY, COMPLETE_QUERY),
            Self::SetU32 { .. } => (32, MESSAGE_SET, COMPLETE_SET),
        };
        let mut command = Command {
            request_id,
            completion_type,
            bytes: alloc::vec![0; length],
        };
        command.put(0, message_type);
        command.put(4, length as u32);
        command.put(8, request_id);

        match self {
            Self::Initialize { max_transfer_size } => {
                command.put(12, 1);
                command.put(16, 0);
                command.put(20, max_transfer_size);
            }
            Self::Query { oid } => command.put(12, oid as u32),
            Self::SetU32 { oid, value } => {
                command.put(12, oid as u32);
                command.put(16, 4);
                command.put(20, 20);
                command.put(28, value);
            }
        }
        command
    }
}

impl Command {
    pub fn request_id(&self) -> u32 {
        self.request_id
    }

    pub fn completion_type(&self) -> u32 {
        self.completion_type
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    fn put(&mut self, offset: usize, value: u32) {
        self.bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
}

pub struct WireMessage<'a> {
    bytes: &'a [u8],
}

impl<'a> WireMessage<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self> {
        if bytes.len() < 8 {
            return Err(Error::Io);
        }
        let length = Self::read(bytes, 4)? as usize;
        if length < 8 || length > bytes.len() {
            return Err(Error::Io);
        }
        Ok(Self {
            bytes: &bytes[..length],
        })
    }

    pub fn kind(&self) -> u32 {
        Self::read(self.bytes, 0).unwrap_or_default()
    }

    pub fn length(&self) -> usize {
        self.bytes.len()
    }

    pub fn validate_completion(&self, command: &Command) -> Result<()> {
        if self.kind() != command.completion_type()
            || Self::read(self.bytes, 8)? != command.request_id()
            || Self::read(self.bytes, 12)? != STATUS_SUCCESS
        {
            return Err(Error::Io);
        }
        Ok(())
    }

    pub fn initialize_max_transfer_size(&self) -> Result<usize> {
        usize::try_from(Self::read(self.bytes, 36)?).map_err(|_| Error::Range)
    }

    pub fn query_data(&self) -> Result<&'a [u8]> {
        let length = Self::read(self.bytes, 16)? as usize;
        let offset = 8usize
            .checked_add(Self::read(self.bytes, 20)? as usize)
            .ok_or(Error::Range)?;
        let end = offset.checked_add(length).ok_or(Error::Range)?;
        self.bytes.get(offset..end).ok_or(Error::Io)
    }

    pub fn packet_range(&self) -> Result<Option<Range<usize>>> {
        if self.kind() != MESSAGE_PACKET {
            return Ok(None);
        }
        if self.bytes.len() < PACKET_HEADER_SIZE {
            return Err(Error::Io);
        }
        let data_offset = 8usize
            .checked_add(Self::read(self.bytes, 8)? as usize)
            .ok_or(Error::Range)?;
        let data_length = Self::read(self.bytes, 12)? as usize;
        let data_end = data_offset.checked_add(data_length).ok_or(Error::Range)?;
        if data_offset < PACKET_HEADER_SIZE || data_end > self.bytes.len() {
            return Err(Error::Io);
        }
        Ok(Some(data_offset..data_end))
    }

    pub fn read_u32(data: &[u8]) -> Result<u32> {
        Self::read(data, 0)
    }

    fn read(bytes: &[u8], offset: usize) -> Result<u32> {
        let end = offset.checked_add(4).ok_or(Error::Range)?;
        let word = bytes.get(offset..end).ok_or(Error::Io)?;
        Ok(u32::from_le_bytes(word.try_into().unwrap()))
    }
}

pub struct Packet<'a> {
    frame: &'a [u8],
}

impl<'a> Packet<'a> {
    pub const fn new(frame: &'a [u8]) -> Self {
        Self { frame }
    }

    pub fn write(&self, output: &mut Vec<u8>) -> Result<()> {
        let message_length = PACKET_HEADER_SIZE
            .checked_add(self.frame.len())
            .ok_or(Error::Range)?;
        let message_length = u32::try_from(message_length).map_err(|_| Error::Range)?;
        let frame_length = u32::try_from(self.frame.len()).map_err(|_| Error::Range)?;
        let required = message_length as usize;
        if output.capacity() < required {
            output
                .try_reserve_exact(required - output.len())
                .map_err(|_| Error::OutOfMemory)?;
        }
        output.resize(required, 0);
        output.fill(0);
        output[0..4].copy_from_slice(&MESSAGE_PACKET.to_le_bytes());
        output[4..8].copy_from_slice(&message_length.to_le_bytes());
        output[8..12].copy_from_slice(&36u32.to_le_bytes());
        output[12..16].copy_from_slice(&frame_length.to_le_bytes());
        output[PACKET_HEADER_SIZE..].copy_from_slice(self.frame);
        Ok(())
    }
}
