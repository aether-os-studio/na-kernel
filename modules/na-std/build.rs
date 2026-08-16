use std::{env, path::PathBuf, process::Command};

fn main() {
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let header = manifest.join("../../kernel/src/mod/rust/api.h");
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("bindings.rs");
    let bindgen = env::var_os("BINDGEN").unwrap_or_else(|| "bindgen".into());
    let target = env::var("TARGET").unwrap();
    let clang_target = match target.as_str() {
        "aarch64-unknown-none-softfloat" => "aarch64-unknown-none",
        "riscv64imac-unknown-none-elf" => "riscv64-unknown-none-elf",
        "loongarch64-unknown-none-softfloat" => "loongarch64-unknown-none",
        target => target,
    };
    let aliases = [
        "PciDevice = pci_device",
        "PciDeviceInfo = na_pci_device_info",
        "PciBarInfo = na_pci_bar_info",
        "PciDriverOps = na_pci_driver_ops",
        "MutexHandle = na_mutex",
        "VirtioDevice = na_virtio_device",
        "VirtioQueue = na_virtio_queue",
        "VirtioDriverOps = na_virtio_driver_ops",
        "FdtDevice = fdt_device",
        "FdtDriverOps = na_fdt_driver_ops",
        "DrmDevice = drm_device",
        "DrmConnector = drm_connector",
        "DrmCrtc = drm_crtc",
        "DrmEncoder = drm_encoder",
        "DrmPlane = drm_plane",
        "DrmDumbBuffer = na_drm_dumb_buffer",
        "DrmModeInfo = na_drm_mode_info",
        "DrmConnectorInfo = na_drm_connector_info",
        "DrmCrtcInfo = na_drm_crtc_info",
        "DrmEncoderInfo = na_drm_encoder_info",
        "DrmPlaneInfo = na_drm_plane_info",
        "DrmFramebufferRequest = na_drm_framebuffer_request",
        "DrmClip = na_drm_clip",
        "DrmPlaneUpdate = na_drm_plane_update",
        "DrmCrtcUpdate = na_drm_crtc_update",
        "DrmPageFlip = na_drm_page_flip",
        "DrmCursorUpdate = na_drm_cursor_update",
        "DrmAtomicProperty = na_drm_atomic_property",
        "DrmDriverInfo = na_drm_driver_info",
        "DrmDriverOps = na_drm_driver_ops",
    ];

    let mut command = Command::new(bindgen);
    command.args([
        header.to_str().unwrap(),
        "--use-core",
        "--ctypes-prefix",
        "core::ffi",
        "--no-layout-tests",
        "--no-doc-comments",
        "--disable-header-comment",
        "--with-derive-default",
        "--formatter",
        "none",
        "--allowlist-type",
        "^(RawSpinLock|UacpiTable|na_.*|pci_device|fdt_device|drm_.*)$",
        "--allowlist-function",
        "^na_.*$",
        "--allowlist-var",
        "^(E[A-Z0-9]+|NA_(VFS|DEVICE)_[A-Z0-9_]+)$",
        "--allowlist-type",
        "^vfs_.*$",
        "--allowlist-function",
        "^(vfs_register_filesystem|vfs_alloc_super|vfs_get_super|vfs_put_super|vfs_alloc_inode|vfs_igrab|vfs_iput|vfs_d_alloc|vfs_dget|vfs_dput|vfs_d_add|vfs_d_instantiate|vfs_d_lookup|vfs_qstr_make|vfs_qstr_dup|vfs_qstr_destroy|device_read|device_write)$",
        "--output",
        output.to_str().unwrap(),
    ]);
    for alias in aliases {
        command.args(["--raw-line", &format!("pub type {alias};")]);
    }
    command.arg("--");
    command.args(["-I../../kernel/src", "-I../../kernel/freestnd-c-hdrs"]);
    command.arg(format!("--target={clang_target}"));

    let status = command.status().expect("failed to run bindgen");
    assert!(status.success(), "bindgen failed");
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-env-changed=BINDGEN");
    println!("cargo:rerun-if-env-changed=TARGET");
}
