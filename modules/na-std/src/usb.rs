use alloc::boxed::Box;
use core::{ffi::CStr, ptr::NonNull};

use crate::{Error, Result, bindings};

const MATCH_VENDOR: u16 = 1 << 0;
const MATCH_PRODUCT: u16 = 1 << 1;
const MATCH_INTERFACE_CLASS: u16 = 1 << 2;
const MATCH_INTERFACE_SUBCLASS: u16 = 1 << 3;
const MATCH_INTERFACE_PROTOCOL: u16 = 1 << 4;

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::UsbDeviceId);

impl DeviceId {
    pub const fn device(vendor_id: u16, product_id: u16) -> Self {
        Self(bindings::UsbDeviceId {
            match_flags: MATCH_VENDOR | MATCH_PRODUCT,
            vendor_id,
            product_id,
            interface_class: 0,
            interface_subclass: 0,
            interface_protocol: 0,
        })
    }

    pub const fn interface(class_code: u8, subclass: u8, protocol: u8) -> Self {
        Self(bindings::UsbDeviceId {
            match_flags: MATCH_INTERFACE_CLASS
                | MATCH_INTERFACE_SUBCLASS
                | MATCH_INTERFACE_PROTOCOL,
            vendor_id: 0,
            product_id: 0,
            interface_class: class_code,
            interface_subclass: subclass,
            interface_protocol: protocol,
        })
    }

    pub const fn vendor_interface(
        vendor_id: u16,
        class_code: u8,
        subclass: u8,
        protocol: u8,
    ) -> Self {
        Self(bindings::UsbDeviceId {
            match_flags: MATCH_VENDOR
                | MATCH_INTERFACE_CLASS
                | MATCH_INTERFACE_SUBCLASS
                | MATCH_INTERFACE_PROTOCOL,
            vendor_id,
            product_id: 0,
            interface_class: class_code,
            interface_subclass: subclass,
            interface_protocol: protocol,
        })
    }

    pub const fn device_interface(
        vendor_id: u16,
        product_id: u16,
        class_code: u8,
        subclass: u8,
        protocol: u8,
    ) -> Self {
        Self(bindings::UsbDeviceId {
            match_flags: MATCH_VENDOR
                | MATCH_PRODUCT
                | MATCH_INTERFACE_CLASS
                | MATCH_INTERFACE_SUBCLASS
                | MATCH_INTERFACE_PROTOCOL,
            vendor_id,
            product_id,
            interface_class: class_code,
            interface_subclass: subclass,
            interface_protocol: protocol,
        })
    }
}

pub trait Driver: Sync + 'static {
    type Binding: Send + 'static;

    const IDS: &'static [DeviceId];

    fn probe(&self, device: Device, interface: Interface) -> Result<Self::Binding>;
}

pub struct DriverBuilder<D: Driver> {
    driver: &'static D,
    name: &'static CStr,
    priority: i32,
}

impl<D: Driver> DriverBuilder<D> {
    pub const fn new(driver: &'static D, name: &'static CStr) -> Self {
        Self {
            driver,
            name,
            priority: 0,
        }
    }

    pub const fn priority(mut self, priority: i32) -> Self {
        self.priority = priority;
        self
    }

    pub fn register(self) -> Result<()> {
        if D::IDS.is_empty() {
            return Err(Error::InvalidArgument);
        }
        let ops = bindings::UsbDriverOps {
            context: (self.driver as *const D).cast_mut().cast(),
            probe: Some(Self::probe),
            remove: Some(Self::remove),
        };
        let status = unsafe {
            bindings::na_usb_driver_register(
                self.name.as_ptr(),
                D::IDS.as_ptr().cast(),
                D::IDS.len(),
                self.priority,
                &ops,
            )
        };
        Error::from_status(status)
    }

    unsafe extern "C" fn probe(
        context: *mut core::ffi::c_void,
        raw_device: *mut bindings::UsbDevice,
        raw_interface: *mut bindings::UsbInterface,
        output: *mut *mut core::ffi::c_void,
    ) -> i32 {
        let Some(device) = Device::from_raw(raw_device) else {
            return Error::NoDevice.status();
        };
        let Some(interface) = Interface::from_raw(raw_interface) else {
            return Error::NoDevice.status();
        };
        if context.is_null() || output.is_null() {
            return Error::InvalidArgument.status();
        }

        let driver = unsafe { &*context.cast::<D>() };
        match driver.probe(device, interface) {
            Ok(binding) => {
                let binding = Box::into_raw(Box::new(binding)).cast();
                unsafe { output.write(binding) };
                0
            }
            Err(error) => error.status(),
        }
    }

    unsafe extern "C" fn remove(_context: *mut core::ffi::c_void, binding: *mut core::ffi::c_void) {
        if !binding.is_null() {
            drop(unsafe { Box::from_raw(binding.cast::<D::Binding>()) });
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Speed {
    Full,
    Low,
    High,
    Super,
    Unknown(u8),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeviceInfo {
    pub vendor_id: u16,
    pub product_id: u16,
    pub speed: Speed,
    pub bus_number: u8,
    pub device_number: u8,
    pub address: u8,
}

#[derive(Clone, Copy)]
pub struct Device {
    raw: NonNull<bindings::UsbDevice>,
}

impl Device {
    fn from_raw(raw: *mut bindings::UsbDevice) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self { raw })
    }

    pub fn info(&self) -> Result<DeviceInfo> {
        let mut raw = bindings::UsbDeviceInfo::default();
        let status = unsafe { bindings::na_usb_device_info(self.raw.as_ptr(), &mut raw) };
        Error::from_status(status)?;
        let speed = match raw.speed {
            0 => Speed::Full,
            1 => Speed::Low,
            2 => Speed::High,
            3 => Speed::Super,
            value => Speed::Unknown(value),
        };
        Ok(DeviceInfo {
            vendor_id: raw.vendor_id,
            product_id: raw.product_id,
            speed,
            bus_number: raw.bus_number,
            device_number: raw.device_number,
            address: raw.address,
        })
    }

    pub fn interfaces(&self) -> Interfaces {
        let count = unsafe { bindings::na_usb_device_interface_count(self.raw.as_ptr()) };
        Interfaces {
            device: *self,
            index: 0,
            count,
        }
    }

    pub fn control(&self, request: ControlRequest, data: ControlData<'_>) -> Result<usize> {
        let (pointer, size) = match data {
            ControlData::None => (core::ptr::null_mut(), 0),
            ControlData::In(buffer) => (buffer.as_mut_ptr().cast(), buffer.len()),
            ControlData::Out(buffer) => (buffer.as_ptr().cast_mut().cast(), buffer.len()),
        };
        let mut actual = 0;
        let status = unsafe {
            bindings::na_usb_control_transfer(
                self.raw.as_ptr(),
                request.request_type,
                request.request,
                request.value,
                request.index,
                pointer,
                size,
                &mut actual,
            )
        };
        Error::from_status(status)?;
        Ok(actual)
    }
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}

pub struct Interfaces {
    device: Device,
    index: usize,
    count: usize,
}

impl Iterator for Interfaces {
    type Item = Interface;

    fn next(&mut self) -> Option<Self::Item> {
        while self.index < self.count {
            let index = self.index;
            self.index += 1;
            let raw =
                unsafe { bindings::na_usb_device_interface_at(self.device.raw.as_ptr(), index) };
            if let Some(interface) = Interface::from_raw(raw) {
                return Some(interface);
            }
        }
        None
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = self.count.saturating_sub(self.index);
        (remaining, Some(remaining))
    }
}

impl ExactSizeIterator for Interfaces {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct InterfaceInfo {
    pub number: u8,
    pub alternate_setting: u8,
    pub class_code: u8,
    pub subclass: u8,
    pub protocol: u8,
}

#[derive(Clone, Copy)]
pub struct Interface {
    raw: NonNull<bindings::UsbInterface>,
}

impl Interface {
    fn from_raw(raw: *mut bindings::UsbInterface) -> Option<Self> {
        NonNull::new(raw).map(|raw| Self { raw })
    }

    pub fn info(&self) -> Result<InterfaceInfo> {
        let mut raw = bindings::UsbInterfaceInfo::default();
        let status = unsafe { bindings::na_usb_interface_info(self.raw.as_ptr(), &mut raw) };
        Error::from_status(status)?;
        Ok(InterfaceInfo {
            number: raw.number,
            alternate_setting: raw.alternate_setting,
            class_code: raw.class_code,
            subclass: raw.subclass,
            protocol: raw.protocol,
        })
    }

    pub fn open_pipe(&self, transfer_type: TransferType, direction: Direction) -> Result<Pipe> {
        let mut raw = core::ptr::null_mut();
        let status = unsafe {
            bindings::na_usb_pipe_open(
                self.raw.as_ptr(),
                transfer_type as u8,
                direction as u8,
                &mut raw,
            )
        };
        Error::from_status(status)?;
        NonNull::new(raw)
            .map(|raw| Pipe { raw, direction })
            .ok_or(Error::NoDevice)
    }
}

unsafe impl Send for Interface {}
unsafe impl Sync for Interface {}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Direction {
    Out = 0,
    In = 0x80,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransferType {
    Control = 0,
    Isochronous = 1,
    Bulk = 2,
    Interrupt = 3,
}

pub struct Pipe {
    raw: NonNull<bindings::UsbPipe>,
    direction: Direction,
}

impl Pipe {
    pub fn read(&self, buffer: &mut [u8]) -> Result<usize> {
        if self.direction != Direction::In {
            return Err(Error::InvalidArgument);
        }
        let mut actual = 0;
        let status = unsafe {
            bindings::na_usb_pipe_read(
                self.raw.as_ptr(),
                buffer.as_mut_ptr().cast(),
                buffer.len(),
                &mut actual,
            )
        };
        Error::from_status(status)?;
        Ok(actual)
    }

    pub fn write(&self, buffer: &[u8]) -> Result<usize> {
        if self.direction != Direction::Out {
            return Err(Error::InvalidArgument);
        }
        let mut actual = 0;
        let status = unsafe {
            bindings::na_usb_pipe_write(
                self.raw.as_ptr(),
                buffer.as_ptr().cast(),
                buffer.len(),
                &mut actual,
            )
        };
        Error::from_status(status)?;
        Ok(actual)
    }
}

impl Drop for Pipe {
    fn drop(&mut self) {
        unsafe { bindings::na_usb_pipe_close(self.raw.as_ptr()) };
    }
}

unsafe impl Send for Pipe {}
unsafe impl Sync for Pipe {}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RequestKind {
    Standard = 0,
    Class = 1,
    Vendor = 2,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Recipient {
    Device = 0,
    Interface = 1,
    Endpoint = 2,
    Other = 3,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ControlRequest {
    request_type: u8,
    request: u8,
    value: u16,
    index: u16,
}

impl ControlRequest {
    pub const fn new(
        direction: Direction,
        kind: RequestKind,
        recipient: Recipient,
        request: u8,
        value: u16,
        index: u16,
    ) -> Self {
        Self {
            request_type: direction as u8 | (kind as u8) << 5 | recipient as u8,
            request,
            value,
            index,
        }
    }
}

pub enum ControlData<'a> {
    None,
    In(&'a mut [u8]),
    Out(&'a [u8]),
}
