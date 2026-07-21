# Nuke built-in rules and variables.
MAKEFLAGS += -rR --no-print-directory
.SUFFIXES:

include build/common-env.mk

KVM ?= 0
HVF ?= 0
SMP ?= 4
MEM ?= 8G
SER ?= 1
MON ?= 0

# Default user QEMU flags. These are appended to the QEMU command calls.
QEMUFLAGS := -m $(MEM) -smp $(SMP)

export EXTRA ?= 

DEBUG ?= 0

ifeq ($(DEBUG), 1)
override QEMUFLAGS := $(QEMUFLAGS) -s -S
endif

ifeq ($(KVM), 1)
override QEMUFLAGS := $(QEMUFLAGS) -cpu host,migratable=off --enable-kvm
else
override QEMUFLAGS := $(QEMUFLAGS) -cpu max
endif

ifeq ($(SER), 1)
override QEMUFLAGS := $(QEMUFLAGS) -serial stdio
endif

ifeq ($(MON), 1)
override QEMUFLAGS := $(QEMUFLAGS) -monitor stdio
else
override QEMUFLAGS := $(QEMUFLAGS)
endif

ifeq ($(HVF), 1)
override QEMUFLAGS := $(QEMUFLAGS) --accel hvf
endif

override QEMUFLAGS := $(QEMUFLAGS) $(EXTRA)

override IMAGE_NAME := naos-$(ARCH)

ifeq ($(MODULE_VERIFY), 0)
MODULES_TARGET := modules
else
MODULES_TARGET := sign-modules
endif

# Toolchain for building the 'limine' executable for the host.
HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

LIBGCC_VERSION ?= 2025-12-08

.PHONY: all
all: $(IMAGE_NAME).img rootfs-$(ARCH).img

.PHONY: all
all-single: single-$(IMAGE_NAME).img

prepare: libgcc_$(ARCH).a
	$(call PRINT_STEP,PREPARE,kernel/get-deps)
	$(Q)./kernel/get-deps

SOFTFLOAT :=

ifeq ($(ARCH), riscv64)
override SOFTFLOAT := -softfloat
endif
ifeq ($(ARCH), loongarch64)
override SOFTFLOAT := -softfloat
endif

libgcc_$(ARCH).a:
	$(call PRINT_STEP,GET,libgcc_$(ARCH).a)
	$(Q)curl -Lo libgcc_$(ARCH).a https://github.com/osdev0/libgcc-binaries/releases/download/$(LIBGCC_VERSION)/libgcc-$(ARCH)$(SOFTFLOAT).a

.PHONY: kernel
kernel:
	$(call PRINT_STEP,MAKE,kernel)
	$(Q)$(MAKE) -C kernel

ifeq ($(BOOT_PROTOCOL),sbi)
kernel: initramfs-$(ARCH).img
endif

ifeq ($(BOOT_PROTOCOL),laboot)
kernel: initramfs-$(ARCH).img
endif

USER_BUILD_INPUTS := user/GNUmakefile user/build_initramfs.sh user/build_rootfs.sh $(shell find user/base -type f -o -type l 2>/dev/null)

user: user/.build-stamp-$(ARCH)
user/.build-stamp-$(ARCH): $(USER_BUILD_INPUTS)
	$(call PRINT_STEP,MAKE,user)
	$(Q)sudo rm -rf user/initramfs-$(ARCH) user/rootfs-$(ARCH)
	$(Q)$(MAKE) -C user
	$(call PRINT_STEP,TOUCH,$@)
	$(Q)touch $@

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	rm -rf obj-modules-$(ARCH) modules-$(ARCH)
	rm -rf $(IMAGE_NAME).img

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	$(MAKE) -C user distclean
	rm -rf obj-modules-$(ARCH) modules-$(ARCH)
	rm -rf *.img assets

clippy:
	$(MAKE) -C kernel clippy

ROOTFS_IMG_SIZE ?= 8192
ROOTFS_EXT_BLOCK_SIZE ?= 1024
ROOTFS_EXT_INODE_SIZE ?= 256
ROOTFS_EXT_FEATURES := extent,64bit,flex_bg,huge_file,dir_nlink,extra_isize,dir_index,metadata_csum,^has_journal,^quota,^metadata_csum_seed,^orphan_file,^project,^encrypt,^verity,^casefold,^inline_data,^ea_inode,^bigalloc,^mmp,^fast_commit,^sparse_super2

rootfs-$(ARCH).img: user/.build-stamp-$(ARCH)
	dd if=/dev/zero bs=1M count=0 seek=$(ROOTFS_IMG_SIZE) of=rootfs-$(ARCH).img
	sudo mkfs.ext4 -b $(ROOTFS_EXT_BLOCK_SIZE) -I $(ROOTFS_EXT_INODE_SIZE) -O $(ROOTFS_EXT_FEATURES) -E lazy_itable_init=0,lazy_journal_init=0 -F -q -d user/rootfs-$(ARCH) rootfs-$(ARCH).img

ifeq ($(ARCH),x86_64)
EFI_FILE_SINGLE = assets/limine/BOOTX64.EFI
else ifeq ($(ARCH),aarch64)
EFI_FILE_SINGLE = assets/limine/BOOTAA64.EFI
else ifeq ($(ARCH),riscv64)
EFI_FILE_SINGLE = assets/limine/BOOTRISCV64.EFI
else ifeq ($(ARCH),loongarch64)
EFI_FILE_SINGLE = assets/limine/BOOTLOONGARCH64.EFI
endif

$(IMAGE_NAME).img: assets/limine $(MODULES_TARGET) kernel initramfs-$(ARCH).img
	dd if=/dev/zero of=$(IMAGE_NAME).img bs=1M seek=0 count=256
	sgdisk --new=1:1M:255M $(IMAGE_NAME).img
	mkfs.vfat -F 32 --offset 2048 -S 512 $(IMAGE_NAME).img
	mcopy -i $(IMAGE_NAME).img@@1M kernel/bin-$(ARCH)/kernel ::/
	mcopy -i $(IMAGE_NAME).img@@1M initramfs-$(ARCH).img ::/initramfs.img
	mmd -i $(IMAGE_NAME).img@@1M ::/EFI ::/EFI/BOOT ::/limine
	mcopy -i $(IMAGE_NAME).img@@1M $(EFI_FILE_SINGLE) ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).img@@1M limine.conf ::/limine/limine.conf

TOTAL_IMG_SIZE=$$(( $(ROOTFS_IMG_SIZE) + 256 ))

single-$(IMAGE_NAME).img: assets/limine $(MODULES_TARGET) kernel initramfs-$(ARCH).img rootfs-$(ARCH).img
	dd if=/dev/zero of=single-$(IMAGE_NAME).img bs=1M count=$(TOTAL_IMG_SIZE)
	sgdisk --new=1:1M:255M --new=2:256M:0 single-$(IMAGE_NAME).img
	mkfs.vfat -F 32 --offset 2048 -S 512 single-$(IMAGE_NAME).img
	mcopy -i single-$(IMAGE_NAME).img@@1M kernel/bin-$(ARCH)/kernel ::/
	mcopy -i single-$(IMAGE_NAME).img@@1M initramfs-$(ARCH).img ::/initramfs.img
	mmd -i single-$(IMAGE_NAME).img@@1M ::/EFI ::/EFI/BOOT ::/limine
	mcopy -i single-$(IMAGE_NAME).img@@1M $(EFI_FILE_SINGLE) ::/EFI/BOOT
	mcopy -i single-$(IMAGE_NAME).img@@1M limine.conf ::/limine/limine.conf

	dd if=rootfs-$(ARCH).img of=single-$(IMAGE_NAME).img bs=1M count=$(ROOTFS_IMG_SIZE) seek=256

.PHONY: run
run: run-$(ARCH)

.PHONY: run-single
run-single: run-$(ARCH)-single

.PHONY: run-x86_64
run-x86_64: assets/ovmf-code-$(ARCH).fd all rootfs-$(ARCH).img
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=$(IMAGE_NAME).img,format=raw,id=harddisk \
		-drive if=none,file=rootfs-$(ARCH).img,format=raw,id=rootdisk \
		-device qemu-xhci,id=xhci \
		-device ahci,id=ahci \
		-device ide-hd,drive=harddisk,bus=ahci.0 \
		-device nvme,drive=rootdisk,serial=5678 \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		-rtc base=utc \
		-display sdl,gl=on \
		-device virtio-vga-gl \
		-vga none \
		$(QEMUFLAGS)

.PHONY: run-x86_64-single
run-x86_64-single: assets/ovmf-code-$(ARCH).fd all-single
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=single-$(IMAGE_NAME).img,format=raw,id=harddisk \
		-device qemu-xhci,id=xhci \
		-device usb-kbd \
		-device usb-mouse \
		-device usb-storage,drive=harddisk,bus=xhci.0 \
		$(QEMUFLAGS)

.PHONY: run-aarch64
run-aarch64: assets/ovmf-code-$(ARCH).fd all rootfs-$(ARCH).img
	qemu-system-$(ARCH) \
		-M virt \
		-device ramfb \
		-device qemu-xhci,id=xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=$(IMAGE_NAME).img,format=raw,id=harddisk \
		-drive if=none,file=rootfs-$(ARCH).img,format=raw,id=rootdisk \
		-device nvme,drive=harddisk,serial=1234 \
		-device nvme,drive=rootdisk,serial=5678 \
		$(QEMUFLAGS)

.PHONY: run-aarch64-single
run-aarch64-single: assets/ovmf-code-$(ARCH).fd all-single
	qemu-system-$(ARCH) \
		-M virt \
		-device ramfb \
		-device qemu-xhci,id=xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=single-$(IMAGE_NAME).img,format=raw,id=harddisk \
		-device usb-storage,drive=harddisk,bus=xhci.0 \
		$(QEMUFLAGS)

.PHONY: run-riscv64
run-riscv64: assets/ovmf-code-$(ARCH).fd all rootfs-$(ARCH).img
ifeq ($(BOOT_PROTOCOL), limine)
	qemu-system-$(ARCH) \
		-M virt,acpi=off \
		-cpu rv64 \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=$(IMAGE_NAME).img,format=raw,id=harddisk \
		-drive if=none,file=rootfs-$(ARCH).img,format=raw,id=rootdisk \
		-device nvme,drive=harddisk,serial=1234 \
		-device nvme,drive=rootdisk,serial=5678 \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		$(QEMUFLAGS)
endif
ifeq ($(BOOT_PROTOCOL), sbi)
	qemu-system-$(ARCH) \
		-M virt \
		-cpu rv64 \
		-kernel kernel/bin-$(ARCH)/kernel \
		-drive if=none,file=rootfs-$(ARCH).img,format=raw,id=rootdisk \
		-device virtio-blk-device,drive=rootdisk,serial=1234 \
		-netdev user,id=net0 \
		-device virtio-net-device,netdev=net0 \
		$(QEMUFLAGS)
endif

.PHONY: run-riscv64
run-riscv64-single: assets/ovmf-code-$(ARCH).fd all-single
	qemu-system-$(ARCH) \
		-M virt \
		-cpu rv64 \
		-nographic \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=single-$(IMAGE_NAME).img,format=raw,id=harddisk \
		-device usb-storage,drive=harddisk \
		$(QEMUFLAGS)

.PHONY: run-loongarch64
run-loongarch64: assets/ovmf-code-$(ARCH).fd $(IMAGE_NAME).img
ifeq ($(BOOT_PROTOCOL), limine)
	qemu-system-$(ARCH) \
		-M virt \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-device usb-mouse \
		-drive if=pflash,unit=0,format=raw,file=assets/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,file=$(IMAGE_NAME).img,format=raw,id=harddisk \
		-drive if=none,file=rootfs-$(ARCH).img,format=raw,id=rootdisk \
		-device virtio-blk-pci,drive=harddisk \
		-device virtio-blk-pci,drive=rootdisk \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		$(QEMUFLAGS)
endif
ifeq ($(BOOT_PROTOCOL), laboot)
	qemu-system-$(ARCH) \
		-M virt \
		-nographic \
		-kernel kernel/bin-$(ARCH)/kernel \
		-drive if=none,file=rootfs-$(ARCH).img,format=raw,id=rootdisk \
		-device virtio-blk-pci,drive=rootdisk \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		$(QEMUFLAGS)
endif

assets/limine:
	rm -rf assets/limine
	git clone https://codeberg.org/Limine/Limine --branch=v11.x-binary --depth=1 assets/limine
	$(MAKE) -C assets/limine \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

assets/ovmf-code-$(ARCH).fd:
	mkdir -p assets
	curl -Lo assets/edk2-ovmf.tar.gz https://github.com/osdev0/edk2-ovmf-nightly/releases/download/nightly-20260514T025248Z/edk2-ovmf.tar.gz
	tar -zxvf assets/edk2-ovmf.tar.gz -C assets/

	cp -r assets/edk2-ovmf/ovmf-code-$(ARCH).fd $@

	case "$(ARCH)" in \
		aarch64) dd if=/dev/zero of=$@ bs=1 count=0 seek=67108864 2>/dev/null;; \
		riscv64) dd if=/dev/zero of=$@ bs=1 count=0 seek=33554432 2>/dev/null;; \
	esac

.PHONY: modules
modules:
	$(MAKE) -C modules

.PHONY: module-signing-keys
module-signing-keys:
	$(call PRINT_STEP,GEN,$(MODULE_SIGN_KEY_DIR))
	$(Q)./kernel/scripts/gen_module_signing_keys.sh "$(MODULE_SIGN_KEY_DIR)" naos_signing_key_pub

ifneq ($(MODULE_VERIFY), 0)
kernel: module-signing-keys
endif

.PHONY: sign-modules
sign-modules: modules module-signing-keys
	$(call PRINT_STEP,SIGN,modules-$(ARCH))
	$(Q)find modules-$(ARCH) -type f -name '*.ko' -exec ./kernel/scripts/sign_module.py {} "$(MODULE_SIGN_PRIV)" \;

.PHONY: initramfs-$(ARCH).img
initramfs-$(ARCH).img: rootfs-$(ARCH).img $(MODULES_TARGET)
	sh mkinitcpio.sh
