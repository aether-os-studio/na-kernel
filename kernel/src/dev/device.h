#pragma once

#include <drivers/bus/pci.h>
#include <libs/llist.h>
#include <fs/vfs/vfs.h>

// 设备类型
enum device_type_t {
    DEV_NULL,  // 空设备
    DEV_CHAR,  // 字符设备
    DEV_BLOCK, // 块设备
    DEV_NET,   // 网络设备
};

// 设备子类型
enum device_subtype_t {
    DEV_TTY = 4,     // TTY 设备
    DEV_PART = 8,    // 磁盘分区
    DEV_SOUND = 116, // 声卡 / ALSA
    DEV_INPUT = 13,  // 输入设备
    DEV_FB = 29,     // 帧缓冲
    DEV_DISK,        // 磁盘
    DEV_NETIF,       // 网卡
    DEV_SYSDEV,      // 系统设备
    DEV_USB,         // USB userspace node
    DEV_GPU = 226,   // 显卡
    DEV_MAX,
};

typedef struct device_t {
    struct llist_header node;

    char *name;      // 设备名
    int type;        // 设备类型
    int subtype;     // 设备子类型
    uint64_t dev;    // 设备号
    uint64_t parent; // 父设备号
    void *ptr;       // 设备指针

    ssize_t (*open)(void *dev, void *arg);
    ssize_t (*close)(void *dev, void *arg);
    // 设备控制
    ssize_t (*ioctl)(void *dev, int cmd, void *args, fd_t *fd);
    // 轮询
    ssize_t (*poll)(void *dev, int events, fd_t *fd);
    // 读设备
    ssize_t (*read)(void *dev, void *buf, uint64_t offset, size_t size,
                    fd_t *fd);
    // 写设备
    ssize_t (*write)(void *dev, void *buf, uint64_t offset, size_t size,
                     fd_t *fd);

    void *(*map)(void *dev, void *addr, size_t offset, size_t size, size_t prot,
                 fd_t *fd);
} device_t;

typedef struct device_file device_file_t;

enum device_cmd_t {
    DEV_CMD_SECTOR_START = 1, // 获得设备扇区开始位置 lba
    DEV_CMD_SECTOR_COUNT,     // 获得设备扇区数量
    DEV_CMD_SECTOR_SIZE,      // 获得设备扇区大小
    DEV_CMD_FLUSH,            // 将块设备写缓存持久化
};

// 安装设备
uint64_t device_install(int type, int subtype, void *ptr, char *name,
                        uint64_t parent, void *open, void *close, void *ioctl,
                        void *poll, void *read, void *write, void *map);
uint64_t device_install_with_minor(int type, int subtype, void *ptr, char *name,
                                   uint64_t parent, void *open, void *close,
                                   void *ioctl, void *poll, void *read,
                                   void *write, void *map, uint64_t minor);
int device_uninstall(uint64_t dev);

// 根据子类型查找设备
device_t *device_find(int type, uint64_t idx);

// 根据设备号查找设备
device_t *device_get(uint64_t dev);

ssize_t device_open(uint64_t dev, fd_t *fd);
ssize_t device_close(uint64_t dev, fd_t *fd);

device_file_t *device_file_context(fd_t *fd);
device_t *device_file_device(fd_t *fd);
void *device_file_private(fd_t *fd);
int device_file_set_private(fd_t *fd, void *private_data);

// 控制设备
ssize_t device_ioctl(uint64_t dev, int cmd, void *args, fd_t *fd);

// 轮询
ssize_t device_poll(uint64_t dev, int events, fd_t *fd);

// 读设备
ssize_t device_read(uint64_t dev, void *buf, uint64_t idx, size_t count,
                    fd_t *fd);

// 写设备
ssize_t device_write(uint64_t dev, void *buf, uint64_t idx, size_t count,
                     fd_t *fd);

void *device_map(uint64_t dev, void *addr, size_t offset, size_t size,
                 size_t prot, fd_t *fd);

void device_init();
