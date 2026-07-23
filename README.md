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

For a complete system build, run `make` from the parent directory. The parent
passes its generated initramfs to SBI/LA boot builds through `INITRAMFS_IMAGE`.
