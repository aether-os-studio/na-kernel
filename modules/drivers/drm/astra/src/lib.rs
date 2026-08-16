#![no_std]
#![deny(unsafe_code)]

extern crate alloc;

mod atom;
mod blocks;
mod device;
mod discovery;
mod display;
mod doorbell;
mod firmware;
mod ioctl;
mod ip;
mod irq;
mod log;
mod mem;
mod regs;
mod ring;
mod uapi;
mod ucode;

use alloc::boxed::Box;

use na_std::pci::{self, DriverBuilder, Probe};
use na_std::{Result, module_entry};

use device::Adapter;

/// AMD vendor id.
const PCI_VENDOR_AMD: u16 = 0x1002;

struct Driver;

static DRIVER: Driver = Driver;

impl pci::Driver for Driver {
    fn matches(&self, info: &pci::DeviceInfo) -> bool {
        // Display-class AMD functions (VGA / 3D controllers).
        info.vendor_id == PCI_VENDOR_AMD && (info.class_code >> 8) == 0x0300
    }

    fn probe(&self, device: pci::Device<'_>) -> Result<Probe> {
        let adapter = Box::leak(Adapter::probe(device)?);
        adapter.init()?;
        display::register(adapter)?;
        Ok(Probe::Claimed)
    }
}

fn init() -> Result<()> {
    DriverBuilder::new(&DRIVER, c"astra").register()
}

module_entry!(init);
