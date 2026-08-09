use alloc::vec::Vec;
use na_std::{
    Error, Result,
    drm::{FileId, Ioctl},
    memory::KernelBuffer,
    user::UserAddress,
};

use crate::{
    buffer::{Buffer, BufferKind},
    device::{Context, FileState, GpuDevice},
    protocol::{self, Command},
};

const DRM_IOCTL_PRIME_HANDLE_TO_FD: u32 = 0xc00c_642d;
const DRM_IOCTL_PRIME_FD_TO_HANDLE: u32 = 0xc00c_642e;
const DRM_IOCTL_GEM_CLOSE: u32 = 0x4008_6409;
const DRM_IOCTL_VIRTGPU_MAP: u32 = 0xc010_6441;
const DRM_IOCTL_VIRTGPU_EXECBUFFER: u32 = 0xc040_6442;
const DRM_IOCTL_VIRTGPU_GETPARAM: u32 = 0xc010_6443;
const DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: u32 = 0xc038_6444;
const DRM_IOCTL_VIRTGPU_RESOURCE_INFO: u32 = 0xc010_6445;
const DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: u32 = 0xc02c_6446;
const DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST: u32 = 0xc02c_6447;
const DRM_IOCTL_VIRTGPU_WAIT: u32 = 0xc008_6448;
const DRM_IOCTL_VIRTGPU_GET_CAPS: u32 = 0xc018_6449;
const DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB: u32 = 0xc030_644a;
const DRM_IOCTL_VIRTGPU_CONTEXT_INIT: u32 = 0xc010_644b;

const PARAM_3D_FEATURES: u64 = 1;
const PARAM_CAPSET_QUERY_FIX: u64 = 2;
const PARAM_RESOURCE_BLOB: u64 = 3;
const PARAM_HOST_VISIBLE: u64 = 4;
const PARAM_CROSS_DEVICE: u64 = 5;
const PARAM_CONTEXT_INIT: u64 = 6;
const PARAM_SUPPORTED_CAPSET_IDS: u64 = 7;
const PARAM_EXPLICIT_DEBUG_NAME: u64 = 8;
const CAPSET_VIRGL: u32 = 1;
const CAPSET_VIRGL2: u32 = 2;
const WAIT_NOWAIT: u32 = 1;
const EXECBUF_FENCE_FD_IN: u32 = 1;
const EXECBUF_FENCE_FD_OUT: u32 = 2;
const EXECBUF_RING_IDX: u32 = 4;

struct Arg<'a>(&'a mut [u8]);

impl Arg<'_> {
    fn bytes(&self, offset: usize, length: usize) -> Result<&[u8]> {
        self.0
            .get(offset..offset.checked_add(length).ok_or(Error::InvalidArgument)?)
            .ok_or(Error::InvalidArgument)
    }

    fn bytes_mut(&mut self, offset: usize, length: usize) -> Result<&mut [u8]> {
        self.0
            .get_mut(offset..offset.checked_add(length).ok_or(Error::InvalidArgument)?)
            .ok_or(Error::InvalidArgument)
    }

    fn u32(&self, offset: usize) -> Result<u32> {
        Ok(u32::from_le_bytes(
            self.bytes(offset, 4)?.try_into().unwrap(),
        ))
    }

    fn u64(&self, offset: usize) -> Result<u64> {
        Ok(u64::from_le_bytes(
            self.bytes(offset, 8)?.try_into().unwrap(),
        ))
    }

    fn set_u32(&mut self, offset: usize, value: u32) -> Result<()> {
        self.bytes_mut(offset, 4)?
            .copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn set_u64(&mut self, offset: usize, value: u64) -> Result<()> {
        self.bytes_mut(offset, 8)?
            .copy_from_slice(&value.to_le_bytes());
        Ok(())
    }
}

impl GpuDevice {
    fn file_mut(state: &mut crate::device::State, file: FileId) -> Result<&mut FileState> {
        state
            .files
            .iter_mut()
            .find(|entry| entry.id == file)
            .ok_or(Error::InvalidArgument)
    }

    fn context_mut(state: &mut crate::device::State, id: u32) -> Result<&mut Context> {
        state
            .contexts
            .iter_mut()
            .find(|context| context.id == id)
            .ok_or(Error::NotFound)
    }

    fn ensure_file(state: &mut crate::device::State, file: FileId) -> Result<&mut FileState> {
        if state.files.iter().all(|entry| entry.id != file) {
            state.files.push(FileState {
                id: file,
                context: None,
                handles: Vec::new(),
            });
        }
        Self::file_mut(state, file)
    }

    fn supported_capsets(state: &crate::device::State) -> u64 {
        state.capsets.iter().fold(0, |mask, capset| {
            if capset.id < 64 {
                mask | (1u64 << capset.id)
            } else {
                mask
            }
        })
    }

    fn create_context(
        &self,
        state: &mut crate::device::State,
        file: FileId,
        capset_id: u32,
    ) -> Result<u32> {
        let id = state.next_context;
        state.next_context = state
            .next_context
            .checked_add(1)
            .ok_or(Error::NoSpace)?
            .max(1);
        let mut command = Command::<96>::new(protocol::CMD_CTX_CREATE);
        command.put_u32(16, id);
        command.put_u32(24, 10);
        if self.device.features() & protocol::F_CONTEXT_INIT != 0 {
            command.put_u32(28, capset_id & 0xff);
        }
        command.bytes_mut()[32..42].copy_from_slice(b"naos-virgl");
        let mut response = [0; 24];
        self.submit(&command, None, &mut response, protocol::RESP_OK_NODATA)?;
        state.contexts.push(Context {
            id,
            resources: Vec::new(),
        });
        Self::ensure_file(state, file)?.context = Some(id);
        Ok(id)
    }

    fn ensure_context(&self, state: &mut crate::device::State, file: FileId) -> Result<u32> {
        if let Some(id) = Self::ensure_file(state, file)?.context {
            return Ok(id);
        }
        let capset_id = if state
            .capsets
            .iter()
            .any(|capset| capset.id == CAPSET_VIRGL2)
        {
            CAPSET_VIRGL2
        } else {
            CAPSET_VIRGL
        };
        self.create_context(state, file, capset_id)
    }

    fn attach_resource(
        &self,
        state: &mut crate::device::State,
        context_id: u32,
        resource_id: u32,
    ) -> Result<()> {
        let context = Self::context_mut(state, context_id)?;
        if context.resources.iter().any(|id| *id == resource_id) {
            return Ok(());
        }
        let mut command = Command::<32>::new(protocol::CMD_CTX_ATTACH_RESOURCE);
        command.put_u32(16, context_id);
        command.put_u32(24, resource_id);
        let mut response = [0; 24];
        self.submit(&command, None, &mut response, protocol::RESP_OK_NODATA)?;
        context.resources.push(resource_id);
        Ok(())
    }

    fn ioctl_getparam(&self, arg: &mut Arg<'_>) -> Result<()> {
        let param = arg.u64(0)?;
        let value_address = UserAddress::new(arg.u64(8)?);
        if value_address.is_null() {
            return Err(Error::InvalidArgument);
        }
        let state = self.state.lock();
        let value = match param {
            PARAM_3D_FEATURES => (self.device.features() & protocol::F_VIRGL != 0) as u64,
            PARAM_CAPSET_QUERY_FIX => 1,
            PARAM_RESOURCE_BLOB | PARAM_HOST_VISIBLE | PARAM_CROSS_DEVICE => 0,
            PARAM_CONTEXT_INIT => (self.device.features() & protocol::F_CONTEXT_INIT != 0) as u64,
            PARAM_SUPPORTED_CAPSET_IDS => Self::supported_capsets(&state),
            PARAM_EXPLICIT_DEBUG_NAME => 1,
            _ => return Err(Error::InvalidArgument),
        };
        value_address.write(&value.to_ne_bytes())
    }

    fn ioctl_get_caps(&self, arg: &mut Arg<'_>) -> Result<()> {
        let capset_id = arg.u32(0)?;
        let mut version = arg.u32(4)?;
        let address = UserAddress::new(arg.u64(8)?);
        let size = arg.u32(16)? as usize;
        let capset = self
            .state
            .lock()
            .capsets
            .iter()
            .find(|capset| capset.id == capset_id)
            .copied()
            .ok_or(Error::InvalidArgument)?;
        if address.is_null() || size == 0 || version > capset.max_version {
            return Err(Error::InvalidArgument);
        }
        if version == 0 {
            version = capset.max_version;
            arg.set_u32(4, version)?;
        }
        if size > capset.max_size as usize {
            return Err(Error::InvalidArgument);
        }
        let response_size = 24usize.checked_add(size).ok_or(Error::OutOfMemory)?;
        let mut response = KernelBuffer::zeroed(response_size)?;
        let mut command = Command::<32>::new(protocol::CMD_GET_CAPSET);
        command.put_u32(24, capset_id);
        command.put_u32(28, version);
        self.submit(
            &command,
            None,
            response.as_mut_slice(),
            protocol::RESP_OK_CAPSET,
        )?;
        address.write(&response.as_slice()[24..24 + size])
    }

    fn ioctl_map(&self, arg: &mut Arg<'_>) -> Result<()> {
        let handle = arg.u32(8)?;
        Self::find_buffer(&self.state.lock(), handle)?;
        arg.set_u64(0, 0x1_0000_0000u64 + handle as u64 * 0x1_0000_0000u64)
    }

    fn ioctl_resource_info(&self, arg: &mut Arg<'_>) -> Result<()> {
        let handle = arg.u32(0)?;
        let state = self.state.lock();
        let buffer = Self::find_buffer(&state, handle)?;
        arg.set_u32(4, buffer.resource_id)?;
        arg.set_u32(8, buffer.memory.length() as u32)?;
        arg.set_u32(12, 0)
    }

    fn ioctl_resource_create(&self, arg: &mut Arg<'_>, file: Option<FileId>) -> Result<()> {
        let file = file.ok_or(Error::InvalidArgument)?;
        let target = arg.u32(0)?;
        let format = arg.u32(4)?;
        let bind = arg.u32(8)?;
        let width = arg.u32(12)?;
        let height = arg.u32(16)?;
        let depth = arg.u32(20)?;
        let array_size = arg.u32(24)?;
        let last_level = arg.u32(28)?;
        let samples = arg.u32(32)?;
        let flags = arg.u32(36)?;
        let size = arg.u32(48)?;
        let stride = arg.u32(52)?;
        if self.device.features() & protocol::F_VIRGL == 0 || width == 0 || height == 0 || size == 0
        {
            return Err(Error::NoDevice);
        }

        let mut state = self.state.lock();
        let handle = state.next_handle;
        state.next_handle = state
            .next_handle
            .checked_add(1)
            .ok_or(Error::NoSpace)?
            .max(1);
        let resource_id = state.next_resource;
        state.next_resource = state
            .next_resource
            .checked_add(1)
            .ok_or(Error::NoSpace)?
            .max(1);
        let buffer = Buffer::sized(
            handle,
            resource_id,
            width,
            height,
            stride,
            size as usize,
            BufferKind::Virgl3D,
        )?;

        let mut create = Command::<72>::new(protocol::CMD_RESOURCE_CREATE_3D);
        for (offset, value) in [
            resource_id,
            target,
            format,
            bind,
            width,
            height,
            depth,
            array_size,
            last_level,
            samples,
            flags,
        ]
        .into_iter()
        .enumerate()
        {
            create.put_u32(24 + offset * 4, value);
        }
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

        let actual_size = u32::try_from(buffer.memory.length()).map_err(|_| Error::NoSpace)?;
        state.buffers.push(buffer);
        Self::ensure_file(&mut state, file)?.handles.push(handle);
        arg.set_u32(40, handle)?;
        arg.set_u32(44, resource_id)?;
        arg.set_u32(48, actual_size)?;
        arg.set_u32(52, stride)?;
        Ok(())
    }

    fn ioctl_context_init(&self, arg: &Arg<'_>, file: Option<FileId>) -> Result<()> {
        let file = file.ok_or(Error::InvalidArgument)?;
        let count = arg.u32(0)? as usize;
        let address = UserAddress::new(arg.u64(8)?);
        if count != 0 && address.is_null() {
            return Err(Error::InvalidArgument);
        }
        let bytes = count.checked_mul(16).ok_or(Error::OutOfMemory)?;
        let mut parameters = if bytes == 0 {
            None
        } else {
            let mut buffer = KernelBuffer::zeroed(bytes)?;
            address.read(buffer.as_mut_slice())?;
            Some(buffer)
        };
        let mut capset_id = CAPSET_VIRGL;
        if let Some(parameters) = parameters.as_mut() {
            for parameter in parameters.as_slice().as_chunks::<16>().0 {
                let kind = u64::from_ne_bytes(parameter[..8].try_into().unwrap());
                let value = u64::from_ne_bytes(parameter[8..].try_into().unwrap());
                match kind {
                    1 => capset_id = u32::try_from(value).map_err(|_| Error::InvalidArgument)?,
                    2 | 3 if value <= 1 => {}
                    4 => {}
                    _ => return Err(Error::InvalidArgument),
                }
            }
        }
        let mut state = self.state.lock();
        if !state.capsets.iter().any(|capset| capset.id == capset_id) {
            return Err(Error::InvalidArgument);
        }
        if Self::ensure_file(&mut state, file)?.context.is_some() {
            return Err(Error::InvalidArgument);
        }
        self.create_context(&mut state, file, capset_id).map(|_| ())
    }

    fn ioctl_execbuffer(&self, arg: &Arg<'_>, file: Option<FileId>) -> Result<()> {
        let file = file.ok_or(Error::InvalidArgument)?;
        let flags = arg.u32(0)?;
        let size = arg.u32(4)? as usize;
        let command_address = UserAddress::new(arg.u64(8)?);
        let handles_address = UserAddress::new(arg.u64(16)?);
        let handle_count = arg.u32(24)? as usize;
        let ring_idx = arg.u32(32)?;
        let input_sync_count = arg.u32(40)?;
        let output_sync_count = arg.u32(44)?;
        if size == 0
            || command_address.is_null()
            || flags & !(EXECBUF_FENCE_FD_IN | EXECBUF_FENCE_FD_OUT | EXECBUF_RING_IDX) != 0
            || flags & (EXECBUF_FENCE_FD_OUT | EXECBUF_RING_IDX) != 0
            || ring_idx != 0
            || input_sync_count != 0
            || output_sync_count != 0
            || arg.u64(48)? != 0
            || arg.u64(56)? != 0
        {
            return Err(Error::InvalidArgument);
        }

        let handle_bytes = handle_count.checked_mul(4).ok_or(Error::OutOfMemory)?;
        let mut handles = if handle_bytes == 0 {
            None
        } else {
            if handles_address.is_null() {
                return Err(Error::InvalidArgument);
            }
            let mut buffer = KernelBuffer::zeroed(handle_bytes)?;
            handles_address.read(buffer.as_mut_slice())?;
            Some(buffer)
        };
        let mut command_data = KernelBuffer::zeroed(size)?;
        command_address.read(command_data.as_mut_slice())?;

        let mut state = self.state.lock();
        let context_id = self.ensure_context(&mut state, file)?;
        if let Some(handles) = handles.as_mut() {
            for bytes in handles.as_slice().as_chunks::<4>().0 {
                let handle = u32::from_ne_bytes(*bytes);
                let resource_id = Self::find_buffer(&state, handle)?.resource_id;
                self.attach_resource(&mut state, context_id, resource_id)?;
            }
        }
        let fence_id = state.next_fence;
        state.next_fence = state
            .next_fence
            .checked_add(1)
            .ok_or(Error::NoSpace)?
            .max(1);
        let mut command = Command::<32>::new(protocol::CMD_SUBMIT_3D);
        command.put_u32(4, protocol::FLAG_FENCE);
        command.put_u64(8, fence_id);
        command.put_u32(16, context_id);
        command.put_u32(24, size as u32);
        let mut response = [0; 24];
        self.submit(
            &command,
            Some(command_data.as_slice()),
            &mut response,
            protocol::RESP_OK_NODATA,
        )?;
        let response_flags = u32::from_le_bytes(response[4..8].try_into().unwrap());
        let response_fence = u64::from_le_bytes(response[8..16].try_into().unwrap());
        if response_flags & protocol::FLAG_FENCE == 0 || response_fence != fence_id {
            return Err(Error::Io);
        }
        Ok(())
    }

    fn ioctl_transfer(&self, arg: &Arg<'_>, command_type: u32) -> Result<()> {
        let state = self.state.lock();
        let buffer = Self::find_buffer(&state, arg.u32(0)?)?;
        let mut command = Command::<72>::new(command_type);
        protocol::box3(
            command.bytes_mut(),
            24,
            [
                arg.u32(4)?,
                arg.u32(8)?,
                arg.u32(12)?,
                arg.u32(16)?,
                arg.u32(20)?,
                arg.u32(24)?,
            ],
        );
        command.put_u64(48, arg.u32(32)? as u64);
        command.put_u32(56, buffer.resource_id);
        command.put_u32(60, arg.u32(28)?);
        command.put_u32(64, arg.u32(36)?);
        command.put_u32(68, arg.u32(40)?);
        if command_type == protocol::CMD_TRANSFER_TO_HOST_3D {
            buffer.memory.sync_for_device();
        }
        let mut response = [0; 24];
        self.submit(&command, None, &mut response, protocol::RESP_OK_NODATA)?;
        if command_type == protocol::CMD_TRANSFER_FROM_HOST_3D {
            buffer.memory.sync_for_cpu();
        }
        Ok(())
    }

    fn ioctl_wait(&self, arg: &Arg<'_>, file: Option<FileId>) -> Result<()> {
        let handle = arg.u32(0)?;
        if arg.u32(4)? & !WAIT_NOWAIT != 0 {
            return Err(Error::InvalidArgument);
        }
        let file = file.ok_or(Error::InvalidArgument)?;
        let state = self.state.lock();
        let entry = state
            .files
            .iter()
            .find(|entry| entry.id == file)
            .ok_or(Error::NotFound)?;
        if !entry.handles.iter().any(|candidate| *candidate == handle) {
            return Err(Error::NotFound);
        }
        Self::find_buffer(&state, handle).map(|_| ())
    }
}

impl GpuDevice {
    pub(crate) fn drm_open(&self, file: FileId) -> Result<()> {
        let mut state = self.state.lock();
        Self::ensure_file(&mut state, file).map(|_| ())
    }

    pub(crate) fn drm_close(&self, file: FileId) {
        let mut state = self.state.lock();
        if let Some(index) = state.files.iter().position(|entry| entry.id == file) {
            let file_state = state.files.remove(index);
            for handle in file_state.handles.iter() {
                if state.buffers.iter().any(|buffer| buffer.handle == *handle) {
                    let _ = self.put_buffer(&mut state, *handle);
                }
            }
            if let Some(context_id) = file_state.context
                && let Some(context_index) = state
                    .contexts
                    .iter()
                    .position(|context| context.id == context_id)
            {
                let mut destroy = Command::<24>::new(protocol::CMD_CTX_DESTROY);
                destroy.put_u32(16, context_id);
                let mut response = [0; 24];
                let _ = self.submit(&destroy, None, &mut response, protocol::RESP_OK_NODATA);
                let _ = state.contexts.remove(context_index);
            }
        }
    }

    pub(crate) fn drm_ioctl(&self, ioctl: Ioctl<'_>) -> Result<usize> {
        let mut arg = Arg(ioctl.arg);
        match ioctl.command {
            DRM_IOCTL_PRIME_HANDLE_TO_FD => {
                let handle = arg.u32(0)?;
                let file = ioctl.file.ok_or(Error::InvalidArgument)?;
                let mut state = self.state.lock();
                let owned = state
                    .files
                    .iter()
                    .find(|entry| entry.id == file)
                    .map(|entry| entry.handles.iter().any(|candidate| *candidate == handle))
                    .unwrap_or(false);
                if !owned {
                    return Err(Error::NotFound);
                }
                let buffer = Self::find_buffer_mut(&mut state, handle)?;
                buffer.ref_count = buffer.ref_count.checked_add(1).ok_or(Error::NoSpace)?;
                Err(Error::NotATerminal)
            }
            DRM_IOCTL_PRIME_FD_TO_HANDLE => {
                let handle = arg.u32(0)?;
                let file = ioctl.file.ok_or(Error::InvalidArgument)?;
                let mut state = self.state.lock();
                Self::find_buffer(&state, handle)?;
                let tracked = state
                    .files
                    .iter()
                    .find(|entry| entry.id == file)
                    .map(|entry| entry.handles.iter().any(|candidate| *candidate == handle))
                    .unwrap_or(false);
                if !tracked {
                    Self::ensure_file(&mut state, file)?.handles.push(handle);
                    let buffer = Self::find_buffer_mut(&mut state, handle)?;
                    buffer.ref_count = buffer.ref_count.checked_add(1).ok_or(Error::NoSpace)?;
                }
                Ok(0)
            }
            DRM_IOCTL_GEM_CLOSE => {
                let handle = arg.u32(0)?;
                let mut state = self.state.lock();
                let should_put = if let Some(file) = ioctl.file {
                    if let Some(entry) = state.files.iter_mut().find(|entry| entry.id == file) {
                        entry
                            .handles
                            .iter()
                            .position(|candidate| *candidate == handle)
                            .map(|index| {
                                entry.handles.remove(index);
                                true
                            })
                            .unwrap_or(false)
                    } else {
                        false
                    }
                } else {
                    true
                };
                if should_put && Self::find_buffer(&state, handle)?.kind == BufferKind::Virgl3D {
                    self.put_buffer(&mut state, handle)?;
                }
                Ok(0)
            }
            DRM_IOCTL_VIRTGPU_GETPARAM => self.ioctl_getparam(&mut arg).map(|_| 0),
            DRM_IOCTL_VIRTGPU_GET_CAPS => self.ioctl_get_caps(&mut arg).map(|_| 0),
            DRM_IOCTL_VIRTGPU_MAP => self.ioctl_map(&mut arg).map(|_| 0),
            DRM_IOCTL_VIRTGPU_RESOURCE_CREATE => {
                self.ioctl_resource_create(&mut arg, ioctl.file).map(|_| 0)
            }
            DRM_IOCTL_VIRTGPU_RESOURCE_INFO => self.ioctl_resource_info(&mut arg).map(|_| 0),
            DRM_IOCTL_VIRTGPU_EXECBUFFER => self.ioctl_execbuffer(&arg, ioctl.file).map(|_| 0),
            DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST => self
                .ioctl_transfer(&arg, protocol::CMD_TRANSFER_FROM_HOST_3D)
                .map(|_| 0),
            DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST => self
                .ioctl_transfer(&arg, protocol::CMD_TRANSFER_TO_HOST_3D)
                .map(|_| 0),
            DRM_IOCTL_VIRTGPU_WAIT => self.ioctl_wait(&arg, ioctl.file).map(|_| 0),
            DRM_IOCTL_VIRTGPU_CONTEXT_INIT => self.ioctl_context_init(&arg, ioctl.file).map(|_| 0),
            DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB => Err(Error::Unsupported),
            _ => Err(Error::NotATerminal),
        }
    }
}
