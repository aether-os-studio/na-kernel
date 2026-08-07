use crate::{Error, Result, bindings};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct UserAddress(u64);

impl UserAddress {
    pub const fn new(address: u64) -> Self {
        Self(address)
    }

    pub const fn is_null(self) -> bool {
        self.0 == 0
    }

    pub fn read(self, destination: &mut [u8]) -> Result<()> {
        let status = unsafe {
            bindings::na_user_read(self.0, destination.as_mut_ptr().cast(), destination.len())
        };
        Error::from_status(status)
    }

    pub fn write(self, source: &[u8]) -> Result<()> {
        let status =
            unsafe { bindings::na_user_write(self.0, source.as_ptr().cast(), source.len()) };
        Error::from_status(status)
    }
}
