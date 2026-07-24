#pragma once

#include <libs/klibc.h>
#include <libs/keys.h>
#include <libs/llist.h>

typedef struct vfs_inode vfs_node_t;

#define KDGETMODE 0x4B3B // 获取终端模式命令
#define KDSETMODE 0x4B3A // 设置终端模式命令
#define KD_TEXT 0x00     // 文本模式
#define KD_GRAPHICS 0x01 // 图形模式

#define KDGKBMODE 0x4B44 /* gets current keyboard mode */
#define KDSKBMODE 0x4B45 /* sets current keyboard mode */
#define K_RAW 0x00       // 原始模式（未处理扫描码）
#define K_XLATE 0x01     // 转换模式（生成ASCII）
#define K_MEDIUMRAW 0x02 // 中等原始模式
#define K_UNICODE 0x03   // Unicode模式

#define VT_OPENQRY 0x5600 /* get next available vt */
#define VT_GETMODE 0x5601 /* get mode of active vt */
#define VT_SETMODE 0x5602

#define VT_GETSTATE 0x5603
#define VT_SENDSIG 0x5604

#define VT_ACTIVATE 0x5606   /* make vt active */
#define VT_WAITACTIVE 0x5607 /* wait for vt active */

struct vt_state {
    uint16_t v_active; // 活动终端号
    uint16_t v_signal; // 等待切换时的信号
    uint16_t v_state;  // 终端状态标志
};

struct vt_mode {
    char mode;    // 终端模式
    char waitv;   // 垂直同步
    short relsig; // 释放信号
    short acqsig; // 获取信号
    short frsig;  // 强制释放信号
};

#define VT_AUTO 0x00    // 自动切换模式
#define VT_PROCESS 0x01 // 进程控制模式

typedef size_t (*event_bit_t)(void *data, uint64_t request, void *arg);

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW 0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_FF 0x15
#define EV_PWR 0x16
#define EV_FF_STATUS 0x17
#define EV_MAX 0x1f
#define EV_CNT (EV_MAX + 1)

#define ABS_MAX 0x3f
#define ABS_CNT (ABS_MAX + 1)

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

struct input_event {
    uint64_t sec;
    uint64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define INPUT_EVENT_RING_SIZE (PAGE_SIZE * 32)

typedef struct dev_input_event {
    char *devname;
    char *physloc;
    vfs_node_t *devnode;

    struct llist_header open_files;
    spinlock_t open_files_lock;

    size_t timesOpened;

    struct input_id inputid;

    size_t properties;
    uint8_t evbit[(EV_CNT + 7) / 8];
    uint8_t keybit[(KEY_CNT + 7) / 8];
    uint8_t relbit[(REL_CNT + 7) / 8];
    uint8_t absbit[(ABS_CNT + 7) / 8];
    struct input_absinfo absinfo[ABS_CNT];

    event_bit_t event_bit;

    char uniq[32];
} dev_input_event_t;

typedef struct attribute {
    char *name;
    char *value;
} attribute_t;

/**
 * Allocate a text sysfs-style attribute for a bus or bus device.
 */
attribute_t *attribute_new(const char *name, const char *value);
void attribute_free(attribute_t *attr);

struct bus_device;

typedef struct bin_attribute {
    char *name;
    ssize_t (*read)(struct bus_device *dev, struct bin_attribute *attr,
                    char *buf, uint64_t off, size_t count);
    ssize_t (*write)(struct bus_device *dev, struct bin_attribute *attr,
                     const char *buf, uint64_t off, size_t count);
} bin_attribute_t;

struct bus_device;

/**
 * Logical bus descriptor that supplies default sysfs-visible metadata for all
 * devices attached to that bus.
 */
typedef struct bus {
    const char *name;
    const char *devices_path;
    const char *drivers_path;
    attribute_t **bus_default_attrs;
    int bus_default_attrs_count;
    bin_attribute_t **bus_default_bin_attrs;
    int bus_default_bin_attrs_count;
} bus_t;

/**
 * Generic sysfs-visible device instance attached to a logical bus.
 */
typedef struct bus_device {
    struct llist_header node;

    bus_t *bus;
    void *private_data;
    char *sysfs_path;
    char *bus_link_path;

    int (*get_device_path)(struct bus_device *device, char *buf,
                           size_t max_count);

    attribute_t **attrs;
    int attrs_count;
    bin_attribute_t **bin_attrs;
    int bin_attrs_count;
} bus_device_t;

typedef struct attributes_builder {
    attribute_t **attrs;
    int count;
    int capability;
} attributes_builder_t;

/**
 * Allocate a dynamic attribute-array builder.
 */
attributes_builder_t *attributes_builder_new();
int attributes_builder_append(attributes_builder_t *builder, attribute_t *attr);
int attributes_builder_append_new(attributes_builder_t *builder,
                                  const char *name, const char *value);
void attributes_builder_free(attributes_builder_t *builder);
void attributes_builder_free_all(attributes_builder_t *builder);

typedef struct bin_attributes_builder {
    bin_attribute_t **bin_attrs;
    int count;
    int capability;
} bin_attributes_builder_t;

/**
 * Allocate a dynamic binary-attribute-array builder.
 */
bin_attributes_builder_t *bin_attributes_builder_new();
int bin_attributes_builder_append(bin_attributes_builder_t *builder,
                                  bin_attribute_t *attr);

/**
 * Install a generic bus device and merge the caller-provided attributes with
 * the bus defaults.
 */
bus_device_t *bus_device_install_internal(
    bus_t *bus, void *dev_data, attribute_t **extra_attrs,
    int extra_attrs_count, bin_attribute_t **extra_bin_attrs,
    int extra_bin_attrs_count,
    int (*get_device_path)(struct bus_device *device, char *buf,
                           size_t max_count));

/**
 * Convenience wrapper for installing a PCI-backed bus device.
 */
bus_device_t *bus_device_install_pci(void *dev_data, attribute_t **extra_attrs,
                                     int extra_attrs_count,
                                     bin_attribute_t **extra_bin_attrs,
                                     int extra_bin_attrs_count);
/**
 * Convenience wrapper for installing a USB-backed bus device.
 */
bus_device_t *bus_device_install_usb(void *dev_data, attribute_t **extra_attrs,
                                     int extra_attrs_count,
                                     bin_attribute_t **extra_bin_attrs,
                                     int extra_bin_attrs_count);
