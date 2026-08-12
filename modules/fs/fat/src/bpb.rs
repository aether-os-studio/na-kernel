use crate::block::{le16, le32};
use na_std::{Error, Result};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FatKind {
    Fat12,
    Fat16,
    Fat32,
}

#[derive(Clone, Copy, Debug)]
pub struct FatBpb {
    pub kind: FatKind,
    pub bytes_per_sector: u32,
    pub sectors_per_cluster: u32,
    pub reserved: u32,
    pub fats: u32,
    pub fat_sectors: u32,
    pub root_entries: u32,
    pub root_dir_sector: u64,
    pub data_sector: u64,
    pub root_cluster: u32,
    pub total_clusters: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct ExFatBpb {
    pub bytes_per_sector: u32,
    pub sectors_per_cluster: u32,
    pub fat_offset: u64,
    pub heap_offset: u64,
    pub cluster_count: u32,
    pub root_cluster: u32,
}

pub fn parse_fat(buf: &[u8]) -> Result<FatBpb> {
    if buf.len() < 512 || le16(buf, 510) != 0xaa55 {
        return Err(Error::InvalidArgument);
    }
    let bps = le16(buf, 11) as u32;
    let spc = buf[13] as u32;
    let reserved = le16(buf, 14) as u32;
    let fats = buf[16] as u32;
    let root_entries = le16(buf, 17) as u32;
    let total = if le16(buf, 19) != 0 {
        le16(buf, 19) as u64
    } else {
        le32(buf, 32) as u64
    };
    let fat_sectors = if le16(buf, 22) != 0 {
        le16(buf, 22) as u32
    } else {
        le32(buf, 36)
    };
    if !bps.is_power_of_two()
        || !(512..=4096).contains(&bps)
        || spc == 0
        || fats == 0
        || fat_sectors == 0
    {
        return Err(Error::InvalidArgument);
    }
    let root_dir_sectors = (root_entries * 32).div_ceil(bps);
    let data_sector = reserved as u64 + fats as u64 * fat_sectors as u64 + root_dir_sectors as u64;
    if total <= data_sector {
        return Err(Error::InvalidArgument);
    }
    let clusters = ((total - data_sector) / spc as u64) as u32;
    let kind = if clusters < 4085 {
        FatKind::Fat12
    } else if clusters < 65525 {
        FatKind::Fat16
    } else {
        FatKind::Fat32
    };
    Ok(FatBpb {
        kind,
        bytes_per_sector: bps,
        sectors_per_cluster: spc,
        reserved,
        fats,
        fat_sectors,
        root_entries,
        root_dir_sector: reserved as u64 + fats as u64 * fat_sectors as u64,
        data_sector,
        root_cluster: if kind == FatKind::Fat32 {
            le32(buf, 44)
        } else {
            0
        },
        total_clusters: clusters,
    })
}

pub fn parse_exfat(buf: &[u8]) -> Result<ExFatBpb> {
    if buf.len() < 512 || &buf[3..11] != b"EXFAT   " {
        return Err(Error::InvalidArgument);
    }
    let shift = buf[108];
    let cshift = buf[109];
    if !(9..=12).contains(&shift) || cshift < shift {
        return Err(Error::InvalidArgument);
    }
    Ok(ExFatBpb {
        bytes_per_sector: 1u32 << shift,
        sectors_per_cluster: 1u32 << (cshift - shift),
        fat_offset: le32(buf, 80) as u64,
        heap_offset: le32(buf, 88) as u64,
        cluster_count: le32(buf, 92),
        root_cluster: le32(buf, 96),
    })
}

pub fn is_exfat(buf: &[u8]) -> bool {
    buf.len() >= 11 && &buf[3..11] == b"EXFAT   "
}
