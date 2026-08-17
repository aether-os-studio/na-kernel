use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};

use na_std::drm::{
    Connection, Connector, ConnectorList, ConnectorType, Crtc, CrtcList, CursorUpdate, DisplayInfo,
    Driver, DumbBuffer, DumbBufferMapping, DumbBufferRequest, Encoder, EncoderList, EncoderType,
    FileId, FramebufferFormat, FramebufferInfo, FramebufferRequest, Ioctl, MmapRequest, Mode,
    OwnedDevice, OwnedDeviceBuilder, PageFlip, Plane, PlaneList, PlaneType, PrimeBuffer,
};
use na_std::memory::PhysicalAddress;
use na_std::sync::{Mutex, SpinLock};
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::{Adapter, Gpu};

const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
const DRM_CAP_CURSOR_WIDTH: u64 = 0x8;
const DRM_CAP_CURSOR_HEIGHT: u64 = 0x9;
const DRM_MODE_CURSOR_BO: u32 = 0x1;
const DRM_MODE_CURSOR_MOVE: u32 = 0x2;
const DCN_CURSOR_SIZE: u32 = 256;
static FORMATS: [u32; 2] = [DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888];
// Linux amdgpu_dm exposes only premultiplied-alpha ARGB8888 on the native
// cursor plane. Keeping this separate from the primary formats lets wlroots
// select the hardware cursor path instead of repainting the whole scene for
// every pointer motion.
static CURSOR_FORMATS: [u32; 1] = [DRM_FORMAT_ARGB8888];

/// DRM device state. The owned GPU gives exclusive access to the VRAM
/// allocator, IP engines and register layer.
pub struct DisplayDevice {
    gpu: Mutex<Gpu>,
    framebuffer: na_std::boot::Framebuffer,
    vram_base: u64,
    fb_start: u64,
    state: Mutex<DispState>,
    files: Mutex<Vec<crate::ioctl::FileState>>,
    prime: Mutex<crate::ioctl::PrimeState>,
    framebuffers: Mutex<crate::ioctl::FramebufferState>,
    cursor: SpinLock<CursorState>,
    primary: Mutex<Option<crate::ioctl::ScanoutBuffer>>,
    primary_handle: AtomicU32,
}

struct CursorState {
    controller: crate::blocks::DcnCursor,
    buffer: Option<crate::ioctl::CursorBuffer>,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    visible: bool,
}

struct DispState {
    next_handle: u32,
    buffers: Vec<DispBuffer>,
}

struct DispBuffer {
    handle: u32,
    /// VRAM aperture offset of the allocation.
    offset: u64,
    size: usize,
}

/// Registers the DRM device after GMC init and transfers the GPU into the
/// registration's pinned callback owner.
impl DisplayDevice {
    pub fn register(gpu: Gpu) -> Result<OwnedDevice<Self>> {
        let scanout = gpu.scanout.ok_or(Error::NoDevice)?;
        let mut framebuffer = na_std::boot::framebuffer()?;
        let vram_base = gpu.vram_base;
        framebuffer.physical_address = vram_base
            .checked_add(scanout.vram_offset)
            .ok_or(Error::InvalidArgument)?;
        framebuffer.width = scanout.width as u64;
        framebuffer.height = scanout.height as u64;
        framebuffer.bpp = 32;
        framebuffer.pitch = scanout.pitch as u64;
        framebuffer.red_mask_size = 8;
        framebuffer.red_mask_shift = 16;
        framebuffer.green_mask_size = 8;
        framebuffer.green_mask_shift = 8;
        framebuffer.blue_mask_size = 8;
        framebuffer.blue_mask_shift = 0;

        let cursor = crate::blocks::DcnCursor::new(gpu.regs.dcn_cursor_regs());
        let fb_start = gpu.gmc.fb_start;
        let pci = gpu.pci.as_device().retain();
        let pci_device = pci.as_device();

        let device = DisplayDevice {
            gpu: Mutex::new(gpu)?,
            framebuffer,
            vram_base,
            fb_start,
            state: Mutex::new(DispState {
                next_handle: 1,
                buffers: Vec::new(),
            })?,
            files: Mutex::new(Vec::new())?,
            prime: Mutex::new(crate::ioctl::PrimeState::new())?,
            framebuffers: Mutex::new(crate::ioctl::FramebufferState::new())?,
            cursor: SpinLock::new(CursorState {
                controller: cursor,
                buffer: None,
                x: 0,
                y: 0,
                width: 0,
                height: 0,
                visible: false,
            }),
            primary: Mutex::new(None)?,
            primary_handle: AtomicU32::new(0),
        };

        // Keep the DRM device attached to the real PCI function so sysfs/udev
        // expose both card and render nodes under 0000:03:00.0. The registration
        // metadata keeps the kernel/sysfs name `astra` while exposing the
        // upstream `amdgpu` UAPI name and version to libdrm/Mesa.
        let drm = OwnedDeviceBuilder::new(
            device,
            c"dri/card",
            na_std::drm::DriverInfo::new(
                c"astra",
                c"amdgpu",
                c"20260813",
                c"NAOS ASTRA AMDGPU-compatible driver",
                3,
                64,
                0,
            ),
        )
        .render_node(true)
        .register(Some(&pci_device))?;
        drm.bind_console(framebuffer)?;

        dev_info!(
            "astra: TTY framebuffer rebound to BAR0 scanout at {:#x}",
            framebuffer.physical_address,
        );

        dev_info!(
            "astra: DRM device registered ({}x{} bpp {} @ {:#x})",
            framebuffer.width,
            framebuffer.height,
            framebuffer.bpp,
            framebuffer.physical_address,
        );
        Ok(drm)
    }
}

/// Mode exposed through DRM. The active ASTRA scanout is currently fixed to
/// CEA-861 1920x1080p60, so report the exact timing programmed into DCN.
impl DisplayDevice {
    fn mode(&self) -> Result<Mode> {
        let width = self.framebuffer.width;
        let height = self.framebuffer.height;
        let w = u16::try_from(width).map_err(|_| Error::InvalidArgument)?;
        let h = u16::try_from(height).map_err(|_| Error::InvalidArgument)?;
        if (width, height) != (1920, 1080) {
            return Err(Error::Unsupported);
        }
        let mut m = Mode::new(w, h, 60);
        m.clock = 148_500;
        m.hsync_start = 2008;
        m.hsync_end = 2052;
        m.htotal = 2200;
        m.vsync_start = 1084;
        m.vsync_end = 1089;
        m.vtotal = 1125;
        m.mode_type = (1 << 3) | (1 << 6);
        Ok(m)
    }

    fn program_primary(
        adapter: &mut Adapter,
        config: &crate::blocks::PrimarySurfaceConfig,
        geometry_changed: bool,
    ) -> Result<()> {
        let mut pipe = crate::blocks::DcnDisplayPipe::new(&mut adapter.regs);
        if geometry_changed {
            // Linux updates DSCL RECOUT/MPC geometry when the plane is
            // enabled or its scaling/position changes, not on an ordinary
            // address-only page flip.
            pipe.set_primary_geometry(config.width, config.height)?;
        }
        if geometry_changed {
            pipe.set_primary_surface(config)
        } else {
            pipe.set_primary_address(config.address, config.meta_address)
        }
    }

    fn program_scanout(
        adapter: &mut Adapter,
        scanout: &crate::ioctl::ScanoutBuffer,
        geometry_changed: bool,
    ) -> Result<()> {
        let config = crate::blocks::PrimarySurfaceConfig {
            address: scanout.gpu_address(),
            meta_address: scanout.meta_address,
            width: scanout.width,
            height: scanout.height,
            pitch: scanout.pitch,
            swizzle: scanout.swizzle,
            num_pipes: scanout.num_pipes,
            pipe_interleave: scanout.pipe_interleave,
            max_compressed_frags: scanout.max_compressed_frags,
            num_pkrs: scanout.num_pkrs,
            meta_pitch: scanout.meta_pitch,
            dcc_independent_block: scanout.dcc_independent_block,
        };
        Self::program_primary(adapter, &config, geometry_changed)
    }

    fn info(&self) -> DisplayInfo {
        DisplayInfo {
            width: self.framebuffer.width as u32,
            height: self.framebuffer.height as u32,
            bits_per_pixel: self.framebuffer.bpp as u32,
        }
    }
}

impl Driver for DisplayDevice {
    fn open(&self, file: FileId) -> Result<()> {
        // Keep the global lock order used by ioctls/close: adapter, files.
        // Linux allocates the per-file VM root from VRAM during open.
        let mut adapter = self.gpu.lock();
        let mut files = self.files.lock();
        if files.iter().any(|state| state.belongs_to(file)) {
            return Err(Error::AlreadyExists);
        }
        let vmid = (1..=crate::ioctl::MAX_USER_VMIDS)
            .find(|vmid| !files.iter().any(|state| state.vmid() == *vmid))
            .ok_or(Error::NoSpace)?;
        files.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
        let state = match crate::ioctl::FileState::new(&mut adapter, file, vmid) {
            Ok(state) => state,
            Err(error) => {
                dev_info!(
                    "astra: initializing AMDGPU file state for VMID {} failed: {:?}",
                    vmid,
                    error,
                );
                if let Err(retire_error) = adapter.flush_gart() {
                    dev_info!(
                        "astra: retiring BOs after failed DRM open failed: {:?}",
                        retire_error,
                    );
                }
                return Err(error);
            }
        };
        files.push(state);
        Ok(())
    }

    fn close(&self, file: FileId) {
        // Match the lock ordering used by private ioctls: adapter, then file
        // table. This also guarantees GART invalidation completes before BO
        // DMA allocations are dropped.
        let mut adapter = self.gpu.lock();
        let mut files = self.files.lock();
        let Some(index) = files.iter().position(|state| state.belongs_to(file)) else {
            return;
        };
        let state = files.remove(index);
        if let Err(error) = state.close(&mut adapter) {
            dev_info!("astra: releasing AMDGPU file state failed: {:?}", error);
        }
        // Consuming `FileState` retires its VM tables and remaining GEM
        // references; perform the one hardware maintenance pass afterwards.
        if let Err(error) = adapter.flush_gart() {
            dev_info!("astra: retiring AMDGPU file BOs failed: {:?}", error);
        }
    }

    fn driver_ioctl(&self, ioctl: Ioctl<'_>) -> Result<usize> {
        let raw_command = ioctl.command;
        let argument_length = ioctl.arg.len();
        let file_number = ioctl.file.map(FileId::raw).unwrap_or(0);
        let command = match crate::uapi::Command::from_ioctl(raw_command) {
            Some(command) => command,
            None => {
                dev_info!(
                    "astra: unsupported AMDGPU ioctl {:#x} on file {} ({} bytes)",
                    raw_command,
                    file_number,
                    argument_length,
                );
                return Err(Error::NotATerminal);
            }
        };
        let result = (|| -> Result<usize> {
            if command == crate::uapi::Command::Info {
                let request = crate::uapi::InfoRequest::parse(ioctl.arg)?;
                let reply = {
                    let adapter = self.gpu.lock();
                    adapter.query_info(&request)?
                };
                request.write_reply(&reply)?;
                return Ok(0);
            }

            match command {
                crate::uapi::Command::GemMmap
                | crate::uapi::Command::Ctx
                | crate::uapi::Command::BoList
                | crate::uapi::Command::GemMetadata
                | crate::uapi::Command::GemOp
                | crate::uapi::Command::GemClose => {
                    let file_id = ioctl.file.ok_or(Error::InvalidArgument)?;
                    let mut files = self.files.lock();
                    let file = files
                        .iter_mut()
                        .find(|state| state.belongs_to(file_id))
                        .ok_or(Error::NotFound)?;
                    match command {
                        crate::uapi::Command::GemMmap => {
                            let _ = file.mmap_offset(ioctl.arg)?;
                        }
                        crate::uapi::Command::Ctx => {
                            let _ = file.manage_context(ioctl.arg)?;
                        }
                        crate::uapi::Command::BoList => {
                            let _ = file.manage_bo_list(ioctl.arg)?;
                        }
                        crate::uapi::Command::GemMetadata => file.metadata(ioctl.arg)?,
                        crate::uapi::Command::GemOp => file.operate_bo(ioctl.arg)?,
                        crate::uapi::Command::GemClose => {
                            let _ = file.close_bo(ioctl.arg)?;
                        }
                        _ => unreachable!(),
                    }
                }
                crate::uapi::Command::GemCreate
                | crate::uapi::Command::Cs
                | crate::uapi::Command::GemWaitIdle
                | crate::uapi::Command::GemVa
                | crate::uapi::Command::WaitCs => {
                    let file_id = ioctl.file.ok_or(Error::InvalidArgument)?;
                    let mut adapter = self.gpu.lock();
                    let mut files = self.files.lock();
                    let file = files
                        .iter_mut()
                        .find(|state| state.belongs_to(file_id))
                        .ok_or(Error::NotFound)?;
                    match command {
                        crate::uapi::Command::GemCreate => {
                            let _ = file.create_bo(&mut adapter, ioctl.arg)?;
                        }
                        crate::uapi::Command::Cs => {
                            let _ = file.submit(&mut adapter, ioctl.arg)?;
                        }
                        crate::uapi::Command::GemWaitIdle => {
                            file.wait_bo(&mut adapter, ioctl.arg)?;
                        }
                        crate::uapi::Command::GemVa => {
                            file.update_va(&mut adapter, ioctl.arg)?;
                        }
                        crate::uapi::Command::WaitCs => {
                            let _ = file.wait_submission(&mut adapter, ioctl.arg)?;
                        }
                        _ => unreachable!(),
                    }
                }
                crate::uapi::Command::Info => unreachable!(),
            }
            Ok(0)
        })();
        if let Err(error) = &result {
            if command == crate::uapi::Command::Info && argument_length == 32 {
                let query = crate::uapi::read_u32(ioctl.arg, 12).unwrap_or(u32::MAX);
                let data = [
                    crate::uapi::read_u32(ioctl.arg, 16).unwrap_or(u32::MAX),
                    crate::uapi::read_u32(ioctl.arg, 20).unwrap_or(u32::MAX),
                    crate::uapi::read_u32(ioctl.arg, 24).unwrap_or(u32::MAX),
                    crate::uapi::read_u32(ioctl.arg, 28).unwrap_or(u32::MAX),
                ];
                dev_info!(
                    "astra: AMDGPU Info query {:#x} data={:x?} ioctl {:#x} failed on file {} ({} bytes): {:?}",
                    query,
                    data,
                    raw_command,
                    file_number,
                    argument_length,
                    error,
                );
            } else {
                dev_info!(
                    "astra: AMDGPU {:?} ioctl {:#x} failed on file {} ({} bytes): {:?}",
                    command,
                    raw_command,
                    file_number,
                    argument_length,
                    error,
                );
            }
        }
        result
    }

    fn mmap(&self, request: MmapRequest) -> Result<PhysicalAddress> {
        let adapter = self.gpu.lock();
        let files = self.files.lock();
        let file = files
            .iter()
            .find(|state| state.belongs_to(request.file))
            .ok_or(Error::NotFound)?;
        file.physical_mapping(&adapter, request.offset, request.length)
    }

    fn prime_export(&self, file: FileId, handle: u32) -> Result<PrimeBuffer> {
        let adapter = self.gpu.lock();
        let files = self.files.lock();
        let mut prime = self.prime.lock();
        prime.export(&adapter, &files, file, handle)
    }

    fn prime_import(&self, file: FileId, token: u64) -> Result<u32> {
        // The file table owns handle serialization; importing an existing
        // object does not touch VM page tables or device registers.
        let mut files = self.files.lock();
        let prime = self.prime.lock();
        prime.import(&mut files, file, token)
    }

    fn prime_release(&self, token: u64) -> Result<()> {
        let mut adapter = self.gpu.lock();
        let mut prime = self.prime.lock();
        prime.remove(token)?;
        adapter.flush_gart()
    }

    fn capability(&self, capability: u64) -> Result<u64> {
        match capability {
            DRM_CAP_CURSOR_WIDTH | DRM_CAP_CURSOR_HEIGHT => Ok(DCN_CURSOR_SIZE as u64),
            _ => Err(Error::Unsupported),
        }
    }

    fn display_info(&self) -> Result<DisplayInfo> {
        Ok(self.info())
    }

    fn framebuffer(&self) -> Result<FramebufferInfo> {
        Ok(FramebufferInfo {
            display: self.info(),
            physical_address: PhysicalAddress::new(self.framebuffer.physical_address),
        })
    }

    fn connectors(&self, resources: &mut ConnectorList<'_>) -> Result<()> {
        let modes = [self.mode()?];
        resources.push(Connector {
            connector_type: ConnectorType::Virtual,
            connection: Connection::Connected,
            encoder_index: Some(0),
            crtc_index: Some(0),
            mm_width: 0,
            mm_height: 0,
            subpixel: 0,
            modes: &modes,
        })
    }

    fn crtcs(&self, resources: &mut CrtcList<'_>) -> Result<()> {
        let info = self.info();
        resources.push(Crtc {
            x: 0,
            y: 0,
            width: info.width,
            height: info.height,
            gamma_size: 0,
            mode: Some(self.mode()?),
        })
    }

    fn encoders(&self, resources: &mut EncoderList<'_>) -> Result<()> {
        resources.push(Encoder {
            encoder_type: EncoderType::Virtual,
            crtc_index: Some(0),
            possible_crtcs: 1,
            possible_clones: 0,
        })
    }

    fn planes(&self, resources: &mut PlaneList<'_>) -> Result<()> {
        resources.push(Plane {
            crtc_index: Some(0),
            possible_crtcs: 1,
            gamma_size: 0,
            plane_type: PlaneType::Primary,
            formats: &FORMATS,
        })?;
        resources.push(Plane {
            crtc_index: Some(0),
            possible_crtcs: 1,
            gamma_size: 0,
            plane_type: PlaneType::Cursor,
            formats: &CURSOR_FORMATS,
        })
    }

    fn create_dumb_buffer(&self, request: DumbBufferRequest) -> Result<DumbBuffer> {
        if request.bits_per_pixel != 32 || request.width == 0 || request.height == 0 {
            return Err(Error::InvalidArgument);
        }
        let pitch = request.width.checked_mul(4).ok_or(Error::InvalidArgument)?;
        let size = pitch
            .checked_mul(request.height)
            .ok_or(Error::InvalidArgument)? as usize;

        let (offset, alloc_size) = {
            let mut guard = self.gpu.lock();
            let adapter: &mut Adapter = &mut guard;
            let bo = adapter.mem.alloc_vram(&mut adapter.regs, size)?;
            (bo.gpu_addr, bo.size)
        };

        let mut state = self.state.lock();
        let handle = state.next_handle;
        state.next_handle = state.next_handle.checked_add(1).ok_or(Error::NoSpace)?;
        state.buffers.push(DispBuffer {
            handle,
            offset,
            size: alloc_size,
        });

        Ok(DumbBuffer {
            handle,
            pitch,
            size: size as u64,
        })
    }

    fn destroy_dumb_buffer(&self, handle: u32) -> Result<()> {
        let mut state = self.state.lock();
        if let Some(pos) = state.buffers.iter().position(|b| b.handle == handle) {
            state.buffers.remove(pos);
        }
        Ok(())
    }

    fn map_dumb_buffer(&self, handle: u32) -> Result<u64> {
        let state = self.state.lock();
        state
            .buffers
            .iter()
            .find(|b| b.handle == handle)
            .map(|_| 0x1_0000_0000u64 + (handle as u64) * 0x1_0000_0000u64)
            .ok_or(Error::NoDevice)
    }

    fn dumb_buffer_mapping(&self, handle: u32) -> Result<DumbBufferMapping> {
        let state = self.state.lock();
        let buffer = state
            .buffers
            .iter()
            .find(|b| b.handle == handle)
            .ok_or(Error::NoDevice)?;
        Ok(DumbBufferMapping {
            physical_address: PhysicalAddress::new(self.vram_base + buffer.offset),
            length: buffer.size,
        })
    }

    fn create_framebuffer(&self, request: FramebufferRequest) -> Result<FramebufferFormat> {
        if request.pixel_format != 0 && !FORMATS.contains(&request.pixel_format) {
            return Err(Error::Unsupported);
        }
        let mut driver_handle = request.handles[0];
        if request.handles[0] != 0 {
            if let Some(file) = request.file {
                let minimum_size = (request.offsets[0] as u64)
                    .checked_add(
                        (request.pitches[0] as u64)
                            .checked_mul(request.height as u64)
                            .ok_or(Error::InvalidArgument)?,
                    )
                    .ok_or(Error::InvalidArgument)?;

                let pinned = {
                    let files = self.files.lock();
                    let mut framebuffers = self.framebuffers.lock();
                    framebuffers.pin(
                        &files,
                        file,
                        request.handles[0],
                        crate::ioctl::FramebufferConfig {
                            minimum_size,
                            width: request.width,
                            height: request.height,
                            pitch: request.pitches[0],
                            offset: request.offsets[0],
                            format: request.pixel_format,
                            flags: request.flags,
                            modifier: request.modifiers[0],
                            meta_pitch: request.pitches[1],
                            meta_offset: request.offsets[1],
                        },
                    )
                };
                match pinned {
                    Ok(handle) => driver_handle = handle,
                    Err(Error::NotFound) => {
                        let state = self.state.lock();
                        state
                            .buffers
                            .iter()
                            .find(|b| b.handle == request.handles[0])
                            .ok_or(Error::NoDevice)?;
                    }
                    Err(error) => return Err(error),
                }
            } else {
                let state = self.state.lock();
                state
                    .buffers
                    .iter()
                    .find(|b| b.handle == request.handles[0])
                    .ok_or(Error::NoDevice)?;
            }
        }
        Ok(FramebufferFormat {
            bits_per_pixel: 32,
            depth: 24,
            driver_handle,
        })
    }

    fn release_framebuffer(&self, handle: u32) {
        if handle < 0x8000_0000 {
            return;
        }
        let mut adapter = self.gpu.lock();
        let mut framebuffers = self.framebuffers.lock();
        let result = framebuffers
            .remove(handle)
            .and_then(|()| adapter.flush_gart());
        if let Err(error) = result {
            dev_info!(
                "astra: releasing KMS framebuffer object {} failed: {:?}",
                handle,
                error
            );
        }
    }

    fn framebuffer_handle(&self, file: FileId, framebuffer_handle: u32) -> Result<u32> {
        let mut files = self.files.lock();
        let framebuffers = self.framebuffers.lock();
        framebuffers.gem_handle(&mut files, file, framebuffer_handle)
    }

    fn set_crtc(&self, update: na_std::drm::CrtcUpdate<'_>) -> Result<()> {
        if update.framebuffer_handle == 0 {
            return Ok(());
        }
        let mut adapter = self.gpu.lock();
        let framebuffers = self.framebuffers.lock();
        let mut primary = self.primary.lock();
        let scanout = framebuffers.scanout(&adapter, update.framebuffer_handle)?;
        if scanout.width != self.framebuffer.width as u32
            || scanout.height != self.framebuffer.height as u32
            || scanout.pitch != self.framebuffer.pitch as u32
            || !FORMATS.contains(&scanout.format)
        {
            return Err(Error::InvalidArgument);
        }
        Self::program_scanout(&mut adapter, &scanout, true)?;
        *primary = Some(scanout);
        self.primary_handle
            .store(update.framebuffer_handle, Ordering::Release);
        Ok(())
    }

    fn page_flip(&self, flip: PageFlip) -> Result<()> {
        // A legacy cursor-only wlroots commit carries the current primary FB
        // through DRM_MODE_PAGE_FLIP to obtain the next vblank event. Linux's
        // atomic helpers leave an unchanged primary plane untouched, so avoid
        // taking the device submission lock or rewriting HUBP in this case.
        if flip.framebuffer_handle != 0
            && self.primary_handle.load(Ordering::Acquire) == flip.framebuffer_handle
        {
            return Ok(());
        }
        let mut adapter = self.gpu.lock();
        let framebuffers = self.framebuffers.lock();
        let mut primary = self.primary.lock();
        let scanout = framebuffers.scanout(&adapter, flip.framebuffer_handle)?;
        if scanout.width != self.framebuffer.width as u32
            || scanout.height != self.framebuffer.height as u32
            || scanout.pitch != self.framebuffer.pitch as u32
            || !FORMATS.contains(&scanout.format)
        {
            return Err(Error::InvalidArgument);
        }
        // Linux's fast-flip path calls only program_surface_flip_and_addr
        // while the FB layout is unchanged.  A modifier/DCC/pitch change is
        // a full plane update and must reprogram HUBP/DPP state.
        let layout_changed = primary
            .as_ref()
            .is_none_or(|current| !current.has_same_layout(&scanout));
        Self::program_scanout(&mut adapter, &scanout, layout_changed)?;
        *primary = Some(scanout);
        self.primary_handle
            .store(flip.framebuffer_handle, Ordering::Release);
        Ok(())
    }

    fn set_cursor(&self, update: CursorUpdate) -> Result<()> {
        if update.flags == 0 || update.flags & !(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE) != 0 {
            return Err(Error::InvalidArgument);
        }

        // GEM/framebuffer lookup takes sleeping locks, so resolve a new cursor
        // surface before entering the short MMIO critical section.  MOVE-only
        // updates consequently take only the cursor spinlock and perform the
        // three direct register writes used by Linux's DCN position path.
        let replacement = if update.flags & DRM_MODE_CURSOR_BO != 0 && update.handle != 0 {
            if update.width == 0
                || update.height == 0
                || update.width > DCN_CURSOR_SIZE
                || update.height > DCN_CURSOR_SIZE
            {
                return Err(Error::InvalidArgument);
            }
            let files = self.files.lock();
            let framebuffers = self.framebuffers.lock();
            Some(framebuffers.cursor_buffer(
                self.fb_start,
                &files,
                update.file,
                update.handle,
                update.width,
                update.height,
            )?)
        } else {
            None
        };

        // The single exposed CRTC is backed by DCN pipe 0. Linux serializes
        // cursor motion with display state, not the global amdgpu submission
        // lock, so MOVE uses the independent direct BAR5 cursor view.
        let mut _retired_buffer = None;
        let mut cursor = self.cursor.lock();

        let x = if update.flags & DRM_MODE_CURSOR_MOVE != 0 {
            update.x
        } else {
            cursor.x
        };
        let y = if update.flags & DRM_MODE_CURSOR_MOVE != 0 {
            update.y
        } else {
            cursor.y
        };

        if update.flags & DRM_MODE_CURSOR_BO != 0 {
            if update.handle == 0 {
                if cursor.buffer.is_some() || cursor.visible {
                    cursor.controller.disable()?;
                }
                _retired_buffer = cursor.buffer.take();
                cursor.width = 0;
                cursor.height = 0;
                cursor.x = x;
                cursor.y = y;
                cursor.visible = false;
                return Ok(());
            }
            let buffer = replacement.ok_or(Error::InvalidArgument)?;
            let pitch = buffer.pitch_pixels();
            let surface_changed = cursor.buffer.as_ref().is_none_or(|current| {
                !current.is_same_surface(&buffer)
                    || cursor.width != update.width
                    || cursor.height != update.height
            });
            if surface_changed {
                cursor
                    .controller
                    .set_attributes(crate::blocks::CursorAttributes {
                        address: buffer.gpu_address(),
                        width: update.width,
                        height: update.height,
                        pitch,
                    })?;
            }
            if surface_changed || x != cursor.x || y != cursor.y {
                let was_visible = cursor.visible;
                cursor.visible = cursor.controller.set_position(
                    crate::blocks::CursorPosition {
                        x,
                        y,
                        width: update.width,
                        height: update.height,
                        viewport_width: self.framebuffer.width as u32,
                        viewport_height: self.framebuffer.height as u32,
                    },
                    was_visible,
                )?;
            }
            if surface_changed {
                _retired_buffer = cursor.buffer.replace(buffer);
            }
            cursor.width = update.width;
            cursor.height = update.height;
        } else if cursor.buffer.is_some() && (x != cursor.x || y != cursor.y) {
            let was_visible = cursor.visible;
            let width = cursor.width;
            let height = cursor.height;
            let visible = cursor.controller.set_position(
                crate::blocks::CursorPosition {
                    x,
                    y,
                    width,
                    height,
                    viewport_width: self.framebuffer.width as u32,
                    viewport_height: self.framebuffer.height as u32,
                },
                was_visible,
            )?;
            cursor.visible = visible;
        }
        cursor.x = x;
        cursor.y = y;
        Ok(())
    }

    fn restore_console(&self) -> Result<()> {
        let offset = self
            .framebuffer
            .physical_address
            .checked_sub(self.vram_base)
            .ok_or(Error::Range)?;
        let address = self.fb_start.checked_add(offset).ok_or(Error::Range)?;
        let surface = crate::blocks::PrimarySurfaceConfig::linear(
            address,
            self.framebuffer.width as u32,
            self.framebuffer.height as u32,
            self.framebuffer.pitch as u32,
        );

        let mut adapter = self.gpu.lock();
        let mut primary = self.primary.lock();
        Self::program_primary(&mut adapter, &surface, true)?;
        *primary = None;
        self.primary_handle.store(0, Ordering::Release);
        drop(primary);
        drop(adapter);

        let mut cursor = self.cursor.lock();
        if cursor.buffer.is_some() || cursor.visible {
            cursor.controller.disable()?;
        }
        cursor.buffer = None;
        cursor.visible = false;
        Ok(())
    }
}
