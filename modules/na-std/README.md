# na-std

`na-std` is the `no_std` Rust interface for NAOS kernel modules. It has two
strict layers:

- `bindings` is generated at build time from
  `kernel/src/mod/rust/api.h`; it contains the raw C ABI and is the only layer
  that calls `extern` functions directly.
- The remaining modules expose ownership-checked Rust objects and traits. A
  driver using these modules does not need to dereference raw pointers or write
  an `unsafe` block.

The public interfaces include:

- `memory`: owned kernel objects, physically contiguous DMA buffers, physical
  resource ranges and explicit DMA cache synchronization.
- `io`: bounds- and alignment-checked volatile MMIO registers.
- `arch`: architecture-specific interfaces. At present x86_64 provides safe
  bounded PIO regions; no PIO implementation leaks into common code.
- `pci`: device snapshots, BAR resources, typed configuration-space access and
  trait-based driver registration.
- `usb`: trait-based function-driver binding, typed interface discovery,
  control requests and owned endpoint pipes.
- `net`: trait-based Ethernet/Wi-Fi device registration with RAII teardown.
- `fdt`: architecture-neutral FDT driver registration, validated properties
  and `reg` resources.
- `acpi`: reference-counted ACPI table mappings released through RAII.
- `drm`: trait-based DRM registration, typed connector/CRTC/encoder/plane
  resources, framebuffer and dumb-buffer lifecycles, dirty updates, legacy
  modesets, page flips and normalized atomic properties.
- `sync`: an interrupt-aware guard over the kernel spinlock implementation.

## Safe driver shape

```rust
#![no_std]
#![forbid(unsafe_code)]

use na_std::{Result, module_entry};
use na_std::pci::{Device, DeviceInfo, Driver, DriverBuilder, Probe};

struct Example;

impl Driver for Example {
    fn matches(&self, info: &DeviceInfo) -> bool {
        info.vendor_id == 0x1234
    }

    fn probe(&self, mut device: Device<'_>) -> Result<Probe> {
        let _command: u16 = device.read_config(0x04)?;
        device.write_config(0x04, 0x0006u16)?;
        Ok(Probe::Claimed)
    }
}

static DRIVER: Example = Example;

fn init() -> Result<()> {
    DriverBuilder::new(&DRIVER, c"example").register()
}

module_entry!(init);
```

Rust module makefiles include `modules/build/rust-module.mk`. `KM_NAME` becomes
the module identity metadata and optional `MODULE_DEPS` entries become explicit
loader dependencies.

`build.rs` invokes `bindgen` and writes the generated bindings to Cargo's
`OUT_DIR`. The generated file is intentionally not tracked; `rust-bindgen` is
provided by the development shell. Update `api.h` when changing this ABI, then
let the next Cargo build regenerate the Rust declarations.

The kernel ABI bridge is kept under `kernel/src/mod/rust/`: core, PCI and FDT
live in separate translation units; memory, synchronization and ACPI have
their own bridge files; and DRM is split into device, resource-management and
modeset paths. The bridge exposes only narrow opaque handles and value
snapshots; private kernel structures are not mirrored in the safe crate.
