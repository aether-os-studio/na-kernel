#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    InvalidArgument,
    OutOfMemory,
    NotFound,
    NoDevice,
    NoSpace,
    Unsupported,
    Kernel(i32),
}

pub type Result<T> = core::result::Result<T, Error>;

impl Error {
    pub(crate) const fn from_status(status: i32) -> Result<()> {
        match status {
            0.. => Ok(()),
            value if value == -(bindings::ENOMEM as i32) => Err(Self::OutOfMemory),
            value if value == -(bindings::ENODEV as i32) => Err(Self::NoDevice),
            value if value == -(bindings::EINVAL as i32) => Err(Self::InvalidArgument),
            value if value == -(bindings::ENOSPC as i32) => Err(Self::NoSpace),
            value if value == -(bindings::ENOENT as i32) => Err(Self::NotFound),
            value if value == -(bindings::ENOSYS as i32) => Err(Self::Unsupported),
            value => Err(Self::Kernel(value)),
        }
    }

    pub(crate) const fn status(self) -> i32 {
        match self {
            Self::InvalidArgument => -(bindings::EINVAL as i32),
            Self::OutOfMemory => -(bindings::ENOMEM as i32),
            Self::NotFound => -(bindings::ENOENT as i32),
            Self::NoDevice => -(bindings::ENODEV as i32),
            Self::NoSpace => -(bindings::ENOSPC as i32),
            Self::Unsupported => -(bindings::ENOSYS as i32),
            Self::Kernel(status) => status,
        }
    }
}
use crate::bindings;
