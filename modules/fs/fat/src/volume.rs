use alloc::{string::String, vec, vec::Vec};

use na_std::{Error, Result, vfs::BlockDevice};

use crate::{
    block::{BlockIo, le16, le32, le64},
    bpb::{self, ExFatBpb, FatBpb, FatKind},
};

#[derive(Clone, Copy, Debug)]
pub struct Node {
    pub cluster: u32,
    pub cluster_hint: u32,
    pub cluster_hint_index: u64,
    pub size: u64,
    pub dir: bool,
    pub contiguous: bool,
    pub entry_offset: u64,
}

pub enum Volume {
    Fat(FatVolume),
    ExFat(ExFatVolume),
}
pub struct FatVolume {
    pub dev: BlockDevice,
    pub bpb: FatBpb,
    next_free_cluster: u32,
}
pub struct ExFatVolume {
    pub dev: BlockDevice,
    pub bpb: ExFatBpb,
}

impl Volume {
    pub fn open(dev: u64) -> Result<Self> {
        let device = BlockDevice::new(dev);
        let mut boot = vec![0; 512];
        device.read_at(0, &mut boot)?;
        if bpb::is_exfat(&boot) {
            Ok(Self::ExFat(ExFatVolume {
                dev: device,
                bpb: bpb::parse_exfat(&boot)?,
            }))
        } else {
            Ok(Self::Fat(FatVolume {
                dev: device,
                bpb: bpb::parse_fat(&boot)?,
                next_free_cluster: 2,
            }))
        }
    }

    pub fn block_size(&self) -> u32 {
        match self {
            Self::Fat(v) => v.bpb.bytes_per_sector,
            Self::ExFat(v) => v.bpb.bytes_per_sector,
        }
    }
    pub fn root(&self) -> Node {
        match self {
            Self::Fat(v) => Node {
                cluster: if v.bpb.kind == FatKind::Fat32 {
                    v.bpb.root_cluster
                } else {
                    0
                },
                cluster_hint: if v.bpb.kind == FatKind::Fat32 {
                    v.bpb.root_cluster
                } else {
                    0
                },
                cluster_hint_index: 0,
                size: 0,
                dir: true,
                contiguous: false,
                entry_offset: u64::MAX,
            },
            Self::ExFat(v) => Node {
                cluster: v.bpb.root_cluster,
                cluster_hint: v.bpb.root_cluster,
                cluster_hint_index: 0,
                size: 0,
                dir: true,
                contiguous: false,
                entry_offset: u64::MAX,
            },
        }
    }
    pub fn lookup(&self, dir: Node, name: &[u8]) -> Result<Node> {
        match self {
            Self::Fat(v) => v.lookup(dir, name),
            Self::ExFat(v) => v.lookup(dir, name),
        }
    }
    pub fn list(
        &self,
        dir: Node,
        pos: usize,
        emit: &mut dyn FnMut(&[u8], Node) -> bool,
    ) -> Result<usize> {
        match self {
            Self::Fat(v) => v.list(dir, pos, emit),
            Self::ExFat(v) => v.list(dir, pos, emit),
        }
    }
    pub fn read(&self, node: Node, pos: u64, out: &mut [u8]) -> Result<usize> {
        match self {
            Self::Fat(v) => v.read(node, pos, out),
            Self::ExFat(v) => v.read(node, pos, out),
        }
    }
    pub fn write(&mut self, node: Node, pos: u64, data: &[u8]) -> Result<Node> {
        match self {
            Self::Fat(v) => v.write(node, pos, data),
            Self::ExFat(_) => Err(Error::Unsupported),
        }
    }
    pub fn create(&mut self, dir: Node, name: &[u8], is_dir: bool) -> Result<Node> {
        match self {
            Self::Fat(v) => v.create(dir, name, is_dir),
            Self::ExFat(_) => Err(Error::Unsupported),
        }
    }
    pub fn remove(&mut self, dir: Node, name: &[u8], expect_dir: bool) -> Result<()> {
        match self {
            Self::Fat(v) => v.remove(dir, name, expect_dir),
            Self::ExFat(_) => Err(Error::Unsupported),
        }
    }
    pub fn rename(
        &mut self,
        old_dir: Node,
        old_name: &[u8],
        new_dir: Node,
        new_name: &[u8],
    ) -> Result<()> {
        match self {
            Self::Fat(v) => v.rename(old_dir, old_name, new_dir, new_name),
            Self::ExFat(_) => Err(Error::Unsupported),
        }
    }
    pub fn truncate(&mut self, node: Node, size: u64) -> Result<Node> {
        match self {
            Self::Fat(v) => v.truncate(node, size),
            Self::ExFat(_) => Err(Error::Unsupported),
        }
    }
    pub fn flush(&self) -> Result<()> {
        match self {
            Self::Fat(v) => v.dev.flush(),
            Self::ExFat(v) => v.dev.flush(),
        }
    }
    pub fn stats(&self) -> (u64, u64, u64) {
        match self {
            Self::Fat(v) => (v.bpb.total_clusters as u64, 0, 0),
            Self::ExFat(v) => (v.bpb.cluster_count as u64, 0, 0),
        }
    }
}

impl FatVolume {
    fn cluster_size(&self) -> u64 {
        self.bpb.bytes_per_sector as u64 * self.bpb.sectors_per_cluster as u64
    }
    fn cluster_offset(&self, c: u32) -> u64 {
        (self.bpb.data_sector + (c.saturating_sub(2) as u64) * self.bpb.sectors_per_cluster as u64)
            * self.bpb.bytes_per_sector as u64
    }
    fn fat_eoc(&self) -> u32 {
        match self.bpb.kind {
            FatKind::Fat12 => 0xfff,
            FatKind::Fat16 => 0xffff,
            FatKind::Fat32 => 0x0fff_ffff,
        }
    }
    fn is_eoc(&self, v: u32) -> bool {
        match self.bpb.kind {
            FatKind::Fat12 => v >= 0xff8,
            FatKind::Fat16 => v >= 0xfff8,
            FatKind::Fat32 => v >= 0x0fff_fff8,
        }
    }

    fn fat_entry(&self, c: u32) -> Result<u32> {
        let (off, len) = match self.bpb.kind {
            FatKind::Fat12 => ((c as u64 * 3) / 2, 2),
            FatKind::Fat16 => (c as u64 * 2, 2),
            FatKind::Fat32 => (c as u64 * 4, 4),
        };
        let base = self.bpb.reserved as u64 * self.bpb.bytes_per_sector as u64;
        let mut b = [0; 4];
        self.dev.read_at(base + off, &mut b[..len])?;
        Ok(match self.bpb.kind {
            FatKind::Fat12 => {
                let x = le16(&b, 0) as u32;
                if c & 1 != 0 {
                    (x >> 4) & 0xfff
                } else {
                    x & 0xfff
                }
            }
            FatKind::Fat16 => le16(&b, 0) as u32,
            FatKind::Fat32 => le32(&b, 0) & 0x0fff_ffff,
        })
    }

    fn fat_write_entry(&self, c: u32, value: u32) -> Result<()> {
        let (off, len) = match self.bpb.kind {
            FatKind::Fat12 => ((c as u64 * 3) / 2, 2),
            FatKind::Fat16 => (c as u64 * 2, 2),
            FatKind::Fat32 => (c as u64 * 4, 4),
        };
        match self.bpb.kind {
            FatKind::Fat12 => {
                let base = self.bpb.reserved as u64 * self.bpb.bytes_per_sector as u64;
                let mut b = [0; 4];
                self.dev.read_at(base + off, &mut b[..len])?;
                let old = le16(&b, 0);
                let v = value as u16 & 0xfff;
                let x = if c & 1 != 0 {
                    (old & 0x000f) | (v << 4)
                } else {
                    (old & 0xf000) | v
                };
                self.write_fat_copies(off, &x.to_le_bytes())?;
            }
            FatKind::Fat16 => self.write_fat_copies(off, &(value as u16).to_le_bytes())?,
            FatKind::Fat32 => self.write_fat_copies(off, &(value | 0xf000_0000).to_le_bytes())?,
        }
        Ok(())
    }
    fn write_fat_copies(&self, offset: u64, data: &[u8]) -> Result<()> {
        for copy in 0..self.bpb.fats {
            let base = (self.bpb.reserved + copy * self.bpb.fat_sectors) as u64
                * self.bpb.bytes_per_sector as u64;
            self.dev.write(base + offset, data)?;
        }
        Ok(())
    }
    fn alloc_cluster(&mut self) -> Result<u32> {
        let end = self.bpb.total_clusters + 2;
        let mut c = self.next_free_cluster.clamp(2, end.saturating_sub(1));
        for _ in 0..self.bpb.total_clusters {
            if self.fat_entry(c)? == 0 {
                self.fat_write_entry(c, self.fat_eoc())?;
                let zero = vec![0; self.cluster_size() as usize];
                self.dev.write(self.cluster_offset(c), &zero)?;
                self.next_free_cluster = if c + 1 < end { c + 1 } else { 2 };
                return Ok(c);
            }
            c += 1;
            if c >= end {
                c = 2;
            }
        }
        Err(Error::NoSpace)
    }
    fn chain(&self, first: u32) -> Result<Vec<u32>> {
        let mut out = Vec::new();
        if first < 2 {
            return Ok(out);
        }
        let mut c = first;
        for _ in 0..=self.bpb.total_clusters {
            out.push(c);
            let next = self.fat_entry(c)?;
            if self.is_eoc(next) {
                return Ok(out);
            }
            if next < 2 || next >= self.bpb.total_clusters + 2 {
                return Err(Error::Io);
            }
            c = next;
        }
        Err(Error::Io)
    }
    fn dir_bytes(&self, dir: Node) -> Result<Vec<u8>> {
        if dir.cluster == 0 {
            let n = self.bpb.root_entries as usize * 32;
            let mut b = vec![0; n];
            self.dev.read_at(
                self.bpb.root_dir_sector * self.bpb.bytes_per_sector as u64,
                &mut b,
            )?;
            return Ok(b);
        }
        let mut b = Vec::new();
        if dir.cluster < 2 || dir.cluster >= self.bpb.total_clusters + 2 {
            return Err(Error::Io);
        }
        for c in self.chain(dir.cluster)? {
            let mut x = vec![0; self.cluster_size() as usize];
            self.dev.read_at(self.cluster_offset(c), &mut x)?;
            b.extend_from_slice(&x);
        }
        Ok(b)
    }
    fn entry_offset(&self, dir: Node, index: u64) -> u64 {
        if dir.cluster == 0 {
            self.bpb.root_dir_sector * self.bpb.bytes_per_sector as u64 + index
        } else {
            self.cluster_offset(dir.cluster) + index
        }
    }
    fn short_name(raw: &[u8], case_flags: u8) -> String {
        let mut s = String::new();
        for &x in &raw[..8] {
            if x == b' ' {
                break;
            }
            s.push(if case_flags & 0x08 != 0 {
                x.to_ascii_lowercase() as char
            } else {
                x as char
            });
        }
        if raw[8..11].iter().any(|&x| x != b' ') {
            s.push('.');
            for &x in &raw[8..11] {
                if x != b' ' {
                    s.push(if case_flags & 0x10 != 0 {
                        x.to_ascii_lowercase() as char
                    } else {
                        x as char
                    });
                }
            }
        }
        s
    }
    fn short_name_checksum(raw: &[u8]) -> u8 {
        let mut sum = 0u8;
        for &c in &raw[..11] {
            sum = ((sum & 1) << 7).wrapping_add(sum >> 1).wrapping_add(c);
        }
        sum
    }
    fn long_name_part(entry: &[u8]) -> String {
        let mut units = Vec::with_capacity(13);
        for off in [1usize, 14, 28] {
            let count = if off == 1 {
                5
            } else if off == 14 {
                6
            } else {
                2
            };
            for i in 0..count {
                let unit = le16(entry, off + i * 2);
                if unit == 0 || unit == 0xffff {
                    break;
                }
                units.push(unit);
            }
        }
        String::from_utf16_lossy(&units)
    }
    fn entries(&self, dir: Node) -> Result<Vec<(String, Node)>> {
        let bytes = self.dir_bytes(dir)?;
        let mut out = Vec::new();
        let mut long_name: Option<String> = None;
        let mut long_checksum: Option<u8> = None;
        for (i, e) in bytes.chunks_exact(32).enumerate() {
            if e[0] == 0 {
                break;
            }
            if e[0] == 0xe5 {
                long_name = None;
                long_checksum = None;
                continue;
            }
            if e[11] == 0x0f {
                // VFAT stores long-name entries immediately before their
                // short entry, in reverse order (N ... 2, 1).
                let part = Self::long_name_part(e);
                let mut name = part;
                if let Some(previous) = long_name.take() {
                    name.push_str(&previous);
                }
                long_name = Some(name);
                long_checksum = Some(e[13]);
                continue;
            }
            if e[11] & 8 != 0 {
                long_name = None;
                long_checksum = None;
                continue;
            }
            let c = (le16(e, 20) as u32) << 16 | le16(e, 26) as u32;
            let short = Self::short_name(&e[..11], e[12]);
            let name = if long_checksum == Some(Self::short_name_checksum(&e[..11])) {
                long_name.take().unwrap_or(short)
            } else {
                long_name = None;
                short
            };
            long_checksum = None;
            // FAT directories contain these mandatory self/parent entries,
            // but they are structural metadata rather than children.  Keep
            // them out of lookup/list results so an empty directory remains
            // removable and readdir matches the other filesystems.
            if name == "." || name == ".." {
                continue;
            }
            out.push((
                name,
                Node {
                    cluster: c,
                    cluster_hint: c,
                    cluster_hint_index: 0,
                    size: le32(e, 28) as u64,
                    dir: e[11] & 0x10 != 0,
                    contiguous: false,
                    entry_offset: self.entry_offset(dir, i as u64 * 32),
                },
            ));
        }
        Ok(out)
    }
    fn lookup(&self, dir: Node, name: &[u8]) -> Result<Node> {
        let t = String::from_utf8_lossy(name);
        self.entries(dir)?
            .into_iter()
            .find(|(n, _)| n.eq_ignore_ascii_case(&t))
            .map(|(_, v)| v)
            .ok_or(Error::NotFound)
    }
    fn list(
        &self,
        dir: Node,
        pos: usize,
        emit: &mut dyn FnMut(&[u8], Node) -> bool,
    ) -> Result<usize> {
        let entries = self.entries(dir)?;
        let mut i = pos;
        while i < entries.len() {
            let (name, node) = &entries[i];
            if !emit(name.as_bytes(), *node) {
                break;
            }
            i += 1;
        }
        Ok(i)
    }
    fn read(&self, node: Node, pos: u64, out: &mut [u8]) -> Result<usize> {
        if pos >= node.size {
            return Ok(0);
        }
        let want = core::cmp::min(out.len(), (node.size - pos) as usize);
        let chain = self.chain(node.cluster)?;
        let mut done = 0;
        let mut p = pos;
        while done < want {
            let c = chain[(p / self.cluster_size()) as usize];
            let off = (p % self.cluster_size()) as usize;
            let n = core::cmp::min(want - done, self.cluster_size() as usize - off);
            self.dev.read_at(
                self.cluster_offset(c) + off as u64,
                &mut out[done..done + n],
            )?;
            done += n;
            p += n as u64;
        }
        Ok(done)
    }

    fn encode_short(name: &[u8]) -> Result<([u8; 11], u8)> {
        let mut out = [b' '; 11];
        let mut case_flags = 0;
        let split = name.iter().position(|&x| x == b'.').unwrap_or(name.len());
        if split == 0 || split > 8 || name.len().saturating_sub(split + 1) > 3 {
            return Err(Error::InvalidArgument);
        }
        for i in 0..split {
            let c = name[i];
            if c == b'/' || c == b' ' {
                return Err(Error::InvalidArgument);
            }
            out[i] = c.to_ascii_uppercase();
        }
        if name[..split].iter().all(|&c| c.is_ascii_lowercase()) {
            case_flags |= 0x08;
        }
        if split < name.len() {
            for (j, &c) in name[split + 1..].iter().enumerate() {
                if c == b'/' || c == b' ' {
                    return Err(Error::InvalidArgument);
                }
                out[8 + j] = c.to_ascii_uppercase();
            }
            if name[split + 1..].iter().all(|&c| c.is_ascii_lowercase()) {
                case_flags |= 0x10;
            }
        }
        Ok((out, case_flags))
    }
    fn create(&mut self, dir: Node, name: &[u8], is_dir: bool) -> Result<Node> {
        let (short, case_flags) = Self::encode_short(name)?;
        if self.lookup(dir, name).is_ok() {
            return Err(Error::AlreadyExists);
        }
        let bytes = self.dir_bytes(dir)?;
        let index = bytes
            .chunks_exact(32)
            .position(|e| e[0] == 0 || e[0] == 0xe5)
            .ok_or(Error::NoSpace)? as u64;
        let cluster = if is_dir { self.alloc_cluster()? } else { 0 };
        let mut e = [0; 32];
        e[..11].copy_from_slice(&short);
        e[11] = if is_dir { 0x10 } else { 0 };
        e[12] = case_flags;
        e[20..22].copy_from_slice(&((cluster >> 16) as u16).to_le_bytes());
        e[26..28].copy_from_slice(&(cluster as u16).to_le_bytes());
        self.dev.write(self.entry_offset(dir, index * 32), &e)?;
        if is_dir {
            let mut first = vec![0; self.cluster_size() as usize];
            first[0..11].copy_from_slice(b".          ");
            first[11] = 0x10;
            first[20..22].copy_from_slice(&((cluster >> 16) as u16).to_le_bytes());
            first[26..28].copy_from_slice(&(cluster as u16).to_le_bytes());
            first[32..43].copy_from_slice(b"..         ");
            first[43] = 0x10;
            self.dev.write(self.cluster_offset(cluster), &first)?;
        }
        Ok(Node {
            cluster,
            cluster_hint: cluster,
            cluster_hint_index: 0,
            size: 0,
            dir: is_dir,
            contiguous: false,
            entry_offset: self.entry_offset(dir, index * 32),
        })
    }
    fn locate(&self, dir: Node, name: &[u8]) -> Result<Node> {
        self.lookup(dir, name)
    }
    fn remove(&self, dir: Node, name: &[u8], expect_dir: bool) -> Result<()> {
        let node = self.locate(dir, name)?;
        if node.dir != expect_dir {
            return Err(if expect_dir {
                Error::NotDirectory
            } else {
                Error::IsDirectory
            });
        }
        if node.dir && !self.entries(node)?.is_empty() {
            return Err(Error::NotEmpty);
        }
        for c in self.chain(node.cluster)? {
            self.fat_write_entry(c, 0)?
        }
        self.dev.write(node.entry_offset, &[0xe5])?;
        Ok(())
    }
    fn rename(&self, old_dir: Node, old_name: &[u8], new_dir: Node, new_name: &[u8]) -> Result<()> {
        let node = self.locate(old_dir, old_name)?;
        if self.lookup(new_dir, new_name).is_ok() {
            return Err(Error::AlreadyExists);
        }
        let (short, case_flags) = Self::encode_short(new_name)?;
        let mut e = [0; 32];
        self.dev.read_at(node.entry_offset, &mut e)?;
        e[..11].copy_from_slice(&short);
        e[12] = case_flags;
        self.dev.write(node.entry_offset, &e)?;
        Ok(())
    }
    fn write(&mut self, mut node: Node, pos: u64, data: &[u8]) -> Result<Node> {
        if node.dir {
            return Err(Error::IsDirectory);
        }
        if data.is_empty() {
            return Ok(node);
        }
        let need = pos.checked_add(data.len() as u64).ok_or(Error::Range)?;
        if node.cluster < 2 {
            let c = self.alloc_cluster()?;
            node.cluster = c;
            node.cluster_hint = c;
            node.cluster_hint_index = 0;
            let mut e = [0; 32];
            self.dev.read_at(node.entry_offset, &mut e)?;
            e[20..22].copy_from_slice(&((c >> 16) as u16).to_le_bytes());
            e[26..28].copy_from_slice(&(c as u16).to_le_bytes());
            self.dev.write(node.entry_offset, &e)?;
        }

        let target_index = pos / self.cluster_size();
        let (mut c, mut cluster_index) =
            if node.cluster_hint >= 2 && node.cluster_hint_index <= target_index {
                (node.cluster_hint, node.cluster_hint_index)
            } else {
                (node.cluster, 0)
            };
        while cluster_index < target_index {
            let next = self.fat_entry(c)?;
            c = if self.is_eoc(next) {
                let allocated = self.alloc_cluster()?;
                self.fat_write_entry(c, allocated)?;
                allocated
            } else if next < 2 || next >= self.bpb.total_clusters + 2 {
                return Err(Error::Io);
            } else {
                next
            };
            cluster_index += 1;
        }

        let mut done = 0;
        let mut p = pos;
        while done < data.len() {
            let off = (p % self.cluster_size()) as usize;
            let n = core::cmp::min(data.len() - done, self.cluster_size() as usize - off);
            self.dev
                .write(self.cluster_offset(c) + off as u64, &data[done..done + n])?;
            done += n;
            p += n as u64;
            if done < data.len() {
                let next = self.fat_entry(c)?;
                c = if self.is_eoc(next) {
                    let allocated = self.alloc_cluster()?;
                    self.fat_write_entry(c, allocated)?;
                    allocated
                } else if next < 2 || next >= self.bpb.total_clusters + 2 {
                    return Err(Error::Io);
                } else {
                    next
                };
                cluster_index += 1;
            }
        }
        let size = core::cmp::max(node.size, need);
        if size != node.size {
            self.dev
                .write(node.entry_offset + 28, &(size as u32).to_le_bytes())?;
        }
        Ok(Node {
            cluster_hint: c,
            cluster_hint_index: cluster_index,
            size,
            ..node
        })
    }
    fn truncate(&self, mut node: Node, size: u64) -> Result<Node> {
        if size > node.size {
            return Ok(node);
        }
        let chain = self.chain(node.cluster)?;
        let keep = ((size + self.cluster_size() - 1) / self.cluster_size()) as usize;
        if keep == 0 {
            for c in chain {
                self.fat_write_entry(c, 0)?;
            }
            node.cluster = 0;
            node.cluster_hint = 0;
            node.cluster_hint_index = 0;
        } else if keep < chain.len() {
            self.fat_write_entry(chain[keep - 1], self.fat_eoc())?;
            for c in &chain[keep..] {
                self.fat_write_entry(*c, 0)?;
            }
            node.cluster_hint = chain[keep - 1];
            node.cluster_hint_index = keep as u64 - 1;
        } else if node.cluster_hint_index >= keep as u64 {
            node.cluster_hint = chain[keep - 1];
            node.cluster_hint_index = keep as u64 - 1;
        }
        let mut e = [0; 32];
        self.dev.read_at(node.entry_offset, &mut e)?;
        if keep == 0 {
            e[20..22].copy_from_slice(&0u16.to_le_bytes());
            e[26..28].copy_from_slice(&0u16.to_le_bytes());
        }
        e[28..32].copy_from_slice(&(size as u32).to_le_bytes());
        self.dev.write(node.entry_offset, &e)?;
        Ok(Node { size, ..node })
    }
}

impl ExFatVolume {
    fn cluster_size(&self) -> u64 {
        self.bpb.bytes_per_sector as u64 * self.bpb.sectors_per_cluster as u64
    }
    fn cluster_offset(&self, c: u32) -> u64 {
        (self.bpb.heap_offset + (c.saturating_sub(2) as u64) * self.bpb.sectors_per_cluster as u64)
            * self.bpb.bytes_per_sector as u64
    }
    fn fat_entry(&self, c: u32) -> Result<u32> {
        let mut b = [0; 4];
        self.dev.read_at(
            self.bpb.fat_offset * self.bpb.bytes_per_sector as u64 + c as u64 * 4,
            &mut b,
        )?;
        Ok(le32(&b, 0) & 0x0fffffff)
    }
    fn dir_bytes(&self, dir: Node) -> Result<Vec<u8>> {
        let mut b = Vec::new();
        let mut c = dir.cluster;
        for _ in 0..self.bpb.cluster_count {
            let mut x = vec![0; self.cluster_size() as usize];
            self.dev.read_at(self.cluster_offset(c), &mut x)?;
            b.extend_from_slice(&x);
            let n = self.fat_entry(c)?;
            if n >= 0xfffffff8 {
                break;
            }
            c = n;
        }
        Ok(b)
    }
    fn entries(&self, dir: Node) -> Result<Vec<(String, Node)>> {
        let b = self.dir_bytes(dir)?;
        let mut out = Vec::new();
        let mut i = 0;
        while i + 32 <= b.len() {
            let e = &b[i..i + 32];
            if e[0] == 0 {
                break;
            }
            if e[0] == 0x85 {
                let sec = e[1] as usize;
                if sec < 2 || i + 32 * (sec + 1) > b.len() {
                    break;
                }
                let s = &b[i + 32..i + 64];
                let mut n = String::new();
                for j in 0..sec - 1 {
                    let x = &b[i + 64 + j * 32..i + 96 + j * 32];
                    for q in (2..32).step_by(2) {
                        let v = le16(x, q);
                        if v != 0 && v != 0xffff {
                            n.push(char::from_u32(v as u32).unwrap_or('?'));
                        }
                    }
                }
                out.push((
                    n,
                    Node {
                        cluster: le32(s, 20),
                        cluster_hint: le32(s, 20),
                        cluster_hint_index: 0,
                        size: le64(s, 24),
                        dir: le16(e, 4) & 0x10 != 0,
                        contiguous: s[1] & 2 != 0,
                        entry_offset: u64::MAX,
                    },
                ));
                i += 32 * (sec + 1);
                continue;
            }
            i += 32;
        }
        Ok(out)
    }
    fn lookup(&self, dir: Node, name: &[u8]) -> Result<Node> {
        let t = String::from_utf8_lossy(name);
        self.entries(dir)?
            .into_iter()
            .find(|(n, _)| n.eq_ignore_ascii_case(&t))
            .map(|(_, v)| v)
            .ok_or(Error::NotFound)
    }
    fn list(
        &self,
        dir: Node,
        pos: usize,
        emit: &mut dyn FnMut(&[u8], Node) -> bool,
    ) -> Result<usize> {
        let e = self.entries(dir)?;
        let mut i = pos;
        while i < e.len() {
            if !emit(e[i].0.as_bytes(), e[i].1) {
                break;
            }
            i += 1;
        }
        Ok(i)
    }
    fn read(&self, node: Node, pos: u64, out: &mut [u8]) -> Result<usize> {
        if pos >= node.size {
            return Ok(0);
        }
        let want = core::cmp::min(out.len(), (node.size - pos) as usize);
        let mut c = node.cluster;
        if !node.contiguous {
            for _ in 0..pos / self.cluster_size() {
                c = self.fat_entry(c)?;
            }
        }
        let mut done = 0;
        let mut off = (pos % self.cluster_size()) as usize;
        while done < want {
            let n = core::cmp::min(want - done, self.cluster_size() as usize - off);
            self.dev.read_at(
                self.cluster_offset(c) + off as u64,
                &mut out[done..done + n],
            )?;
            done += n;
            off = 0;
            if done < want && !node.contiguous {
                c = self.fat_entry(c)?;
            } else if done < want {
                c = c.saturating_add(1);
            }
        }
        Ok(done)
    }
}
