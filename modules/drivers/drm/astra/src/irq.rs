//! MSI interrupt handler for the IH rings.
//!
//! The handler must not touch the shared init-time `Regs` state, so it
//! owns dedicated BAR5/BAR2 mappings and its own GART-mapped writeback
//! buffer; all mutable state lives behind an IRQ-safe spinlock. Phase 1
//! consumes interrupt vectors (advancing the rptr) without dispatching.

use na_std::io::MmioRegion;
use na_std::pci::IrqCallback;
use na_std::sync::SpinLock;

use crate::dev_info;
use crate::mem::Bo;

/// Writeback slot offset (32 bytes per slot); slot 0 receives the IH
/// ring 0 wptr shadow.
const WB_SLOT_IH_WPTR: usize = 0;

pub struct IhHandler {
    state: SpinLock<IhHandlerState>,
}

struct IhHandlerState {
    /// Dedicated BAR5/BAR2 mappings for interrupt-context register
    /// access (separate from the init-time mappings).
    bar5: MmioRegion,
    bar2: MmioRegion,
    /// Dedicated GART-mapped writeback buffer.
    wb: Bo,
    /// Full dword registers used without the shared `Regs` accessor.
    ih_rb_rptr: u32,
    ih_rb_wptr1: u32,
    ih_rb_rptr1: u32,
    /// Doorbell dword indexes for the rptr updates.
    doorbell: u32,
    doorbell1: u32,
    ptr_mask: u32,
    rptr: u32,
    rptr1: u32,
    irq_count: u32,
}

pub struct IhConfig {
    pub bar5: MmioRegion,
    pub bar2: MmioRegion,
    pub wb: Bo,
    pub ih_rb_rptr: u32,
    pub ih_rb_wptr1: u32,
    pub ih_rb_rptr1: u32,
    pub doorbell: u32,
    pub doorbell1: u32,
    pub ring_size_dw: u32,
}

impl IhHandler {
    pub fn new(config: IhConfig) -> Self {
        Self {
            state: SpinLock::new(IhHandlerState {
                bar5: config.bar5,
                bar2: config.bar2,
                wb: config.wb,
                ih_rb_rptr: config.ih_rb_rptr,
                ih_rb_wptr1: config.ih_rb_wptr1,
                ih_rb_rptr1: config.ih_rb_rptr1,
                doorbell: config.doorbell,
                doorbell1: config.doorbell1,
                ptr_mask: config.ring_size_dw - 1,
                rptr: 0,
                rptr1: 0,
                irq_count: 0,
            }),
        }
    }

    /// GPU address of the ring-0 wptr writeback slot.
    pub fn wb_gpu_addr(&self) -> u64 {
        self.state.lock().wb.gpu_addr
    }
}

impl IrqCallback for IhHandler {
    fn irq(&self, _irq_num: u64) {
        let mut state = self.state.lock();
        // Rebind as a plain reference so field-disjoint borrows split.
        let s = &mut *state;
        let Some(cpu) = s.wb.cpu.as_mut() else {
            return;
        };
        cpu.sync_for_cpu();

        // Ring 0: wptr arrives through the writeback shadow.
        let wptr = cpu
            .as_slice()
            .get(WB_SLOT_IH_WPTR * 32..WB_SLOT_IH_WPTR * 32 + 4)
            .map(|bytes| u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) & s.ptr_mask)
            .unwrap_or(0);
        if wptr != s.rptr {
            let _ = s.bar5.write::<u32>(s.ih_rb_rptr as usize * 4, wptr);
            let _ = s
                .bar2
                .write::<u32>(s.doorbell as usize * core::mem::size_of::<u32>(), wptr);
            s.rptr = wptr;
            s.irq_count += 1;
        }

        // Ring 1 has no wptr writeback; read the register directly.
        let wptr1 = s
            .bar5
            .read::<u32>(s.ih_rb_wptr1 as usize * 4)
            .map(|w| w & s.ptr_mask)
            .unwrap_or(0);
        if wptr1 != s.rptr1 {
            let _ = s.bar5.write::<u32>(s.ih_rb_rptr1 as usize * 4, wptr1);
            let _ = s
                .bar2
                .write::<u32>(s.doorbell1 as usize * core::mem::size_of::<u32>(), wptr1);
            s.rptr1 = wptr1;
        }

        if s.irq_count == 1 {
            dev_info!("astra: first IH interrupt received (MSI working)");
        }
    }
}
