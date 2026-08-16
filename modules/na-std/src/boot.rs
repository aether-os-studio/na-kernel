//! Boot framebuffer (the VBIOS/firmware-provided linear scanout).

use crate::{Error, Result, bindings};

/// Boot framebuffer parameters: the physical address of the linear scanout
/// buffer plus its mode and colour mask layout.
#[derive(Clone, Copy, Debug)]
pub struct Framebuffer {
    pub physical_address: u64,
    pub width: u64,
    pub height: u64,
    pub bpp: u64,
    pub pitch: u64,
    pub red_mask_size: u8,
    pub red_mask_shift: u8,
    pub green_mask_size: u8,
    pub green_mask_shift: u8,
    pub blue_mask_size: u8,
    pub blue_mask_shift: u8,
}

/// Returns the boot framebuffer, or `Error::NotFound` when the platform did
/// not provide one.
pub fn framebuffer() -> Result<Framebuffer> {
    let mut raw = bindings::na_boot_framebuffer_t {
        physical_address: 0,
        width: 0,
        height: 0,
        bpp: 0,
        pitch: 0,
        red_mask_size: 0,
        red_mask_shift: 0,
        green_mask_size: 0,
        green_mask_shift: 0,
        blue_mask_size: 0,
        blue_mask_shift: 0,
    };
    let status = unsafe { bindings::na_boot_get_framebuffer(&mut raw) };
    Error::from_status(status as i32)?;
    Ok(Framebuffer {
        physical_address: raw.physical_address,
        width: raw.width,
        height: raw.height,
        bpp: raw.bpp,
        pitch: raw.pitch,
        red_mask_size: raw.red_mask_size,
        red_mask_shift: raw.red_mask_shift,
        green_mask_size: raw.green_mask_size,
        green_mask_shift: raw.green_mask_shift,
        blue_mask_size: raw.blue_mask_size,
        blue_mask_shift: raw.blue_mask_shift,
    })
}

/// Rebinds the graphical TTYs to a driver-owned scanout framebuffer while
/// retaining their flanterm grids and cursor state.
pub fn rebind_framebuffer(framebuffer: Framebuffer) -> Result<()> {
    let raw = bindings::na_boot_framebuffer_t {
        physical_address: framebuffer.physical_address,
        width: framebuffer.width,
        height: framebuffer.height,
        bpp: framebuffer.bpp,
        pitch: framebuffer.pitch,
        red_mask_size: framebuffer.red_mask_size,
        red_mask_shift: framebuffer.red_mask_shift,
        green_mask_size: framebuffer.green_mask_size,
        green_mask_shift: framebuffer.green_mask_shift,
        blue_mask_size: framebuffer.blue_mask_size,
        blue_mask_shift: framebuffer.blue_mask_shift,
    };
    let status = unsafe { bindings::na_tty_rebind_framebuffer(&raw) };
    Error::from_status(status as i32)
}
