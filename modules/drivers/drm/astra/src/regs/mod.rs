//! Register offset and field constants, generated at build time from the
//! Linux amdgpu `asic_reg` headers (see `build.rs`).
//!
//! Layout per module: `<register>` (dword offset within the IP block),
//! `<register>_BASE_IDX` (index into the block's discovery base address
//! array) and `<register>__<field>__SHIFT`/`MASK` field constants.

include!(concat!(env!("OUT_DIR"), "/astra_regs_mod.rs"));

mod reg_access;

pub use reg_access::{DcnCursorRegs, Regs, get_field, set_field};

/// Resolves `<reg>_BASE_IDX` for a register constant at compile time:
/// `ridx!(gc::mmCP_STAT)` → `gc::base_idx("mmCP_STAT")`.
#[macro_export]
macro_rules! ridx {
    ($module:ident :: $reg:ident) => {
        $module::base_idx(stringify!($reg))
    };
}
