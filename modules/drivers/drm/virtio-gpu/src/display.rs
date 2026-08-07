use na_std::{
    Error, KernelLog, Result,
    drm::{
        AtomicCommit, Connection, Connector, ConnectorList, ConnectorType, Crtc, CrtcList,
        CrtcUpdate, CursorUpdate, DisplayInfo, Driver, DumbBuffer, DumbBufferMapping,
        DumbBufferRequest, Encoder, EncoderList, EncoderType, FileId, FramebufferFormat,
        FramebufferInfo, FramebufferRequest, FramebufferUpdate, Ioctl, Mode, PageFlip, Plane,
        PlaneList, PlaneType, PlaneUpdate,
    },
};

use crate::{
    buffer::{Buffer, BufferKind},
    device::{GpuDevice, State},
    protocol::{self, Command},
};

pub struct DisplayState {
    pub info: DisplayInfo,
    pub changed: bool,
}

impl DisplayState {
    fn mode(info: DisplayInfo) -> Result<Mode> {
        let width = u16::try_from(info.width).map_err(|_| Error::InvalidArgument)?;
        let height = u16::try_from(info.height).map_err(|_| Error::InvalidArgument)?;
        let hsync_start = info.width.checked_add(16).ok_or(Error::InvalidArgument)?;
        let hsync_end = hsync_start.checked_add(96).ok_or(Error::InvalidArgument)?;
        let htotal = hsync_end.checked_add(48).ok_or(Error::InvalidArgument)?;
        let vsync_start = info.height.checked_add(10).ok_or(Error::InvalidArgument)?;
        let vsync_end = vsync_start.checked_add(2).ok_or(Error::InvalidArgument)?;
        let vtotal = vsync_end.checked_add(33).ok_or(Error::InvalidArgument)?;
        let clock = ((htotal as u64 * vtotal as u64 * 60 + 500) / 1000) as u32;
        let mut mode = Mode::new(width, height, 60);
        mode.clock = clock;
        mode.hsync_start = u16::try_from(hsync_start).map_err(|_| Error::InvalidArgument)?;
        mode.hsync_end = u16::try_from(hsync_end).map_err(|_| Error::InvalidArgument)?;
        mode.htotal = u16::try_from(htotal).map_err(|_| Error::InvalidArgument)?;
        mode.vsync_start = u16::try_from(vsync_start).map_err(|_| Error::InvalidArgument)?;
        mode.vsync_end = u16::try_from(vsync_end).map_err(|_| Error::InvalidArgument)?;
        mode.vtotal = u16::try_from(vtotal).map_err(|_| Error::InvalidArgument)?;
        mode.mode_type = (1 << 3) | (1 << 6);
        Ok(mode)
    }

    pub fn query(queue: &na_std::virtio::Queue) -> Result<Self> {
        KernelLog::write(c"virtio-gpu: querying display info\n");
        let request = Command::<24>::new(protocol::CMD_GET_DISPLAY_INFO);
        let mut response = [0; 24 + protocol::MAX_SCANOUTS * 24];
        queue.submit(request.bytes(), None, &mut response)?;
        if let Err(error) = protocol::check_response(&response, protocol::RESP_OK_DISPLAY_INFO) {
            KernelLog::write(c"virtio-gpu: display query response mismatch\n");
            return Err(error);
        }
        let enabled = u32::from_le_bytes(response[40..44].try_into().unwrap());
        let width = u32::from_le_bytes(response[32..36].try_into().unwrap());
        let height = u32::from_le_bytes(response[36..40].try_into().unwrap());
        if enabled == 0 || width == 0 || height == 0 {
            return Err(Error::NoDevice);
        }
        Ok(Self {
            info: DisplayInfo {
                width,
                height,
                bits_per_pixel: 32,
            },
            changed: false,
        })
    }
}

impl GpuDevice {
    fn create_resource(
        &self,
        state: &mut State,
        width: u32,
        height: u32,
        format: u32,
    ) -> Result<Buffer> {
        let handle = state.next_handle;
        state.next_handle = state.next_handle.checked_add(1).ok_or(Error::NoSpace)?;
        let resource_id = state.next_resource;
        state.next_resource = state.next_resource.checked_add(1).ok_or(Error::NoSpace)?;
        let pitch = width
            .checked_mul(4)
            .and_then(|value| value.checked_add(63))
            .map(|value| value & !63)
            .ok_or(Error::OutOfMemory)?;
        let buffer = Buffer::new(handle, resource_id, width, height, pitch)?;
        let mut create = Command::<40>::new(protocol::CMD_RESOURCE_CREATE_2D);
        create.put_u32(24, resource_id);
        create.put_u32(28, format);
        create.put_u32(32, width);
        create.put_u32(36, height);
        let mut response = [0; 24];
        self.submit(&create, None, &mut response, protocol::RESP_OK_NODATA)?;
        let entry = buffer.attach_entry();
        let mut attach = Command::<32>::new(protocol::CMD_RESOURCE_ATTACH_BACKING);
        attach.put_u32(24, resource_id);
        attach.put_u32(28, 1);
        self.submit(
            &attach,
            Some(&entry),
            &mut response,
            protocol::RESP_OK_NODATA,
        )?;
        Ok(buffer)
    }

    fn present(
        &self,
        buffer: &Buffer,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        set_scanout: bool,
    ) -> Result<()> {
        if x >= buffer.width || y >= buffer.height {
            return Ok(());
        }
        let width = width.min(buffer.width - x);
        let height = height.min(buffer.height - y);
        if width == 0 || height == 0 {
            return Ok(());
        }

        let mut response = [0; 24];
        if set_scanout {
            let mut scanout = Command::<56>::new(protocol::CMD_SET_SCANOUT);
            protocol::rect(scanout.bytes_mut(), 24, 0, 0, buffer.width, buffer.height);
            scanout.put_u32(40, 0);
            scanout.put_u32(44, buffer.resource_id);
            self.submit(&scanout, None, &mut response, protocol::RESP_OK_NODATA)?;
        }
        if buffer.kind == BufferKind::Dumb2D {
            buffer.memory.sync_for_device();
            let mut transfer = Command::<56>::new(protocol::CMD_TRANSFER_TO_HOST_2D);
            protocol::rect(transfer.bytes_mut(), 24, x, y, width, height);
            transfer.put_u64(40, y as u64 * buffer.pitch as u64 + x as u64 * 4);
            transfer.put_u32(48, buffer.resource_id);
            self.submit(&transfer, None, &mut response, protocol::RESP_OK_NODATA)?;
        }
        let mut flush = Command::<48>::new(protocol::CMD_RESOURCE_FLUSH);
        protocol::rect(flush.bytes_mut(), 24, x, y, width, height);
        flush.put_u32(40, buffer.resource_id);
        self.submit(&flush, None, &mut response, protocol::RESP_OK_NODATA)
    }
}

const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
const DRM_FORMAT_XBGR8888: u32 = 0x3432_4258;
const DRM_FORMAT_ABGR8888: u32 = 0x3432_4241;
static FORMATS: [u32; 4] = [
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_ABGR8888,
];

impl Driver for GpuDevice {
    fn open(&self, file: FileId) -> Result<()> {
        self.drm_open(file)
    }

    fn close(&self, file: FileId) {
        self.drm_close(file);
    }

    fn driver_ioctl(&self, ioctl: Ioctl<'_>) -> Result<usize> {
        self.drm_ioctl(ioctl)
    }

    fn display_info(&self) -> Result<DisplayInfo> {
        Ok(self.state.lock().display.info)
    }

    fn framebuffer(&self) -> Result<FramebufferInfo> {
        let state = self.state.lock();
        let handle = state.current_scanout.ok_or(Error::NoDevice)?;
        let buffer = Self::find_buffer(&state, handle)?;
        Ok(FramebufferInfo {
            display: state.display.info,
            physical_address: buffer.memory.physical_address(),
        })
    }

    fn create_dumb_buffer(&self, request: DumbBufferRequest) -> Result<DumbBuffer> {
        if request.bits_per_pixel != 32 || request.width == 0 || request.height == 0 {
            return Err(Error::InvalidArgument);
        }
        let mut state = self.state.lock();
        let buffer = self.create_resource(
            &mut state,
            request.width,
            request.height,
            protocol::FORMAT_B8G8R8X8_UNORM,
        )?;
        let result = DumbBuffer {
            handle: buffer.handle,
            pitch: buffer.pitch,
            size: buffer.memory.length() as u64,
        };
        state.buffers.push(buffer)?;
        Ok(result)
    }

    fn destroy_dumb_buffer(&self, handle: u32) -> Result<()> {
        let mut state = self.state.lock();
        if Self::find_buffer(&state, handle)?.kind != BufferKind::Dumb2D {
            return Err(Error::InvalidArgument);
        }
        self.put_buffer(&mut state, handle)
    }

    fn map_dumb_buffer(&self, handle: u32) -> Result<u64> {
        Self::find_buffer(&self.state.lock(), handle)?;
        Ok(0x1_0000_0000u64 + (handle as u64) * 0x1_0000_0000u64)
    }

    fn dumb_buffer_mapping(&self, handle: u32) -> Result<DumbBufferMapping> {
        let state = self.state.lock();
        let buffer = Self::find_buffer(&state, handle)?;
        Ok(DumbBufferMapping {
            physical_address: buffer.memory.physical_address(),
            length: buffer.memory.length(),
        })
    }

    fn create_framebuffer(&self, request: FramebufferRequest) -> Result<FramebufferFormat> {
        if request.pixel_format != 0 && !FORMATS.contains(&request.pixel_format) {
            return Err(Error::Unsupported);
        }
        let mut state = self.state.lock();
        let buffer = Self::find_buffer_mut(&mut state, request.handles[0])?;
        if buffer.width < request.width || buffer.height < request.height {
            return Err(Error::InvalidArgument);
        }
        let depth = matches!(
            request.pixel_format,
            DRM_FORMAT_ARGB8888 | DRM_FORMAT_ABGR8888
        ) as u32
            * 8
            + 24;
        buffer.ref_count = buffer.ref_count.checked_add(1).ok_or(Error::NoSpace)?;
        Ok(FramebufferFormat {
            bits_per_pixel: 32,
            depth,
        })
    }

    fn release_framebuffer(&self, handle: u32) {
        let _ = self.put_buffer(&mut self.state.lock(), handle);
    }

    fn dirty_framebuffer(&self, update: FramebufferUpdate<'_>) -> Result<()> {
        let state = self.state.lock();
        let buffer = Self::find_buffer(&state, update.framebuffer_handle)?;
        if update.clips.is_empty() {
            return self.present(buffer, 0, 0, buffer.width, buffer.height, false);
        }
        for clip in update.clips {
            self.present(
                buffer,
                clip.x1() as u32,
                clip.y1() as u32,
                clip.x2().saturating_sub(clip.x1()) as u32,
                clip.y2().saturating_sub(clip.y1()) as u32,
                false,
            )?;
        }
        Ok(())
    }

    fn connectors(&self, resources: &mut ConnectorList<'_>) -> Result<()> {
        let info = self.state.lock().display.info;
        let modes = [DisplayState::mode(info)?];
        resources.push(Connector {
            connector_type: ConnectorType::Virtual,
            connection: Connection::Connected,
            encoder_index: Some(0),
            crtc_index: Some(0),
            mm_width: 0,
            mm_height: 0,
            subpixel: 0,
            modes: &modes,
        })?;
        Ok(())
    }

    fn crtcs(&self, resources: &mut CrtcList<'_>) -> Result<()> {
        let info = self.state.lock().display.info;
        let mode = DisplayState::mode(info)?;
        resources.push(Crtc {
            x: 0,
            y: 0,
            width: info.width,
            height: info.height,
            gamma_size: 0,
            mode: Some(mode),
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

    fn set_crtc(&self, update: CrtcUpdate<'_>) -> Result<()> {
        let mut state = self.state.lock();
        let resource_id = if update.framebuffer_handle == 0 {
            0
        } else {
            Self::find_buffer(&state, update.framebuffer_handle)?.resource_id
        };
        let width = update
            .mode
            .map(|mode| mode.hdisplay as u32)
            .unwrap_or(state.display.info.width);
        let height = update
            .mode
            .map(|mode| mode.vdisplay as u32)
            .unwrap_or(state.display.info.height);
        if resource_id != 0 {
            let set_scanout = state.current_scanout != Some(update.framebuffer_handle);
            let buffer = Self::find_buffer(&state, update.framebuffer_handle)?;
            self.present(buffer, update.x, update.y, width, height, set_scanout)?;
        } else if state.current_scanout.is_some() {
            let mut command = Command::<56>::new(protocol::CMD_SET_SCANOUT);
            protocol::rect(command.bytes_mut(), 24, 0, 0, 0, 0);
            command.put_u32(40, 0);
            command.put_u32(44, 0);
            let mut response = [0; 24];
            self.submit(&command, None, &mut response, protocol::RESP_OK_NODATA)?;
        }
        state.current_scanout = (resource_id != 0).then_some(update.framebuffer_handle);
        Ok(())
    }

    fn set_plane(&self, update: PlaneUpdate) -> Result<()> {
        self.set_crtc(CrtcUpdate {
            crtc_id: update.crtc_id,
            framebuffer_id: update.framebuffer_id,
            framebuffer_handle: update.framebuffer_handle,
            x: update.crtc_x.max(0) as u32,
            y: update.crtc_y.max(0) as u32,
            gamma_size: 0,
            mode: None,
            connector_ids: &[],
        })
    }

    fn page_flip(&self, flip: PageFlip) -> Result<()> {
        let mut state = self.state.lock();
        let set_scanout = state.current_scanout != Some(flip.framebuffer_handle);
        let buffer = Self::find_buffer(&state, flip.framebuffer_handle)?;
        self.present(buffer, 0, 0, buffer.width, buffer.height, set_scanout)?;
        state.current_scanout = Some(flip.framebuffer_handle);
        Ok(())
    }

    fn set_cursor(&self, _cursor: CursorUpdate) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn atomic_commit(&self, _commit: AtomicCommit<'_>) -> Result<()> {
        const PROPERTY_FB_ID: u32 = 3;
        const TEST_ONLY: u32 = 0x0100;

        let mut framebuffer = None;
        for property in _commit.properties {
            if property.property_id() == PROPERTY_FB_ID {
                framebuffer = Some((
                    property.value() as u32,
                    property.framebuffer_handle().unwrap_or(0),
                ));
            }
        }
        if _commit.flags & TEST_ONLY != 0 {
            if let Some((_, handle)) = framebuffer {
                if handle != 0 {
                    Self::find_buffer(&self.state.lock(), handle)?;
                }
            }
            return Ok(());
        }

        let Some((framebuffer_id, framebuffer_handle)) = framebuffer else {
            return Ok(());
        };
        self.set_crtc(CrtcUpdate {
            crtc_id: 0,
            framebuffer_id,
            framebuffer_handle,
            x: 0,
            y: 0,
            gamma_size: 0,
            mode: None,
            connector_ids: &[],
        })
    }
}
