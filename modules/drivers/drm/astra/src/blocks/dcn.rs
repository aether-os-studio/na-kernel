//! DCN 3.0.2 display block: brings up a single HDMI output at a fixed
//! mode. Mirrors Linux `dcn302_resource.c` + `dcn30_hwseq.c` +
//! `dce110_apply_single_controller_ctx_to_hw` + `link_dpms.c`.

use na_std::{Error, Result};

use crate::atom::{CONNECTOR_OBJECT_ID_HDMI_TYPE_A, DisplayPath};
use crate::dev_info;
use crate::device::{Adapter, ScanoutInfo};
use crate::ip::{HwIp, IpBlock, IpVersion};
use crate::mem::Bo;
use crate::regs::dcn3_0_2 as dcn;
use crate::regs::{DcnCursorRegs, Regs, get_field, set_field};

use super::dmub::Dmub;

/// Default fixed mode until EDID parsing lands (CEA-861 1920x1080@60).
const DEFAULT_PCLK_KHZ: u32 = 148_500;

const DMUB_VBIOS_DIGX_ENCODER_CONTROL: u8 = 0;
const DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL: u8 = 1;
const DMUB_VBIOS_SET_PIXEL_CLOCK: u8 = 2;

/// DCN cursor surface attributes for the active pipe-0 HUBP/DPP pair.
pub struct CursorAttributes {
    pub address: u64,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
}

/// DCN cursor position for the active, unscaled pipe-0 viewport.
pub struct CursorPosition {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub viewport_width: u32,
    pub viewport_height: u32,
}

/// Direct DCN cursor controller for the active HUBP/DPP pair. It owns the
/// independent BAR5 register view, so cursor motion stays outside the global
/// GPU submission lock while still being serialized by its containing state.
pub struct DcnCursor {
    regs: DcnCursorRegs,
}

impl DcnCursor {
    pub fn new(regs: DcnCursorRegs) -> Self {
        Self { regs }
    }

    fn rmw(&mut self, reg: u32, base_idx: usize, shift: u64, mask: u64, value: u64) -> Result<()> {
        let current = self.regs.read_dcn(reg, base_idx)?;
        self.regs
            .write_dcn(reg, base_idx, set_field(current, shift, mask, value))
    }

    /// Disables the DCN3 cursor in both HUBP and DPP.
    pub fn disable(&mut self) -> Result<()> {
        let hubp = dcn::mmCURSOR0_0_CURSOR_CONTROL_BASE_IDX as usize;
        let dpp = dcn::mmCNVC_CUR0_CURSOR0_CONTROL_BASE_IDX as usize;
        self.rmw(
            dcn::mmCURSOR0_0_CURSOR_CONTROL,
            hubp,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE__SHIFT,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK,
            0,
        )?;
        self.rmw(
            dcn::mmCNVC_CUR0_CURSOR0_CONTROL,
            dpp,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE__SHIFT,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK,
            0,
        )
    }

    /// Programs cursor address, size and format, matching Linux's
    /// `hubp2_cursor_set_attributes()` + `dpp1_set_cursor_attributes()`.
    /// This is deliberately separate from position updates: the Linux MOVE path
    /// does not relatch the cursor surface address or rewrite its attributes.
    pub fn set_attributes(&mut self, attributes: CursorAttributes) -> Result<()> {
        let hubp = dcn::mmCURSOR0_0_CURSOR_CONTROL_BASE_IDX as usize;
        let dpp = dcn::mmCNVC_CUR0_CURSOR0_CONTROL_BASE_IDX as usize;

        if attributes.address == 0
            || attributes.width == 0
            || attributes.height == 0
            || attributes.width > 256
            || attributes.height > 256
        {
            return Err(Error::InvalidArgument);
        }
        let hw_pitch = match attributes.pitch {
            64 => 0,
            128 => 1,
            256 => 2,
            _ => return Err(Error::InvalidArgument),
        };
        // Linux hubp2_get_lines_per_chunk(), 32-bit premultiplied-alpha mode.
        let lines_per_chunk = match attributes.width {
            1..=32 => 4,
            33..=64 => 3,
            65..=128 => 2,
            _ => 1,
        };

        // hubp2_cursor_set_attributes(): write HIGH before LOW because the low
        // address write latches the complete cursor surface address.
        self.regs.write_dcn(
            dcn::mmCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH,
            dcn::mmCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH_BASE_IDX as usize,
            (attributes.address >> 32) as u32,
        )?;
        self.regs.write_dcn(
            dcn::mmCURSOR0_0_CURSOR_SURFACE_ADDRESS,
            dcn::mmCURSOR0_0_CURSOR_SURFACE_ADDRESS_BASE_IDX as usize,
            attributes.address as u32,
        )?;
        self.regs.write_dcn(
            dcn::mmCURSOR0_0_CURSOR_SIZE,
            dcn::mmCURSOR0_0_CURSOR_SIZE_BASE_IDX as usize,
            set_field(
                set_field(
                    0,
                    dcn::CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH__SHIFT,
                    dcn::CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK,
                    attributes.width as u64,
                ),
                dcn::CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT__SHIFT,
                dcn::CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT_MASK,
                attributes.height as u64,
            ),
        )?;
        let mut control = self.regs.read_dcn(dcn::mmCURSOR0_0_CURSOR_CONTROL, hubp)?;
        control = set_field(
            control,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE__SHIFT,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE_MASK,
            2, // CURSOR_MODE_COLOR_PRE_MULTIPLIED_ALPHA
        );
        control = set_field(
            control,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_2X_MAGNIFY__SHIFT,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_2X_MAGNIFY_MASK,
            0,
        );
        control = set_field(
            control,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH__SHIFT,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH_MASK,
            hw_pitch,
        );
        control = set_field(
            control,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_LINES_PER_CHUNK__SHIFT,
            dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_LINES_PER_CHUNK_MASK,
            lines_per_chunk,
        );
        self.regs
            .write_dcn(dcn::mmCURSOR0_0_CURSOR_CONTROL, hubp, control)?;
        self.regs.write_dcn(
            dcn::mmHUBPREQ0_CURSOR_SETTINGS,
            dcn::mmHUBPREQ0_CURSOR_SETTINGS_BASE_IDX as usize,
            set_field(
                0,
                dcn::HUBPREQ0_CURSOR_SETTINGS__CURSOR0_CHUNK_HDL_ADJUST__SHIFT,
                dcn::HUBPREQ0_CURSOR_SETTINGS__CURSOR0_CHUNK_HDL_ADJUST_MASK,
                3,
            ),
        )?;

        // dpp3_set_cursor_attributes(). Degamma ROM remains disabled, matching
        // Linux unless the CRTC color-management state explicitly requests it.
        let mut dpp_control = self.regs.read_dcn(dcn::mmCNVC_CUR0_CURSOR0_CONTROL, dpp)?;
        dpp_control = set_field(
            dpp_control,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE__SHIFT,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE_MASK,
            2,
        );
        dpp_control = set_field(
            dpp_control,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_EXPANSION_MODE__SHIFT,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_EXPANSION_MODE_MASK,
            0,
        );
        dpp_control = set_field(
            dpp_control,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_ROM_EN__SHIFT,
            dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_ROM_EN_MASK,
            0,
        );
        self.regs
            .write_dcn(dcn::mmCNVC_CUR0_CURSOR0_CONTROL, dpp, dpp_control)
    }

    /// Programs only cursor position and visibility, matching Linux's
    /// `hubp2_cursor_set_position()` + `dpp1_set_cursor_position()` MOVE path.
    /// `was_visible` is the DC software shadow used to avoid two control-register
    /// read/modify/write cycles on every mouse motion, as Linux does.
    pub fn set_position(&mut self, position: CursorPosition, was_visible: bool) -> Result<bool> {
        let hubp = dcn::mmCURSOR0_0_CURSOR_CONTROL_BASE_IDX as usize;
        let dpp = dcn::mmCNVC_CUR0_CURSOR0_CONTROL_BASE_IDX as usize;

        if position.width == 0
            || position.height == 0
            || position.width > 256
            || position.height > 256
        {
            return Err(Error::InvalidArgument);
        }

        // dcn10_set_cursor_position() represents negative screen coordinates by
        // clamping the position to zero and increasing the hardware hotspot.
        let hot_x = if position.x < 0 {
            position
                .x
                .unsigned_abs()
                .min(position.width.saturating_sub(1))
        } else {
            0
        };
        let hot_y = if position.y < 0 {
            position
                .y
                .unsigned_abs()
                .min(position.height.saturating_sub(1))
        } else {
            0
        };
        let position_x = position.x.max(0) as u32;
        let position_y = position.y.max(0) as u32;
        let visible = position.x < position.viewport_width as i32
            && position.y < position.viewport_height as i32
            && i64::from(position.x) + i64::from(position.width) > 0
            && i64::from(position.y) + i64::from(position.height) > 0;

        // The Navi23 DCN302 hub timer runs at the 100 MHz display crystal divided
        // by two. This is the same ref/pixel conversion performed by
        // hubp2_cursor_set_position() for the unscaled 148.5 MHz pipe.
        let src_x_offset = i64::from(position_x) - i64::from(hot_x);
        let dst_x_offset = ((src_x_offset.max(0) as u64) * 50_000 / u64::from(DEFAULT_PCLK_KHZ))
            .min(0x1fff) as u32;
        if visible != was_visible {
            self.rmw(
                dcn::mmCURSOR0_0_CURSOR_CONTROL,
                hubp,
                dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE__SHIFT,
                dcn::CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK,
                visible as u64,
            )?;
        }
        self.regs.write_dcn(
            dcn::mmCURSOR0_0_CURSOR_POSITION,
            dcn::mmCURSOR0_0_CURSOR_POSITION_BASE_IDX as usize,
            set_field(
                set_field(
                    0,
                    dcn::CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION__SHIFT,
                    dcn::CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK,
                    position_x as u64,
                ),
                dcn::CURSOR0_0_CURSOR_POSITION__CURSOR_Y_POSITION__SHIFT,
                dcn::CURSOR0_0_CURSOR_POSITION__CURSOR_Y_POSITION_MASK,
                position_y as u64,
            ),
        )?;
        self.regs.write_dcn(
            dcn::mmCURSOR0_0_CURSOR_HOT_SPOT,
            dcn::mmCURSOR0_0_CURSOR_HOT_SPOT_BASE_IDX as usize,
            set_field(
                set_field(
                    0,
                    dcn::CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_X__SHIFT,
                    dcn::CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_X_MASK,
                    hot_x as u64,
                ),
                dcn::CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_Y__SHIFT,
                dcn::CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_Y_MASK,
                hot_y as u64,
            ),
        )?;
        self.regs.write_dcn(
            dcn::mmCURSOR0_0_CURSOR_DST_OFFSET,
            dcn::mmCURSOR0_0_CURSOR_DST_OFFSET_BASE_IDX as usize,
            set_field(
                0,
                dcn::CURSOR0_0_CURSOR_DST_OFFSET__CURSOR_DST_X_OFFSET__SHIFT,
                dcn::CURSOR0_0_CURSOR_DST_OFFSET__CURSOR_DST_X_OFFSET_MASK,
                dst_x_offset as u64,
            ),
        )?;
        if visible != was_visible {
            self.rmw(
                dcn::mmCNVC_CUR0_CURSOR0_CONTROL,
                dpp,
                dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE__SHIFT,
                dcn::CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK,
                visible as u64,
            )?;
        }
        Ok(visible)
    }
}

/// Complete DCN3 graphics-plane state consumed by
/// `hubp3_program_surface_config()` followed by
/// `hubp2_program_surface_flip_and_addr()`.
pub struct PrimarySurfaceConfig {
    pub address: u64,
    pub meta_address: Option<u64>,
    pub width: u32,
    pub height: u32,
    /// DRM pitch in bytes for the supported 32-bpp formats.
    pub pitch: u32,
    pub swizzle: u32,
    /// Encoded GB_ADDR_CONFIG fields (log2 values), matching the values
    /// Linux ultimately writes through hubp3_program_tiling().
    pub num_pipes: u32,
    pub pipe_interleave: u32,
    pub max_compressed_frags: u32,
    pub num_pkrs: u32,
    pub meta_pitch: u32,
    pub dcc_independent_block: u32,
}

/// Mutable view of the active DCN302 display pipe. The short-lived borrow
/// groups primary-plane programming without creating a second register owner
/// in the display device.
pub struct DcnDisplayPipe<'a> {
    regs: &'a mut Regs,
}

impl<'a> DcnDisplayPipe<'a> {
    pub fn new(regs: &'a mut Regs) -> Self {
        Self { regs }
    }
}

/// Linux DCN3 `hubp21_program_requestor()` for the supported 32-bpp graphics
/// plane.  These registers are part of the pipe state, not firmware defaults:
/// switching from the VBIOS linear surface to a Mesa tiled surface changes
/// the detile swath height and therefore requires a fresh RQ setup.
impl DcnBlock {
    fn program_primary_requestor(
        regs: &mut Regs,
        width: u32,
        pitch_pixels: u32,
        swizzle: u32,
    ) -> Result<()> {
        if width == 0 || pitch_pixels < width {
            return Err(Error::InvalidArgument);
        }

        // dcn30/display_rq_dlg_calc_30.c, dm_444_32 horizontal scan:
        // an RGB 256-byte block is 8x8 pixels for every non-linear swizzle.
        // A 184-KiB DCN3 DET can use full 256-byte requests while two complete
        // swaths fit; otherwise DML halves the stored swath height for 128-byte
        // requests.
        let swath_height = if swizzle == 0 {
            0
        } else {
            let swath_width = width.checked_add(7).ok_or(Error::Range)? & !7;
            let full_swath_bytes = swath_width
                .checked_mul(8)
                .and_then(|value| value.checked_mul(4))
                .ok_or(Error::Range)?;
            if full_swath_bytes.checked_mul(2).ok_or(Error::Range)? > 184 * 1024 {
                2 // log2(4 lines), 128-byte requests
            } else {
                3 // log2(8 lines), 256-byte requests
            }
        };

        // DML's PTE row height field is log2(row_height) - 3.  Linear scanout
        // derives a pitch-dependent row height; tiled 4-KiB blocks use 32 lines,
        // while the 64/256-KiB modes used by GFX10 use 128 lines.
        let pte_row_height = if swizzle == 0 {
            let row_lines = (688_128 / pitch_pixels).max(1);
            row_lines.ilog2().min(7).saturating_sub(3)
        } else {
            match swizzle >> 2 {
                1 | 5 => 2,
                _ => 4,
            }
        };

        // extract_rq_sizing_regs() with the DCN3 IP constants:
        // chunk=8 KiB, min_chunk=1 KiB, meta_chunk=2 KiB,
        // min_meta_chunk=256 B, DPTE/VM group=2 KiB.
        let hubp = dcn::mmHUBP0_DCHUBP_REQ_SIZE_CONFIG_BASE_IDX as usize;
        let mut request_size = 0;
        request_size = set_field(
            request_size,
            dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__SWATH_HEIGHT__SHIFT,
            dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__SWATH_HEIGHT_MASK,
            swath_height,
        );
        request_size = set_field(
            request_size,
            dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__PTE_ROW_HEIGHT_LINEAR__SHIFT,
            dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__PTE_ROW_HEIGHT_LINEAR_MASK,
            pte_row_height as u64,
        );
        for (shift, mask, value) in [
            (
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__CHUNK_SIZE__SHIFT,
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__CHUNK_SIZE_MASK,
                3,
            ),
            (
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__MIN_CHUNK_SIZE__SHIFT,
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__MIN_CHUNK_SIZE_MASK,
                3,
            ),
            (
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__META_CHUNK_SIZE__SHIFT,
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__META_CHUNK_SIZE_MASK,
                1,
            ),
            (
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__MIN_META_CHUNK_SIZE__SHIFT,
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__MIN_META_CHUNK_SIZE_MASK,
                3,
            ),
            (
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__DPTE_GROUP_SIZE__SHIFT,
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__DPTE_GROUP_SIZE_MASK,
                5,
            ),
            (
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__VM_GROUP_SIZE__SHIFT,
                dcn::HUBP0_DCHUBP_REQ_SIZE_CONFIG__VM_GROUP_SIZE_MASK,
                5,
            ),
        ] {
            request_size = set_field(request_size, shift, mask, value);
        }
        regs.write_dcn(dcn::mmHUBP0_DCHUBP_REQ_SIZE_CONFIG, hubp, request_size)?;
        // XRGB/ARGB is a single-plane format; Linux zero-initializes the chroma
        // request registers before hubp21_program_requestor().
        regs.write_dcn(dcn::mmHUBP0_DCHUBP_REQ_SIZE_CONFIG_C, hubp, 0)?;

        let expansion = set_field(
            set_field(
                set_field(
                    set_field(
                        0,
                        dcn::HUBPREQ0_DCN_EXPANSION_MODE__DRQ_EXPANSION_MODE__SHIFT,
                        dcn::HUBPREQ0_DCN_EXPANSION_MODE__DRQ_EXPANSION_MODE_MASK,
                        2,
                    ),
                    dcn::HUBPREQ0_DCN_EXPANSION_MODE__PRQ_EXPANSION_MODE__SHIFT,
                    dcn::HUBPREQ0_DCN_EXPANSION_MODE__PRQ_EXPANSION_MODE_MASK,
                    1,
                ),
                dcn::HUBPREQ0_DCN_EXPANSION_MODE__MRQ_EXPANSION_MODE__SHIFT,
                dcn::HUBPREQ0_DCN_EXPANSION_MODE__MRQ_EXPANSION_MODE_MASK,
                1,
            ),
            dcn::HUBPREQ0_DCN_EXPANSION_MODE__CRQ_EXPANSION_MODE__SHIFT,
            dcn::HUBPREQ0_DCN_EXPANSION_MODE__CRQ_EXPANSION_MODE_MASK,
            1,
        );
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCN_EXPANSION_MODE,
            dcn::mmHUBPREQ0_DCN_EXPANSION_MODE_BASE_IDX as usize,
            expansion,
        )?;

        // Single-plane RGB owns the entire DET buffer.
        Self::rmw(
            regs,
            dcn::mmHUBPRET0_HUBPRET_CONTROL,
            dcn::mmHUBPRET0_HUBPRET_CONTROL_BASE_IDX as usize,
            dcn::HUBPRET0_HUBPRET_CONTROL__DET_BUF_PLANE1_BASE_ADDRESS__SHIFT,
            dcn::HUBPRET0_HUBPRET_CONTROL__DET_BUF_PLANE1_BASE_ADDRESS_MASK,
            0,
        )
    }
}

/// Linux `min_set_viewport()` writes the primary, secondary (stereo), and
/// chroma viewport banks together.  Keeping all banks coherent prevents an
/// inherited VBIOS bank from becoming visible during a surface transition.
impl DcnBlock {
    fn program_primary_viewports(regs: &mut Regs, width: u32, height: u32) -> Result<()> {
        let hubp = dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_BASE_IDX as usize;
        let dimension = (height << 16) | width;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, hubp, dimension)?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_START, hubp, 0)?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_SEC_VIEWPORT_DIMENSION, hubp, dimension)?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_SEC_VIEWPORT_START, hubp, 0)?;
        // RGB is single-plane, so its chroma viewport is zero exactly as in the
        // zero-initialized Linux scaler state.
        regs.write_dcn(dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C, hubp, 0)?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_PRI_VIEWPORT_START_C, hubp, 0)?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C, hubp, 0)?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_SEC_VIEWPORT_START_C, hubp, 0)
    }
}

/// Linux `dpp1_dscl_set_scaler_manual_scale()` for a full-screen 1:1 RGB
/// primary plane.  Even when scaling is bypassed, DC programs RECOUT and MPC
/// size explicitly; leaving either register inherited from firmware clips
/// both the primary surface and the DPP hardware cursor to the stale rect.
impl DcnDisplayPipe<'_> {
    pub fn set_primary_geometry(&mut self, width: u32, height: u32) -> Result<()> {
        let regs = &mut *self.regs;
        if width == 0 || height == 0 || width > 0x3fff || height > 0x3fff {
            return Err(Error::InvalidArgument);
        }

        let dscl = dcn::mmDSCL0_SCL_MODE_BASE_IDX as usize;

        // dpp1_dscl_set_scaler_manual_scale(): disable AutoCal and clear the
        // boundary mode before programming the manual RECOUT rectangle.
        regs.write_dcn(dcn::mmDSCL0_DSCL_AUTOCAL, dscl, 0)?;
        regs.write_dcn(dcn::mmDSCL0_DSCL_CONTROL, dscl, 0)?;
        regs.write_dcn(dcn::mmDSCL0_RECOUT_START, dscl, 0)?;
        regs.write_dcn(
            dcn::mmDSCL0_RECOUT_SIZE,
            dscl,
            set_field(
                set_field(
                    0,
                    dcn::DSCL0_RECOUT_SIZE__RECOUT_WIDTH__SHIFT,
                    dcn::DSCL0_RECOUT_SIZE__RECOUT_WIDTH_MASK,
                    width as u64,
                ),
                dcn::DSCL0_RECOUT_SIZE__RECOUT_HEIGHT__SHIFT,
                dcn::DSCL0_RECOUT_SIZE__RECOUT_HEIGHT_MASK,
                height as u64,
            ),
        )?;
        regs.write_dcn(
            dcn::mmDSCL0_MPC_SIZE,
            dscl,
            set_field(
                set_field(
                    0,
                    dcn::DSCL0_MPC_SIZE__MPC_WIDTH__SHIFT,
                    dcn::DSCL0_MPC_SIZE__MPC_WIDTH_MASK,
                    width as u64,
                ),
                dcn::DSCL0_MPC_SIZE__MPC_HEIGHT__SHIFT,
                dcn::DSCL0_MPC_SIZE__MPC_HEIGHT_MASK,
                height as u64,
            ),
        )?;

        // Identity ratios select SCALING_444_BYPASS (0), not DSCL_BYPASS (6).
        DcnBlock::rmw(
            regs,
            dcn::mmDSCL0_SCL_MODE,
            dscl,
            dcn::DSCL0_SCL_MODE__DSCL_MODE__SHIFT,
            dcn::DSCL0_SCL_MODE__DSCL_MODE_MASK,
            0,
        )?;

        // DCN3 processes DSCL data in float format, so Linux only programs the
        // interleave/alpha bits here.  XRGB primary scanout has no per-pixel
        // alpha.  Dimgrey Cavefish selects the dcn302 debug defaults, whose
        // use_max_lb=true makes dpp1_dscl_find_lb_memory_config() return
        // LB_MEMORY_CONFIG_0 for every non-4:2:0 plane.  Do not use the generic
        // first-fit fallback here: it would select config 1 at 1920 pixels and
        // partition the line buffer differently from Linux on this ASIC.
        regs.write_dcn(dcn::mmDSCL0_LB_DATA_FORMAT, dscl, 0)?;
        regs.write_dcn(
            dcn::mmDSCL0_LB_MEMORY_CTRL,
            dscl,
            set_field(
                set_field(
                    0,
                    dcn::DSCL0_LB_MEMORY_CTRL__MEMORY_CONFIG__SHIFT,
                    dcn::DSCL0_LB_MEMORY_CTRL__MEMORY_CONFIG_MASK,
                    0,
                ),
                dcn::DSCL0_LB_MEMORY_CTRL__LB_MAX_PARTITIONS__SHIFT,
                dcn::DSCL0_LB_MEMORY_CTRL__LB_MAX_PARTITIONS_MASK,
                63,
            ),
        )
    }
}

/// Linux `hubp3_program_surface_config()` + `min_set_viewport()` +
/// `dcn20_update_plane_addr()` for the active pipe-0 graphics plane.  Surface
/// layout must be updated together with the address: Mesa alternates tiled
/// scanout BOs whose swizzle cannot be inherited from the VBIOS linear FB.
impl DcnDisplayPipe<'_> {
    pub fn set_primary_surface(&mut self, config: &PrimarySurfaceConfig) -> Result<()> {
        let regs = &mut *self.regs;
        if config.address == 0
            || config.width == 0
            || config.height == 0
            || config.pitch == 0
            || config.pitch & 3 != 0
            || config.swizzle > 0x1f
            || config.dcc_independent_block > 3
        {
            return Err(Error::InvalidArgument);
        }

        let hubp = dcn::mmHUBP0_DCSURF_ADDR_CONFIG_BASE_IDX as usize;
        let hubpreq = dcn::mmHUBPREQ0_DCSURF_SURFACE_CONTROL_BASE_IDX as usize;
        let dcc_enabled = config.meta_address.is_some();
        if dcc_enabled != (config.meta_pitch != 0) {
            return Err(Error::InvalidArgument);
        }

        let pitch_pixels = config.pitch / 4;
        DcnBlock::program_primary_requestor(regs, config.width, pitch_pixels, config.swizzle)?;

        // hubp3_dcc_control_sienna_cichlid().
        let mut surface_control = regs.read_dcn(dcn::mmHUBPREQ0_DCSURF_SURFACE_CONTROL, hubpreq)?;
        surface_control = set_field(
            surface_control,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK,
            dcc_enabled as u64,
        );
        surface_control = set_field(
            surface_control,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_IND_BLK__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_IND_BLK_MASK,
            config.dcc_independent_block as u64,
        );
        surface_control = set_field(
            surface_control,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_IND_BLK_C__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_IND_BLK_C_MASK,
            0,
        );
        surface_control = set_field(
            surface_control,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_EN__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_EN_MASK,
            dcc_enabled as u64,
        );
        surface_control = set_field(
            surface_control,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_IND_BLK__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_IND_BLK_MASK,
            config.dcc_independent_block as u64,
        );
        surface_control = set_field(
            surface_control,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_IND_BLK_C__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_CONTROL__SECONDARY_SURFACE_DCC_IND_BLK_C_MASK,
            0,
        );
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_SURFACE_CONTROL,
            hubpreq,
            surface_control,
        )?;

        // hubp3_program_tiling().  GFX10 ignores the older bank/SE/RB fields.
        let addr_config = set_field(
            set_field(
                set_field(
                    set_field(
                        regs.read_dcn(dcn::mmHUBP0_DCSURF_ADDR_CONFIG, hubp)?,
                        dcn::HUBP0_DCSURF_ADDR_CONFIG__NUM_PIPES__SHIFT,
                        dcn::HUBP0_DCSURF_ADDR_CONFIG__NUM_PIPES_MASK,
                        config.num_pipes as u64,
                    ),
                    dcn::HUBP0_DCSURF_ADDR_CONFIG__PIPE_INTERLEAVE__SHIFT,
                    dcn::HUBP0_DCSURF_ADDR_CONFIG__PIPE_INTERLEAVE_MASK,
                    config.pipe_interleave as u64,
                ),
                dcn::HUBP0_DCSURF_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT,
                dcn::HUBP0_DCSURF_ADDR_CONFIG__MAX_COMPRESSED_FRAGS_MASK,
                config.max_compressed_frags as u64,
            ),
            dcn::HUBP0_DCSURF_ADDR_CONFIG__NUM_PKRS__SHIFT,
            dcn::HUBP0_DCSURF_ADDR_CONFIG__NUM_PKRS_MASK,
            config.num_pkrs as u64,
        );
        regs.write_dcn(dcn::mmHUBP0_DCSURF_ADDR_CONFIG, hubp, addr_config)?;
        let tiling_config = set_field(
            set_field(
                set_field(
                    regs.read_dcn(dcn::mmHUBP0_DCSURF_TILING_CONFIG, hubp)?,
                    dcn::HUBP0_DCSURF_TILING_CONFIG__SW_MODE__SHIFT,
                    dcn::HUBP0_DCSURF_TILING_CONFIG__SW_MODE_MASK,
                    config.swizzle as u64,
                ),
                dcn::HUBP0_DCSURF_TILING_CONFIG__META_LINEAR__SHIFT,
                dcn::HUBP0_DCSURF_TILING_CONFIG__META_LINEAR_MASK,
                0,
            ),
            dcn::HUBP0_DCSURF_TILING_CONFIG__PIPE_ALIGNED__SHIFT,
            dcn::HUBP0_DCSURF_TILING_CONFIG__PIPE_ALIGNED_MASK,
            0,
        );
        regs.write_dcn(dcn::mmHUBP0_DCSURF_TILING_CONFIG, hubp, tiling_config)?;

        // hubp2_program_size(): pitch is in pixels and encoded minus one; DCC
        // meta pitch is already in the units supplied by addrlib/Mesa.
        let surface_pitch = set_field(
            set_field(
                regs.read_dcn(
                    dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH,
                    dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH_BASE_IDX as usize,
                )?,
                dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH__SHIFT,
                dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK,
                (pitch_pixels - 1) as u64,
            ),
            dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__META_PITCH__SHIFT,
            dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__META_PITCH_MASK,
            if dcc_enabled {
                (config.meta_pitch - 1) as u64
            } else {
                0
            },
        );
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH,
            dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH_BASE_IDX as usize,
            surface_pitch,
        )?;

        // Full, unscaled viewport at (0, 0), including Linux's secondary/chroma
        // viewport banks.
        DcnBlock::program_primary_viewports(regs, config.width, config.height)?;
        let mut surface_config = regs.read_dcn(dcn::mmHUBP0_DCSURF_SURFACE_CONFIG, hubp)?;
        surface_config = set_field(
            surface_config,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT__SHIFT,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK,
            8,
        );
        surface_config = set_field(
            surface_config,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE__SHIFT,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE_MASK,
            0,
        );
        surface_config = set_field(
            surface_config,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__H_MIRROR_EN__SHIFT,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__H_MIRROR_EN_MASK,
            0,
        );
        regs.write_dcn(dcn::mmHUBP0_DCSURF_SURFACE_CONFIG, hubp, surface_config)?;

        // hubp2_program_pixel_format(SURFACE_PIXEL_FORMAT_GRPH_ARGB8888).
        // The surface-format value is shared with ABGR; HUBPRET selects the
        // component ordering.  XRGB8888/ARGB8888 use B=2 and R=3.
        let hubpret = dcn::mmHUBPRET0_HUBPRET_CONTROL_BASE_IDX as usize;
        let mut hubpret_control = regs.read_dcn(dcn::mmHUBPRET0_HUBPRET_CONTROL, hubpret)?;
        hubpret_control = set_field(
            hubpret_control,
            dcn::HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B__SHIFT,
            dcn::HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK,
            2,
        );
        hubpret_control = set_field(
            hubpret_control,
            dcn::HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R__SHIFT,
            dcn::HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK,
            3,
        );
        regs.write_dcn(dcn::mmHUBPRET0_HUBPRET_CONTROL, hubpret, hubpret_control)?;

        // dpp3_cnv_setup(SURFACE_PIXEL_FORMAT_GRPH_ARGB8888,
        // EXPANSION_MODE_ZERO, COLOR_SPACE_SRGB).  Program the full format state
        // on each new primary surface instead of inheriting the VBIOS pipe state.
        let cnvc = dcn::mmCNVC_CFG0_FORMAT_CONTROL_BASE_IDX as usize;
        let format_control = set_field(
            set_field(
                set_field(
                    set_field(
                        set_field(
                            set_field(
                                set_field(
                                    set_field(
                                        set_field(
                                            0,
                                            dcn::CNVC_CFG0_FORMAT_CONTROL__CNVC_BYPASS__SHIFT,
                                            dcn::CNVC_CFG0_FORMAT_CONTROL__CNVC_BYPASS_MASK,
                                            0,
                                        ),
                                        dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_EXPANSION_MODE__SHIFT,
                                        dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_EXPANSION_MODE_MASK,
                                        1,
                                    ),
                                    dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CNV16__SHIFT,
                                    dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CNV16_MASK,
                                    0,
                                ),
                                dcn::CNVC_CFG0_FORMAT_CONTROL__CNVC_BYPASS_MSB_ALIGN__SHIFT,
                                dcn::CNVC_CFG0_FORMAT_CONTROL__CNVC_BYPASS_MSB_ALIGN_MASK,
                                0,
                            ),
                            dcn::CNVC_CFG0_FORMAT_CONTROL__CLAMP_POSITIVE__SHIFT,
                            dcn::CNVC_CFG0_FORMAT_CONTROL__CLAMP_POSITIVE_MASK,
                            0,
                        ),
                        dcn::CNVC_CFG0_FORMAT_CONTROL__CLAMP_POSITIVE_C__SHIFT,
                        dcn::CNVC_CFG0_FORMAT_CONTROL__CLAMP_POSITIVE_C_MASK,
                        0,
                    ),
                    dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CROSSBAR_R__SHIFT,
                    dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CROSSBAR_R_MASK,
                    0,
                ),
                dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CROSSBAR_G__SHIFT,
                dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CROSSBAR_G_MASK,
                1,
            ),
            dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CROSSBAR_B__SHIFT,
            dcn::CNVC_CFG0_FORMAT_CONTROL__FORMAT_CROSSBAR_B_MASK,
            2,
        );
        let format_control = set_field(
            format_control,
            dcn::CNVC_CFG0_FORMAT_CONTROL__ALPHA_EN__SHIFT,
            dcn::CNVC_CFG0_FORMAT_CONTROL__ALPHA_EN_MASK,
            1,
        );
        regs.write_dcn(dcn::mmCNVC_CFG0_FORMAT_CONTROL, cnvc, format_control)?;
        regs.write_dcn(
            dcn::mmCNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT,
            dcn::mmCNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT_BASE_IDX as usize,
            set_field(
                set_field(
                    0,
                    dcn::CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT__SHIFT,
                    dcn::CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT_MASK,
                    8,
                ),
                dcn::CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_ALPHA_PLANE_ENABLE__SHIFT,
                dcn::CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_ALPHA_PLANE_ENABLE_MASK,
                0,
            ),
        )?;
        regs.write_dcn(
            dcn::mmCNVC_CFG0_PRE_DEALPHA,
            dcn::mmCNVC_CFG0_PRE_DEALPHA_BASE_IDX as usize,
            0,
        )?;
        regs.write_dcn(
            dcn::mmCNVC_CFG0_PRE_REALPHA,
            dcn::mmCNVC_CFG0_PRE_REALPHA_BASE_IDX as usize,
            0,
        )?;
        DcnBlock::rmw(
            regs,
            dcn::mmCM0_CM_POST_CSC_CONTROL,
            dcn::mmCM0_CM_POST_CSC_CONTROL_BASE_IDX as usize,
            dcn::CM0_CM_POST_CSC_CONTROL__CM_POST_CSC_MODE__SHIFT,
            dcn::CM0_CM_POST_CSC_CONTROL__CM_POST_CSC_MODE_MASK,
            0,
        )?;

        self.set_primary_address(config.address, config.meta_address)
    }
}

/// Linux `hubp3_program_surface_flip_and_addr()`.  An ordinary page flip
/// changes only the base addresses; requestor, tiling, pitch, viewport and
/// DPP format state stay programmed until the plane layout changes.
impl DcnDisplayPipe<'_> {
    pub fn set_primary_address(&mut self, address: u64, meta_address: Option<u64>) -> Result<()> {
        let regs = &mut *self.regs;
        if address == 0 {
            return Err(Error::InvalidArgument);
        }

        // Normal vblank flip, VMID 0.  Linux writes metadata first, then primary
        // HIGH and LOW; the final LOW write latches the complete address set.
        DcnBlock::rmw(
            regs,
            dcn::mmHUBPREQ0_DCSURF_FLIP_CONTROL,
            dcn::mmHUBPREQ0_DCSURF_FLIP_CONTROL_BASE_IDX as usize,
            dcn::HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE__SHIFT,
            dcn::HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK,
            0,
        )?;
        DcnBlock::rmw(
            regs,
            dcn::mmHUBPREQ0_VMID_SETTINGS_0,
            dcn::mmHUBPREQ0_VMID_SETTINGS_0_BASE_IDX as usize,
            dcn::HUBPREQ0_VMID_SETTINGS_0__VMID__SHIFT,
            dcn::HUBPREQ0_VMID_SETTINGS_0__VMID_MASK,
            0,
        )?;
        if let Some(meta_address) = meta_address {
            regs.write_dcn(
                dcn::mmHUBPREQ0_DCSURF_PRIMARY_META_SURFACE_ADDRESS_HIGH,
                dcn::mmHUBPREQ0_DCSURF_PRIMARY_META_SURFACE_ADDRESS_HIGH_BASE_IDX as usize,
                (meta_address >> 32) as u32,
            )?;
            regs.write_dcn(
                dcn::mmHUBPREQ0_DCSURF_PRIMARY_META_SURFACE_ADDRESS,
                dcn::mmHUBPREQ0_DCSURF_PRIMARY_META_SURFACE_ADDRESS_BASE_IDX as usize,
                meta_address as u32,
            )?;
        }
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX as usize,
            (address >> 32) as u32,
        )?;
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX as usize,
            address as u32,
        )
    }
}

/// One stream timing (Linux `dc_crtc_timing` subset).
#[derive(Clone, Copy)]
struct Timing {
    pub h_total: u16,
    pub h_addressable: u16,
    pub h_front_porch: u16,
    pub _h_back_porch: u16,
    pub h_sync_width: u16,
    pub v_total: u16,
    pub v_addressable: u16,
    pub v_front_porch: u16,
    pub _v_back_porch: u16,
    pub v_sync_width: u16,
    pub h_positive: bool,
    pub v_positive: bool,
    /// Pixel clock in kHz.
    pub pix_clk_khz: u32,
}

impl Timing {
    /// CEA-861 1920x1080@60.
    fn hdmi_1080p60() -> Self {
        Self {
            h_total: 2200,
            h_addressable: 1920,
            h_front_porch: 88,
            _h_back_porch: 148,
            h_sync_width: 44,
            v_total: 1125,
            v_addressable: 1080,
            v_front_porch: 4,
            _v_back_porch: 36,
            v_sync_width: 5,
            h_positive: true,
            v_positive: true,
            pix_clk_khz: DEFAULT_PCLK_KHZ,
        }
    }
}

pub struct DcnBlock {
    _version: IpVersion,
    /// Framebuffer (VRAM BO) the DCN scans out.
    fb: Option<Bo>,
    /// Current timing.
    timing: Timing,
    /// DMCUB service used for all DCN302 VBIOS command tables.
    dmub: Dmub,
    /// Selected physical HDMI connector/encoder path from displayobjectinfo.
    path: Option<DisplayPath>,
    /// Framebuffer dimensions.
    width: u16,
    height: u16,
}

impl DcnBlock {
    pub fn new(version: IpVersion) -> Self {
        let timing = Timing::hdmi_1080p60();
        Self {
            _version: version,
            fb: None,
            timing,
            dmub: Dmub::new(),
            path: None,
            width: timing.h_addressable,
            height: timing.v_addressable,
        }
    }

    fn rmw(
        regs: &mut Regs,
        reg: u32,
        base_idx: usize,
        shift: u64,
        mask: u64,
        value: u64,
    ) -> Result<()> {
        let current = regs.read_dcn(reg, base_idx)?;
        regs.write_dcn(reg, base_idx, set_field(current, shift, mask, value))
    }

    /// HPD mask used by Linux's DCN30 GPIO translation for the connector.
    fn hpd_y_mask(path: DisplayPath) -> u32 {
        match path.hpd_sel {
            1 => dcn::DC_GPIO_HPD_Y__DC_GPIO_HPD1_Y_MASK as u32,
            2 => dcn::DC_GPIO_HPD_Y__DC_GPIO_HPD2_Y_MASK as u32,
            3 => dcn::DC_GPIO_HPD_Y__DC_GPIO_HPD3_Y_MASK as u32,
            4 => dcn::DC_GPIO_HPD_Y__DC_GPIO_HPD4_Y_MASK as u32,
            5 => dcn::DC_GPIO_HPD_Y__DC_GPIO_HPD5_Y_MASK as u32,
            6 => dcn::DC_GPIO_HPD_Y__DC_GPIO_HPD6_Y_MASK as u32,
            _ => 0,
        }
    }

    /// Linux `dcn302_hwseq.c` `dcn302_hubp_pg_control`/`dcn302_dpp_pg_control`:
    /// power on HUBP0 (DOMAIN0) and DPP0 (DOMAIN1).
    fn power_on_frontend(&self, regs: &mut Regs) -> Result<()> {
        regs.write_dcn(
            dcn::mmDC_IP_REQUEST_CNTL,
            dcn::mmDC_IP_REQUEST_CNTL_BASE_IDX as usize,
            set_field(
                0,
                dcn::DC_IP_REQUEST_CNTL__IP_REQUEST_EN__SHIFT,
                dcn::DC_IP_REQUEST_CNTL__IP_REQUEST_EN_MASK,
                1,
            ),
        )?;
        for (cfg, status, power, power_mask, fsm, fsm_mask) in [
            (
                dcn::mmDOMAIN0_PG_CONFIG,
                dcn::mmDOMAIN0_PG_STATUS,
                dcn::DOMAIN0_PG_CONFIG__DOMAIN0_POWER_GATE__SHIFT,
                dcn::DOMAIN0_PG_CONFIG__DOMAIN0_POWER_GATE_MASK,
                dcn::DOMAIN0_PG_STATUS__DOMAIN0_PGFSM_PWR_STATUS__SHIFT,
                dcn::DOMAIN0_PG_STATUS__DOMAIN0_PGFSM_PWR_STATUS_MASK,
            ),
            (
                dcn::mmDOMAIN1_PG_CONFIG,
                dcn::mmDOMAIN1_PG_STATUS,
                dcn::DOMAIN1_PG_CONFIG__DOMAIN1_POWER_GATE__SHIFT,
                dcn::DOMAIN1_PG_CONFIG__DOMAIN1_POWER_GATE_MASK,
                dcn::DOMAIN1_PG_STATUS__DOMAIN1_PGFSM_PWR_STATUS__SHIFT,
                dcn::DOMAIN1_PG_STATUS__DOMAIN1_PGFSM_PWR_STATUS_MASK,
            ),
        ] {
            Self::rmw(
                regs,
                cfg,
                dcn::mmDOMAIN0_PG_CONFIG_BASE_IDX as usize,
                power,
                power_mask,
                0,
            )?;
            // Wait for power-on (status 0) with a bounded poll.
            let mut powered = false;
            for _ in 0..1000 {
                let value = regs.read_dcn(status, dcn::mmDOMAIN0_PG_STATUS_BASE_IDX as usize)?;
                if get_field(value, fsm, fsm_mask) == 0 {
                    powered = true;
                    break;
                }
                na_std::time::delay(core::time::Duration::from_micros(10));
            }
            if !powered {
                regs.write_dcn(
                    dcn::mmDC_IP_REQUEST_CNTL,
                    dcn::mmDC_IP_REQUEST_CNTL_BASE_IDX as usize,
                    0,
                )?;
                dev_info!(
                    "astra: DCN frontend power timeout: cfg {:#x}, status {:#x}",
                    cfg,
                    status,
                );
                return Err(Error::Io);
            }
        }
        regs.write_dcn(
            dcn::mmDC_IP_REQUEST_CNTL,
            dcn::mmDC_IP_REQUEST_CNTL_BASE_IDX as usize,
            0,
        )?;
        // DIO (DIG/AFMT) memory power: ungate everything.
        regs.write_dcn(
            dcn::mmDIO_MEM_PWR_CTRL,
            dcn::mmDIO_MEM_PWR_CTRL_BASE_IDX as usize,
            0,
        )?;
        Ok(())
    }

    /// Linux `optc1_program_timing` (1080p60).
    fn program_timing(&self, regs: &mut Regs) -> Result<()> {
        let t = self.timing;
        let otg = dcn::mmOTG0_OTG_H_TOTAL_BASE_IDX as usize;

        regs.write_dcn(dcn::mmOTG0_OTG_H_TOTAL, otg, (t.h_total - 1) as u32)?;
        regs.write_dcn(
            dcn::mmOTG0_OTG_H_SYNC_A,
            otg,
            set_field(
                0,
                dcn::OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_END__SHIFT,
                dcn::OTG0_OTG_H_SYNC_A__OTG_H_SYNC_A_END_MASK,
                t.h_sync_width as u64,
            ),
        )?;
        let h_blank_start = t.h_total - t.h_front_porch;
        let h_blank_end = h_blank_start - t.h_addressable;
        regs.write_dcn(
            dcn::mmOTG0_OTG_H_BLANK_START_END,
            otg,
            set_field(
                set_field(
                    0,
                    dcn::OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START__SHIFT,
                    dcn::OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START_MASK,
                    h_blank_start as u64,
                ),
                dcn::OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT,
                dcn::OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END_MASK,
                h_blank_end as u64,
            ),
        )?;
        Self::rmw(
            regs,
            dcn::mmOTG0_OTG_H_SYNC_A_CNTL,
            otg,
            dcn::OTG0_OTG_H_SYNC_A_CNTL__OTG_H_SYNC_A_POL__SHIFT,
            dcn::OTG0_OTG_H_SYNC_A_CNTL__OTG_H_SYNC_A_POL_MASK,
            if t.h_positive { 0 } else { 1 },
        )?;

        regs.write_dcn(dcn::mmOTG0_OTG_V_TOTAL, otg, (t.v_total - 1) as u32)?;
        regs.write_dcn(
            dcn::mmOTG0_OTG_V_SYNC_A,
            otg,
            set_field(
                0,
                dcn::OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_END__SHIFT,
                dcn::OTG0_OTG_V_SYNC_A__OTG_V_SYNC_A_END_MASK,
                t.v_sync_width as u64,
            ),
        )?;
        let v_blank_start = t.v_total - t.v_front_porch;
        let v_blank_end = v_blank_start - t.v_addressable;
        regs.write_dcn(
            dcn::mmOTG0_OTG_V_BLANK_START_END,
            otg,
            set_field(
                set_field(
                    0,
                    dcn::OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT,
                    dcn::OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK,
                    v_blank_start as u64,
                ),
                dcn::OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT,
                dcn::OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK,
                v_blank_end as u64,
            ),
        )?;
        Self::rmw(
            regs,
            dcn::mmOTG0_OTG_V_SYNC_A_CNTL,
            otg,
            dcn::OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL__SHIFT,
            dcn::OTG0_OTG_V_SYNC_A_CNTL__OTG_V_SYNC_A_POL_MASK,
            if t.v_positive { 0 } else { 1 },
        )?;
        Ok(())
    }

    /// Linux `dce112_program_pix_clk` through the DCN302 DMUB command table.
    fn program_pixel_clock(&mut self, regs: &mut Regs) -> Result<()> {
        let path = self.path.ok_or(Error::NoDevice)?;
        let mut params = [0u8; 16];
        params[..4].copy_from_slice(&(self.timing.pix_clk_khz * 10).to_le_bytes());
        params[4] = 20 + path.phy_id; // ATOM_COMBOPHY_PLL0 + transmitter
        params[5] = path.encoder_obj_id;
        params[6] = 3; // ATOM_ENCODER_MODE_HDMI
        params[7] = 0x02; // PIXEL_CLOCK_V7_MISC_PROG_PHYPLL
        params[8] = 0; // ATOM_CRTC1 / OTG0
        params[9] = 0; // 8 bpc
        self.dmub
            .execute_vbios(regs, DMUB_VBIOS_SET_PIXEL_CLOCK, &params)?;
        dev_info!(
            "astra: DCN pixel clock {} kHz programmed via DMCUB on PHY{}",
            self.timing.pix_clk_khz,
            path.phy_id,
        );
        Ok(())
    }

    /// Linux `optc1_enable_optc_clock(true)`, which precedes pixel-clock
    /// programming in `dcn20_enable_stream_timing`.
    fn enable_optc_clock(&self, regs: &mut Regs) -> Result<()> {
        let optc = dcn::mmODM0_OPTC_INPUT_CLOCK_CONTROL_BASE_IDX as usize;
        let mut value = regs.read_dcn(dcn::mmODM0_OPTC_INPUT_CLOCK_CONTROL, optc)?;
        value = set_field(
            value,
            dcn::ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN__SHIFT,
            dcn::ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_EN_MASK,
            1,
        );
        value = set_field(
            value,
            dcn::ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS__SHIFT,
            dcn::ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_GATE_DIS_MASK,
            1,
        );
        regs.write_dcn(dcn::mmODM0_OPTC_INPUT_CLOCK_CONTROL, optc, value)?;
        let mut on = false;
        for _ in 0..1000 {
            value = regs.read_dcn(dcn::mmODM0_OPTC_INPUT_CLOCK_CONTROL, optc)?;
            if get_field(
                value,
                dcn::ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON__SHIFT,
                dcn::ODM0_OPTC_INPUT_CLOCK_CONTROL__OPTC_INPUT_CLK_ON_MASK,
            ) == 1
            {
                on = true;
                break;
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        if !on {
            dev_info!(
                "astra: OPTC input clock failed to turn on ({:#010x})",
                value
            );
            return Err(Error::Io);
        }

        let otg = dcn::mmOTG0_OTG_CLOCK_CONTROL_BASE_IDX as usize;
        value = regs.read_dcn(dcn::mmOTG0_OTG_CLOCK_CONTROL, otg)?;
        value = set_field(
            value,
            dcn::OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN__SHIFT,
            dcn::OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_EN_MASK,
            1,
        );
        value = set_field(
            value,
            dcn::OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS__SHIFT,
            dcn::OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_GATE_DIS_MASK,
            1,
        );
        regs.write_dcn(dcn::mmOTG0_OTG_CLOCK_CONTROL, otg, value)?;
        on = false;
        for _ in 0..1000 {
            value = regs.read_dcn(dcn::mmOTG0_OTG_CLOCK_CONTROL, otg)?;
            if get_field(
                value,
                dcn::OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON__SHIFT,
                dcn::OTG0_OTG_CLOCK_CONTROL__OTG_CLOCK_ON_MASK,
            ) == 1
            {
                on = true;
                break;
            }
            na_std::time::delay(core::time::Duration::from_micros(1));
        }
        if !on {
            dev_info!("astra: OTG clock failed to turn on ({:#010x})", value);
            return Err(Error::Io);
        }
        Ok(())
    }

    /// Linux `dcn20_enable_stream_timing` clock + `optc1_enable_crtc`.
    fn enable_optc(&self, regs: &mut Regs) -> Result<()> {
        let otg = dcn::mmOTG0_OTG_CONTROL_BASE_IDX as usize;
        // OTG master enable; VTG runs with the programmed timing.
        Self::rmw(
            regs,
            dcn::mmOTG0_OTG_CONTROL,
            otg,
            dcn::OTG0_OTG_CONTROL__OTG_MASTER_EN__SHIFT,
            dcn::OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK,
            1,
        )?;
        Ok(())
    }

    /// Linux `hubp3_program_surface_config` + `min_set_viewport` +
    /// `dcn20_update_plane_addr` + `hubp2_set_blank_regs(false)`.
    fn program_hubp(&self, regs: &mut Regs, fb_start: u64) -> Result<()> {
        let base = dcn::mmHUBP0_DCSURF_ADDR_CONFIG_BASE_IDX as usize;
        let t = self.timing;
        let fb = self.fb.as_ref().ok_or(Error::NoDevice)?;

        // hubp3_init(): enable the DCN21-133 ECO before any surface setup.
        // Linux requires this for consistent DPTE/meta row start on flips.
        regs.write_dcn(
            dcn::mmHUBP0_HUBPREQ_DEBUG,
            dcn::mmHUBP0_HUBPREQ_DEBUG_BASE_IDX as usize,
            1 << 26,
        )?;
        Self::program_primary_requestor(regs, t.h_addressable as u32, t.h_addressable as u32, 0)?;

        // Linear tiling: SW_MODE=0, single pipe.
        regs.write_dcn(
            dcn::mmHUBP0_DCSURF_ADDR_CONFIG,
            base,
            set_field(
                set_field(
                    0,
                    dcn::HUBP0_DCSURF_ADDR_CONFIG__NUM_PIPES__SHIFT,
                    dcn::HUBP0_DCSURF_ADDR_CONFIG__NUM_PIPES_MASK,
                    0,
                ),
                dcn::HUBP0_DCSURF_ADDR_CONFIG__PIPE_INTERLEAVE__SHIFT,
                dcn::HUBP0_DCSURF_ADDR_CONFIG__PIPE_INTERLEAVE_MASK,
                0,
            ),
        )?;
        regs.write_dcn(dcn::mmHUBP0_DCSURF_TILING_CONFIG, base, 0)?;

        // Surface pitch (in pixels) - 1.
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH,
            dcn::mmHUBPREQ0_DCSURF_SURFACE_PITCH_BASE_IDX as usize,
            set_field(
                0,
                dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH__SHIFT,
                dcn::HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK,
                (t.h_addressable - 1) as u64,
            ),
        )?;

        // Viewport = full surface at 0,0 in every viewport bank.
        Self::program_primary_viewports(regs, t.h_addressable as u32, t.v_addressable as u32)?;

        // Pixel format ARGB8888 = 8 (hubp2_program_pixel_format).
        Self::rmw(
            regs,
            dcn::mmHUBP0_DCSURF_SURFACE_CONFIG,
            base,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT__SHIFT,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK,
            8,
        )?;
        // No rotation / mirror.
        Self::rmw(
            regs,
            dcn::mmHUBP0_DCSURF_SURFACE_CONFIG,
            base,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE__SHIFT,
            dcn::HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE_MASK,
            0,
        )?;

        // Surface address (GPU address of the framebuffer BO).
        let gpu_addr = fb_start
            .checked_add(fb.gpu_addr)
            .ok_or(Error::InvalidArgument)?;
        // DCN latches the complete address when the low register is written,
        // so Linux programs HIGH first and LOW last.
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX as usize,
            (gpu_addr >> 32) as u32,
        )?;
        regs.write_dcn(
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,
            dcn::mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX as usize,
            (gpu_addr & 0xffff_ffff) as u32,
        )?;
        // Unblank the HUBP (HUBP_BLANK_EN=0) + enable TTU.
        Self::rmw(
            regs,
            dcn::mmHUBP0_DCHUBP_CNTL,
            dcn::mmHUBP0_DCHUBP_CNTL_BASE_IDX as usize,
            dcn::HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN__SHIFT,
            dcn::HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK,
            0,
        )?;
        Self::rmw(
            regs,
            dcn::mmHUBP0_DCHUBP_CNTL,
            dcn::mmHUBP0_DCHUBP_CNTL_BASE_IDX as usize,
            dcn::HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE__SHIFT,
            dcn::HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK,
            0,
        )?;
        Ok(())
    }

    /// Linux `mpc1_insert_plane` (single top layer),
    /// `mpc2_update_blending`, and `mpc1_set_out_mux`.
    fn program_mpc(&self, regs: &mut Regs) -> Result<()> {
        let mpcc = dcn::mmMPCC0_MPCC_TOP_SEL_BASE_IDX as usize;
        // Top layer only: BOT_SEL=0xf, MODE=TOP_LAYER_ONLY(2), TOP=dpp0.
        regs.write_dcn(
            dcn::mmMPCC0_MPCC_TOP_SEL,
            mpcc,
            set_field(
                0,
                dcn::MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL__SHIFT,
                dcn::MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL_MASK,
                0,
            ),
        )?;
        regs.write_dcn(
            dcn::mmMPCC0_MPCC_BOT_SEL,
            mpcc,
            set_field(
                0,
                dcn::MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL__SHIFT,
                dcn::MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL_MASK,
                0xf,
            ),
        )?;
        regs.write_dcn(
            dcn::mmMPCC0_MPCC_OPP_ID,
            mpcc,
            set_field(
                0,
                dcn::MPCC0_MPCC_OPP_ID__MPCC_OPP_ID__SHIFT,
                dcn::MPCC0_MPCC_OPP_ID__MPCC_OPP_ID_MASK,
                0,
            ),
        )?;
        let mut control = regs.read_dcn(dcn::mmMPCC0_MPCC_CONTROL, mpcc)?;
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_MODE__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_MODE_MASK,
            2,
        );
        // XRGB8888 bottom/only plane: no per-pixel alpha, so Linux selects
        // GLOBAL_ALPHA with fully opaque alpha/gain and non-premultiplied
        // input.  Program the whole mpc2_update_blending() state rather than
        // inheriting firmware values.
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE_MASK,
            2,
        );
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_ALPHA_MULTIPLIED_MODE__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_ALPHA_MULTIPLIED_MODE_MASK,
            0,
        );
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_BLND_ACTIVE_OVERLAP_ONLY__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_BLND_ACTIVE_OVERLAP_ONLY_MASK,
            0,
        );
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_GLOBAL_ALPHA__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_GLOBAL_ALPHA_MASK,
            0xff,
        );
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_GLOBAL_GAIN__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_GLOBAL_GAIN_MASK,
            0xff,
        );
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_BG_BPC__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_BG_BPC_MASK,
            4,
        );
        control = set_field(
            control,
            dcn::MPCC0_MPCC_CONTROL__MPCC_BOT_GAIN_MODE__SHIFT,
            dcn::MPCC0_MPCC_CONTROL__MPCC_BOT_GAIN_MODE_MASK,
            0,
        );
        regs.write_dcn(dcn::mmMPCC0_MPCC_CONTROL, mpcc, control)?;
        regs.write_dcn(
            dcn::mmMPCC0_MPCC_TOP_GAIN,
            dcn::mmMPCC0_MPCC_TOP_GAIN_BASE_IDX as usize,
            set_field(
                0,
                dcn::MPCC0_MPCC_TOP_GAIN__MPCC_TOP_GAIN__SHIFT,
                dcn::MPCC0_MPCC_TOP_GAIN__MPCC_TOP_GAIN_MASK,
                0x1f000,
            ),
        )?;
        regs.write_dcn(
            dcn::mmMPCC0_MPCC_BOT_GAIN_INSIDE,
            dcn::mmMPCC0_MPCC_BOT_GAIN_INSIDE_BASE_IDX as usize,
            set_field(
                0,
                dcn::MPCC0_MPCC_BOT_GAIN_INSIDE__MPCC_BOT_GAIN_INSIDE__SHIFT,
                dcn::MPCC0_MPCC_BOT_GAIN_INSIDE__MPCC_BOT_GAIN_INSIDE_MASK,
                0x1f000,
            ),
        )?;
        regs.write_dcn(
            dcn::mmMPCC0_MPCC_BOT_GAIN_OUTSIDE,
            dcn::mmMPCC0_MPCC_BOT_GAIN_OUTSIDE_BASE_IDX as usize,
            set_field(
                0,
                dcn::MPCC0_MPCC_BOT_GAIN_OUTSIDE__MPCC_BOT_GAIN_OUTSIDE__SHIFT,
                dcn::MPCC0_MPCC_BOT_GAIN_OUTSIDE__MPCC_BOT_GAIN_OUTSIDE_MASK,
                0x1f000,
            ),
        )?;
        Self::rmw(
            regs,
            dcn::mmMPCC0_MPCC_UPDATE_LOCK_SEL,
            mpcc,
            dcn::MPCC0_MPCC_UPDATE_LOCK_SEL__MPCC_UPDATE_LOCK_SEL__SHIFT,
            dcn::MPCC0_MPCC_UPDATE_LOCK_SEL__MPCC_UPDATE_LOCK_SEL_MASK,
            0,
        )?;
        // Route OPP0 → MPCC0.
        regs.write_dcn(
            dcn::mmMPC_OUT0_MUX,
            dcn::mmMPC_OUT0_MUX_BASE_IDX as usize,
            set_field(
                0,
                dcn::MPC_OUT0_MUX__MPC_OUT_MUX__SHIFT,
                dcn::MPC_OUT0_MUX__MPC_OUT_MUX_MASK,
                0,
            ),
        )?;
        Ok(())
    }

    /// Linux `dcn10_link_encoder_hw_init`: initialize the selected PHY via
    /// the VBIOS transmitter command before the first mode set.
    fn init_link_encoder(&mut self, regs: &mut Regs) -> Result<()> {
        let path = self.path.ok_or(Error::NoDevice)?;
        let mut tx = [0u8; 60];
        tx[0] = path.phy_id;
        tx[1] = 7; // ATOM_TRANSMITTER_ACTION_INIT
        tx[2] = 2; // SIGNAL_TYPE_NONE maps to ATOM transmitter DVI mode
        tx[3] = 4;
        tx[8] = path.hpd_sel;
        tx[10] = path.connector_obj_id;
        self.dmub
            .execute_vbios(regs, DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL, &tx)?;
        dev_info!(
            "astra: DCN link encoder PHY{} initialized (connector {} enum {}, HPD{})",
            path.phy_id,
            path.connector_obj_id,
            path.connector_enum_id,
            path.hpd_sel,
        );
        Ok(())
    }

    /// Linux `dcn10_link_encoder_setup` + stream encoder setup +
    /// `dcn10_link_encoder_enable_tmds_output`, using DMUB VBIOS commands.
    fn program_dig(&mut self, regs: &mut Regs) -> Result<()> {
        let path = self.path.ok_or(Error::NoDevice)?;
        let dig = dcn::mmDIG0_DIG_BE_CNTL_BASE_IDX as usize;
        let be = Self::link_be_reg(path.phy_id)?;
        let pattern = Self::link_pattern_reg(path.phy_id)?;
        // TMDS-HDMI mode.
        Self::rmw(
            regs,
            be,
            dig,
            dcn::DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT,
            dcn::DIG0_DIG_BE_CNTL__DIG_MODE_MASK,
            3,
        )?;
        // DCN10+ programs DIGA FE routing in DIG_BE_CNTL; the VBIOS
        // transmitter parameter therefore keeps digfe_sel at zero.
        Self::rmw(
            regs,
            be,
            dig,
            dcn::DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT,
            dcn::DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK,
            1,
        )?;
        // Clock pattern (default 0x63 does not work).
        Self::rmw(
            regs,
            pattern,
            dig,
            dcn::DIG0_DIG_CLOCK_PATTERN__DIG_CLOCK_PATTERN__SHIFT,
            dcn::DIG0_DIG_CLOCK_PATTERN__DIG_CLOCK_PATTERN_MASK,
            0x1f,
        )?;
        // Reset the FIFO then take it out of reset.
        Self::rmw(
            regs,
            dcn::mmDIG0_DIG_FE_CNTL,
            dig,
            dcn::DIG0_DIG_FE_CNTL__DIG_START__SHIFT,
            dcn::DIG0_DIG_FE_CNTL__DIG_START_MASK,
            1,
        )?;
        na_std::time::delay(core::time::Duration::from_micros(1));
        Self::rmw(
            regs,
            dcn::mmDIG0_DIG_FE_CNTL,
            dig,
            dcn::DIG0_DIG_FE_CNTL__DIG_START__SHIFT,
            dcn::DIG0_DIG_FE_CNTL__DIG_START_MASK,
            0,
        )?;

        let mut enc = [0u8; 12];
        enc[0] = 0; // DIGA stream encoder
        enc[1] = 0x0f; // ATOM_ENCODER_CMD_STREAM_SETUP
        enc[2] = 3; // HDMI
        enc[3] = 4;
        enc[4..8].copy_from_slice(&(self.timing.pix_clk_khz / 10).to_le_bytes());
        enc[8] = 2; // PANEL_8BIT_PER_COLOR
        self.dmub
            .execute_vbios(regs, DMUB_VBIOS_DIGX_ENCODER_CONTROL, &enc)?;

        let mut tx = [0u8; 60];
        tx[0] = path.phy_id;
        tx[1] = 1; // ATOM_TRANSMITTER_ACTION_ENABLE
        tx[2] = 3; // HDMI
        tx[3] = 4;
        tx[4..8].copy_from_slice(&(self.timing.pix_clk_khz / 10).to_le_bytes());
        tx[8] = path.hpd_sel;
        tx[9] = 0; // FE routing was programmed directly above
        tx[10] = path.connector_obj_id;
        self.dmub
            .execute_vbios(regs, DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL, &tx)?;
        dev_info!(
            "astra: DCN DIGA -> PHY{} HDMI link enabled via DMCUB",
            path.phy_id,
        );
        Ok(())
    }

    fn link_be_reg(phy: u8) -> Result<u32> {
        [
            dcn::mmDIG0_DIG_BE_CNTL,
            dcn::mmDIG1_DIG_BE_CNTL,
            dcn::mmDIG2_DIG_BE_CNTL,
            dcn::mmDIG3_DIG_BE_CNTL,
            dcn::mmDIG4_DIG_BE_CNTL,
            dcn::mmDIG5_DIG_BE_CNTL,
        ]
        .get(phy as usize)
        .copied()
        .ok_or(Error::Range)
    }

    fn link_pattern_reg(phy: u8) -> Result<u32> {
        [
            dcn::mmDIG0_DIG_CLOCK_PATTERN,
            dcn::mmDIG1_DIG_CLOCK_PATTERN,
            dcn::mmDIG2_DIG_CLOCK_PATTERN,
            dcn::mmDIG3_DIG_CLOCK_PATTERN,
            dcn::mmDIG4_DIG_CLOCK_PATTERN,
            dcn::mmDIG5_DIG_CLOCK_PATTERN,
        ]
        .get(phy as usize)
        .copied()
        .ok_or(Error::Range)
    }
}

impl IpBlock for DcnBlock {
    fn hw_ip(&self) -> HwIp {
        HwIp::Dm
    }

    fn name(&self) -> &'static str {
        "DCN 3.0.2"
    }

    /// Linux `dm_sw_init`: DMUB regions first, then the driver scanout BO.
    fn sw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        self.dmub.sw_init(dev)?;
        let paths = dev.atom.as_ref().ok_or(Error::NoDevice)?.display_paths();
        for path in &paths {
            dev_info!(
                "astra: display path connector {} enum {}, encoder {} enum {}, PHY{}, HPD pin {} -> HPD{}",
                path.connector_obj_id,
                path.connector_enum_id,
                path.encoder_obj_id,
                path.encoder_enum_id,
                path.phy_id,
                path.hpd_pin_id,
                path.hpd_sel,
            );
        }
        // Linux's hardware-mode HPD path reads the GPIO Y register. Prefer
        // the physically connected HDMI connector; this board exposes two
        // HDMI object paths with different PHY/HPD assignments.
        let hpd_y = dev
            .regs
            .read_dcn(dcn::mmDC_GPIO_HPD_Y, dcn::mmDC_GPIO_HPD_Y_BASE_IDX as usize)?;
        dev_info!("astra: DCN HPD GPIO Y={:#010x}", hpd_y);
        self.path = paths
            .iter()
            .copied()
            .find(|path| {
                path.connector_obj_id == CONNECTOR_OBJECT_ID_HDMI_TYPE_A
                    && Self::hpd_y_mask(*path) != 0
                    && hpd_y & Self::hpd_y_mask(*path) != 0
            })
            .or_else(|| {
                paths
                    .iter()
                    .copied()
                    .find(|path| path.connector_obj_id == CONNECTOR_OBJECT_ID_HDMI_TYPE_A)
            });
        if self.path.is_none() {
            dev_info!("astra: no HDMI Type-A path found in displayobjectinfo");
            return Err(Error::NoDevice);
        }
        let size = (self.width as usize) * (self.height as usize) * 4;
        self.fb = Some(dev.mem.alloc_vram(&mut dev.regs, size)?);
        dev_info!(
            "astra: DCN 3.0.2 framebuffer {}x{} at VRAM offset {:#x}",
            self.width,
            self.height,
            self.fb.as_ref().map(|b| b.gpu_addr).unwrap_or(0),
        );
        Ok(())
    }

    /// Linux `dcn30_init_hw` + `dce110_apply_single_controller_ctx_to_hw`.
    fn hw_init(&mut self, dev: &mut Adapter) -> Result<()> {
        dev_info!("astra: DCN stage: DMUB hardware init");
        self.dmub.hw_init(dev)?;
        let regs = &mut dev.regs;
        dev_info!("astra: DCN stage: link encoder init");
        self.init_link_encoder(regs)?;
        dev_info!("astra: DCN stage: frontend power");
        self.power_on_frontend(regs)?;
        dev_info!("astra: DCN stage: OPTC clock");
        self.enable_optc_clock(regs)?;
        dev_info!("astra: DCN stage: pixel clock");
        self.program_pixel_clock(regs)?;
        dev_info!("astra: DCN stage: timing");
        self.program_timing(regs)?;
        dev_info!("astra: DCN stage: HUBP");
        self.program_hubp(regs, dev.gmc.fb_start)?;
        dev_info!("astra: DCN stage: DPP scaler");
        DcnDisplayPipe::new(regs).set_primary_geometry(
            self.timing.h_addressable as u32,
            self.timing.v_addressable as u32,
        )?;
        dev_info!("astra: DCN stage: MPC");
        self.program_mpc(regs)?;
        dev_info!(
            "astra: DCN pipe geometry: RECOUT_START={:#010x} RECOUT_SIZE={:#010x} MPC_SIZE={:#010x} SCL_MODE={:#010x} LB_MEMORY_CTRL={:#010x} MPCC_CONTROL={:#010x}",
            regs.read_dcn(
                dcn::mmDSCL0_RECOUT_START,
                dcn::mmDSCL0_RECOUT_START_BASE_IDX as usize,
            )?,
            regs.read_dcn(
                dcn::mmDSCL0_RECOUT_SIZE,
                dcn::mmDSCL0_RECOUT_SIZE_BASE_IDX as usize,
            )?,
            regs.read_dcn(
                dcn::mmDSCL0_MPC_SIZE,
                dcn::mmDSCL0_MPC_SIZE_BASE_IDX as usize,
            )?,
            regs.read_dcn(
                dcn::mmDSCL0_SCL_MODE,
                dcn::mmDSCL0_SCL_MODE_BASE_IDX as usize,
            )?,
            regs.read_dcn(
                dcn::mmDSCL0_LB_MEMORY_CTRL,
                dcn::mmDSCL0_LB_MEMORY_CTRL_BASE_IDX as usize,
            )?,
            regs.read_dcn(
                dcn::mmMPCC0_MPCC_CONTROL,
                dcn::mmMPCC0_MPCC_CONTROL_BASE_IDX as usize,
            )?,
        );
        dev_info!("astra: DCN stage: DIG/HDMI");
        self.program_dig(regs)?;
        dev_info!("astra: DCN stage: OPTC enable");
        self.enable_optc(regs)?;

        let fb = self.fb.as_ref().ok_or(Error::NoDevice)?;
        dev.scanout = Some(ScanoutInfo {
            vram_offset: fb.gpu_addr,
            width: self.width as u32,
            height: self.height as u32,
            pitch: self.width as u32 * 4,
        });

        dev_info!(
            "astra: DCN 3.0.2 HDMI {}x{}@60 output enabled (fb gpu {:#x}, offset {:#x})",
            self.width,
            self.height,
            dev.gmc.fb_start + self.fb.as_ref().map(|b| b.gpu_addr).unwrap_or(0),
            self.fb.as_ref().map(|b| b.gpu_addr).unwrap_or(0),
        );
        Ok(())
    }
}
