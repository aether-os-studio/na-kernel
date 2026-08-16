#pragma once

#include <block/block.h>
#include <drivers/bus/usb.h>
#include <task/wait.h>

// CBW/CSW结构体
typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
} __attribute__((packed)) usb_msc_cbw_t;

typedef struct {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;
} __attribute__((packed)) usb_msc_csw_t;

typedef struct usb_msc_device usb_msc_device_t;
typedef struct usb_msc_lun usb_msc_lun_t;

// 函数指针类型定义
typedef uint64_t (*usb_msc_read_func_t)(usb_msc_lun_t *lun, uint64_t lba,
                                        void *buf, uint64_t count);
typedef uint64_t (*usb_msc_write_func_t)(usb_msc_lun_t *lun, uint64_t lba,
                                         void *buf, uint64_t count);

struct usb_msc_lun {
    usb_msc_device_t *ctrl;
    uint8_t lun;
    uint32_t block_size;
    uint64_t block_count;
    bool registered;
};

struct usb_msc_device {
    usb_device_t *udev;
    usb_device_interface_t *iface;
    usb_pipe_t *bulk_in;
    usb_pipe_t *bulk_out;
    // 串行化单命令传输；USB bulk 传输路径会阻塞（等待事件），
    // 因此必须使用可睡眠的互斥锁而不是自旋锁。
    wait_mutex_t lock;
    uint32_t next_tag;
    uint8_t lun_count;
    usb_msc_lun_t *luns;
};

uint64_t usb_msc_read_blocks(void *dev, uint64_t lba, void *buf,
                             uint64_t count);
uint64_t usb_msc_write_blocks(void *dev, uint64_t lba, void *buf,
                              uint64_t count);
