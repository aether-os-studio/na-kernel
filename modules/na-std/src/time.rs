use core::time::Duration;

use crate::bindings;

pub fn monotonic() -> Duration {
    Duration::from_nanos(unsafe { bindings::na_monotonic_time_ns() })
}

pub fn delay(duration: Duration) {
    let micros = duration.as_micros().min(u64::MAX as u128) as u64;
    if micros != 0 {
        unsafe { bindings::na_delay_us(micros) };
    }
}
