#!/bin/bash

set -x # show cmds
set -e # fail globally

SCRIPT=$(realpath "$0")
SCRIPTPATH=$(dirname "$SCRIPT")
ROOTFS_DISTRO=${ROOTFS_DISTRO:-void}

if [ "$ROOTFS_DISTRO" == alpine ]; then
    if [ "$ARCH" != riscv64 ]; then
        echo "Alpine rootfs is currently supported only for riscv64" >&2
        exit 1
    fi

    APK_INSTALL_PATH="$SCRIPTPATH/cache/apk"
    APK_PACKAGE_CACHE_PATH="$SCRIPTPATH/cache/apk-packages-$ARCH"
    APK_XZ_PATH="$SCRIPTPATH/cache/apk-tools-static.apk"
    APK_XZ_URI="http://mirror.sjtu.edu.cn/alpine/edge/main/x86_64/apk-tools-static-3.0.6-r0.apk"

    ALPINE_VERSION=edge
    ALPINE_MIRROR_ROOT="http://mirror.sjtu.edu.cn/alpine"
    ALPINE_MIRROR="$ALPINE_MIRROR_ROOT/$ALPINE_VERSION"

    mkdir -p "$(dirname "$APK_XZ_PATH")"
    [ -f "$APK_XZ_PATH" ] || curl -Lo "$APK_XZ_PATH" "$APK_XZ_URI"

    mkdir -p "$APK_INSTALL_PATH"
    tar -xf "$APK_XZ_PATH" -C "$APK_INSTALL_PATH"

    mkdir -p "$APK_PACKAGE_CACHE_PATH"

    sudo "$APK_INSTALL_PATH/sbin/apk.static" \
        --arch "$ARCH" \
        -U \
        --allow-untrusted \
        --root "$ROOTFS_SYSROOT" \
        --cache-dir "$APK_PACKAGE_CACHE_PATH" \
        -X "$ALPINE_MIRROR/main" \
        -X "$ALPINE_MIRROR/community" \
        --initdb add \
        alpine-base bash coreutils util-linux bind-tools pciutils sudo iw \
        gcc binutils make strace git \
        musl-locales ncurses tzdata which shadow grep elfutils curl ca-certificates \
        seatd seatd-launch eudev dbus dbus-x11 \
        labwc foot fuzzel xwayland xrandr xkeyboard-config \
        thunar \
        adwaita-icon-theme adwaita-xfce-icon-theme adw-gtk3 font-dejavu \
        gdk-pixbuf librsvg shared-mime-info hicolor-icon-theme \
        mesa mesa-dri-gallium mesa-egl mesa-gbm mesa-gl libdisplay-info \
        fastfetch mesa-demos lite-xl qemu-system-x86_64

    sudo ln -sf /usr/share/zoneinfo/Asia/Shanghai "$ROOTFS_SYSROOT/etc/localtime"

    sudo cp -rf "$SCRIPTPATH/base/"* "$ROOTFS_SYSROOT/"
    sudo chmod +x "$ROOTFS_SYSROOT/usr/lib/aether/init"
    sudo ln -sf /usr/lib/aether/init "$ROOTFS_SYSROOT/sbin/init"

    exit 0
fi

if [ "$ROOTFS_DISTRO" != void ]; then
    echo "Unsupported rootfs distribution: $ROOTFS_DISTRO" >&2
    exit 1
fi

XBPS_INSTALL_PATH="$SCRIPTPATH/cache/xbps"
XBPS_XZ_PATH="$SCRIPTPATH/cache/xbps-static-latest.tar.xz"
XBPS_XZ_URI="http://repo-default.voidlinux.org/static/xbps-static-latest.$(uname -m)-musl.tar.xz"

mkdir -p "$(dirname "$XBPS_XZ_PATH")"
[ -f "$XBPS_XZ_PATH" ] || curl -Lo "$XBPS_XZ_PATH" "$XBPS_XZ_URI"

mkdir -p "$(dirname "$XBPS_INSTALL_PATH")"
[ -d "$XBPS_INSTALL_PATH" ] || mkdir -p "$XBPS_INSTALL_PATH" && tar -xf $XBPS_XZ_PATH -C $XBPS_INSTALL_PATH

[ $ARCH == aarch64 ] && export ARCH_SPEC=aarch64

sudo XBPS_ARCH=$ARCH $XBPS_INSTALL_PATH/usr/bin/xbps-install -S -r $ROOTFS_SYSROOT -R "http://mirrors.tuna.tsinghua.edu.cn/voidlinux/current/$ARCH_SPEC" \
    base-minimal bash coreutils util-linux inetutils bind-utils pciutils usbutils sudo iw \
    gcc binutils make strace git \
    glibc-locales ncurses tzdata which shadow grep elfutils curl htop \
    seatd eudev dbus xfce4 labwc xorg-server-xwayland xrandr xkeyboard-config \
    fastfetch mesa mesa-dri mesa-demos lite-xl qemu-system-amd64 libwebkit2gtk41 \
    adwaita-icon-theme noto-fonts-cjk

sudo ln -sf /usr/share/zoneinfo/Asia/Shanghai $ROOTFS_SYSROOT/etc/localtime

sudo chroot $ROOTFS_SYSROOT /bin/bash --login -c "xbps-reconfigure -f glibc-locales"
sudo chroot $ROOTFS_SYSROOT /bin/bash --login -c "echo \"root:root\" | chpasswd"

sudo cp -r $SCRIPTPATH/base/* $ROOTFS_SYSROOT/

sudo mkdir -p $ROOTFS_SYSROOT/etc/runit/runsvdir/default/
sudo ln -sf /etc/sv/seatd $ROOTFS_SYSROOT/etc/runit/runsvdir/default/seatd
sudo ln -sf /etc/sv/udevd $ROOTFS_SYSROOT/etc/runit/runsvdir/default/udevd
sudo ln -sf /etc/sv/dbus $ROOTFS_SYSROOT/etc/runit/runsvdir/default/dbus
sudo ln -sf /etc/sv/elogind $ROOTFS_SYSROOT/etc/runit/runsvdir/default/elogind
sudo ln -sf /etc/sv/polkitd $ROOTFS_SYSROOT/etc/runit/runsvdir/default/polkitd
sudo ln -sf /etc/sv/aether-xfce $ROOTFS_SYSROOT/etc/runit/runsvdir/default/aether-xfce

sudo ln -sf /run/runit/runsvdir/current $ROOTFS_SYSROOT/var/service
