use alloc::boxed::Box;
use core::{ffi::CStr, marker::PhantomData, ptr::NonNull};

use crate::{
    Error, Result, bindings,
    memory::{Borrowed, PhysicalRange},
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Address {
    pub segment: u16,
    pub bus: u8,
    pub device: u8,
    pub function: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeviceInfo {
    pub address: Address,
    pub class_code: u32,
    pub vendor_id: u16,
    pub device_id: u16,
    pub subsystem_vendor_id: u16,
    pub subsystem_device_id: u16,
    pub revision_id: u8,
    pub irq_line: u8,
    pub irq_pin: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Bar {
    Memory {
        range: PhysicalRange,
        prefetchable: bool,
    },
    Port {
        base: u16,
        length: u16,
    },
}

pub struct Device<'a> {
    raw: Borrowed<'a, bindings::PciDevice>,
}

pub struct BarResource {
    device: NonNull<bindings::PciDevice>,
    index: u8,
    bar: Bar,
}

impl BarResource {
    pub const fn index(&self) -> u8 {
        self.index
    }

    pub const fn bar(&self) -> Bar {
        self.bar
    }
}

impl Drop for BarResource {
    fn drop(&mut self) {
        unsafe { bindings::na_pci_bar_release(self.device.as_ptr(), self.index) };
    }
}

unsafe impl Send for BarResource {}

impl Device<'_> {
    const COMMAND_OFFSET: u16 = 0x04;
    const COMMAND_MEMORY_SPACE: u16 = 1 << 1;
    const COMMAND_BUS_MASTER: u16 = 1 << 2;

    pub fn info(&self) -> Result<DeviceInfo> {
        let mut raw = bindings::PciDeviceInfo::default();
        let status = unsafe { bindings::na_pci_device_info(self.raw.ptr.as_ptr(), &mut raw) };
        Error::from_status(status)?;
        Ok(DeviceInfo {
            address: Address {
                segment: raw.segment,
                bus: raw.bus,
                device: raw.slot,
                function: raw.function,
            },
            class_code: raw.class_code,
            vendor_id: raw.vendor_id,
            device_id: raw.device_id,
            subsystem_vendor_id: raw.subsystem_vendor_id,
            subsystem_device_id: raw.subsystem_device_id,
            revision_id: raw.revision_id,
            irq_line: raw.irq_line,
            irq_pin: raw.irq_pin,
        })
    }

    pub fn bar(&self, index: u8) -> Result<Bar> {
        let mut raw = bindings::PciBarInfo::default();
        let status = unsafe { bindings::na_pci_bar_info(self.raw.ptr.as_ptr(), index, &mut raw) };
        Error::from_status(status)?;

        if raw.is_mmio {
            return Ok(Bar::Memory {
                range: PhysicalRange::new(raw.address, raw.size)?,
                prefetchable: raw.prefetchable,
            });
        }

        let base = u16::try_from(raw.address).map_err(|_| Error::InvalidArgument)?;
        let length = u16::try_from(raw.size).map_err(|_| Error::InvalidArgument)?;
        Ok(Bar::Port { base, length })
    }

    pub fn rom_bar(&self) -> Result<Bar> {
        let mut raw = bindings::PciBarInfo::default();
        let status = unsafe { bindings::na_pci_rom_bar(self.raw.ptr.as_ptr(), &mut raw) };
        Error::from_status(status)?;
        Ok(Bar::Memory {
            range: PhysicalRange::new(raw.address, raw.size)?,
            prefetchable: raw.prefetchable,
        })
    }

    pub fn read_config<T: ConfigValue>(&self, offset: u16) -> Result<T> {
        let mut value = 0;
        let status = unsafe {
            bindings::na_pci_config_read(self.raw.ptr.as_ptr(), offset, T::WIDTH, &mut value)
        };
        Error::from_status(status)?;
        Ok(T::from_u32(value))
    }

    pub fn write_config<T: ConfigValue>(&mut self, offset: u16, value: T) -> Result<()> {
        let status = unsafe {
            bindings::na_pci_config_write(self.raw.ptr.as_ptr(), offset, T::WIDTH, value.into_u32())
        };
        Error::from_status(status)
    }

    pub fn enable_memory_and_bus_master(&mut self) -> Result<()> {
        let command = self.read_config::<u16>(Self::COMMAND_OFFSET)?;
        let required = Self::COMMAND_MEMORY_SPACE | Self::COMMAND_BUS_MASTER;
        self.write_config(Self::COMMAND_OFFSET, command | required)?;

        if self.read_config::<u16>(Self::COMMAND_OFFSET)? & required != required {
            return Err(Error::Io);
        }
        Ok(())
    }

    pub fn claim_bar(&self, index: u8) -> Result<BarResource> {
        let bar = self.bar(index)?;
        Error::from_status(unsafe { bindings::na_pci_bar_claim(self.raw.ptr.as_ptr(), index) })?;
        Ok(BarResource {
            device: self.raw.ptr,
            index,
            bar,
        })
    }

    pub(crate) fn from_raw(raw: *mut bindings::PciDevice) -> Option<Self> {
        NonNull::new(raw).map(|ptr| Self {
            raw: Borrowed {
                ptr,
                lifetime: PhantomData,
            },
        })
    }

    pub(crate) fn raw_ptr(&self) -> *mut bindings::PciDevice {
        self.raw.ptr.as_ptr()
    }

    /// Retains access to the kernel PCI device beyond the probe callback.
    /// The device object lives as long as the PCI bus enumeration does.
    pub fn retain(self) -> DeviceHandle {
        DeviceHandle {
            ptr: self.raw.ptr.as_ptr(),
        }
    }
}

/// Long-lived handle to a kernel PCI device (see `Device::retain`).
pub struct DeviceHandle {
    ptr: *mut bindings::PciDevice,
}

unsafe impl Send for DeviceHandle {}
unsafe impl Sync for DeviceHandle {}

impl DeviceHandle {
    /// Reborrows the device for kernel calls (MSI setup, config access).
    pub fn as_device(&self) -> Device<'_> {
        // SAFETY: the kernel PCI device outlives every bus enumeration.
        Device::from_raw(self.ptr).unwrap()
    }
}

mod sealed {
    pub trait Sealed {}
    impl Sealed for u8 {}
    impl Sealed for u16 {}
    impl Sealed for u32 {}
}

pub trait ConfigValue: sealed::Sealed + Copy {
    const WIDTH: u8;
    fn from_u32(value: u32) -> Self;
    fn into_u32(self) -> u32;
}

macro_rules! config_value {
    ($type:ty, $width:expr) => {
        impl ConfigValue for $type {
            const WIDTH: u8 = $width;

            fn from_u32(value: u32) -> Self {
                value as Self
            }

            fn into_u32(self) -> u32 {
                self as u32
            }
        }
    };
}

config_value!(u8, 1);
config_value!(u16, 2);
config_value!(u32, 4);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Probe {
    Claimed,
    Continue,
}

pub trait Driver: Sync + 'static {
    fn matches(&self, device: &DeviceInfo) -> bool;
    fn probe(&self, device: Device<'_>) -> Result<Probe>;
}

pub struct DriverBuilder<D: Driver> {
    driver: &'static D,
    name: &'static CStr,
    flags: i32,
}

impl<D: Driver> DriverBuilder<D> {
    pub const fn new(driver: &'static D, name: &'static CStr) -> Self {
        Self {
            driver,
            name,
            flags: 0,
        }
    }

    pub const fn flags(mut self, flags: i32) -> Self {
        self.flags = flags;
        self
    }

    pub fn register(self) -> Result<()> {
        let ops = bindings::PciDriverOps {
            context: (self.driver as *const D).cast_mut().cast(),
            matches: Some(Self::matches),
            probe: Some(Self::probe),
        };
        let status =
            unsafe { bindings::na_pci_driver_register(self.name.as_ptr(), 0, self.flags, &ops) };
        Error::from_status(status)
    }

    unsafe extern "C" fn matches(
        context: *mut core::ffi::c_void,
        raw: *mut bindings::PciDevice,
    ) -> bool {
        let Some(device) = Device::from_raw(raw) else {
            return false;
        };
        let Ok(info) = device.info() else {
            return false;
        };
        let driver = unsafe { &*context.cast::<D>() };
        driver.matches(&info)
    }

    unsafe extern "C" fn probe(
        context: *mut core::ffi::c_void,
        raw: *mut bindings::PciDevice,
    ) -> i32 {
        let Some(device) = Device::from_raw(raw) else {
            return Error::NoDevice.status();
        };
        let driver = unsafe { &*context.cast::<D>() };
        match driver.probe(device) {
            Ok(Probe::Claimed) => 0,
            Ok(Probe::Continue) => 1,
            Err(error) => error.status(),
        }
    }
}

pub trait IrqCallback: Sync {
    /// Runs in interrupt context — keep the handler short.
    fn irq(&self, irq_num: u64);
}

/// Message-Signaled Interrupt registered with the kernel on behalf of a
/// Rust PCI driver. Keeps the callback alive; released when dropped.
pub struct MsiIrq<C: IrqCallback + 'static> {
    handle: u64,
    callback: Box<C>,
}

impl<C: IrqCallback + 'static> MsiIrq<C> {
    /// Programs MSI (or MSI-X when `prefer_msix`) and registers `callback`
    /// as the interrupt handler.
    pub fn setup(device: &Device<'_>, prefer_msix: bool, callback: C, name: &CStr) -> Result<Self> {
        let callback = Box::new(callback);
        let mut handle = 0;
        let data = callback.as_ref() as *const C as *mut core::ffi::c_void;
        let status = unsafe {
            bindings::na_msi_setup_irq(
                device.raw.ptr.as_ptr(),
                prefer_msix,
                Some(Self::irq_shim),
                data,
                name.as_ptr(),
                &mut handle,
            )
        };
        Error::from_status(status)?;
        Ok(Self { handle, callback })
    }

    pub fn callback(&self) -> &C {
        &self.callback
    }

    extern "C" fn irq_shim(irq_num: u64, data: *mut core::ffi::c_void) {
        // SAFETY: `data` points into the boxed callback owned by the live
        // `MsiIrq`; Drop unregisters the IRQ before freeing that box.
        let callback = unsafe { &*(data as *const C) };
        callback.irq(irq_num);
    }
}

impl<C: IrqCallback + 'static> Drop for MsiIrq<C> {
    fn drop(&mut self) {
        unsafe { bindings::na_msi_release_irq(self.handle) };
    }
}
