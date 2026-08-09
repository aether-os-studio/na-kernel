#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    InvalidArgument,
    Io,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    NotDirectory,
    IsDirectory,
    NotEmpty,
    TooManyLinks,
    CrossDevice,
    NoDevice,
    NoSpace,
    NoData,
    Range,
    PermissionDenied,
    NotATerminal,
    Unsupported,
    Kernel(i32),
}

pub type Result<T> = core::result::Result<T, Error>;

impl Error {
    pub const fn from_status(status: i32) -> Result<()> {
        match status {
            0.. => Ok(()),
            value if value == -(bindings::ENOMEM as i32) => Err(Self::OutOfMemory),
            value if value == -(bindings::ENODEV as i32) => Err(Self::NoDevice),
            value if value == -(bindings::EINVAL as i32) => Err(Self::InvalidArgument),
            value if value == -(bindings::EIO as i32) => Err(Self::Io),
            value if value == -(bindings::ENOSPC as i32) => Err(Self::NoSpace),
            value if value == -(bindings::ENODATA as i32) => Err(Self::NoData),
            value if value == -(bindings::ERANGE as i32) => Err(Self::Range),
            value if value == -(bindings::EPERM as i32) => Err(Self::PermissionDenied),
            value if value == -(bindings::ENOENT as i32) => Err(Self::NotFound),
            value if value == -(bindings::EEXIST as i32) => Err(Self::AlreadyExists),
            value if value == -(bindings::ENOTDIR as i32) => Err(Self::NotDirectory),
            value if value == -(bindings::EISDIR as i32) => Err(Self::IsDirectory),
            value if value == -(bindings::ENOTEMPTY as i32) => Err(Self::NotEmpty),
            value if value == -(bindings::EMLINK as i32) => Err(Self::TooManyLinks),
            value if value == -(bindings::EXDEV as i32) => Err(Self::CrossDevice),
            value
                if value == -(bindings::ENOSYS as i32) || value == -(bindings::ENOTSUP as i32) =>
            {
                Err(Self::Unsupported)
            }
            value if value == -(bindings::ENOTTY as i32) => Err(Self::NotATerminal),
            value => Err(Self::Kernel(value)),
        }
    }

    pub const fn status(self) -> i32 {
        match self {
            Self::InvalidArgument => -(bindings::EINVAL as i32),
            Self::Io => -(bindings::EIO as i32),
            Self::OutOfMemory => -(bindings::ENOMEM as i32),
            Self::NotFound => -(bindings::ENOENT as i32),
            Self::AlreadyExists => -(bindings::EEXIST as i32),
            Self::NotDirectory => -(bindings::ENOTDIR as i32),
            Self::IsDirectory => -(bindings::EISDIR as i32),
            Self::NotEmpty => -(bindings::ENOTEMPTY as i32),
            Self::TooManyLinks => -(bindings::EMLINK as i32),
            Self::CrossDevice => -(bindings::EXDEV as i32),
            Self::NoDevice => -(bindings::ENODEV as i32),
            Self::NoSpace => -(bindings::ENOSPC as i32),
            Self::NoData => -(bindings::ENODATA as i32),
            Self::Range => -(bindings::ERANGE as i32),
            Self::PermissionDenied => -(bindings::EPERM as i32),
            Self::NotATerminal => -(bindings::ENOTTY as i32),
            Self::Unsupported => -(bindings::ENOTSUP as i32),
            Self::Kernel(status) => status,
        }
    }
}
use crate::bindings;
