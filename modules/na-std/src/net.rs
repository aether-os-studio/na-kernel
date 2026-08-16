use alloc::boxed::Box;
use core::{ffi::CStr, ptr::NonNull, slice};

use crate::{Error, Result, bindings};

pub const ETHERNET_FRAME_OVERHEAD: usize = 18;

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DeviceKind {
    Ethernet = 0,
    Wifi = 1,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MacAddress([u8; 6]);

impl MacAddress {
    pub const fn new(bytes: [u8; 6]) -> Self {
        Self(bytes)
    }

    pub const fn bytes(self) -> [u8; 6] {
        self.0
    }

    pub fn is_unicast(self) -> bool {
        self.0 != [0; 6] && self.0 != [0xff; 6] && self.0[0] & 1 == 0
    }
}

pub trait Device: Send + Sync + 'static {
    fn transmit(&self, frame: &[u8]) -> Result<()>;
    fn receive(&self, frame: &mut [u8]) -> Result<usize>;
}

pub struct DeviceBuilder<D: Device> {
    device: D,
    name: Option<&'static CStr>,
    kind: DeviceKind,
    mac: MacAddress,
    mtu: u32,
}

impl<D: Device> DeviceBuilder<D> {
    pub const fn new(device: D, mac: MacAddress, mtu: u32) -> Self {
        Self {
            device,
            name: None,
            kind: DeviceKind::Ethernet,
            mac,
            mtu,
        }
    }

    pub const fn name(mut self, name: &'static CStr) -> Self {
        self.name = Some(name);
        self
    }

    pub const fn kind(mut self, kind: DeviceKind) -> Self {
        self.kind = kind;
        self
    }

    pub fn register(self) -> Result<Registration<D>> {
        if !self.mac.is_unicast() || self.mtu == 0 {
            return Err(Error::InvalidArgument);
        }

        let device = Box::new(self.device);
        let config = bindings::NetDeviceConfig {
            name: self.name.map_or(core::ptr::null(), CStr::as_ptr),
            kind: self.kind as u32,
            mac: self.mac.bytes(),
            mtu: self.mtu,
        };
        let ops = bindings::NetDeviceOps {
            context: (&*device as *const D).cast_mut().cast(),
            transmit: Some(Self::transmit),
            receive: Some(Self::receive),
        };
        let mut raw = core::ptr::null_mut();
        let status = unsafe { bindings::na_net_device_register(&config, &ops, &mut raw) };
        Error::from_status(status)?;
        let raw = NonNull::new(raw).ok_or(Error::NoDevice)?;
        Ok(Registration {
            raw,
            device: Some(device),
        })
    }

    unsafe extern "C" fn transmit(
        context: *mut core::ffi::c_void,
        data: *const core::ffi::c_void,
        size: usize,
    ) -> i32 {
        if context.is_null() || (size != 0 && data.is_null()) || size > i32::MAX as usize {
            return Error::InvalidArgument.status();
        }
        if size == 0 {
            return 0;
        }
        let device = unsafe { &*context.cast::<D>() };
        let frame = unsafe { slice::from_raw_parts(data.cast::<u8>(), size) };
        device
            .transmit(frame)
            .map_or_else(Error::status, |()| size as i32)
    }

    unsafe extern "C" fn receive(
        context: *mut core::ffi::c_void,
        data: *mut core::ffi::c_void,
        size: usize,
    ) -> i32 {
        if context.is_null() || (size != 0 && data.is_null()) || size > i32::MAX as usize {
            return Error::InvalidArgument.status();
        }
        if size == 0 {
            return 0;
        }
        let device = unsafe { &*context.cast::<D>() };
        let frame = unsafe { slice::from_raw_parts_mut(data.cast::<u8>(), size) };
        match device.receive(frame) {
            Ok(received) if received <= size && received <= i32::MAX as usize => received as i32,
            Ok(_) => Error::Range.status(),
            Err(error) => error.status(),
        }
    }
}

pub struct Registration<D: Device> {
    raw: NonNull<bindings::NetRegistration>,
    device: Option<Box<D>>,
}

impl<D: Device> Registration<D> {
    pub fn set_link(&self, link_up: bool) -> Result<()> {
        let status = unsafe { bindings::na_net_device_set_link(self.raw.as_ptr(), link_up) };
        Error::from_status(status)
    }
}

impl<D: Device> Drop for Registration<D> {
    fn drop(&mut self) {
        let status = unsafe { bindings::na_net_device_unregister(self.raw.as_ptr()) };
        if status < 0 {
            let _ = self.device.take().map(Box::leak);
        }
    }
}

unsafe impl<D: Device> Send for Registration<D> {}
unsafe impl<D: Device> Sync for Registration<D> {}
