# na-kernel

`na-kernel` contains only the freestanding kernel and its loadable kernel
modules.

## Targets

- `make prepare` downloads kernel-only build dependencies.
- `make kernel` builds the kernel.
- `make modules` builds loadable kernel modules.
- `make` builds both kernel and modules.

The development flake contains only compiler, linker, dependency-fetching and
module-signing tools; disk, rootfs, initramfs and VM tooling belong to the
parent repository.

Safe Rust kernel modules use the `modules/na-std` crate and the shared
`modules/build/rust-module.mk` build rules. The development shell supplies a
nightly Rust toolchain with `rust-src` so all supported freestanding targets can
build `core` without architecture-specific code leaking into common modules.

For a complete system build, run `make` from the parent directory. The parent
passes its generated initramfs to SBI/LA boot builds through `INITRAMFS_IMAGE`.

## Module dependencies

Every loadable module gets its identity from `KM_NAME`; `modules/build/module.mk`
embeds that name in the resulting ELF file. A module declares each direct
module dependency in C:

```c
#include <mod/module.h>

MODULE_DEPENDS("sound");
```

The loader builds the dependency graph from these metadata records before it
maps any module. Undefined symbols are resolved against the kernel and the
module's declared direct dependencies only. Dependencies are not inherited, so
a module must also declare a transitive dependency when it imports that
dependency's symbols itself. Different dependency trees may export the same
symbol name without sharing a global module namespace.
