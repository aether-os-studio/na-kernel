//! AMDGPU private ioctl implementation. INFO queries are stateless; GEM
//! handles, mmap offsets, metadata and GPU-VA mappings follow Linux's
//! per-`drm_file` ownership model.

use alloc::{sync::Arc, vec::Vec};

use na_std::drm::{FileId, PrimeBuffer};
use na_std::memory::PhysicalAddress;
use na_std::sync::Mutex;
use na_std::user::UserAddress;
use na_std::{Error, Result, time};

use crate::blocks;
use crate::device::Adapter;
use crate::firmware::UcodeId;
use crate::ip::{HwIp, IpVersion};
use crate::mem::{Bo, GPU_PAGE_SIZE, Place};
use crate::regs::gc10_3_0 as gc;
use crate::uapi::{self, DeviceInfo, HeapInfo, HwIpInfo, InfoRequest, MemoryInfo};

const GPU_COUNTER_FREQ_KHZ: u32 = 100_000;
const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
const VA_RESERVED_BOTTOM: u64 = 1 << 16;
const VA_RESERVED_TOP: u64 = (1 << 16) + (2 << 20) + (2 << 20);
// ASTRA currently programs 3 * 9 page-table index bits plus the 12-bit
// page offset. Report the implemented 39-bit VM range until per-file VM
// roots are introduced in phase 3.
const VM_SIZE: u64 = 1 << 39;
const GEM_MMAP_OFFSET_BASE: u64 = 0x20_0000_0000;
// Linux uses idr_alloc(..., 1, AMDGPU_VM_MAX_NUM_CTX, ...), so 4096 is an
// exclusive upper bound and context IDs 1..=4095 are available per file.
const MAX_CONTEXT_ID: u32 = 4095;
const MAX_BO_LIST_ENTRIES: usize = 1 << 20;
const MAX_CS_CHUNKS: usize = 64;
pub const MAX_USER_VMIDS: u32 = 15;

const PTE_VALID: u64 = 1 << 0;
const PTE_SYSTEM: u64 = 1 << 1;
const PTE_SNOOPED: u64 = 1 << 2;
const PTE_EXECUTABLE: u64 = 1 << 4;
const PTE_READABLE: u64 = 1 << 5;
const PTE_WRITEABLE: u64 = 1 << 6;
const PTE_MTYPE_SHIFT: u64 = 48;
const PTE_NOALLOC: u64 = 1 << 58;
const PDE_PTE: u64 = 1 << 54;
const PTE_ADDRESS_MASK: u64 = 0x0000_ffff_ffff_f000;
const VRAM_TABLE_FLAGS: u64 = PTE_VALID;

fn mapping_pte_flags(vm_flags: u32, bo_flags: u64, place: Place) -> u64 {
    let mut flags = PTE_VALID;
    if place == Place::Gart {
        flags |= PTE_SYSTEM | PTE_SNOOPED;
    }
    if vm_flags & uapi::AMDGPU_VM_PAGE_READABLE != 0 {
        flags |= PTE_READABLE;
    }
    if vm_flags & uapi::AMDGPU_VM_PAGE_WRITEABLE != 0 {
        flags |= PTE_WRITEABLE;
    }
    if vm_flags & uapi::AMDGPU_VM_PAGE_EXECUTABLE != 0 {
        flags |= PTE_EXECUTABLE;
    }
    if vm_flags & uapi::AMDGPU_VM_PAGE_NOALLOC != 0 {
        flags |= PTE_NOALLOC;
    }

    // Linux gmc_v10_0_get_vm_pte(): DEFAULT/NC and unknown encodings are
    // non-coherent, while WC/CC/UC select Navi10 MTYPE values 1/2/3.
    let mtype = match vm_flags & uapi::AMDGPU_VM_MTYPE_MASK {
        uapi::AMDGPU_VM_MTYPE_WC => 1,
        uapi::AMDGPU_VM_MTYPE_CC => 2,
        uapi::AMDGPU_VM_MTYPE_UC => 3,
        _ => 0,
    };
    flags |= mtype << PTE_MTYPE_SHIFT;
    if bo_flags
        & (uapi::AMDGPU_GEM_CREATE_COHERENT
            | uapi::AMDGPU_GEM_CREATE_EXT_COHERENT
            | uapi::AMDGPU_GEM_CREATE_UNCACHED)
        != 0
    {
        flags &= !(7 << PTE_MTYPE_SHIFT);
        flags |= 3 << PTE_MTYPE_SHIFT;
    }
    flags
}

pub struct FileState {
    file: FileId,
    vm: UserVm,
    next_handle: u32,
    next_mmap_offset: u64,
    next_context_id: u32,
    next_bo_list_handle: u32,
    next_sequence: u64,
    buffers: Vec<UserBuffer>,
    mappings: Vec<VaMapping>,
    contexts: Vec<ContextState>,
    bo_lists: Vec<BoList>,
    submissions: Vec<Submission>,
}

struct UserVm {
    vmid: u32,
    root: Bo,
    pdb1: Bo,
    pdb0: Vec<Pdb0Table>,
    ptb: Vec<PtbTable>,
}

struct Pdb0Table {
    pdb1_index: u16,
    page: Bo,
}

struct PtbTable {
    pdb1_index: u16,
    pdb0_index: u16,
    page: Bo,
}

struct BoList {
    handle: u32,
    buffers: Vec<BoListEntry>,
}

struct BoListEntry {
    handle: u32,
    object: SharedObject,
}

struct Submission {
    sequence: u64,
    context_id: u32,
    ip_type: u32,
    ip_instance: u32,
    ring: u32,
    fence: crate::ip::CompletionFence,
    /// Scheduler-job ownership: referenced BOs remain alive until the
    /// hardware fence signals, then normal `Arc`/`Drop` retirement applies.
    objects: Vec<SharedObject>,
}

pub struct PrimeState {
    next_token: u64,
    exports: Vec<PrimeExport>,
}

pub struct FramebufferState {
    next_handle: u32,
    buffers: Vec<FramebufferRef>,
}

struct PrimeExport {
    token: u64,
    object: SharedObject,
}

struct FramebufferRef {
    handle: u32,
    object: SharedObject,
    width: u32,
    height: u32,
    pitch: u32,
    offset: u32,
    format: u32,
    flags: u32,
    modifier: u64,
    meta_pitch: u32,
    meta_offset: u32,
    tiling_info: u64,
}

struct ContextState {
    id: u32,
    priority: i32,
}

struct UserBuffer {
    handle: u32,
    object: SharedObject,
    mmap_offset: u64,
}

type SharedObject = Arc<Mutex<UserObject>>;

/// Reference held while DCN scans a userspace cursor BO.  Keeping the GEM
/// object in this RAII wrapper mirrors the framebuffer reference retained by
/// Linux's cursor plane state: closing the userspace handle cannot retire the
/// backing BO until the hardware cursor has been replaced or disabled.
pub(crate) struct CursorBuffer {
    _object: SharedObject,
    gpu_address: u64,
    pitch_pixels: u32,
}

/// GEM reference retained while HUBP scans out a KMS framebuffer.
pub(crate) struct ScanoutBuffer {
    _object: SharedObject,
    gpu_address: u64,
    pub(crate) meta_address: Option<u64>,
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) pitch: u32,
    pub(crate) format: u32,
    pub(crate) swizzle: u32,
    pub(crate) num_pipes: u32,
    pub(crate) pipe_interleave: u32,
    pub(crate) max_compressed_frags: u32,
    pub(crate) num_pkrs: u32,
    pub(crate) meta_pitch: u32,
    pub(crate) dcc_independent_block: u32,
}

impl ScanoutBuffer {
    pub(crate) const fn gpu_address(&self) -> u64 {
        self.gpu_address
    }

    pub(crate) fn has_same_layout(&self, other: &Self) -> bool {
        self.width == other.width
            && self.height == other.height
            && self.pitch == other.pitch
            && self.format == other.format
            && self.swizzle == other.swizzle
            && self.num_pipes == other.num_pipes
            && self.pipe_interleave == other.pipe_interleave
            && self.max_compressed_frags == other.max_compressed_frags
            && self.num_pkrs == other.num_pkrs
            && self.meta_pitch == other.meta_pitch
            && self.dcc_independent_block == other.dcc_independent_block
            && self.meta_address.is_some() == other.meta_address.is_some()
    }
}

impl CursorBuffer {
    pub(crate) const fn gpu_address(&self) -> u64 {
        self.gpu_address
    }

    pub(crate) const fn pitch_pixels(&self) -> u32 {
        self.pitch_pixels
    }
}

struct UserObject {
    bo: Bo,
    alignment: u64,
    preferred_domains: u64,
    actual_domain: u64,
    flags: u64,
    metadata_flags: u64,
    tiling_info: u64,
    metadata: Vec<u8>,
}

fn fence_completed(adapter: &mut Adapter, fence: crate::ip::CompletionFence) -> Result<bool> {
    Ok(adapter
        .wb
        .as_mut()
        .ok_or(Error::NoDevice)?
        .read_u64(fence.gpu_address)?
        >= fence.value)
}

fn wait_fence(
    adapter: &mut Adapter,
    fence: crate::ip::CompletionFence,
    timeout: u64,
) -> Result<bool> {
    loop {
        if fence_completed(adapter, fence)? {
            return Ok(true);
        }
        if timeout != u64::MAX && time::monotonic().as_nanos() >= timeout as u128 {
            return Ok(false);
        }
        time::delay(core::time::Duration::from_micros(1));
    }
}

fn reap_submissions(adapter: &mut Adapter, file: &mut FileState) -> Result<()> {
    let mut retired_bo = false;
    let mut index = 0;
    while index < file.submissions.len() {
        if fence_completed(adapter, file.submissions[index].fence)? {
            retired_bo |= file.submissions[index]
                .objects
                .iter()
                .any(|object| Arc::strong_count(object) == 1);
            file.submissions.remove(index);
        } else {
            index += 1;
        }
    }
    if retired_bo {
        blocks::flush_pending_gart(adapter)?;
    }
    Ok(())
}

fn wait_all_submissions(adapter: &mut Adapter, file: &mut FileState) -> Result<()> {
    for submission in &file.submissions {
        let _ = wait_fence(adapter, submission.fence, u64::MAX)?;
    }
    reap_submissions(adapter, file)
}

#[derive(Clone, Copy, Debug)]
struct VaMapping {
    handle: u32,
    address: u64,
    offset: u64,
    size: u64,
    flags: u32,
}

impl FileState {
    pub fn new(adapter: &mut Adapter, file: FileId, vmid: u32) -> Result<Self> {
        Ok(Self {
            file,
            vm: UserVm::new(adapter, vmid)?,
            next_handle: 1,
            next_mmap_offset: GEM_MMAP_OFFSET_BASE,
            next_context_id: 1,
            next_bo_list_handle: 1,
            next_sequence: 1,
            buffers: Vec::new(),
            mappings: Vec::new(),
            contexts: Vec::new(),
            bo_lists: Vec::new(),
            submissions: Vec::new(),
        })
    }

    pub fn belongs_to(&self, file: FileId) -> bool {
        self.file == file
    }

    pub fn vmid(&self) -> u32 {
        self.vm.vmid
    }

    fn buffer(&self, handle: u32) -> Result<&UserBuffer> {
        self.buffers
            .iter()
            .find(|buffer| buffer.handle == handle)
            .ok_or(Error::NotFound)
    }

    fn allocate_handle(&mut self) -> Result<u32> {
        let handle = self.next_handle;
        if handle == 0 {
            return Err(Error::NoSpace);
        }
        self.next_handle = handle.checked_add(1).ok_or(Error::NoSpace)?;
        Ok(handle)
    }

    fn allocate_mmap_offset(&mut self, size: usize) -> Result<u64> {
        let offset = self.next_mmap_offset;
        self.next_mmap_offset = offset
            .checked_add(size.next_multiple_of(GPU_PAGE_SIZE) as u64)
            .ok_or(Error::NoSpace)?;
        Ok(offset)
    }

    fn context(&self, id: u32) -> Result<&ContextState> {
        self.contexts
            .iter()
            .find(|context| context.id == id)
            .ok_or(Error::InvalidArgument)
    }

    fn allocate_context_id(&mut self) -> Result<u32> {
        let mut candidate = self.next_context_id.max(1);
        for _ in 0..MAX_CONTEXT_ID {
            if !self.contexts.iter().any(|context| context.id == candidate) {
                self.next_context_id = if candidate == MAX_CONTEXT_ID {
                    1
                } else {
                    candidate + 1
                };
                return Ok(candidate);
            }
            candidate = if candidate == MAX_CONTEXT_ID {
                1
            } else {
                candidate + 1
            };
        }
        Err(Error::NoSpace)
    }

    fn allocate_bo_list_handle(&mut self) -> Result<u32> {
        let start = self.next_bo_list_handle.max(1);
        let mut candidate = start;
        loop {
            if !self.bo_lists.iter().any(|list| list.handle == candidate) {
                self.next_bo_list_handle = candidate.checked_add(1).unwrap_or(1);
                return Ok(candidate);
            }
            candidate = candidate.checked_add(1).unwrap_or(1);
            if candidate == start {
                return Err(Error::NoSpace);
            }
        }
    }

    fn allocate_sequence(&mut self) -> Result<u64> {
        let sequence = self.next_sequence;
        if sequence == 0 {
            return Err(Error::NoSpace);
        }
        self.next_sequence = sequence.checked_add(1).ok_or(Error::NoSpace)?;
        Ok(sequence)
    }
}

impl UserVm {
    fn alloc_table(adapter: &mut Adapter, clear_flags: u64) -> Result<Bo> {
        let table =
            adapter
                .mem
                .alloc_vram_aligned(&mut adapter.regs, GPU_PAGE_SIZE, GPU_PAGE_SIZE)?;
        let dst = Self::table_gpu_addr(adapter, &table)?;
        // Linux amdgpu_vm_pt_clear() uses the selected VM update backend for
        // every newly allocated table. Navi23's gmc_v10_0 leaves
        // translate_further disabled, so every non-PTB level is initialized
        // with PDE_PTE and PTBs carry EXECUTABLE for fault priority.
        blocks::update_vm_table(adapter, dst, 0, 512, 0, clear_flags)?;
        Ok(table)
    }

    /// Linux `amdgpu_bo_gpu_offset_no_check`: address used by SDMA VMID0
    /// when writing the VRAM-resident page table.
    fn table_gpu_addr(adapter: &Adapter, table: &Bo) -> Result<u64> {
        if table.place != Place::Vram {
            return Err(Error::InvalidArgument);
        }
        adapter
            .gmc
            .vram_start
            .checked_add(table.gpu_addr)
            .ok_or(Error::Range)
    }

    /// Linux `amdgpu_gmc_pd_addr` / `amdgpu_gmc_vram_mc2pa`: physical VRAM
    /// address stored in PDEs and VM context root registers.
    fn table_pa(adapter: &Adapter, table: &Bo) -> Result<u64> {
        if table.place != Place::Vram {
            return Err(Error::InvalidArgument);
        }
        adapter
            .gmc
            .vram_base_offset
            .checked_add(table.gpu_addr)
            .map(|address| address & PTE_ADDRESS_MASK)
            .ok_or(Error::Range)
    }

    fn new(adapter: &mut Adapter, vmid: u32) -> Result<Self> {
        if vmid == 0 || vmid > MAX_USER_VMIDS {
            return Err(Error::InvalidArgument);
        }
        // Linux amdgpu_vm_pt_create() places page directories/page tables in
        // VRAM on discrete GPUs.  System-memory tables are the APU fallback;
        // using them here made the CPF walker fault despite valid CPU-side
        // entries.
        let root = Self::alloc_table(adapter, PDE_PTE)?;
        let pdb1 = Self::alloc_table(adapter, PDE_PTE)?;
        let value = Self::table_pa(adapter, &pdb1)? | VRAM_TABLE_FLAGS;
        Self::write_entry(adapter, &root, 0, value)?;
        Ok(Self {
            vmid,
            root,
            pdb1,
            pdb0: Vec::new(),
            ptb: Vec::new(),
        })
    }

    fn root_pde(&self, adapter: &Adapter) -> Result<u64> {
        Ok(Self::table_pa(adapter, &self.root)? | VRAM_TABLE_FLAGS)
    }

    fn write_entry(adapter: &mut Adapter, table: &Bo, index: usize, value: u64) -> Result<()> {
        let at = index.checked_mul(8).ok_or(Error::Range)?;
        if at + 8 > table.size {
            return Err(Error::Range);
        }
        let dst = Self::table_gpu_addr(adapter, table)?
            .checked_add(at as u64)
            .ok_or(Error::Range)?;
        blocks::update_vm_table(adapter, dst, value, 1, 0, 0)
    }

    fn ensure_ptb(
        &mut self,
        adapter: &mut Adapter,
        pdb1_index: u16,
        pdb0_index: u16,
    ) -> Result<usize> {
        if let Some(index) = self
            .ptb
            .iter()
            .position(|table| table.pdb1_index == pdb1_index && table.pdb0_index == pdb0_index)
        {
            return Ok(index);
        }

        let pdb0_pos = if let Some(index) = self
            .pdb0
            .iter()
            .position(|table| table.pdb1_index == pdb1_index)
        {
            index
        } else {
            self.pdb0.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
            let page = Self::alloc_table(adapter, PDE_PTE)?;
            // gmc_v10_0_get_vm_pde() does not add BFS on Navi23 because
            // translate_further is false. A VRAM child PDE is address+VALID.
            let value = Self::table_pa(adapter, &page)? | VRAM_TABLE_FLAGS;
            Self::write_entry(adapter, &self.pdb1, pdb1_index as usize, value)?;
            self.pdb0.push(Pdb0Table { pdb1_index, page });
            self.pdb0.len() - 1
        };

        self.ptb.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
        let page = Self::alloc_table(adapter, PTE_EXECUTABLE)?;
        // The PDB0 -> PTB PDE is likewise address+VALID. PTE_TF is only
        // added by gmc_v10_0_get_vm_pde() when translate_further is enabled.
        let value = Self::table_pa(adapter, &page)? | VRAM_TABLE_FLAGS;
        Self::write_entry(
            adapter,
            &self.pdb0[pdb0_pos].page,
            pdb0_index as usize,
            value,
        )?;
        self.ptb.push(PtbTable {
            pdb1_index,
            pdb0_index,
            page,
        });
        Ok(self.ptb.len() - 1)
    }

    fn indices(va: u64) -> (u16, u16, usize) {
        (
            ((va >> 30) & 0x1ff) as u16,
            ((va >> 21) & 0x1ff) as u16,
            ((va >> 12) & 0x1ff) as usize,
        )
    }

    fn map_range(
        &mut self,
        adapter: &mut Adapter,
        va: u64,
        physical: u64,
        size: u64,
        flags: u64,
    ) -> Result<()> {
        let mut done = 0u64;
        while done < size {
            let current_va = va.checked_add(done).ok_or(Error::Range)?;
            let (pdb1_index, pdb0_index, pte_index) = Self::indices(current_va);
            let ptb = self.ensure_ptb(adapter, pdb1_index, pdb0_index)?;
            let remaining_pages = (size - done) / GPU_PAGE_SIZE as u64;
            let count = remaining_pages
                .min((512 - pte_index) as u64)
                .try_into()
                .map_err(|_| Error::Range)?;
            let dst = Self::table_gpu_addr(adapter, &self.ptb[ptb].page)?
                .checked_add((pte_index * 8) as u64)
                .ok_or(Error::Range)?;
            blocks::update_vm_table(
                adapter,
                dst,
                physical.checked_add(done).ok_or(Error::Range)?,
                count,
                GPU_PAGE_SIZE as u32,
                flags,
            )?;
            done = done
                .checked_add(count as u64 * GPU_PAGE_SIZE as u64)
                .ok_or(Error::Range)?;
        }
        Ok(())
    }

    fn clear_range(&mut self, adapter: &mut Adapter, va: u64, size: u64) -> Result<()> {
        let mut done = 0u64;
        while done < size {
            let current_va = va.checked_add(done).ok_or(Error::Range)?;
            let (pdb1_index, pdb0_index, pte_index) = Self::indices(current_va);
            let remaining_pages = (size - done) / GPU_PAGE_SIZE as u64;
            let count: u32 = remaining_pages
                .min((512 - pte_index) as u64)
                .try_into()
                .map_err(|_| Error::Range)?;
            if let Some(ptb) = self
                .ptb
                .iter()
                .find(|table| table.pdb1_index == pdb1_index && table.pdb0_index == pdb0_index)
            {
                let dst = Self::table_gpu_addr(adapter, &ptb.page)?
                    .checked_add((pte_index * 8) as u64)
                    .ok_or(Error::Range)?;
                blocks::update_vm_table(adapter, dst, 0, count, 0, PTE_EXECUTABLE)?;
            }
            done = done
                .checked_add(count as u64 * GPU_PAGE_SIZE as u64)
                .ok_or(Error::Range)?;
        }
        Ok(())
    }
}

impl PrimeState {
    pub fn new() -> Self {
        Self {
            next_token: 1,
            exports: Vec::new(),
        }
    }

    fn allocate_token(&mut self) -> Result<u64> {
        let token = self.next_token;
        if token == 0 {
            return Err(Error::NoSpace);
        }
        self.next_token = token.checked_add(1).ok_or(Error::NoSpace)?;
        Ok(token)
    }
}

impl FramebufferState {
    pub fn new() -> Self {
        Self {
            next_handle: 0x8000_0000,
            buffers: Vec::new(),
        }
    }

    fn allocate_handle(&mut self) -> Result<u32> {
        let handle = self.next_handle;
        if handle == 0 {
            return Err(Error::NoSpace);
        }
        self.next_handle = handle.checked_add(1).ok_or(Error::NoSpace)?;
        Ok(handle)
    }
}

fn sanitize_context_priority(priority: i32) -> i32 {
    match priority {
        uapi::AMDGPU_CTX_PRIORITY_VERY_LOW
        | uapi::AMDGPU_CTX_PRIORITY_LOW
        | uapi::AMDGPU_CTX_PRIORITY_NORMAL
        | uapi::AMDGPU_CTX_PRIORITY_HIGH
        | uapi::AMDGPU_CTX_PRIORITY_VERY_HIGH => priority,
        // Linux deliberately maps all other values, including UNSET, to
        // NORMAL for backwards compatibility.
        _ => uapi::AMDGPU_CTX_PRIORITY_NORMAL,
    }
}

pub fn context(file: &mut FileState, bytes: &mut [u8]) -> Result<(u32, u32)> {
    let request = uapi::ContextRequest::parse(bytes)?;
    match request.operation {
        uapi::AMDGPU_CTX_OP_ALLOC_CTX => {
            if request.flags != 0 {
                return Err(Error::InvalidArgument);
            }
            file.contexts
                .try_reserve(1)
                .map_err(|_| Error::OutOfMemory)?;
            let id = file.allocate_context_id()?;
            file.contexts.push(ContextState {
                id,
                priority: sanitize_context_priority(request.priority),
            });
            bytes.fill(0);
            uapi::put_u32(bytes, 0, id);
            Ok((request.operation, id))
        }
        uapi::AMDGPU_CTX_OP_FREE_CTX => {
            if request.flags != 0 {
                return Err(Error::InvalidArgument);
            }
            let index = file
                .contexts
                .iter()
                .position(|context| context.id == request.context_id)
                .ok_or(Error::InvalidArgument)?;
            file.contexts.remove(index);
            bytes.fill(0);
            Ok((request.operation, request.context_id))
        }
        uapi::AMDGPU_CTX_OP_QUERY_STATE | uapi::AMDGPU_CTX_OP_QUERY_STATE2 => {
            if request.flags != 0 {
                return Err(Error::InvalidArgument);
            }
            let context = file.context(request.context_id)?;
            let _priority = context.priority;
            // ASTRA has not observed a reset, VRAM loss, guilty submission,
            // or RAS event for this context. The QUERY_STATE reset_status
            // field is therefore AMDGPU_CTX_NO_RESET (zero), and QUERY_STATE2
            // returns an empty flag set.
            bytes.fill(0);
            Ok((request.operation, request.context_id))
        }
        uapi::AMDGPU_CTX_OP_GET_STABLE_PSTATE => {
            if request.flags != 0 {
                return Err(Error::InvalidArgument);
            }
            let _context = file.context(request.context_id)?;
            bytes.fill(0);
            uapi::put_u32(bytes, 0, uapi::AMDGPU_CTX_STABLE_PSTATE_NONE);
            Ok((request.operation, request.context_id))
        }
        uapi::AMDGPU_CTX_OP_SET_STABLE_PSTATE => {
            if request.flags & !uapi::AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK != 0
                || request.flags > uapi::AMDGPU_CTX_STABLE_PSTATE_PEAK
            {
                return Err(Error::InvalidArgument);
            }
            let _context = file.context(request.context_id)?;
            if request.flags != uapi::AMDGPU_CTX_STABLE_PSTATE_NONE {
                // Linux routes non-default stable pstate requests through
                // the SMU/DPM clock-control path. That bridge is not wired
                // yet, so do not report a clock change that never occurred.
                return Err(Error::Unsupported);
            }
            bytes.fill(0);
            Ok((request.operation, request.context_id))
        }
        _ => Err(Error::InvalidArgument),
    }
}

fn read_user_bytes(address: u64, length: usize) -> Result<Vec<u8>> {
    if address == 0 || length == 0 {
        return Err(Error::InvalidArgument);
    }
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(length)
        .map_err(|_| Error::OutOfMemory)?;
    bytes.resize(length, 0);
    UserAddress::new(address).read(&mut bytes)?;
    Ok(bytes)
}

fn parse_bo_entries(
    file: &FileState,
    count: u32,
    entry_size: u32,
    pointer: u64,
) -> Result<Vec<BoListEntry>> {
    let count = usize::try_from(count).map_err(|_| Error::InvalidArgument)?;
    let entry_size = usize::try_from(entry_size).map_err(|_| Error::InvalidArgument)?;
    if count > MAX_BO_LIST_ENTRIES || (count != 0 && (entry_size < 8 || pointer == 0)) {
        return Err(Error::InvalidArgument);
    }
    let total = count
        .checked_mul(entry_size)
        .filter(|total| *total <= 64 << 20)
        .ok_or(Error::InvalidArgument)?;
    let bytes = if total == 0 {
        Vec::new()
    } else {
        read_user_bytes(pointer, total)?
    };
    let mut entries = Vec::new();
    entries.try_reserve(count).map_err(|_| Error::OutOfMemory)?;
    for index in 0..count {
        let at = index * entry_size;
        let handle = uapi::read_u32(&bytes, at)?;
        if handle == 0
            || entries
                .iter()
                .any(|entry: &BoListEntry| entry.handle == handle)
        {
            continue;
        }
        entries.push(BoListEntry {
            handle,
            object: file.buffer(handle)?.object.clone(),
        });
    }
    Ok(entries)
}

fn release_bo_list(adapter: &mut Adapter, list: BoList) -> Result<()> {
    let mut first_error = None;
    for entry in list.buffers {
        if let Err(error) = release_object(adapter, entry.object) {
            if first_error.is_none() {
                first_error = Some(error);
            }
        }
    }
    first_error.map_or(Ok(()), Err)
}

pub fn bo_list(adapter: &mut Adapter, file: &mut FileState, bytes: &mut [u8]) -> Result<u32> {
    let request = uapi::BoListRequest::parse(bytes)?;
    match request.operation {
        uapi::AMDGPU_BO_LIST_OP_CREATE => {
            if request.list_handle != 0 {
                return Err(Error::InvalidArgument);
            }
            let entries = parse_bo_entries(
                file,
                request.bo_number,
                request.bo_info_size,
                request.bo_info_ptr,
            )?;
            file.bo_lists
                .try_reserve(1)
                .map_err(|_| Error::OutOfMemory)?;
            let handle = file.allocate_bo_list_handle()?;
            file.bo_lists.push(BoList {
                handle,
                buffers: entries,
            });
            bytes.fill(0);
            uapi::put_u32(bytes, 0, handle);
            Ok(handle)
        }
        uapi::AMDGPU_BO_LIST_OP_UPDATE => {
            let index = file
                .bo_lists
                .iter()
                .position(|list| list.handle == request.list_handle)
                .ok_or(Error::NotFound)?;
            let entries = parse_bo_entries(
                file,
                request.bo_number,
                request.bo_info_size,
                request.bo_info_ptr,
            )?;
            let old = core::mem::replace(
                &mut file.bo_lists[index],
                BoList {
                    handle: request.list_handle,
                    buffers: entries,
                },
            );
            release_bo_list(adapter, old)?;
            bytes.fill(0);
            uapi::put_u32(bytes, 0, request.list_handle);
            Ok(request.list_handle)
        }
        uapi::AMDGPU_BO_LIST_OP_DESTROY => {
            let index = file
                .bo_lists
                .iter()
                .position(|list| list.handle == request.list_handle)
                .ok_or(Error::NotFound)?;
            let old = file.bo_lists.remove(index);
            release_bo_list(adapter, old)?;
            bytes.fill(0);
            Ok(request.list_handle)
        }
        _ => Err(Error::InvalidArgument),
    }
}

fn push_unique_object(objects: &mut Vec<SharedObject>, object: SharedObject) -> Result<()> {
    if objects.iter().any(|current| Arc::ptr_eq(current, &object)) {
        return Ok(());
    }
    objects.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
    objects.push(object);
    Ok(())
}

#[derive(Clone, Copy)]
struct SyncobjPoint {
    handle: u32,
    point: u64,
    timeline: bool,
}

fn parse_syncobj_chunk(
    chunk: uapi::CsChunk,
    timeline: bool,
    output: &mut Vec<SyncobjPoint>,
) -> Result<()> {
    let length = usize::try_from(chunk.length_dw)
        .ok()
        .and_then(|length| length.checked_mul(4))
        .filter(|length| *length != 0 && *length <= 1 << 20)
        .ok_or(Error::InvalidArgument)?;
    let entry_size = if timeline { 16 } else { 4 };
    if length % entry_size != 0 {
        return Err(Error::InvalidArgument);
    }
    let bytes = read_user_bytes(chunk.data, length)?;
    output
        .try_reserve(length / entry_size)
        .map_err(|_| Error::OutOfMemory)?;
    for at in (0..length).step_by(entry_size) {
        let handle = uapi::read_u32(&bytes, at)?;
        if handle == 0 {
            return Err(Error::InvalidArgument);
        }
        let point = if timeline {
            uapi::read_u64(&bytes, at + 8)?
        } else {
            1
        };
        output.push(SyncobjPoint {
            handle,
            point,
            timeline,
        });
    }
    Ok(())
}

pub fn cs(adapter: &mut Adapter, file: &mut FileState, bytes: &mut [u8]) -> Result<u64> {
    reap_submissions(adapter, file)?;
    let request = uapi::CsRequest::parse(bytes)?;
    if request.flags != 0
        || request.num_chunks == 0
        || request.num_chunks as usize > MAX_CS_CHUNKS
        || request.chunks == 0
    {
        return Err(Error::InvalidArgument);
    }
    let _context = file.context(request.context_id)?;

    let mut objects = Vec::new();
    let mut bo_list_supplied = request.bo_list_handle != 0;
    if request.bo_list_handle != 0 {
        let list = file
            .bo_lists
            .iter()
            .find(|list| list.handle == request.bo_list_handle)
            .ok_or(Error::NotFound)?;
        objects
            .try_reserve(list.buffers.len())
            .map_err(|_| Error::OutOfMemory)?;
        for entry in &list.buffers {
            push_unique_object(&mut objects, entry.object.clone())?;
        }
    }

    let pointer_bytes = read_user_bytes(
        request.chunks,
        request.num_chunks as usize * core::mem::size_of::<u64>(),
    )?;
    let mut ibs = Vec::new();
    let mut engine = None;
    let mut inline_bo_list = false;
    let mut user_fence: Option<(SharedObject, u32)> = None;
    let mut waits = Vec::new();
    let mut signals = Vec::new();
    let mut dependencies = Vec::new();

    for index in 0..request.num_chunks as usize {
        let pointer = uapi::read_u64(&pointer_bytes, index * 8)?;
        let descriptor = read_user_bytes(pointer, 16)?;
        let chunk = uapi::CsChunk::parse(&descriptor)?;
        match chunk.id {
            uapi::AMDGPU_CHUNK_ID_BO_HANDLES => {
                if inline_bo_list || request.bo_list_handle != 0 || chunk.length_dw != 6 {
                    return Err(Error::InvalidArgument);
                }
                let data = read_user_bytes(chunk.data, uapi::BO_LIST_SIZE)?;
                let list = uapi::BoListRequest::parse(&data)?;
                let entries =
                    parse_bo_entries(file, list.bo_number, list.bo_info_size, list.bo_info_ptr)?;
                for entry in entries {
                    push_unique_object(&mut objects, entry.object)?;
                }
                inline_bo_list = true;
                bo_list_supplied = true;
            }
            uapi::AMDGPU_CHUNK_ID_IB => {
                if chunk.length_dw != 8 {
                    return Err(Error::InvalidArgument);
                }
                let data = read_user_bytes(chunk.data, 32)?;
                let ib = uapi::CsIb::parse(&data)?;
                if ib.flags & !uapi::AMDGPU_IB_FLAGS_MASK != 0
                    || ib.flags & uapi::AMDGPU_IB_FLAGS_SECURE != 0
                    || ib.ib_bytes == 0
                    || ib.ib_bytes & 3 != 0
                    || ib.va_start & 3 != 0
                    || ib.ib_bytes / 4 > 0x000f_ffff
                    || ib.ip_instance != 0
                    || !matches!(
                        ib.ip_type,
                        uapi::AMDGPU_HW_IP_GFX | uapi::AMDGPU_HW_IP_COMPUTE
                    )
                    || (ib.ip_type == uapi::AMDGPU_HW_IP_COMPUTE
                        && ib.flags & uapi::AMDGPU_IB_FLAG_CE != 0)
                {
                    return Err(Error::InvalidArgument);
                }
                let selected = (ib.ip_type, ib.ip_instance, ib.ring);
                if engine.is_some_and(|engine| engine != selected) {
                    return Err(Error::Unsupported);
                }
                engine = Some(selected);
                ibs.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
                ibs.push(crate::ip::UserIb {
                    va_start: ib.va_start,
                    length_dw: ib.ib_bytes / 4,
                    flags: ib.flags,
                });
            }
            uapi::AMDGPU_CHUNK_ID_FENCE => {
                if chunk.length_dw != 2 || user_fence.is_some() {
                    return Err(Error::InvalidArgument);
                }
                let data = read_user_bytes(chunk.data, 8)?;
                let handle = uapi::read_u32(&data, 0)?;
                let offset = uapi::read_u32(&data, 4)?;
                let object = file.buffer(handle)?.object.clone();
                if (offset as u64)
                    .checked_add(8)
                    .filter(|end| *end <= object.lock().bo.size as u64)
                    .is_none()
                {
                    return Err(Error::InvalidArgument);
                }
                push_unique_object(&mut objects, object.clone())?;
                user_fence = Some((object, offset));
            }
            uapi::AMDGPU_CHUNK_ID_SYNCOBJ_IN => {
                parse_syncobj_chunk(chunk, false, &mut waits)?;
            }
            uapi::AMDGPU_CHUNK_ID_SYNCOBJ_OUT => {
                parse_syncobj_chunk(chunk, false, &mut signals)?;
            }
            uapi::AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT => {
                parse_syncobj_chunk(chunk, true, &mut waits)?;
            }
            uapi::AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL => {
                parse_syncobj_chunk(chunk, true, &mut signals)?;
            }
            uapi::AMDGPU_CHUNK_ID_DEPENDENCIES | uapi::AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES => {
                let length = chunk.length_dw as usize * 4;
                if length == 0 || length % 24 != 0 {
                    return Err(Error::InvalidArgument);
                }
                let data = read_user_bytes(chunk.data, length)?;
                dependencies
                    .try_reserve(length / 24)
                    .map_err(|_| Error::OutOfMemory)?;
                for at in (0..length).step_by(24) {
                    let dep_ip_type = uapi::read_u32(&data, at)?;
                    let dep_ip_instance = uapi::read_u32(&data, at + 4)?;
                    let dep_ring = uapi::read_u32(&data, at + 8)?;
                    let dep_context = uapi::read_u32(&data, at + 12)?;
                    let dep_handle = uapi::read_u64(&data, at + 16)?;
                    let _ = file.context(dep_context)?;
                    if let Some(submission) = file.submissions.iter().find(|submission| {
                        submission.sequence == dep_handle
                            && submission.context_id == dep_context
                            && submission.ip_type == dep_ip_type
                            && submission.ip_instance == dep_ip_instance
                            && submission.ring == dep_ring
                    }) {
                        dependencies.push(submission.fence);
                    } else if dep_handle == 0 || dep_handle >= file.next_sequence {
                        return Err(Error::InvalidArgument);
                    }
                    // A missing older handle was already reaped and is
                    // therefore complete, matching Linux's fence lookup.
                }
            }
            _ => return Err(Error::Unsupported),
        }
    }

    let (ip_type, ip_instance, ring) = engine.ok_or(Error::InvalidArgument)?;
    for ib in &ibs {
        let end = ib
            .va_start
            .checked_add(ib.length_dw as u64 * 4)
            .ok_or(Error::InvalidArgument)?;
        let mapping = file
            .mappings
            .iter()
            .find(|mapping| {
                ib.va_start >= mapping.address
                    && end <= mapping.address + mapping.size
                    && mapping.flags & uapi::AMDGPU_VM_PAGE_READABLE != 0
                    && mapping.flags & uapi::AMDGPU_VM_PAGE_EXECUTABLE != 0
            })
            .ok_or(Error::InvalidArgument)?;
        let object = file.buffer(mapping.handle)?.object.clone();
        if bo_list_supplied && !objects.iter().any(|entry| Arc::ptr_eq(entry, &object)) {
            return Err(Error::InvalidArgument);
        }
        push_unique_object(&mut objects, object)?;
    }

    for object in &objects {
        if let Some(cpu) = object.lock().bo.cpu.as_ref() {
            cpu.sync_for_device();
        }
    }
    for dependency in dependencies {
        let _ = wait_fence(adapter, dependency, u64::MAX)?;
    }
    for wait in waits {
        file.file.wait_syncobj(wait.handle, wait.point, i64::MAX)?;
    }

    let root_pde = file.vm.root_pde(adapter)?;
    let sequence = file.allocate_sequence()?;
    let ring_fence = if let Some((object, offset)) = user_fence {
        let object = object.lock();
        let gpu_addr = object
            .bo
            .gpu_addr
            .checked_add(offset as u64)
            .ok_or(Error::Range)?;
        Some(crate::ip::UserFence { gpu_addr, sequence })
    } else {
        None
    };
    // Reserve scheduler ownership before ringing the doorbell.  Once a job
    // is submitted, no later allocation failure may drop its BO references.
    file.submissions
        .try_reserve(1)
        .map_err(|_| Error::OutOfMemory)?;
    let completion = blocks::submit_user_ibs(
        adapter,
        ip_type,
        ring,
        file.vm.vmid,
        root_pde,
        request.context_id,
        &ibs,
        ring_fence,
    )?;
    file.submissions.push(Submission {
        sequence,
        context_id: request.context_id,
        ip_type,
        ip_instance,
        ring,
        fence: completion,
        objects,
    });

    let fence_cpu_address = adapter
        .wb
        .as_ref()
        .ok_or(Error::NoDevice)?
        .cpu_address(completion.gpu_address)?;
    for signal in signals {
        file.file.attach_syncobj_fence(
            signal.handle,
            signal.point,
            signal.timeline,
            fence_cpu_address,
            completion.value,
        )?;
    }
    bytes.fill(0);
    uapi::put_u64(bytes, 0, sequence);
    Ok(sequence)
}

pub fn wait_cs(adapter: &mut Adapter, file: &mut FileState, bytes: &mut [u8]) -> Result<u64> {
    let request = uapi::WaitCsRequest::parse(bytes)?;
    let _context = file.context(request.context_id)?;
    let handle = if request.handle == u64::MAX {
        file.submissions
            .iter()
            .filter(|submission| {
                submission.context_id == request.context_id
                    && submission.ip_type == request.ip_type
                    && submission.ip_instance == request.ip_instance
                    && submission.ring == request.ring
            })
            .map(|submission| submission.sequence)
            .max()
            .unwrap_or(0)
    } else {
        request.handle
    };
    let submission = if handle == 0 {
        None
    } else if let Some(submission) = file.submissions.iter().find(|submission| {
        submission.sequence == handle
            && submission.context_id == request.context_id
            && submission.ip_type == request.ip_type
            && submission.ip_instance == request.ip_instance
            && submission.ring == request.ring
    }) {
        Some(submission.fence)
    } else if handle >= file.next_sequence {
        return Err(Error::InvalidArgument);
    } else {
        None
    };
    let mut status = 0;
    if let Some(fence) = submission {
        status = (!wait_fence(adapter, fence, request.timeout)?) as u64;
        if status == 0 {
            reap_submissions(adapter, file)?;
        }
    }
    bytes.fill(0);
    uapi::put_u64(bytes, 0, status);
    Ok(status)
}

fn allocate_bo(adapter: &mut Adapter, request: uapi::GemCreateRequest) -> Result<(Bo, u64)> {
    if request.bo_size == 0
        || request.domains == 0
        || request.domains & !uapi::AMDGPU_GEM_DOMAIN_MASK != 0
        || request.domain_flags & !uapi::AMDGPU_GEM_CREATE_SETTABLE_MASK != 0
        || request.domain_flags & uapi::AMDGPU_GEM_CREATE_ENCRYPTED != 0
        || request.domains
            & (uapi::AMDGPU_GEM_DOMAIN_GDS
                | uapi::AMDGPU_GEM_DOMAIN_GWS
                | uapi::AMDGPU_GEM_DOMAIN_OA
                | uapi::AMDGPU_GEM_DOMAIN_DOORBELL)
            != 0
    {
        return Err(Error::InvalidArgument);
    }

    let size = usize::try_from(request.bo_size).map_err(|_| Error::OutOfMemory)?;
    let alignment = if request.alignment == 0 {
        GPU_PAGE_SIZE
    } else {
        usize::try_from(request.alignment).map_err(|_| Error::InvalidArgument)?
    };
    if alignment < GPU_PAGE_SIZE || !alignment.is_power_of_two() {
        return Err(Error::InvalidArgument);
    }

    let prefer_vram = request.domains & uapi::AMDGPU_GEM_DOMAIN_VRAM != 0;
    let bo = if prefer_vram {
        match adapter
            .mem
            .alloc_vram_aligned(&mut adapter.regs, size, alignment)
        {
            Ok(bo) => bo,
            Err(Error::OutOfMemory) => {
                adapter
                    .mem
                    .alloc_gart_aligned(&mut adapter.regs, size, alignment)?
            }
            Err(error) => return Err(error),
        }
    } else {
        // CPU-domain BOs in ASTRA use the same physically-contiguous backing
        // as GTT BOs. They remain GPU-addressable, which is the useful Linux
        // fallback for Mesa's upload and staging allocations.
        adapter
            .mem
            .alloc_gart_aligned(&mut adapter.regs, size, alignment)?
    };
    if bo.place == Place::Gart {
        blocks::flush_pending_gart(adapter)?;
    }
    let actual_domain = match bo.place {
        Place::Gart => uapi::AMDGPU_GEM_DOMAIN_GTT,
        Place::Vram => uapi::AMDGPU_GEM_DOMAIN_VRAM,
    };
    Ok((bo, actual_domain))
}

pub fn gem_create(adapter: &mut Adapter, file: &mut FileState, bytes: &mut [u8]) -> Result<u32> {
    let request = uapi::GemCreateRequest::parse(bytes)?;
    let alloc_size = request
        .bo_size
        .checked_add(GPU_PAGE_SIZE as u64 - 1)
        .map(|value| value & !(GPU_PAGE_SIZE as u64 - 1))
        .filter(|value| *value != 0)
        .ok_or(Error::OutOfMemory)?;
    let handle = file.next_handle;
    let next_handle = handle.checked_add(1).ok_or(Error::NoSpace)?;
    if handle == 0 {
        return Err(Error::NoSpace);
    }
    let mmap_offset = file.next_mmap_offset;
    let next_mmap_offset = mmap_offset
        .checked_add(alloc_size)
        .filter(|value| *value < i64::MAX as u64)
        .ok_or(Error::NoSpace)?;
    file.buffers
        .try_reserve(1)
        .map_err(|_| Error::OutOfMemory)?;

    let (bo, actual_domain) = allocate_bo(adapter, request)?;
    debug_assert_eq!(bo.size as u64, alloc_size);
    let object = Arc::new(Mutex::new(UserObject {
        bo,
        alignment: request.alignment.max(GPU_PAGE_SIZE as u64),
        preferred_domains: request.domains,
        actual_domain,
        // Linux unconditionally clears VRAM allocations.
        flags: request.domain_flags | uapi::AMDGPU_GEM_CREATE_VRAM_CLEARED,
        metadata_flags: 0,
        tiling_info: 0,
        metadata: Vec::new(),
    })?);
    file.next_handle = next_handle;
    file.next_mmap_offset = next_mmap_offset;
    file.buffers.push(UserBuffer {
        handle,
        object,
        mmap_offset,
    });
    uapi::GemCreateRequest::write_reply(bytes, handle)?;
    Ok(handle)
}

pub fn gem_mmap(file: &FileState, bytes: &mut [u8]) -> Result<u64> {
    if bytes.len() != uapi::GEM_MMAP_SIZE {
        return Err(Error::InvalidArgument);
    }
    let handle = uapi::read_u32(bytes, 0)?;
    let buffer = file.buffer(handle)?;
    if buffer.object.lock().flags & uapi::AMDGPU_GEM_CREATE_NO_CPU_ACCESS != 0 {
        return Err(Error::PermissionDenied);
    }
    let offset = buffer.mmap_offset;
    bytes.fill(0);
    uapi::put_u64(bytes, 0, offset);
    Ok(offset)
}

pub fn mmap_physical(
    adapter: &Adapter,
    file: &FileState,
    offset: u64,
    length: usize,
) -> Result<PhysicalAddress> {
    if length == 0 || offset & (GPU_PAGE_SIZE as u64 - 1) != 0 {
        return Err(Error::InvalidArgument);
    }
    let buffer = file
        .buffers
        .iter()
        .find(|buffer| {
            let object = buffer.object.lock();
            offset >= buffer.mmap_offset && offset - buffer.mmap_offset < object.bo.size as u64
        })
        .ok_or(Error::Unsupported)?;
    let object = buffer.object.lock();
    if object.flags & uapi::AMDGPU_GEM_CREATE_NO_CPU_ACCESS != 0 {
        return Err(Error::PermissionDenied);
    }
    let buffer_offset = offset - buffer.mmap_offset;
    let length = u64::try_from(length).map_err(|_| Error::InvalidArgument)?;
    if buffer_offset
        .checked_add(length)
        .filter(|end| *end <= object.bo.size as u64)
        .is_none()
    {
        return Err(Error::InvalidArgument);
    }
    let base = match object.bo.place {
        Place::Vram => adapter
            .vram_base
            .checked_add(object.bo.gpu_addr)
            .ok_or(Error::Range)?,
        Place::Gart => {
            let cpu = object.bo.cpu.as_ref().ok_or(Error::NoDevice)?;
            cpu.sync_for_cpu();
            cpu.physical_address().get()
        }
    };
    Ok(PhysicalAddress::new(
        base.checked_add(buffer_offset).ok_or(Error::Range)?,
    ))
}

pub fn gem_metadata(file: &mut FileState, bytes: &mut [u8]) -> Result<()> {
    if bytes.len() != uapi::GEM_METADATA_SIZE {
        return Err(Error::InvalidArgument);
    }
    let handle = uapi::read_u32(bytes, 0)?;
    let operation = uapi::read_u32(bytes, 4)?;
    let buffer = file.buffer(handle)?;
    let mut object = buffer.object.lock();
    match operation {
        uapi::AMDGPU_GEM_METADATA_OP_SET_METADATA => {
            let size = uapi::read_u32(bytes, 24)? as usize;
            if size > 256 {
                return Err(Error::InvalidArgument);
            }
            object.metadata_flags = uapi::read_u64(bytes, 8)?;
            object.tiling_info = uapi::read_u64(bytes, 16)?;
            object.metadata.clear();
            object
                .metadata
                .try_reserve(size)
                .map_err(|_| Error::OutOfMemory)?;
            object.metadata.extend_from_slice(&bytes[28..28 + size]);
        }
        uapi::AMDGPU_GEM_METADATA_OP_GET_METADATA => {
            let flags = object.metadata_flags;
            let tiling = object.tiling_info;
            bytes[8..].fill(0);
            uapi::put_u64(bytes, 8, flags);
            uapi::put_u64(bytes, 16, tiling);
            uapi::put_u32(bytes, 24, object.metadata.len() as u32);
            bytes[28..28 + object.metadata.len()].copy_from_slice(&object.metadata);
        }
        _ => return Err(Error::InvalidArgument),
    }
    Ok(())
}

pub fn gem_wait_idle(adapter: &mut Adapter, file: &mut FileState, bytes: &mut [u8]) -> Result<()> {
    if bytes.len() != uapi::GEM_WAIT_IDLE_SIZE {
        return Err(Error::InvalidArgument);
    }
    let handle = uapi::read_u32(bytes, 0)?;
    let flags = uapi::read_u32(bytes, 4)?;
    let timeout = uapi::read_u64(bytes, 8)?;
    if flags != 0 {
        return Err(Error::InvalidArgument);
    }
    let object = file.buffer(handle)?.object.clone();
    let domain = object.lock().actual_domain as u32;
    let mut busy = false;
    for submission in &file.submissions {
        if submission
            .objects
            .iter()
            .any(|current| Arc::ptr_eq(current, &object))
            && !wait_fence(adapter, submission.fence, timeout)?
        {
            busy = true;
            break;
        }
    }
    if !busy {
        reap_submissions(adapter, file)?;
    }
    bytes.fill(0);
    uapi::put_u32(bytes, 0, busy as u32);
    uapi::put_u32(bytes, 4, domain);
    Ok(())
}

pub fn gem_op(file: &mut FileState, bytes: &mut [u8]) -> Result<()> {
    if bytes.len() != uapi::GEM_OP_SIZE {
        return Err(Error::InvalidArgument);
    }
    let handle = uapi::read_u32(bytes, 0)?;
    let operation = uapi::read_u32(bytes, 4)?;
    let value = uapi::read_u64(bytes, 8)?;
    let buffer = file.buffer(handle)?;
    let mut object = buffer.object.lock();
    match operation {
        uapi::AMDGPU_GEM_OP_GET_GEM_CREATE_INFO => {
            if value == 0 {
                return Err(Error::InvalidArgument);
            }
            let mut info = [0u8; uapi::GEM_CREATE_SIZE];
            uapi::put_u64(&mut info, 0, object.bo.size as u64);
            uapi::put_u64(&mut info, 8, object.alignment);
            uapi::put_u64(&mut info, 16, object.preferred_domains);
            uapi::put_u64(&mut info, 24, object.flags);
            UserAddress::new(value).write(&info)?;
        }
        uapi::AMDGPU_GEM_OP_SET_PLACEMENT => {
            let domains = value
                & (uapi::AMDGPU_GEM_DOMAIN_CPU
                    | uapi::AMDGPU_GEM_DOMAIN_GTT
                    | uapi::AMDGPU_GEM_DOMAIN_VRAM);
            if domains == 0 || value != domains {
                return Err(Error::InvalidArgument);
            }
            // Linux updates preferred/allowed placement here; migration is
            // deferred. ASTRA likewise keeps the current physical placement.
            object.preferred_domains = domains;
        }
        _ => return Err(Error::InvalidArgument),
    }
    Ok(())
}

fn clear_va_range(
    adapter: &mut Adapter,
    file: &mut FileState,
    address: u64,
    size: u64,
) -> Result<()> {
    let end = address.checked_add(size).ok_or(Error::InvalidArgument)?;
    file.vm.clear_range(adapter, address, size)?;
    let mut mappings = Vec::new();
    mappings
        .try_reserve(file.mappings.len().saturating_add(1))
        .map_err(|_| Error::OutOfMemory)?;
    for mapping in file.mappings.iter().copied() {
        let mapping_end = mapping.address + mapping.size;
        if mapping_end <= address || mapping.address >= end {
            mappings.push(mapping);
            continue;
        }
        if mapping.address < address {
            mappings.push(VaMapping {
                size: address - mapping.address,
                ..mapping
            });
        }
        if mapping_end > end {
            mappings.push(VaMapping {
                address: end,
                offset: mapping.offset + (end - mapping.address),
                size: mapping_end - end,
                ..mapping
            });
        }
    }
    file.mappings = mappings;
    Ok(())
}

pub fn gem_va(adapter: &mut Adapter, file: &mut FileState, bytes: &[u8]) -> Result<()> {
    let request = uapi::GemVaRequest::parse(bytes)?;
    let valid_flags = uapi::AMDGPU_VM_DELAY_UPDATE
        | uapi::AMDGPU_VM_PAGE_READABLE
        | uapi::AMDGPU_VM_PAGE_WRITEABLE
        | uapi::AMDGPU_VM_PAGE_EXECUTABLE
        | uapi::AMDGPU_VM_MTYPE_MASK
        | uapi::AMDGPU_VM_PAGE_NOALLOC;
    let prt_flags = uapi::AMDGPU_VM_DELAY_UPDATE | uapi::AMDGPU_VM_PAGE_PRT;
    if (request.flags & !valid_flags != 0) && (request.flags & !prt_flags != 0) {
        return Err(Error::InvalidArgument);
    }
    if request.vm_timeline_point != 0
        || request.vm_timeline_syncobj_out != 0
        || request.num_syncobj_handles != 0
        || request.input_fence_syncobj_handles != 0
    {
        // Timeline syncobj integration is phase 5. Mesa's ordinary VA path
        // sends zeroes for these fields.
        return Err(Error::Unsupported);
    }
    if request.map_size == 0
        || request.va_address & (GPU_PAGE_SIZE as u64 - 1) != 0
        || request.offset_in_bo & (GPU_PAGE_SIZE as u64 - 1) != 0
        || request.map_size & (GPU_PAGE_SIZE as u64 - 1) != 0
        || request.va_address < VA_RESERVED_BOTTOM
        || request
            .va_address
            .checked_add(request.map_size)
            .filter(|end| *end <= VM_SIZE.saturating_sub(VA_RESERVED_TOP))
            .is_none()
    {
        return Err(Error::InvalidArgument);
    }

    // Linux orders destructive VM updates behind reservation fences.  ASTRA
    // has one scheduler fence per submission, so wait only for operations
    // that remove or replace mappings; ordinary MAP remains asynchronous.
    if matches!(
        request.operation,
        uapi::AMDGPU_VA_OP_REPLACE | uapi::AMDGPU_VA_OP_UNMAP | uapi::AMDGPU_VA_OP_CLEAR
    ) {
        wait_all_submissions(adapter, file)?;
    }

    match request.operation {
        uapi::AMDGPU_VA_OP_MAP | uapi::AMDGPU_VA_OP_REPLACE => {
            if request.flags & uapi::AMDGPU_VM_PAGE_PRT != 0 {
                return Err(Error::Unsupported);
            }
            let object = file.buffer(request.handle)?.object.clone();
            let object = object.lock();
            let bo_size = object.bo.size as u64;
            if request
                .offset_in_bo
                .checked_add(request.map_size)
                .filter(|end| *end <= bo_size)
                .is_none()
            {
                return Err(Error::InvalidArgument);
            }
            if request.operation == uapi::AMDGPU_VA_OP_REPLACE {
                clear_va_range(adapter, file, request.va_address, request.map_size)?;
            } else if file.mappings.iter().any(|mapping| {
                let end = request.va_address + request.map_size;
                let mapping_end = mapping.address + mapping.size;
                request.va_address < mapping_end && end > mapping.address
            }) {
                return Err(Error::InvalidArgument);
            }

            let physical_base = match object.bo.place {
                Place::Gart => object
                    .bo
                    .cpu
                    .as_ref()
                    .ok_or(Error::NoDevice)?
                    .physical_address()
                    .get(),
                Place::Vram => adapter
                    .gmc
                    .vram_base_offset
                    .checked_add(object.bo.gpu_addr)
                    .ok_or(Error::Range)?,
            };
            let pte_flags = mapping_pte_flags(request.flags, object.flags, object.bo.place);
            let physical = physical_base
                .checked_add(request.offset_in_bo)
                .ok_or(Error::Range)?;
            file.vm.map_range(
                adapter,
                request.va_address,
                physical,
                request.map_size,
                pte_flags,
            )?;
            drop(object);
            file.mappings
                .try_reserve(1)
                .map_err(|_| Error::OutOfMemory)?;
            file.mappings.push(VaMapping {
                handle: request.handle,
                address: request.va_address,
                offset: request.offset_in_bo,
                size: request.map_size,
                flags: request.flags,
            });
        }
        uapi::AMDGPU_VA_OP_UNMAP => {
            let index = file
                .mappings
                .iter()
                .position(|mapping| {
                    mapping.handle == request.handle && mapping.address == request.va_address
                })
                .ok_or(Error::NotFound)?;
            let mapping = file.mappings[index];
            if request.map_size != mapping.size {
                return Err(Error::InvalidArgument);
            }
            clear_va_range(adapter, file, request.va_address, request.map_size)?;
        }
        uapi::AMDGPU_VA_OP_CLEAR => {
            clear_va_range(adapter, file, request.va_address, request.map_size)?;
        }
        _ => return Err(Error::InvalidArgument),
    }
    Ok(())
}

fn wipe_object(adapter: &mut Adapter, object: &mut UserObject) -> Result<()> {
    if object.flags & uapi::AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE == 0 {
        return Ok(());
    }
    match object.bo.place {
        Place::Gart => {
            let cpu = object.bo.cpu.as_mut().ok_or(Error::NoDevice)?;
            cpu.as_mut_slice().fill(0);
            cpu.sync_for_device();
        }
        Place::Vram => {
            let zero = [0u32; 1024];
            let mut offset = object.bo.gpu_addr;
            for _ in 0..object.bo.size / GPU_PAGE_SIZE {
                adapter.regs.vram_write_dwords(offset, &zero)?;
                offset += GPU_PAGE_SIZE as u64;
            }
        }
    }
    Ok(())
}

fn release_object(adapter: &mut Adapter, object: SharedObject) -> Result<()> {
    let wipe = if Arc::strong_count(&object) == 1 {
        wipe_object(adapter, &mut object.lock())
    } else {
        Ok(())
    };
    drop(object);
    let retire = blocks::flush_pending_gart(adapter);
    wipe.and(retire)
}

pub fn gem_close(adapter: &mut Adapter, file: &mut FileState, bytes: &[u8]) -> Result<u32> {
    if bytes.len() != uapi::GEM_CLOSE_SIZE {
        return Err(Error::InvalidArgument);
    }
    let handle = uapi::read_u32(bytes, 0)?;
    if handle == 0 {
        return Err(Error::InvalidArgument);
    }
    let index = file
        .buffers
        .iter()
        .position(|buffer| buffer.handle == handle)
        .ok_or(Error::NotFound)?;
    wait_all_submissions(adapter, file)?;
    let mappings: Vec<_> = file
        .mappings
        .iter()
        .copied()
        .filter(|mapping| mapping.handle == handle)
        .collect();
    for mapping in mappings {
        clear_va_range(adapter, file, mapping.address, mapping.size)?;
    }
    let buffer = file.buffers.remove(index);
    release_object(adapter, buffer.object)?;
    Ok(handle)
}

pub fn release_file(adapter: &mut Adapter, file: &mut FileState) -> Result<()> {
    file.contexts.clear();
    wait_all_submissions(adapter, file)?;
    file.submissions.clear();
    let mut first_error = None;
    for list in file.bo_lists.drain(..) {
        if let Err(error) = release_bo_list(adapter, list) {
            if first_error.is_none() {
                first_error = Some(error);
            }
        }
    }
    let mappings = core::mem::take(&mut file.mappings);
    for mapping in mappings {
        if let Err(error) = file.vm.clear_range(adapter, mapping.address, mapping.size) {
            if first_error.is_none() {
                first_error = Some(error);
            }
        }
    }
    for buffer in file.buffers.drain(..) {
        if let Err(error) = release_object(adapter, buffer.object) {
            if first_error.is_none() {
                first_error = Some(error);
            }
        }
    }
    first_error.map_or(Ok(()), Err)
}

pub fn prime_export(
    adapter: &Adapter,
    files: &[FileState],
    prime: &mut PrimeState,
    file_id: FileId,
    handle: u32,
) -> Result<PrimeBuffer> {
    let file = files
        .iter()
        .find(|file| file.belongs_to(file_id))
        .ok_or(Error::NotFound)?;
    let object = file.buffer(handle)?.object.clone();
    let object_guard = object.lock();
    let physical_address = match object_guard.bo.place {
        Place::Vram => adapter
            .vram_base
            .checked_add(object_guard.bo.gpu_addr)
            .ok_or(Error::Range)?,
        Place::Gart => object_guard
            .bo
            .cpu
            .as_ref()
            .ok_or(Error::NoDevice)?
            .physical_address()
            .get(),
    };
    let length = object_guard.bo.size;
    drop(object_guard);

    prime
        .exports
        .try_reserve(1)
        .map_err(|_| Error::OutOfMemory)?;
    let token = prime.allocate_token()?;
    prime.exports.push(PrimeExport { token, object });
    Ok(PrimeBuffer {
        physical_address: PhysicalAddress::new(physical_address),
        length,
        token,
    })
}

pub fn prime_import(
    files: &mut [FileState],
    prime: &PrimeState,
    file_id: FileId,
    token: u64,
) -> Result<u32> {
    let object = prime
        .exports
        .iter()
        .find(|export| export.token == token)
        .map(|export| export.object.clone())
        .ok_or(Error::NotFound)?;
    let file = files
        .iter_mut()
        .find(|file| file.belongs_to(file_id))
        .ok_or(Error::NotFound)?;

    file.buffers
        .try_reserve(1)
        .map_err(|_| Error::OutOfMemory)?;
    let size = object.lock().bo.size;
    let handle = file.allocate_handle()?;
    let mmap_offset = file.allocate_mmap_offset(size)?;
    file.buffers.push(UserBuffer {
        handle,
        object,
        mmap_offset,
    });
    Ok(handle)
}

pub fn prime_release(adapter: &mut Adapter, prime: &mut PrimeState, token: u64) -> Result<()> {
    let index = prime
        .exports
        .iter()
        .position(|export| export.token == token)
        .ok_or(Error::NotFound)?;
    let export = prime.exports.remove(index);
    release_object(adapter, export.object)
}

pub fn framebuffer_pin(
    files: &[FileState],
    framebuffers: &mut FramebufferState,
    file_id: FileId,
    gem_handle: u32,
    minimum_size: u64,
    width: u32,
    height: u32,
    pitch: u32,
    offset: u32,
    format: u32,
    flags: u32,
    modifier: u64,
    meta_pitch: u32,
    meta_offset: u32,
) -> Result<u32> {
    let file = files
        .iter()
        .find(|file| file.belongs_to(file_id))
        .ok_or(Error::NotFound)?;
    let object = file.buffer(gem_handle)?.object.clone();
    let guard = object.lock();
    if minimum_size > guard.bo.size as u64 {
        return Err(Error::InvalidArgument);
    }
    let bo_gpu_address = guard.bo.gpu_addr;
    let bo_size = guard.bo.size;
    let bo_place = guard.bo.place;
    let metadata_flags = guard.metadata_flags;
    let tiling_info = guard.tiling_info;
    drop(guard);

    framebuffers
        .buffers
        .try_reserve(1)
        .map_err(|_| Error::OutOfMemory)?;
    let handle = framebuffers.allocate_handle()?;
    framebuffers.buffers.push(FramebufferRef {
        handle,
        object,
        width,
        height,
        pitch,
        offset,
        format,
        flags,
        modifier,
        meta_pitch,
        meta_offset,
        tiling_info,
    });
    crate::dev_info!(
        "astra: KMS FB {} GEM {}: {}x{} pitch {} offset {:#x} format {:#010x}, BO {:?} gpu {:#x} size {:#x}, flags {:#x} modifier {:#018x}, metadata {:#018x} tiling {:#018x}",
        handle,
        gem_handle,
        width,
        height,
        pitch,
        offset,
        format,
        bo_place,
        bo_gpu_address,
        bo_size,
        flags,
        modifier,
        metadata_flags,
        tiling_info,
    );
    Ok(handle)
}

pub fn framebuffer_release(
    adapter: &mut Adapter,
    framebuffers: &mut FramebufferState,
    handle: u32,
) -> Result<()> {
    let index = framebuffers
        .buffers
        .iter()
        .position(|buffer| buffer.handle == handle)
        .ok_or(Error::NotFound)?;
    let buffer = framebuffers.buffers.remove(index);
    release_object(adapter, buffer.object)
}

/// Linux DRM_MODE_GETFB creates a fresh GEM handle for the requesting
/// `drm_file`; framebuffer objects retain the underlying GEM object but not
/// the temporary handle used by AddFB2.
pub(crate) fn framebuffer_gem_handle(
    files: &mut [FileState],
    framebuffers: &FramebufferState,
    file_id: FileId,
    framebuffer_handle: u32,
) -> Result<u32> {
    let object = framebuffers
        .buffers
        .iter()
        .find(|buffer| buffer.handle == framebuffer_handle)
        .map(|buffer| buffer.object.clone())
        .ok_or(Error::NotFound)?;
    let file = files
        .iter_mut()
        .find(|file| file.belongs_to(file_id))
        .ok_or(Error::NotFound)?;
    if let Some(buffer) = file
        .buffers
        .iter()
        .find(|buffer| Arc::ptr_eq(&buffer.object, &object))
    {
        return Ok(buffer.handle);
    }
    file.buffers
        .try_reserve(1)
        .map_err(|_| Error::OutOfMemory)?;
    let size = object.lock().bo.size;
    let handle = file.allocate_handle()?;
    let mmap_offset = file.allocate_mmap_offset(size)?;
    file.buffers.push(UserBuffer {
        handle,
        object,
        mmap_offset,
    });
    Ok(handle)
}

/// Linux's display prepare_fb pins the object and records its GPU address in
/// the active framebuffer state. ASTRA currently has no TTM migration, so a
/// scanout BO must already reside in contiguous VRAM.
pub(crate) fn framebuffer_scanout(
    adapter: &Adapter,
    framebuffers: &FramebufferState,
    framebuffer_handle: u32,
) -> Result<ScanoutBuffer> {
    let framebuffer = framebuffers
        .buffers
        .iter()
        .find(|buffer| buffer.handle == framebuffer_handle)
        .ok_or(Error::NotFound)?;
    let object = framebuffer.object.clone();
    let guard = object.lock();
    if guard.bo.place != Place::Vram {
        return Err(Error::NoSpace);
    }
    let bo_gpu_address = adapter
        .gmc
        .fb_start
        .checked_add(guard.bo.gpu_addr)
        .ok_or(Error::Range)?;
    let gpu_address = bo_gpu_address
        .checked_add(framebuffer.offset as u64)
        .ok_or(Error::Range)?;

    // Linux amdgpu_display_framebuffer_init() converts implicit BO tiling
    // flags to a framebuffer modifier before amdgpu_dm fills DCN plane
    // attributes.  Preserve exactly that distinction: an explicit modifier
    // is authoritative, while legacy AddFB2 gets its layout from GEM
    // metadata (AMDGPU_GEM_METADATA_OP_SET_METADATA).
    const DRM_MODE_FB_MODIFIERS: u32 = 1 << 1;
    const DRM_FORMAT_MOD_VENDOR_AMD: u64 = 0x02;
    let explicit_modifier = framebuffer.flags & DRM_MODE_FB_MODIFIERS != 0;
    let (
        swizzle,
        dcc_offset,
        meta_pitch,
        independent_64b,
        independent_128b,
        tile_version,
        amd_tiled,
    ) = if explicit_modifier {
        if framebuffer.modifier == 0 {
            (0, None, 0, false, false, 0, false)
        } else {
            if framebuffer.modifier >> 56 != DRM_FORMAT_MOD_VENDOR_AMD {
                return Err(Error::Unsupported);
            }
            let swizzle = ((framebuffer.modifier >> 8) & 0x1f) as u32;
            let dcc = framebuffer.modifier & (1 << 13) != 0;
            if dcc && (framebuffer.meta_pitch == 0 || framebuffer.meta_offset == 0) {
                return Err(Error::InvalidArgument);
            }
            (
                swizzle,
                dcc.then_some(framebuffer.meta_offset as u64),
                if dcc { framebuffer.meta_pitch } else { 0 },
                dcc && framebuffer.modifier & (1 << 16) != 0,
                dcc && framebuffer.modifier & (1 << 17) != 0,
                (framebuffer.modifier & 0xff) as u32,
                true,
            )
        }
    } else {
        let swizzle = (framebuffer.tiling_info & 0x1f) as u32;
        let has_xor = swizzle >= 16;
        let tile_version = if swizzle != 0 && (swizzle & 3) == 1 && !has_xor {
            // convert_tiling_flags_to_modifier(): non-X S swizzles use the
            // canonical GFX9 modifier even on GFX10.3.
            1
        } else if swizzle != 0 {
            3 // AMD_FMT_MOD_TILE_VER_GFX10_RBPLUS
        } else {
            0
        };
        let dcc_offset_256b = (framebuffer.tiling_info >> 5) & 0x00ff_ffff;
        let dcc = dcc_offset_256b != 0;
        let dcc_offset = dcc_offset_256b
            .checked_mul(256)
            .and_then(|offset| offset.checked_add(framebuffer.offset as u64));
        (
            swizzle,
            if dcc {
                Some(dcc_offset.ok_or(Error::Range)?)
            } else {
                None
            },
            if dcc {
                (((framebuffer.tiling_info >> 29) & 0x3fff) + 1) as u32
            } else {
                0
            },
            dcc && framebuffer.tiling_info & (1 << 43) != 0,
            // convert_tiling_flags_to_modifier() derives 128-byte block
            // independence from the GFX generation, rather than trusting
            // the legacy metadata bit.  GFX10.3 RB+ always advertises it.
            dcc && tile_version >= 3,
            tile_version,
            swizzle != 0,
        )
    };
    let meta_address = dcc_offset
        .map(|offset| {
            if offset >= guard.bo.size as u64 {
                return Err(Error::InvalidArgument);
            }
            bo_gpu_address.checked_add(offset).ok_or(Error::Range)
        })
        .transpose()?;
    // enum hubp_ind_block_size, selected exactly as
    // amdgpu_dm_plane_fill_gfx9_plane_attributes_from_modifiers().
    let dcc_independent_block = if tile_version >= 3 {
        match (independent_64b, independent_128b) {
            (true, true) => 3,
            (false, true) => 2,
            (true, false) => 1,
            (false, false) => 0,
        }
    } else if independent_64b {
        1
    } else {
        0
    };
    let gb_addr_config = adapter.gfx_info.gb_addr_config;
    let device_num_pipes = gb_addr_config & 0x7;
    let device_num_pkrs = (gb_addr_config >> 8) & 0x7;
    let (num_pipes, num_pkrs) = if amd_tiled {
        if explicit_modifier {
            (
                (((framebuffer.modifier >> 21) & 0x7) as u32).min(5),
                ((framebuffer.modifier >> 27) & 0x7) as u32,
            )
        } else {
            // convert_tiling_flags_to_modifier() followed by
            // amdgpu_dm_plane_fill_gfx9_tiling_info_from_modifier().
            let micro = swizzle & 3;
            if micro == 0 {
                return Err(Error::InvalidArgument);
            }
            let block_size_bits = match swizzle >> 2 {
                0 => 8,
                1 | 5 => 12,
                2 | 4 | 6 => 16,
                7 => 18,
                _ => return Err(Error::InvalidArgument),
            };
            if swizzle >= 16 {
                let pipe_xor_bits = (block_size_bits - 8).min(device_num_pipes);
                let packers = (block_size_bits - 8 - pipe_xor_bits).min(device_num_pkrs);
                (pipe_xor_bits, packers)
            } else {
                (0, 0)
            }
        }
    } else {
        (device_num_pipes, device_num_pkrs)
    };
    drop(guard);
    Ok(ScanoutBuffer {
        _object: object,
        gpu_address,
        meta_address,
        width: framebuffer.width,
        height: framebuffer.height,
        pitch: framebuffer.pitch,
        format: framebuffer.format,
        swizzle,
        num_pipes,
        pipe_interleave: (gb_addr_config >> 3) & 0x7,
        max_compressed_frags: (gb_addr_config >> 6) & 0x3,
        num_pkrs,
        meta_pitch,
        dcc_independent_block,
    })
}

/// Linux `amdgpu_dm_plane_helper_prepare_fb()` pins cursor planes in
/// contiguous VRAM and stores `amdgpu_bo_gpu_offset()` in the framebuffer.
/// ASTRA BOs do not migrate yet, so a cursor allocation which fell back to
/// GART cannot satisfy that contract and is rejected instead of programming
/// DCN with an incompatible address.
pub(crate) fn cursor_pin(
    adapter: &Adapter,
    files: &[FileState],
    framebuffers: &FramebufferState,
    file_id: FileId,
    handle: u32,
    width: u32,
    height: u32,
) -> Result<CursorBuffer> {
    let file = files
        .iter()
        .find(|file| file.belongs_to(file_id))
        .ok_or(Error::NotFound)?;
    let object = file.buffer(handle)?.object.clone();

    let framebuffer = framebuffers
        .buffers
        .iter()
        .find(|framebuffer| {
            framebuffer.width == width
                && framebuffer.height == height
                && Arc::ptr_eq(&framebuffer.object, &object)
        })
        .ok_or(Error::NotFound)?;
    if !matches!(
        framebuffer.format,
        DRM_FORMAT_XRGB8888 | DRM_FORMAT_ARGB8888
    ) || framebuffer.pitch % 4 != 0
    {
        return Err(Error::InvalidArgument);
    }
    let pitch_pixels = framebuffer.pitch / 4;
    if !matches!(pitch_pixels, 64 | 128 | 256) || pitch_pixels < width {
        return Err(Error::InvalidArgument);
    }

    let guard = object.lock();
    let required = (framebuffer.pitch as u64)
        .checked_mul(height as u64)
        .and_then(|bytes| bytes.checked_add(framebuffer.offset as u64))
        .ok_or(Error::InvalidArgument)?;
    if required > guard.bo.size as u64 {
        return Err(Error::InvalidArgument);
    }
    if guard.bo.place != Place::Vram {
        return Err(Error::NoSpace);
    }
    let gpu_address = adapter
        .gmc
        .fb_start
        .checked_add(guard.bo.gpu_addr)
        .and_then(|address| address.checked_add(framebuffer.offset as u64))
        .ok_or(Error::Range)?;
    drop(guard);
    Ok(CursorBuffer {
        _object: object,
        gpu_address,
        pitch_pixels,
    })
}

fn scalar_u32(value: u32) -> Vec<u8> {
    value.to_le_bytes().to_vec()
}

fn scalar_u64(value: u64) -> Vec<u8> {
    value.to_le_bytes().to_vec()
}

/// Linux `amdgpu_ring_max_ibs()`, indexed by the public AMDGPU HW IP type.
/// These are ABI submission limits rather than the number of queues exposed
/// by this device, so Linux reports an entry for every defined IP type.
fn max_ibs() -> Vec<u8> {
    let mut limits = [49u32; uapi::AMDGPU_HW_IP_NUM];
    limits[uapi::AMDGPU_HW_IP_GFX as usize] = 192;
    limits[uapi::AMDGPU_HW_IP_COMPUTE as usize] = 125;
    limits[uapi::AMDGPU_HW_IP_VCN_JPEG as usize] = 16;
    limits[uapi::AMDGPU_HW_IP_VPE as usize] = 49;
    let mut bytes = [0u8; uapi::AMDGPU_HW_IP_NUM * core::mem::size_of::<u32>()];
    for (index, limit) in limits.into_iter().enumerate() {
        uapi::put_u32(&mut bytes, index * core::mem::size_of::<u32>(), limit);
    }
    bytes.to_vec()
}

fn ring_mask(count: u32) -> u32 {
    if count >= 32 {
        u32::MAX
    } else if count == 0 {
        0
    } else {
        (1 << count) - 1
    }
}

fn sdma_count(adapter: &Adapter) -> u32 {
    [HwIp::Sdma0, HwIp::Sdma1, HwIp::Sdma2, HwIp::Sdma3]
        .into_iter()
        .filter(|ip| adapter.versions[ip.index()][0] != 0)
        .count() as u32
}

fn ip_count(adapter: &Adapter, ip: u32) -> Result<u32> {
    let gc = adapter.versions[HwIp::Gc.index()][0] != 0;
    match ip {
        uapi::AMDGPU_HW_IP_GFX | uapi::AMDGPU_HW_IP_COMPUTE if gc => Ok(1),
        // Do not advertise user queues until their private CS emit/fence path
        // exists. The engines are initialized, but userspace cannot submit
        // to them through ASTRA yet.
        uapi::AMDGPU_HW_IP_DMA if sdma_count(adapter) != 0 => Ok(0),
        uapi::AMDGPU_HW_IP_UVD
        | uapi::AMDGPU_HW_IP_UVD_ENC
        | uapi::AMDGPU_HW_IP_VCN_DEC
        | uapi::AMDGPU_HW_IP_VCN_ENC
        | uapi::AMDGPU_HW_IP_VCN_JPEG => Ok(0),
        uapi::AMDGPU_HW_IP_VCE => Err(Error::InvalidArgument),
        _ => Err(Error::InvalidArgument),
    }
}

fn hw_ip_info(adapter: &Adapter, ip: u32, instance: u32) -> Result<Vec<u8>> {
    if instance != 0 {
        return Err(Error::InvalidArgument);
    }
    let (version, rings, start_align, size_align) = match ip {
        uapi::AMDGPU_HW_IP_GFX => (adapter.versions[HwIp::Gc.index()][0], 2, 32, 32),
        uapi::AMDGPU_HW_IP_COMPUTE => (adapter.versions[HwIp::Gc.index()][0], 8, 32, 32),
        uapi::AMDGPU_HW_IP_DMA => (adapter.versions[HwIp::Sdma0.index()][0], 0, 256, 4),
        uapi::AMDGPU_HW_IP_UVD | uapi::AMDGPU_HW_IP_VCN_DEC => {
            (adapter.versions[HwIp::Uvd.index()][0], 0, 256, 64)
        }
        uapi::AMDGPU_HW_IP_UVD_ENC | uapi::AMDGPU_HW_IP_VCN_ENC => {
            (adapter.versions[HwIp::Uvd.index()][0], 0, 256, 4)
        }
        uapi::AMDGPU_HW_IP_VCN_JPEG => (adapter.versions[HwIp::Uvd.index()][0], 0, 256, 64),
        _ => return Err(Error::InvalidArgument),
    };
    if version == 0 {
        return Err(Error::InvalidArgument);
    }
    let version = IpVersion::from_full(version);
    Ok(HwIpInfo {
        major: version.major as u32,
        minor: version.minor as u32,
        capabilities: 0,
        ib_start_alignment: start_align,
        ib_size_alignment: size_align,
        available_rings: ring_mask(rings),
        discovery_version: ((version.major as u32) << 16)
            | ((version.minor as u32) << 8)
            | version.revision as u32,
    }
    .encode())
}

fn device_info(adapter: &Adapter) -> Vec<u8> {
    let gfx = adapter.gfx_info;
    DeviceInfo {
        device_id: adapter.info.device_id as u32,
        chip_rev: 0,
        // Linux nv_common_early_init: GC 10.3.4 external_rev = rev_id + 0x3c.
        external_rev: 0x3c,
        pci_rev: adapter.info.revision_id as u32,
        family: uapi::AMDGPU_FAMILY_NV,
        num_shader_engines: gfx.max_shader_engines,
        num_shader_arrays_per_engine: gfx.max_sh_per_se,
        gpu_counter_freq: GPU_COUNTER_FREQ_KHZ,
        max_engine_clock: adapter.clocks.max_engine_clock_khz,
        max_memory_clock: adapter.clocks.max_memory_clock_khz,
        cu_active_number: gfx.cu_active_number,
        cu_bitmap: gfx.cu_bitmap,
        enabled_rb_pipes_mask: gfx.backend_enable_mask,
        num_rb_pipes: gfx.max_backends_per_se * gfx.max_shader_engines,
        num_hw_gfx_contexts: gfx.max_hw_contexts,
        virtual_address_offset: VA_RESERVED_BOTTOM,
        virtual_address_max: VM_SIZE.saturating_sub(VA_RESERVED_TOP),
        virtual_address_alignment: 4096,
        pte_fragment_size: 4096,
        gart_page_size: 4096,
        vram_type: adapter.gmc.vram_type,
        vram_bit_width: adapter.gmc.vram_width,
        gc_double_offchip_lds_buf: gfx.double_offchip_lds_buf,
        wave_front_size: gfx.wave_front_size,
        num_shader_visible_vgprs: gfx.max_gprs,
        num_cu_per_sh: gfx.max_cu_per_sh,
        num_tcc_blocks: gfx.max_texture_channel_caches,
        gs_vgt_table_depth: gfx.gs_vgt_table_depth,
        gs_prim_buffer_depth: gfx.gs_prim_buffer_depth,
        max_gs_waves_per_vgt: gfx.max_gs_threads,
        cu_ao_bitmap: gfx.cu_ao_bitmap,
        pa_sc_tile_steering_override: gfx.pa_sc_tile_steering_override,
        min_engine_clock: adapter.clocks.min_engine_clock_khz,
        min_memory_clock: adapter.clocks.min_memory_clock_khz,
        tcp_cache_size: gfx.gc_tcp_l1_size,
        num_sqc_per_wgp: gfx.gc_num_sqc_per_wgp,
        sqc_data_cache_size: gfx.gc_l1_data_cache_size_per_sqc,
        sqc_inst_cache_size: gfx.gc_l1_instruction_cache_size_per_sqc,
        gl1c_cache_size: gfx.gc_gl1c_size_per_instance * gfx.gc_gl1c_per_sa,
        gl2c_cache_size: gfx.gc_gl2c_per_gpu,
    }
    .encode()
}

fn heaps(adapter: &Adapter) -> MemoryInfo {
    let vram_total = adapter.gmc.real_vram_size;
    let visible_total = adapter.gmc.visible_vram_size.min(vram_total);
    let gtt_total = adapter.gmc.gart_size;
    let vram_usage = adapter.mem.vram_usage().min(vram_total);
    let visible_usage = vram_usage.min(visible_total);
    let gtt_usage = adapter.mem.gart_usage().min(gtt_total);
    let vram_usable = vram_total.saturating_sub(adapter.gmc.vram_reserved_size);
    let visible_usable = visible_total.min(vram_usable);
    let gtt_usable = gtt_total;
    MemoryInfo {
        vram: HeapInfo {
            total: vram_total,
            usable: vram_usable,
            usage: vram_usage,
            max_allocation: vram_usable * 3 / 4,
        },
        visible_vram: HeapInfo {
            total: visible_total,
            usable: visible_usable,
            usage: visible_usage,
            max_allocation: visible_usable * 3 / 4,
        },
        gtt: HeapInfo {
            total: gtt_total,
            usable: gtt_usable,
            usage: gtt_usage,
            max_allocation: gtt_usable * 3 / 4,
        },
    }
}

fn firmware_info(adapter: &Adapter, fw_type: u32, instance: u32, index: u32) -> Result<Vec<u8>> {
    if instance != 0 {
        return Err(Error::InvalidArgument);
    }
    let id = match fw_type {
        uapi::AMDGPU_INFO_FW_GFX_ME => UcodeId::CpMe,
        uapi::AMDGPU_INFO_FW_GFX_PFP => UcodeId::CpPfp,
        uapi::AMDGPU_INFO_FW_GFX_CE => UcodeId::CpCe,
        uapi::AMDGPU_INFO_FW_GFX_RLC => UcodeId::RlcG,
        uapi::AMDGPU_INFO_FW_GFX_MEC => match index {
            0 => UcodeId::CpMec1,
            1 => UcodeId::CpMec2,
            _ => return Err(Error::InvalidArgument),
        },
        uapi::AMDGPU_INFO_FW_SMC => UcodeId::Smc,
        uapi::AMDGPU_INFO_FW_SDMA => match index {
            0 => UcodeId::Sdma0,
            1 => UcodeId::Sdma1,
            2 => UcodeId::Sdma2,
            3 => UcodeId::Sdma3,
            _ => return Err(Error::InvalidArgument),
        },
        uapi::AMDGPU_INFO_FW_VCN => UcodeId::Vcn,
        uapi::AMDGPU_INFO_FW_DMCUB => UcodeId::Dmcub,
        _ => return Err(Error::InvalidArgument),
    };
    let firmware = adapter
        .fw
        .iter()
        .find(|firmware| firmware.id == id)
        .ok_or(Error::InvalidArgument)?;
    let mut bytes = [0u8; 8];
    uapi::put_u32(&mut bytes, 0, firmware.fw_version);
    // GFX/RLC/SDMA v1 headers place ucode_feature_version immediately
    // after the 32-byte common header. Other firmware types report zero.
    let feature = match id {
        UcodeId::CpMe
        | UcodeId::CpPfp
        | UcodeId::CpCe
        | UcodeId::CpMec1
        | UcodeId::CpMec2
        | UcodeId::RlcG
        | UcodeId::Sdma0
        | UcodeId::Sdma1
        | UcodeId::Sdma2
        | UcodeId::Sdma3 => firmware
            .data
            .get(32..36)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
            .unwrap_or(0),
        _ => 0,
    };
    uapi::put_u32(&mut bytes, 4, feature);
    Ok(bytes.to_vec())
}

fn read_mmr_registers(adapter: &Adapter, request: &InfoRequest) -> Result<Vec<u8>> {
    const MAX_REGISTER_COUNT: u32 = 128;
    const INDEX_MASK: u32 = 0xff;
    const SH_INDEX_SHIFT: u32 = 8;

    let first = request.data[0];
    let count = request.data[1];
    let instance = request.data[2];
    let se = instance & INDEX_MASK;
    let sh = (instance >> SH_INDEX_SHIFT) & INDEX_MASK;

    // Match amdgpu_info_ioctl's bounds before consulting the ASIC-specific
    // register whitelist.  0xff means broadcast/all instances.
    if count > MAX_REGISTER_COUNT || (se != INDEX_MASK && se >= 4) || (sh != INDEX_MASK && sh >= 4)
    {
        return Err(Error::InvalidArgument);
    }

    let gb_addr_config = adapter
        .regs
        .base_u32(HwIp::Gc, 0, crate::ridx!(gc::mmGB_ADDR_CONFIG))?
        .checked_add(gc::mmGB_ADDR_CONFIG)
        .ok_or(Error::InvalidArgument)?;
    let byte_len = usize::try_from(count)
        .map_err(|_| Error::InvalidArgument)?
        .checked_mul(core::mem::size_of::<u32>())
        .ok_or(Error::InvalidArgument)?;
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(byte_len)
        .map_err(|_| Error::OutOfMemory)?;

    for index in 0..count {
        let offset = first.checked_add(index).ok_or(Error::InvalidArgument)?;
        // Linux nv_allowed_read_registers exposes this non-indexed register
        // and returns adev->gfx.config.gb_addr_config rather than issuing a
        // fresh MMIO read.  Keep the same whitelist and cached-value model;
        // arbitrary userspace MMIO access must never be allowed.
        let value = if offset == gb_addr_config {
            adapter.gfx_info.gb_addr_config
        } else {
            return Err(Error::InvalidArgument);
        };
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    Ok(bytes)
}

pub fn info(adapter: &Adapter, request: &InfoRequest) -> Result<Vec<u8>> {
    match request.query {
        uapi::AMDGPU_INFO_ACCEL_WORKING => Ok(scalar_u32(1)),
        uapi::AMDGPU_INFO_HW_IP_COUNT => Ok(scalar_u32(ip_count(adapter, request.data[0])?)),
        uapi::AMDGPU_INFO_HW_IP_INFO => hw_ip_info(adapter, request.data[0], request.data[1]),
        uapi::AMDGPU_INFO_DEV_INFO => Ok(device_info(adapter)),
        uapi::AMDGPU_INFO_TIMESTAMP => {
            // 100 MHz GPU counter: one tick per 10 ns.
            Ok(scalar_u64(
                time::monotonic().as_nanos().min(u64::MAX as u128) as u64 / 10,
            ))
        }
        uapi::AMDGPU_INFO_FW_VERSION => {
            firmware_info(adapter, request.data[0], request.data[1], request.data[2])
        }
        uapi::AMDGPU_INFO_VRAM_USAGE => Ok(scalar_u64(adapter.mem.vram_usage())),
        uapi::AMDGPU_INFO_VIS_VRAM_USAGE => Ok(scalar_u64(
            adapter.mem.vram_usage().min(adapter.gmc.visible_vram_size),
        )),
        uapi::AMDGPU_INFO_GTT_USAGE => Ok(scalar_u64(adapter.mem.gart_usage())),
        uapi::AMDGPU_INFO_VRAM_GTT => {
            let heaps = heaps(adapter);
            let mut bytes = [0u8; 24];
            uapi::put_u64(&mut bytes, 0, heaps.vram.usable);
            uapi::put_u64(&mut bytes, 8, heaps.visible_vram.usable);
            uapi::put_u64(&mut bytes, 16, heaps.gtt.usable);
            Ok(bytes.to_vec())
        }
        uapi::AMDGPU_INFO_READ_MMR_REG => read_mmr_registers(adapter, request),
        uapi::AMDGPU_INFO_MEMORY => Ok(heaps(adapter).encode()),
        uapi::AMDGPU_INFO_GDS_CONFIG => {
            let mut bytes = [0u8; 32];
            uapi::put_u32(&mut bytes, 4, 64 << 10);
            uapi::put_u32(&mut bytes, 8, 64 << 10);
            uapi::put_u32(&mut bytes, 16, 64);
            uapi::put_u32(&mut bytes, 24, 16);
            Ok(bytes.to_vec())
        }
        uapi::AMDGPU_INFO_NUM_BYTES_MOVED
        | uapi::AMDGPU_INFO_NUM_EVICTIONS
        | uapi::AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS => Ok(scalar_u64(0)),
        uapi::AMDGPU_INFO_VRAM_LOST_COUNTER | uapi::AMDGPU_INFO_RAS_ENABLED_FEATURES => {
            Ok(scalar_u32(0))
        }
        uapi::AMDGPU_INFO_MAX_IBS => Ok(max_ibs()),
        _ => Err(Error::InvalidArgument),
    }
}
