//! Minimal buffer-object allocator replacing the ttm/GEM layers during
//! init: GART (system memory mapped through the GART page table) and
//! visible VRAM (CPU-accessed through the BAR0 aperture).

use alloc::{sync::Arc, vec::Vec};

use na_std::memory::DmaBuffer;
use na_std::sync::SpinLock;
use na_std::{Error, Result};

use crate::regs::Regs;

pub const GPU_PAGE_SIZE: usize = 4096;

/// AMDGPU PTE flag bits (amdgpu_vm.h).
const PTE_VALID: u64 = 1 << 0;
const PTE_SYSTEM: u64 = 1 << 1;
const PTE_SNOOPED: u64 = 1 << 2;
const PTE_EXECUTABLE: u64 = 1 << 4;
const PTE_READABLE: u64 = 1 << 5;
const PTE_WRITEABLE: u64 = 1 << 6;
const PTE_MTYPE_UC: u64 = 3 << 48;
const PTE_ADDR_MASK: u64 = 0x0000_FFFF_FFFF_F000;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Place {
    Gart,
    Vram,
}

/// A buffer object: GART BOs own a physically-contiguous DMA buffer
/// (GART PTEs map it at `gpu_addr`); VRAM BOs are accessed through the
/// BAR0 aperture at `gpu_addr` (a VRAM offset while `fb_start == 0`).
pub struct Bo {
    pub gpu_addr: u64,
    pub size: usize,
    pub place: Place,
    pub cpu: Option<DmaBuffer>,
    release: ReleasePolicy,
    retire: Arc<SpinLock<RetireQueue>>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ReleasePolicy {
    Reuse,
    Wipe,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RetireState {
    Mapped,
    AwaitingFlush,
}

struct RetiredBo {
    gpu_addr: u64,
    size: usize,
    place: Place,
    /// Keeps system-memory backing alive until the GART invalidate completes.
    cpu: Option<DmaBuffer>,
    release: ReleasePolicy,
    state: RetireState,
}

struct RetireQueue {
    pending: Vec<RetiredBo>,
    /// Every live BO owns one pre-reserved queue slot. This makes `Bo::drop`
    /// allocation-free even when it runs while handling another error.
    live: usize,
    /// Records temporarily removed by allocator maintenance retain their
    /// reserved slot until they are either requeued or fully retired.
    in_flight: usize,
}

impl RetireQueue {
    fn new() -> Self {
        Self {
            pending: Vec::new(),
            live: 0,
            in_flight: 0,
        }
    }
}

/// Pre-reserved queue capacity owned by an allocation attempt.  Abandoning
/// the attempt returns its live-object slot automatically; committing it
/// transfers that slot and the backing allocation into a `Bo`.
struct RetireReservation {
    retire: Arc<SpinLock<RetireQueue>>,
    committed: bool,
}

impl RetireReservation {
    fn reserve(retire: &Arc<SpinLock<RetireQueue>>) -> Result<Self> {
        {
            let mut queue = retire.lock();
            let required = queue
                .live
                .checked_add(queue.in_flight)
                .and_then(|value| value.checked_add(1))
                .ok_or(Error::OutOfMemory)?;
            let free = queue.pending.capacity().saturating_sub(queue.pending.len());
            if free < required {
                queue
                    .pending
                    .try_reserve(required)
                    .map_err(|_| Error::OutOfMemory)?;
            }
            queue.live += 1;
        }
        Ok(Self {
            retire: retire.clone(),
            committed: false,
        })
    }

    fn commit(mut self, gpu_addr: u64, size: usize, place: Place, cpu: Option<DmaBuffer>) -> Bo {
        self.committed = true;
        Bo {
            gpu_addr,
            size,
            place,
            cpu,
            release: ReleasePolicy::Reuse,
            retire: self.retire.clone(),
        }
    }
}

impl Drop for RetireReservation {
    fn drop(&mut self) {
        if !self.committed {
            let mut queue = self.retire.lock();
            debug_assert!(queue.live != 0);
            queue.live = queue.live.saturating_sub(1);
        }
    }
}

/// A retirement record temporarily removed from the shared queue.  Unless
/// `finish` consumes it, Drop requeues the record and restores accounting,
/// making every early-return path lossless.
struct RetiredGuard {
    retire: Arc<SpinLock<RetireQueue>>,
    record: Option<RetiredBo>,
}

impl RetiredGuard {
    fn record(&self) -> &RetiredBo {
        self.record.as_ref().expect("retirement record consumed")
    }

    fn record_mut(&mut self) -> &mut RetiredBo {
        self.record.as_mut().expect("retirement record consumed")
    }

    fn finish(mut self) {
        self.record = None;
        let mut queue = self.retire.lock();
        debug_assert!(queue.in_flight != 0);
        queue.in_flight = queue.in_flight.saturating_sub(1);
    }
}

impl Drop for RetiredGuard {
    fn drop(&mut self) {
        let Some(record) = self.record.take() else {
            return;
        };
        let mut queue = self.retire.lock();
        debug_assert!(queue.in_flight != 0);
        queue.in_flight = queue.in_flight.saturating_sub(1);
        queue.pending.push(record);
    }
}

impl RetiredBo {
    fn wipe(&mut self, regs: &mut Regs) -> Result<()> {
        if self.release != ReleasePolicy::Wipe {
            return Ok(());
        }
        match self.place {
            Place::Gart => {
                let cpu = self.cpu.as_mut().ok_or(Error::NoDevice)?;
                cpu.as_mut_slice().fill(0);
                cpu.sync_for_device();
            }
            Place::Vram => {
                let zero = [0u32; 1024];
                let mut offset = self.gpu_addr;
                for _ in 0..self.size / GPU_PAGE_SIZE {
                    regs.vram_write_dwords(offset, &zero)?;
                    offset += GPU_PAGE_SIZE as u64;
                }
            }
        }
        self.release = ReleasePolicy::Reuse;
        Ok(())
    }
}

impl Bo {
    /// Applies Linux's `AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE` policy to
    /// this ownership tree.  The final owner queues the wipe automatically.
    pub(crate) fn wipe_on_release(mut self) -> Self {
        self.release = ReleasePolicy::Wipe;
        self
    }
}

impl Drop for Bo {
    fn drop(&mut self) {
        let retired = RetiredBo {
            gpu_addr: self.gpu_addr,
            size: self.size,
            place: self.place,
            cpu: self.cpu.take(),
            release: self.release,
            state: RetireState::Mapped,
        };
        let mut queue = self.retire.lock();
        debug_assert!(queue.live != 0);
        queue.live = queue.live.saturating_sub(1);
        // Every live/in-flight BO owns pre-reserved capacity, so this push
        // cannot allocate from Drop.
        queue.pending.push(retired);
    }
}

#[derive(Clone, Copy)]
struct VramReservation {
    start: u64,
    end: u64,
}

#[derive(Clone, Copy)]
struct FreeRange {
    start: u64,
    end: u64,
}

/// GART/VRAM space manager. The GART page table itself lives at the
/// bottom of visible VRAM (allocated by `init_table`).
pub struct BoAllocator {
    retire: Arc<SpinLock<RetireQueue>>,
    pub gart_start: u64,
    pub gart_end: u64,
    pub visible_vram: u64,
    next_gart_va: u64,
    next_vram: u64,
    next_vram_top: u64,
    free_gart: Vec<FreeRange>,
    free_vram: Vec<FreeRange>,
    gart_allocated: u64,
    vram_allocated: u64,
    vram_reservations: Vec<VramReservation>,
    pub table: Option<Bo>,
    pub table_flags: u64,
    /// Zeroed dummy page (system memory) for protection faults.
    pub dummy_page_addr: u64,
    dummy_page: Option<DmaBuffer>,
    pte_generation: u64,
    flushed_generation: u64,
    gart_enabled: bool,
}

impl BoAllocator {
    pub fn new() -> Self {
        Self {
            retire: Arc::new(SpinLock::new(RetireQueue::new())),
            gart_start: 0,
            gart_end: 0,
            visible_vram: 0,
            next_gart_va: 0,
            next_vram: 0,
            next_vram_top: 0,
            free_gart: Vec::new(),
            free_vram: Vec::new(),
            gart_allocated: 0,
            vram_allocated: 0,
            vram_reservations: Vec::new(),
            table: None,
            table_flags: 0,
            dummy_page_addr: 0,
            dummy_page: None,
            pte_generation: 0,
            flushed_generation: 0,
            gart_enabled: false,
        }
    }

    /// Bytes currently consumed from the two visible-VRAM bump arenas.
    /// This includes kernel BOs, matching Linux's heap-usage semantics.
    pub fn vram_usage(&self) -> u64 {
        self.vram_allocated.min(self.visible_vram)
    }

    /// Bytes currently mapped in the driver-managed GART VA arena.
    pub fn gart_usage(&self) -> u64 {
        self.gart_allocated
    }

    /// Sets the GART and visible-VRAM ranges (called once the GMC block
    /// has computed the apertures).
    pub fn init_ranges(
        &mut self,
        gart_start: u64,
        gart_end: u64,
        visible_vram: u64,
        reserved_vram: u64,
    ) {
        self.gart_start = gart_start;
        self.gart_end = gart_end;
        self.next_gart_va = gart_start;
        self.visible_vram = visible_vram;
        self.next_vram = reserved_vram.next_multiple_of(GPU_PAGE_SIZE as u64);
        self.next_vram_top = visible_vram;
        self.free_gart.clear();
        self.free_vram.clear();
        self.gart_allocated = 0;
        self.vram_allocated = self.next_vram;
        self.vram_reservations.clear();
    }

    /// Marks an existing VRAM range unavailable to BO allocation, matching
    /// Linux `amdgpu_bo_create_kernel_at` reservations made before the PSP
    /// TMR is allocated.
    pub fn reserve_vram(&mut self, start: u64, size: u64) -> Result<()> {
        let end = start
            .checked_add(size)
            .filter(|end| *end <= self.visible_vram)
            .ok_or(Error::Range)?;
        if size == 0 || start < self.next_vram {
            return Err(Error::InvalidArgument);
        }
        self.vram_reservations
            .try_reserve(1)
            .map_err(|_| Error::OutOfMemory)?;
        self.vram_reservations.push(VramReservation { start, end });
        Ok(())
    }

    /// Initializes the GART page table in VRAM and the zero dummy page.
    pub fn init_table(&mut self, regs: &mut Regs, dummy_page: DmaBuffer) -> Result<()> {
        let num_pages = (self.gart_end - self.gart_start + 1) >> 12;
        let table_size = (num_pages * 8) as usize;
        self.table_flags = PTE_MTYPE_UC | PTE_EXECUTABLE;

        self.dummy_page_addr = dummy_page.physical_address().get();
        let table = self.alloc_vram(regs, table_size)?;
        // Fill the table with the bare PTE flags (invalid entries);
        // each PTE is 8 bytes: flags low dword, 0 high dword.
        let mut chunk = alloc::vec![0u32; 1024];
        for pair in chunk.as_chunks_mut::<2>().0 {
            pair[0] = self.table_flags as u32;
        }
        let mut pos = table.gpu_addr;
        for _ in 0..table_size / 4096 {
            regs.vram_write_dwords(pos, &chunk)?;
            pos += 4096;
        }
        self.table = Some(table);
        // The VM fault-default registers retain this physical address for the
        // lifetime of the device, so keep the allocation alive as Linux does.
        self.dummy_page = Some(dummy_page);
        Ok(())
    }

    /// Allocates a GART BO: contiguous system memory mapped at a GART
    /// virtual address through the page table.
    pub fn alloc_gart(&mut self, regs: &mut Regs, size: usize) -> Result<Bo> {
        self.alloc_gart_aligned(regs, size, GPU_PAGE_SIZE)
    }

    /// Allocates a GART BO with an explicit GPU-address alignment. PSP's
    /// primary firmware buffer is addressed in 1 MiB units and therefore
    /// must use the same 1 MiB alignment as Linux `amdgpu_bo_create_kernel`.
    pub fn alloc_gart_aligned(
        &mut self,
        regs: &mut Regs,
        size: usize,
        alignment: usize,
    ) -> Result<Bo> {
        if alignment == 0 || !alignment.is_power_of_two() {
            return Err(Error::InvalidArgument);
        }
        // Linux BOs/GTT mappings are page granular. A sub-page hardware
        // requirement such as SDMA INDIRECT's 32-byte IB alignment is
        // therefore already satisfied by the page-aligned allocation.
        let alignment = alignment.max(GPU_PAGE_SIZE);
        let size = size.next_multiple_of(GPU_PAGE_SIZE);
        let cpu = DmaBuffer::zeroed(size)?;
        let va = self.alloc_gart_va_aligned(size, alignment)?;
        let reservation = RetireReservation::reserve(&self.retire)?;
        if let Err(error) = self.map_pte(regs, va, cpu.physical_address().get(), size) {
            // A failed BAR write may have installed a prefix of the mapping.
            // Transfer the backing into the normal RAII retire path instead
            // of freeing DMA memory while a valid PTE might still reference it.
            self.gart_allocated = self.gart_allocated.saturating_add(size as u64);
            {
                let _failed_mapping = reservation.commit(va, size, Place::Gart, Some(cpu));
            }
            let _ = self.prepare_retirements(regs);
            self.complete_retirements();
            return Err(error);
        }
        self.gart_allocated = self.gart_allocated.saturating_add(size as u64);
        Ok(reservation.commit(va, size, Place::Gart, Some(cpu)))
    }

    /// Allocates a visible-VRAM BO (CPU access through the BAR0 aperture).
    pub fn alloc_vram(&mut self, regs: &mut Regs, size: usize) -> Result<Bo> {
        self.alloc_vram_aligned(regs, size, GPU_PAGE_SIZE)
    }

    /// Allocates a visible-VRAM BO with an explicit MC-address alignment.
    /// Linux gives the PSP TMR `PSP_TMR_ALIGNMENT` while preferring VRAM
    /// over GTT, so the minimal allocator needs the same placement primitive.
    pub fn alloc_vram_aligned(
        &mut self,
        regs: &mut Regs,
        size: usize,
        alignment: usize,
    ) -> Result<Bo> {
        if alignment == 0 || !alignment.is_power_of_two() {
            return Err(Error::InvalidArgument);
        }
        let alignment = alignment.max(GPU_PAGE_SIZE);
        let size = size.next_multiple_of(GPU_PAGE_SIZE);
        let reservation = RetireReservation::reserve(&self.retire)?;
        let mask = alignment as u64 - 1;
        let reused = Self::alloc_free_range(&mut self.free_vram, size as u64, alignment as u64);
        let mut bump_end = None;
        let gpu_addr = if let Some(gpu_addr) = reused {
            gpu_addr
        } else {
            let mut gpu_addr = self
                .next_vram
                .checked_add(mask)
                .map(|value| value & !mask)
                .ok_or(Error::OutOfMemory)?;
            let end = loop {
                let end = gpu_addr
                    .checked_add(size as u64)
                    .filter(|end| *end <= self.next_vram_top)
                    .ok_or(Error::OutOfMemory)?;
                let next = self
                    .vram_reservations
                    .iter()
                    .filter(|reserved| gpu_addr < reserved.end && end > reserved.start)
                    .map(|reserved| reserved.end)
                    .max();
                match next {
                    Some(next) => {
                        gpu_addr = next
                            .checked_add(mask)
                            .map(|value| value & !mask)
                            .ok_or(Error::OutOfMemory)?;
                    }
                    None => break end,
                }
            };
            bump_end = Some(end);
            gpu_addr
        };
        // Zero through the aperture (aperture-backed offsets only).
        let zero = [0u32; 1024];
        let mut pos = gpu_addr;
        for _ in 0..size / 4096 {
            if let Err(error) = regs.vram_write_dwords(pos, &zero) {
                if reused.is_some() {
                    Self::free_range(&mut self.free_vram, gpu_addr, size as u64);
                }
                return Err(error);
            }
            pos += 4096;
        }
        if let Some(end) = bump_end {
            self.next_vram = end;
        }
        self.vram_allocated = self.vram_allocated.saturating_add(size as u64);
        Ok(reservation.commit(gpu_addr, size, Place::Vram, None))
    }

    /// Allocates a no-CPU-access VRAM BO from the top of VRAM, matching
    /// TTM's `TTM_PL_FLAG_TOPDOWN` placement.  Linux creates the PSP TMR with
    /// a null CPU mapping, so its address must come from this allocator.
    pub fn alloc_vram_top_down_aligned(
        &mut self,
        regs: &mut Regs,
        size: usize,
        alignment: usize,
    ) -> Result<Bo> {
        if alignment == 0 || !alignment.is_power_of_two() {
            return Err(Error::InvalidArgument);
        }
        let alignment = alignment.max(GPU_PAGE_SIZE);
        let size = size.next_multiple_of(GPU_PAGE_SIZE);
        let reservation = RetireReservation::reserve(&self.retire)?;
        let mask = alignment as u64 - 1;
        let mut ceiling = self.next_vram_top;
        let gpu_addr = loop {
            let gpu_addr = ceiling
                .checked_sub(size as u64)
                .map(|value| value & !mask)
                .filter(|value| *value >= self.next_vram)
                .ok_or(Error::OutOfMemory)?;
            let end = gpu_addr
                .checked_add(size as u64)
                .ok_or(Error::OutOfMemory)?;
            let next_ceiling = self
                .vram_reservations
                .iter()
                .filter(|reserved| gpu_addr < reserved.end && end > reserved.start)
                .map(|reserved| reserved.start)
                .min();
            match next_ceiling {
                Some(next) => ceiling = next,
                None => break gpu_addr,
            }
        };
        let zero = [0u32; 1024];
        let mut pos = gpu_addr;
        for _ in 0..size / GPU_PAGE_SIZE {
            regs.vram_write_dwords(pos, &zero)?;
            pos += GPU_PAGE_SIZE as u64;
        }
        self.next_vram_top = gpu_addr;
        self.vram_allocated = self.vram_allocated.saturating_add(size as u64);
        Ok(reservation.commit(gpu_addr, size, Place::Vram, None))
    }

    /// Maps `size` bytes of physically-contiguous memory at `va`.
    pub fn map_pte(&mut self, regs: &mut Regs, va: u64, pa: u64, size: usize) -> Result<()> {
        let flags =
            PTE_VALID | PTE_SYSTEM | PTE_SNOOPED | PTE_READABLE | PTE_WRITEABLE | self.table_flags;
        let mut pos = va;
        let mut pa = pa;
        for _ in 0..size / GPU_PAGE_SIZE {
            self.write_pte(regs, pos, pa, flags)?;
            pos += GPU_PAGE_SIZE as u64;
            pa += GPU_PAGE_SIZE as u64;
        }
        self.pte_generation = self.pte_generation.wrapping_add(1);
        Ok(())
    }

    /// Applies ownership-driven BO retirement. GART backing remains owned by
    /// the retire record until its PTEs are invalidated and a later TLB flush
    /// completes; VRAM ranges can be reclaimed immediately.
    pub fn prepare_retirements(&mut self, regs: &mut Regs) -> Result<()> {
        let count = self.retire.lock().pending.len();
        for _ in 0..count {
            let mut retired = self.take_retired().ok_or(Error::NoDevice)?;
            retired.record_mut().wipe(regs)?;
            match retired.record().place {
                Place::Vram => {
                    let record = retired.record();
                    Self::free_range(&mut self.free_vram, record.gpu_addr, record.size as u64);
                    self.vram_allocated = self.vram_allocated.saturating_sub(record.size as u64);
                    retired.finish();
                }
                Place::Gart if retired.record().state == RetireState::Mapped => {
                    let mut pos = retired.record().gpu_addr;
                    let mut result = Ok(());
                    for _ in 0..retired.record().size / GPU_PAGE_SIZE {
                        if let Err(error) = self.write_pte(regs, pos, 0, self.table_flags) {
                            result = Err(error);
                            break;
                        }
                        pos += GPU_PAGE_SIZE as u64;
                    }
                    result?;
                    self.pte_generation = self.pte_generation.wrapping_add(1);
                    retired.record_mut().state = RetireState::AwaitingFlush;
                }
                Place::Gart => {}
            }
        }
        Ok(())
    }

    /// Completes records made inaccessible by the most recent successful
    /// GART invalidate. Dropping a record here finally frees its DMA backing.
    pub fn complete_retirements(&mut self) {
        if self.gart_enabled && self.flushed_generation != self.pte_generation {
            return;
        }
        let count = self.retire.lock().pending.len();
        for _ in 0..count {
            let retired = match self.take_retired() {
                Some(retired) => retired,
                None => break,
            };
            if retired.record().place == Place::Gart
                && retired.record().state == RetireState::AwaitingFlush
            {
                let record = retired.record();
                Self::free_range(&mut self.free_gart, record.gpu_addr, record.size as u64);
                self.gart_allocated = self.gart_allocated.saturating_sub(record.size as u64);
                retired.finish();
            }
        }
    }

    fn take_retired(&self) -> Option<RetiredGuard> {
        let mut queue = self.retire.lock();
        let record = queue.pending.pop()?;
        queue.in_flight += 1;
        Some(RetiredGuard {
            retire: self.retire.clone(),
            record: Some(record),
        })
    }

    fn write_pte(&mut self, regs: &mut Regs, va: u64, pa: u64, flags: u64) -> Result<()> {
        let table = self.table.as_ref().ok_or(Error::NoDevice)?;
        let index = (va - self.gart_start) >> 12;
        let value = (pa & PTE_ADDR_MASK) | flags;
        regs.vram_write_dwords(
            table.gpu_addr + index * 8,
            &[value as u32, (value >> 32) as u32],
        )
    }

    /// Reads a GART PTE back from the VRAM page table. This is used to
    /// verify the exact mapping consumed by PSP before its first command.
    pub fn read_pte(&self, regs: &mut Regs, va: u64) -> Result<u64> {
        if va < self.gart_start || va > self.gart_end {
            return Err(Error::Range);
        }
        let table = self.table.as_ref().ok_or(Error::NoDevice)?;
        let index = (va - self.gart_start) >> 12;
        let mut words = [0u32; 2];
        regs.vram_read_dwords(table.gpu_addr + index * 8, &mut words)?;
        Ok(words[0] as u64 | ((words[1] as u64) << 32))
    }

    pub fn expected_system_pte(&self, pa: u64) -> u64 {
        let flags =
            PTE_VALID | PTE_SYSTEM | PTE_SNOOPED | PTE_READABLE | PTE_WRITEABLE | self.table_flags;
        (pa & PTE_ADDR_MASK) | flags
    }

    pub fn mark_gart_enabled_and_flushed(&mut self) {
        self.gart_enabled = true;
        self.flushed_generation = self.pte_generation;
    }

    pub fn needs_gart_flush(&self) -> bool {
        self.gart_enabled && self.flushed_generation != self.pte_generation
    }

    pub fn mark_gart_flushed(&mut self) {
        self.flushed_generation = self.pte_generation;
    }

    fn alloc_gart_va_aligned(&mut self, size: usize, alignment: usize) -> Result<u64> {
        if let Some(va) = Self::alloc_free_range(&mut self.free_gart, size as u64, alignment as u64)
        {
            return Ok(va);
        }
        let va = self
            .next_gart_va
            .checked_add(alignment as u64 - 1)
            .map(|value| value & !(alignment as u64 - 1))
            .ok_or(Error::OutOfMemory)?;
        let end = va
            .checked_add(size as u64)
            .filter(|e| *e <= self.gart_end)
            .ok_or(Error::OutOfMemory)?;
        self.next_gart_va = end;
        Ok(va)
    }

    fn alloc_free_range(ranges: &mut Vec<FreeRange>, size: u64, alignment: u64) -> Option<u64> {
        let mut selected = None;
        for (index, range) in ranges.iter().copied().enumerate() {
            let start = range.start.checked_add(alignment - 1)? & !(alignment - 1);
            let end = start.checked_add(size)?;
            if end <= range.end {
                selected = Some((index, range, start, end));
                break;
            }
        }
        let (index, range, start, end) = selected?;
        ranges.remove(index);
        if end < range.end {
            ranges.insert(
                index,
                FreeRange {
                    start: end,
                    end: range.end,
                },
            );
        }
        if range.start < start {
            ranges.insert(
                index,
                FreeRange {
                    start: range.start,
                    end: start,
                },
            );
        }
        Some(start)
    }

    fn free_range(ranges: &mut Vec<FreeRange>, start: u64, size: u64) {
        let Some(end) = start.checked_add(size) else {
            return;
        };
        ranges.push(FreeRange { start, end });
        ranges.sort_unstable_by_key(|range| range.start);
        let mut index = 0;
        while index + 1 < ranges.len() {
            if ranges[index].end >= ranges[index + 1].start {
                ranges[index].end = ranges[index].end.max(ranges[index + 1].end);
                ranges.remove(index + 1);
            } else {
                index += 1;
            }
        }
    }
}

impl Default for BoAllocator {
    fn default() -> Self {
        Self::new()
    }
}

/// Writeback scratch: 128 slots of 32 bytes each (Linux `AMDGPU_MAX_WB`).
pub struct Wb {
    pub bo: Bo,
    next_slot: usize,
}

impl Wb {
    pub fn new(bo: Bo) -> Self {
        Self { bo, next_slot: 0 }
    }

    /// Allocates a new writeback slot; returns its GPU address.
    pub fn get(&mut self) -> Result<u64> {
        let slot = self.next_slot;
        if slot >= 128 {
            return Err(Error::OutOfMemory);
        }
        self.next_slot += 1;
        Ok(self.bo.gpu_addr + (slot * 32) as u64)
    }

    /// Updates a ring write-pointer slot by GPU address, matching Linux's
    /// `atomic64_set(ring->wptr_cpu_addr, value)` before ringing a 64-bit
    /// doorbell.
    pub fn write_u64(&mut self, gpu_addr: u64, value: u64) -> Result<()> {
        let offset = gpu_addr
            .checked_sub(self.bo.gpu_addr)
            .and_then(|offset| usize::try_from(offset).ok())
            .ok_or(Error::Range)?;
        let cpu = self.bo.cpu.as_mut().ok_or(Error::NoDevice)?;
        let dst = cpu
            .as_mut_slice()
            .get_mut(offset..offset + core::mem::size_of::<u64>())
            .ok_or(Error::Range)?;
        dst.copy_from_slice(&value.to_le_bytes());
        cpu.sync_for_device();
        Ok(())
    }

    /// Updates a ring write-pointer slot by GPU address for engines whose
    /// Linux set_wptr callback uses a 32-bit writeback value.
    pub fn write_u32(&mut self, gpu_addr: u64, value: u32) -> Result<()> {
        let offset = gpu_addr
            .checked_sub(self.bo.gpu_addr)
            .and_then(|offset| usize::try_from(offset).ok())
            .ok_or(Error::Range)?;
        let cpu = self.bo.cpu.as_mut().ok_or(Error::NoDevice)?;
        let dst = cpu
            .as_mut_slice()
            .get_mut(offset..offset + core::mem::size_of::<u32>())
            .ok_or(Error::Range)?;
        dst.copy_from_slice(&value.to_le_bytes());
        cpu.sync_for_device();
        Ok(())
    }

    pub fn read_u64(&self, gpu_addr: u64) -> Result<u64> {
        let offset = gpu_addr
            .checked_sub(self.bo.gpu_addr)
            .and_then(|offset| usize::try_from(offset).ok())
            .ok_or(Error::Range)?;
        let cpu = self.bo.cpu.as_ref().ok_or(Error::NoDevice)?;
        // The writeback BO is DMA coherent on ASTRA's x86_64 platform. Linux
        // polls these scheduler fences with READ_ONCE rather than invalidating
        // the complete WB allocation for each observation.
        cpu.read_volatile_u64(offset)
    }

    /// Stable CPU virtual address corresponding to a GPU writeback address.
    /// The writeback BO lives for the adapter lifetime and is DMA coherent on
    /// the x86_64 platform supported by ASTRA.
    pub fn cpu_address(&self, gpu_addr: u64) -> Result<u64> {
        let offset = gpu_addr
            .checked_sub(self.bo.gpu_addr)
            .and_then(|offset| usize::try_from(offset).ok())
            .ok_or(Error::Range)?;
        let cpu = self.bo.cpu.as_ref().ok_or(Error::NoDevice)?;
        if offset
            .checked_add(core::mem::size_of::<u64>())
            .filter(|end| *end <= cpu.length())
            .is_none()
        {
            return Err(Error::Range);
        }
        Ok(cpu.address().checked_add(offset).ok_or(Error::Range)? as u64)
    }
}
