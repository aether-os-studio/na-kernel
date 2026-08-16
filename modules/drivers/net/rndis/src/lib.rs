#![no_std]
#![forbid(unsafe_code)]

extern crate alloc;

mod device;
mod protocol;

use na_std::{Result, module_entry, usb};

use device::RndisDevice;

const USB_CLASS_COMMUNICATIONS: u8 = 0x02;
const USB_CLASS_MISCELLANEOUS: u8 = 0xef;
const USB_CLASS_WIRELESS_CONTROLLER: u8 = 0xe0;

struct Driver;

impl usb::Driver for Driver {
    type Binding = na_std::net::Registration<RndisDevice>;

    const IDS: &'static [usb::DeviceId] = &[
        usb::DeviceId::device_interface(0x1630, 0x0042, USB_CLASS_COMMUNICATIONS, 0x02, 0xff),
        usb::DeviceId::vendor_interface(0x238b, USB_CLASS_COMMUNICATIONS, 0x02, 0xff),
        usb::DeviceId::vendor_interface(0x19d2, USB_CLASS_WIRELESS_CONTROLLER, 0x01, 0x03),
        usb::DeviceId::vendor_interface(0x19d2, USB_CLASS_COMMUNICATIONS, 0x02, 0xff),
        usb::DeviceId::interface(USB_CLASS_COMMUNICATIONS, 0x02, 0xff),
        usb::DeviceId::device_interface(0x1bc7, 0x7030, USB_CLASS_WIRELESS_CONTROLLER, 0x01, 0x03),
        usb::DeviceId::interface(USB_CLASS_MISCELLANEOUS, 0x01, 0x01),
        usb::DeviceId::interface(USB_CLASS_WIRELESS_CONTROLLER, 0x01, 0x03),
        usb::DeviceId::interface(USB_CLASS_MISCELLANEOUS, 0x04, 0x01),
    ];

    fn probe(&self, device: usb::Device, control: usb::Interface) -> Result<Self::Binding> {
        RndisDevice::bind(device, control)
    }
}

static DRIVER: Driver = Driver;

fn init() -> Result<()> {
    usb::DriverBuilder::new(&DRIVER, c"rndis").register()
}

module_entry!(init);
