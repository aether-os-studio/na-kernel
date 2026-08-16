use alloc::boxed::Box;
use alloc::vec::Vec;

use na_std::drm::{
    Connection, Connector, ConnectorList, ConnectorType, Crtc, CrtcList, CursorUpdate, DisplayInfo,
    Driver, DumbBuffer, DumbBufferMapping, DumbBufferRequest, Encoder, EncoderList, EncoderType,
    FileId, FramebufferFormat, FramebufferInfo, FramebufferRequest, Ioctl, MmapRequest, Mode,
    PageFlip, Plane, PlaneList, PlaneType, PrimeBuffer,
};
use na_std::memory::PhysicalAddress;
use na_std::sync::Mutex;
use na_std::{Error, Result};

use crate::dev_info;
use crate::device::Adapter;

const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
const DRM_CAP_CURSOR_WIDTH: u64 = 0x8;
const DRM_CAP_CURSOR_HEIGHT: u64 = 0x9;
const DRM_MODE_CURSOR_BO: u32 = 0x1;
const DRM_MODE_CURSOR_MOVE: u32 = 0x2;
const DCN_CURSOR_SIZE: u32 = 256;
static FORMATS: [u32; 2] = [DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888];

/// DRM device state. The `'static` adapter gives exclusive access to the
/// VRAM allocator and register layer for dumb-buffer allocation.
pub struct DisplayDevice {
    adapter: Mutex<&'static mut Adapter>,
    framebuffer: na_std::boot::Framebuffer,
    vram_base: u64,
    state: Mutex<DispState>,
    files: Mutex<Vec<crate::ioctl::FileState>>,
    prime: Mutex<crate::ioctl::PrimeState>,
    framebuffers: Mutex<crate::ioctl::FramebufferState>,
    cursor: Mutex<CursorState>,
    cursor_regs: Mutex<crate::regs::DcnCursorRegs>,
    primary: Mutex<Option<crate::ioctl::ScanoutBuffer>>,
}

struct CursorState {
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

/// Registers the simpledrm-like DRM device after GMC init has populated the
/// VRAM allocator. Takes ownership of the `'static` adapter.
pub fn register(adapter: &'static mut Adapter) -> Result<()> {
    let scanout = adapter.scanout.ok_or(Error::NoDevice)?;
    let mut framebuffer = na_std::boot::framebuffer()?;
    let vram_base = adapter.vram_base;
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

    na_std::boot::rebind_framebuffer(framebuffer)?;
    dev_info!(
        "astra: TTY framebuffer rebound to BAR0 scanout at {:#x}",
        framebuffer.physical_address,
    );
    let cursor_regs = adapter.regs.dcn_cursor_regs();

    let device: &'static DisplayDevice = Box::leak(Box::new(DisplayDevice {
        adapter: Mutex::new(adapter)?,
        framebuffer,
        vram_base,
        state: Mutex::new(DispState {
            next_handle: 1,
            buffers: Vec::new(),
        })?,
        files: Mutex::new(Vec::new())?,
        prime: Mutex::new(crate::ioctl::PrimeState::new())?,
        framebuffers: Mutex::new(crate::ioctl::FramebufferState::new())?,
        cursor: Mutex::new(CursorState {
            buffer: None,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            visible: false,
        })?,
        cursor_regs: Mutex::new(cursor_regs)?,
        primary: Mutex::new(None)?,
    }));

    // Keep the DRM device attached to the real PCI function so sysfs/udev
    // expose both card and render nodes under 0000:03:00.0. The registration
    // metadata keeps the kernel/sysfs name `astra` while exposing the
    // upstream `amdgpu` UAPI name and version to libdrm/Mesa.
    let drm = {
        let adapter = device.adapter.lock();
        let pci_device = adapter.pci.as_ref().map(|pci| pci.as_device());
        na_std::drm::DeviceBuilder::new(
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
        .register(pci_device.as_ref())?
    };
    let _ = Box::leak(Box::new(drm));

    dev_info!(
        "astra: DRM device registered ({}x{} bpp {} @ {:#x})",
        framebuffer.width,
        framebuffer.height,
        framebuffer.bpp,
        framebuffer.physical_address,
    );
    Ok(())
}

/// Mode exposed through DRM. The active ASTRA scanout is currently fixed to
/// CEA-861 1920x1080p60, so report the exact timing programmed into DCN.
fn mode(width: u64, height: u64) -> Result<Mode> {
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

impl DisplayDevice {
    fn program_scanout(
        adapter: &mut Adapter,
        scanout: &crate::ioctl::ScanoutBuffer,
        geometry_changed: bool,
    ) -> Result<()> {
        if geometry_changed {
            // Linux updates DSCL RECOUT/MPC geometry when the plane is
            // enabled or its scaling/position changes, not on an ordinary
            // address-only page flip.
            crate::blocks::program_primary_geometry(
                &mut adapter.regs,
                scanout.width,
                scanout.height,
            )?;
        }
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
        if geometry_changed {
            crate::blocks::program_primary_surface(&mut adapter.regs, &config)
        } else {
            crate::blocks::program_primary_address(
                &mut adapter.regs,
                config.address,
                config.meta_address,
            )
        }
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
        let mut adapter = self.adapter.lock();
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
                if let Err(retire_error) = crate::blocks::flush_pending_gart(&mut adapter) {
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
        let mut adapter = self.adapter.lock();
        let mut files = self.files.lock();
        let Some(index) = files.iter().position(|state| state.belongs_to(file)) else {
            return;
        };
        let mut state = files.remove(index);
        if let Err(error) = crate::ioctl::release_file(&mut adapter, &mut state) {
            dev_info!("astra: releasing AMDGPU file state failed: {:?}", error);
        }
        // `FileState` owns its VM tables and remaining GEM references. Their
        // Drop implementations enqueue retirement; perform the one required
        // GART maintenance pass only after the whole ownership tree is gone.
        drop(state);
        if let Err(error) = crate::blocks::flush_pending_gart(&mut adapter) {
            dev_info!("astra: retiring AMDGPU file BOs failed: {:?}", error);
        }
    }

    fn driver_ioctl(&self, ioctl: Ioctl<'_>) -> Result<usize> {
        let raw_command = ioctl.command;
        let argument_length = ioctl.arg.len();
        let file_number = ioctl.file.map(FileId::raw).unwrap_or(0);
        let command = match crate::uapi::command(raw_command) {
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
                    let adapter = self.adapter.lock();
                    crate::ioctl::info(&adapter, &request)?
                };
                request.write_reply(&reply)?;
                return Ok(0);
            }

            let file_id = ioctl.file.ok_or(Error::InvalidArgument)?;
            let mut adapter = self.adapter.lock();
            let mut files = self.files.lock();
            let file = files
                .iter_mut()
                .find(|state| state.belongs_to(file_id))
                .ok_or(Error::NotFound)?;
            match command {
                crate::uapi::Command::GemCreate => {
                    let _ = crate::ioctl::gem_create(&mut adapter, file, ioctl.arg)?;
                }
                crate::uapi::Command::GemMmap => {
                    let _ = crate::ioctl::gem_mmap(file, ioctl.arg)?;
                }
                crate::uapi::Command::Ctx => {
                    let _ = crate::ioctl::context(file, ioctl.arg)?;
                }
                crate::uapi::Command::BoList => {
                    let _ = crate::ioctl::bo_list(&mut adapter, file, ioctl.arg)?;
                }
                crate::uapi::Command::Cs => {
                    let _ = crate::ioctl::cs(&mut adapter, file, ioctl.arg)?;
                }
                crate::uapi::Command::GemMetadata => {
                    crate::ioctl::gem_metadata(file, ioctl.arg)?;
                }
                crate::uapi::Command::GemWaitIdle => {
                    crate::ioctl::gem_wait_idle(&mut adapter, file, ioctl.arg)?;
                }
                crate::uapi::Command::GemVa => {
                    crate::ioctl::gem_va(&mut adapter, file, ioctl.arg)?;
                }
                crate::uapi::Command::WaitCs => {
                    let _ = crate::ioctl::wait_cs(&mut adapter, file, ioctl.arg)?;
                }
                crate::uapi::Command::GemOp => {
                    crate::ioctl::gem_op(file, ioctl.arg)?;
                }
                crate::uapi::Command::GemClose => {
                    let _ = crate::ioctl::gem_close(&mut adapter, file, ioctl.arg)?;
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
        let adapter = self.adapter.lock();
        let files = self.files.lock();
        let file = files
            .iter()
            .find(|state| state.belongs_to(request.file))
            .ok_or(Error::NotFound)?;
        crate::ioctl::mmap_physical(&adapter, file, request.offset, request.length)
    }

    fn prime_export(&self, file: FileId, handle: u32) -> Result<PrimeBuffer> {
        let adapter = self.adapter.lock();
        let files = self.files.lock();
        let mut prime = self.prime.lock();
        crate::ioctl::prime_export(&adapter, &files, &mut prime, file, handle)
    }

    fn prime_import(&self, file: FileId, token: u64) -> Result<u32> {
        // Serialize object reference changes with GEM_CLOSE/release_file.
        let _adapter = self.adapter.lock();
        let mut files = self.files.lock();
        let prime = self.prime.lock();
        crate::ioctl::prime_import(&mut files, &prime, file, token)
    }

    fn prime_release(&self, token: u64) -> Result<()> {
        let mut adapter = self.adapter.lock();
        let mut prime = self.prime.lock();
        crate::ioctl::prime_release(&mut adapter, &mut prime, token)
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
        let modes = [mode(self.framebuffer.width, self.framebuffer.height)?];
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
            mode: Some(mode(self.framebuffer.width, self.framebuffer.height)?),
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
            let mut guard = self.adapter.lock();
            let adapter: &mut Adapter = &mut **guard;
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
                    let _adapter = self.adapter.lock();
                    let files = self.files.lock();
                    let mut framebuffers = self.framebuffers.lock();
                    crate::ioctl::framebuffer_pin(
                        &files,
                        &mut framebuffers,
                        file,
                        request.handles[0],
                        minimum_size,
                        request.width,
                        request.height,
                        request.pitches[0],
                        request.offsets[0],
                        request.pixel_format,
                        request.flags,
                        request.modifiers[0],
                        request.pitches[1],
                        request.offsets[1],
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
        let mut adapter = self.adapter.lock();
        let mut framebuffers = self.framebuffers.lock();
        if let Err(error) =
            crate::ioctl::framebuffer_release(&mut adapter, &mut framebuffers, handle)
        {
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
        crate::ioctl::framebuffer_gem_handle(&mut files, &framebuffers, file, framebuffer_handle)
    }

    fn set_crtc(&self, update: na_std::drm::CrtcUpdate<'_>) -> Result<()> {
        if update.framebuffer_handle == 0 {
            return Ok(());
        }
        let mut adapter = self.adapter.lock();
        let framebuffers = self.framebuffers.lock();
        let mut primary = self.primary.lock();
        let scanout =
            crate::ioctl::framebuffer_scanout(&adapter, &framebuffers, update.framebuffer_handle)?;
        if scanout.width != self.framebuffer.width as u32
            || scanout.height != self.framebuffer.height as u32
            || scanout.pitch != self.framebuffer.pitch as u32
            || !FORMATS.contains(&scanout.format)
        {
            return Err(Error::InvalidArgument);
        }
        Self::program_scanout(&mut adapter, &scanout, true)?;
        *primary = Some(scanout);
        Ok(())
    }

    fn page_flip(&self, flip: PageFlip) -> Result<()> {
        let mut adapter = self.adapter.lock();
        let framebuffers = self.framebuffers.lock();
        let mut primary = self.primary.lock();
        let scanout =
            crate::ioctl::framebuffer_scanout(&adapter, &framebuffers, flip.framebuffer_handle)?;
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
        Ok(())
    }

    fn set_cursor(&self, update: CursorUpdate) -> Result<()> {
        if update.flags == 0 || update.flags & !(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE) != 0 {
            return Err(Error::InvalidArgument);
        }

        // The single exposed CRTC is backed by DCN pipe 0. Linux serializes
        // cursor motion with display state, not the global amdgpu submission
        // lock, so MOVE uses the independent direct BAR5 cursor view.
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
                let mut regs = self.cursor_regs.lock();
                crate::blocks::disable_cursor(&mut regs)?;
                cursor.buffer = None;
                cursor.width = 0;
                cursor.height = 0;
                cursor.x = x;
                cursor.y = y;
                cursor.visible = false;
                return Ok(());
            }
            if update.width == 0 || update.height == 0 || update.width > 256 || update.height > 256
            {
                return Err(Error::InvalidArgument);
            }
            // Object ownership has already been checked by the DRM core;
            // GEM lookup still uses the originating drm_file, as Linux
            // cursor_set2 does. Release these locks before touching DCN.
            let buffer = {
                let adapter = self.adapter.lock();
                let files = self.files.lock();
                let framebuffers = self.framebuffers.lock();
                crate::ioctl::cursor_pin(
                    &adapter,
                    &files,
                    &framebuffers,
                    update.file,
                    update.handle,
                    update.width,
                    update.height,
                )?
            };
            let pitch = buffer.pitch_pixels();
            let mut regs = self.cursor_regs.lock();
            crate::blocks::program_cursor_attributes(
                &mut regs,
                crate::blocks::CursorAttributes {
                    address: buffer.gpu_address(),
                    width: update.width,
                    height: update.height,
                    pitch,
                },
            )?;
            cursor.visible = crate::blocks::program_cursor_position(
                &mut regs,
                crate::blocks::CursorPosition {
                    x,
                    y,
                    width: update.width,
                    height: update.height,
                    viewport_width: self.framebuffer.width as u32,
                    viewport_height: self.framebuffer.height as u32,
                },
                cursor.visible,
            )?;
            cursor.buffer = Some(buffer);
            cursor.width = update.width;
            cursor.height = update.height;
        } else if cursor.buffer.is_some() {
            let mut regs = self.cursor_regs.lock();
            cursor.visible = crate::blocks::program_cursor_position(
                &mut regs,
                crate::blocks::CursorPosition {
                    x,
                    y,
                    width: cursor.width,
                    height: cursor.height,
                    viewport_width: self.framebuffer.width as u32,
                    viewport_height: self.framebuffer.height as u32,
                },
                cursor.visible,
            )?;
        }
        cursor.x = x;
        cursor.y = y;
        Ok(())
    }
}
