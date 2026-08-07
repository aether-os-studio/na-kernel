use na_std::{
    Error, KernelLog, Result,
    drm::{self, FileId},
    memory::{KernelBox, KernelVec},
    sync::Mutex,
    virtio,
};

use crate::{buffer::Buffer, display::DisplayState, protocol};

pub struct State {
    pub display: DisplayState,
    pub buffers: KernelVec<Buffer>,
    pub capsets: KernelVec<Capset>,
    pub contexts: KernelVec<Context>,
    pub files: KernelVec<FileState>,
    pub next_handle: u32,
    pub next_resource: u32,
    pub next_context: u32,
    pub next_fence: u64,
    pub current_scanout: Option<u32>,
}

#[derive(Clone, Copy)]
pub struct Capset {
    pub id: u32,
    pub max_version: u32,
    pub max_size: u32,
}

pub struct Context {
    pub id: u32,
    pub capset_id: u32,
    pub resources: KernelVec<u32>,
}

pub struct FileState {
    pub id: FileId,
    pub context: Option<u32>,
    pub handles: KernelVec<u32>,
}

pub struct GpuDevice {
    pub(crate) device: virtio::Device,
    pub(crate) control: virtio::Queue,
    pub(crate) state: Mutex<State>,
}

impl GpuDevice {
    pub fn new(mut device: virtio::Device) -> Result<KernelBox<Self>> {
        let control = device.queue(protocol::QUEUE_CONTROL)?;
        KernelLog::write(c"virtio-gpu: control queue ready\n");
        device.finish();
        let display = DisplayState::query(&control)?;
        let mut capsets = KernelVec::new();
        let capset_count = device.config_read(12);
        for index in 0..capset_count {
            if let Some(capset) = Self::query_capset(&control, index)? {
                capsets.push(capset)?;
            }
        }
        let state = Mutex::new(State {
            display,
            buffers: KernelVec::new(),
            capsets,
            contexts: KernelVec::new(),
            files: KernelVec::new(),
            next_handle: 1,
            next_resource: 1,
            next_context: 1,
            next_fence: 1,
            current_scanout: None,
        })?;
        KernelBox::new(Self {
            device,
            control,
            state,
        })
    }

    fn query_capset(queue: &virtio::Queue, index: u32) -> Result<Option<Capset>> {
        let mut request = protocol::Command::<32>::new(protocol::CMD_GET_CAPSET_INFO);
        request.put_u32(24, index);
        let mut response = [0; 40];
        queue.submit(request.bytes(), None, &mut response)?;
        let kind = protocol::response_type(&response)?;
        if kind >= 0x1200 {
            return Ok(None);
        }
        protocol::check_response(&response, protocol::RESP_OK_CAPSET_INFO)?;
        Ok(Some(Capset {
            id: u32::from_le_bytes(response[24..28].try_into().unwrap()),
            max_version: u32::from_le_bytes(response[28..32].try_into().unwrap()),
            max_size: u32::from_le_bytes(response[32..36].try_into().unwrap()),
        }))
    }

    pub fn start(&'static self) -> Result<()> {
        self.device.set_config_handler(self);
        let pci = self.device.pci_device();
        let drm = drm::DeviceBuilder::new(
            self,
            c"dri/card",
            c"virtio_gpu",
            c"20260610",
            c"NaOS virtio GPU DRM",
        )
        .render_node(true)
        .register(pci.as_ref())?;
        KernelLog::write(c"virtio-gpu: drm registered\n");
        let _ = KernelBox::leak(KernelBox::new(drm)?);
        Ok(())
    }

    pub(crate) fn submit<const N: usize>(
        &self,
        request: &protocol::Command<N>,
        extra: Option<&[u8]>,
        response: &mut [u8],
        expected: u32,
    ) -> Result<()> {
        self.control.submit(request.bytes(), extra, response)?;
        self.control_response(response, expected)
    }

    fn control_response(&self, response: &[u8], expected: u32) -> Result<()> {
        protocol::check_response(response, expected)
    }

    pub(crate) fn find_buffer<'a>(state: &'a State, handle: u32) -> Result<&'a Buffer> {
        state
            .buffers
            .iter()
            .find(|buffer| buffer.handle == handle)
            .ok_or(Error::NotFound)
    }

    pub(crate) fn find_buffer_mut<'a>(state: &'a mut State, handle: u32) -> Result<&'a mut Buffer> {
        state
            .buffers
            .iter_mut()
            .find(|buffer| buffer.handle == handle)
            .ok_or(Error::NotFound)
    }

    pub(crate) fn put_buffer(&self, state: &mut State, handle: u32) -> Result<()> {
        let index = state
            .buffers
            .iter()
            .position(|buffer| buffer.handle == handle)
            .ok_or(Error::NotFound)?;
        let buffer = state.buffers.get_mut(index).ok_or(Error::NotFound)?;
        if buffer.ref_count > 1 {
            buffer.ref_count -= 1;
            return Ok(());
        }

        let resource_id = buffer.resource_id;
        for context_index in 0..state.contexts.len() {
            let Some((context_id, resource_index)) =
                state.contexts.get(context_index).and_then(|context| {
                    context
                        .resources
                        .iter()
                        .position(|candidate| *candidate == resource_id)
                        .map(|resource_index| (context.id, resource_index))
                })
            else {
                continue;
            };
            let mut detach = protocol::Command::<32>::new(protocol::CMD_CTX_DETACH_RESOURCE);
            detach.put_u32(16, context_id);
            detach.put_u32(24, resource_id);
            let mut response = [0; 24];
            self.submit(&detach, None, &mut response, protocol::RESP_OK_NODATA)?;
            let _ = state
                .contexts
                .get_mut(context_index)
                .and_then(|context| context.resources.remove(resource_index));
        }

        let mut response = [0; 24];
        let mut detach = protocol::Command::<32>::new(protocol::CMD_RESOURCE_DETACH_BACKING);
        detach.put_u32(24, resource_id);
        self.submit(&detach, None, &mut response, protocol::RESP_OK_NODATA)?;
        let mut unref = protocol::Command::<32>::new(protocol::CMD_RESOURCE_UNREF);
        unref.put_u32(24, resource_id);
        self.submit(&unref, None, &mut response, protocol::RESP_OK_NODATA)?;
        let _ = state.buffers.remove(index).ok_or(Error::NotFound)?;
        Ok(())
    }
}

impl virtio::ConfigHandler for GpuDevice {
    fn changed(&self) {
        self.state.lock().display.changed = true;
    }
}
