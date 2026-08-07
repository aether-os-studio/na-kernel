#![no_std]
#![forbid(unsafe_code)]

mod buffer;
mod device;
mod display;
mod ioctl;
mod protocol;

use na_std::{Result, module_entry, virtio};

use device::GpuDevice;

struct Driver;

static DRIVER: Driver = Driver;

impl virtio::Driver for Driver {
    const DEVICE_TYPE: u32 = virtio::DEVICE_GPU;
    const FEATURES: u64 = protocol::SUPPORTED_FEATURES;

    fn probe(&self, device: virtio::Device) -> Result<()> {
        let gpu = GpuDevice::new(device)?;
        let gpu = na_std::memory::KernelBox::leak(gpu);
        gpu.start()?;
        Ok(())
    }
}

fn init() -> Result<()> {
    virtio::DriverBuilder::new(&DRIVER, c"virtio-gpu").register()
}

module_entry!(init);
