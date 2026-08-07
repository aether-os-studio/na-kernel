use core::arch::asm;

use crate::{Error, Result, pci::Bar};

pub struct PortRegion {
    base: u16,
    length: u16,
}

impl PortRegion {
    pub fn from_bar(bar: Bar) -> Result<Self> {
        match bar {
            Bar::Port { base, length } if length != 0 => Ok(Self { base, length }),
            _ => Err(Error::InvalidArgument),
        }
    }

    pub fn read_u8(&self, offset: u16) -> Result<u8> {
        let port = self.port(offset, 1)?;
        let value: u8;
        unsafe { asm!("in al, dx", in("dx") port, out("al") value, options(nomem, nostack)) };
        Ok(value)
    }

    pub fn read_u16(&self, offset: u16) -> Result<u16> {
        let port = self.port(offset, 2)?;
        let value: u16;
        unsafe { asm!("in ax, dx", in("dx") port, out("ax") value, options(nomem, nostack)) };
        Ok(value)
    }

    pub fn read_u32(&self, offset: u16) -> Result<u32> {
        let port = self.port(offset, 4)?;
        let value: u32;
        unsafe { asm!("in eax, dx", in("dx") port, out("eax") value, options(nomem, nostack)) };
        Ok(value)
    }

    pub fn write_u8(&mut self, offset: u16, value: u8) -> Result<()> {
        let port = self.port(offset, 1)?;
        unsafe { asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack)) };
        Ok(())
    }

    pub fn write_u16(&mut self, offset: u16, value: u16) -> Result<()> {
        let port = self.port(offset, 2)?;
        unsafe { asm!("out dx, ax", in("dx") port, in("ax") value, options(nomem, nostack)) };
        Ok(())
    }

    pub fn write_u32(&mut self, offset: u16, value: u32) -> Result<()> {
        let port = self.port(offset, 4)?;
        unsafe { asm!("out dx, eax", in("dx") port, in("eax") value, options(nomem, nostack)) };
        Ok(())
    }

    fn port(&self, offset: u16, width: u16) -> Result<u16> {
        let end = offset.checked_add(width).ok_or(Error::InvalidArgument)?;
        if end > self.length {
            return Err(Error::InvalidArgument);
        }
        self.base.checked_add(offset).ok_or(Error::InvalidArgument)
    }
}
