#pragma once

#include <libs/klibc.h>
#include <mm/mm.h>
#include <drivers/bus/pci.h>

typedef enum virtio_device_type {
    VIRTIO_DEVICE_TYPE_RESERVED = 0,
    VIRTIO_DEVICE_TYPE_NETWORK = 1, // 网络设备
    VIRTIO_DEVICE_TYPE_BLOCK = 2,   // 块设备
    VIRTIO_DEVICE_TYPE_CONSOLE = 3, // 控制台
    VIRTIO_DEVICE_TYPE_ENTROPY = 4, // 熵源（随机数）
    VIRTIO_DEVICE_TYPE_BALLOON = 5, // 内存气球
    VIRTIO_DEVICE_TYPE_SCSI = 8,    // SCSI 主机
    VIRTIO_DEVICE_TYPE_9P = 9,      // 9P 传输
    VIRTIO_DEVICE_TYPE_GPU = 16,    // GPU
    VIRTIO_DEVICE_TYPE_INPUT = 18,  // 输入设备（键盘/鼠标/触摸板）
    VIRTIO_DEVICE_TYPE_VSOCK = 19,  // Socket
    VIRTIO_DEVICE_TYPE_CRYPTO = 20, // 加密设备
    VIRTIO_DEVICE_TYPE_SOUND = 25,  // 音频设备
    VIRTIO_DEVICE_TYPE_FS = 26,     // 文件系统
    VIRTIO_DEVICE_TYPE_PMEM = 27,   // 持久内存
} virtio_device_type_t;

#define VIRTIO_F_RING_INDIRECT_DESC (1ULL << 28)
#define VIRTIO_F_RING_EVENT_IDX (1ULL << 29)
#define VIRTIO_F_VERSION_1 (1ULL << 32)

struct virtio_driver;
typedef struct virtio_driver virtio_driver_t;
struct virtio_device_driver;
typedef struct virtio_device_driver virtio_device_driver_t;

typedef void (*virtio_interrupt_handler_t)(void *opaque, uint8_t isr_status);

typedef struct virtio_driver_op {
    virtio_driver_t *(*init)(
        void *data); // This data may be pci_device or mmio_device
    virtio_device_type_t (*get_device_type)(void *data);
    uint64_t (*get_features)(void *data);
    void (*set_features)(void *data, uint64_t features);
    uint32_t (*get_max_queue_size)(void *data, uint16_t queue);
    void (*notify)(void *data, uint16_t queue);
    uint32_t (*get_status)(void *data);
    void (*set_status)(void *data, uint32_t status);
    void (*queue_set)(void *data, uint16_t queue, uint32_t size,
                      uint64_t descriptors_paddr, uint64_t driver_area_paddr,
                      uint64_t device_area_paddr);
    bool (*queue_used)(void *data, uint16_t queue);
    bool (*requires_legacy_layout)(void *data);
    uint32_t (*read_config_space)(void *data, uint32_t offset);
    void (*write_config_space)(void *data, uint32_t offset, uint32_t value);
    bool (*supports_interrupts)(void *data);
    void (*set_interrupt_handler)(void *data,
                                  virtio_interrupt_handler_t handler,
                                  void *opaque);
    uint64_t (*interrupt_seq)(void *data);
    int (*wait_interrupt)(void *data, uint64_t observed_seq,
                          int64_t timeout_ns);
} virtio_driver_op_t;

struct virtio_driver {
    void *data;
    virtio_driver_op_t *op;
    virtio_device_type_t device_type;
    const char *transport_name;
    virtio_device_driver_t *bound_driver;
};

struct virtio_device_driver {
    const char *name;
    virtio_device_type_t device_type;
    void *data;
    int (*probe)(virtio_driver_t *driver);
    void (*remove)(virtio_driver_t *driver);
    void (*shutdown)(virtio_driver_t *driver);
};

typedef struct virtio_buffer {
    uint64_t addr;
    uint32_t size;
} virtio_buffer_t;

struct virtqueue;
typedef struct virtqueue virtqueue_t;

uint64_t virtio_begin_init(virtio_driver_t *driver,
                           uint64_t supported_features);
void virtio_finish_init(virtio_driver_t *driver);
void virtio_driver_reset(virtio_driver_t *driver);
int virtio_register_device_driver(virtio_device_driver_t *driver);
int virtio_unregister_device_driver(virtio_device_driver_t *driver);
bool virtio_driver_has_feature(virtio_driver_t *driver,
                               unsigned int feature_bit);
uint32_t virtio_driver_read_config_u32(virtio_driver_t *driver,
                                       uint32_t offset);
void virtio_driver_write_config_u32(virtio_driver_t *driver, uint32_t offset,
                                    uint32_t value);
bool virtio_driver_supports_interrupts(virtio_driver_t *driver);
uint64_t virtio_driver_interrupt_seq(virtio_driver_t *driver);
int virtio_driver_wait_interrupt(virtio_driver_t *driver, uint64_t observed_seq,
                                 int64_t timeout_ns);
void virtio_driver_set_interrupt_handler(virtio_driver_t *driver,
                                         virtio_interrupt_handler_t handler,
                                         void *opaque);
void *virtio_driver_parent_native(virtio_driver_t *driver);
bool virtio_driver_get_shm_region(virtio_driver_t *driver, uint8_t id,
                                  uint64_t *addr, uint64_t *len);
virtio_device_type_t virtio_driver_get_device_type(virtio_driver_t *driver);
void virtio_bus_init(void);
