#pragma once

#include <libs/klibc.h>
#include <libs/llist.h>

#define MAX_BLKDEV_NUM 64

/**
 * Registered block device descriptor. Drivers provide sector-sized read/write
 * callbacks while higher layers manage lookup, mount state, and ioctl helpers.
 */
typedef struct blkdev {
    struct llist_header list;
    char *name;
    void *ptr;
    uint64_t block_size;
    uint64_t size;
    uint64_t max_op_size;
    uint64_t id;
    bool mounted;
    uint64_t (*read)(void *data, uint64_t lba, void *buffer, uint64_t size);
    uint64_t (*write)(void *data, uint64_t lba, void *buffer, uint64_t size);
    int (*flush)(void *data);
} blkdev_t;

extern struct llist_header blk_dev_list;
extern uint64_t blk_devnum;
extern spinlock_t blk_dev_lock;

/**
 * Look up a block device by its driver-private backing pointer.
 */
blkdev_t *find_blkdev_by_ptr(void *ptr);
/**
 * Look up a block device by numeric id.
 */
blkdev_t *find_blkdev_by_id(uint64_t id);
/**
 * Look up a block device by exported name.
 */
blkdev_t *find_blkdev_by_name(const char *name);

/**
 * Insert a fully initialized block device into the global registry.
 */
void blkdev_register(blkdev_t *dev);
void blkdev_unregister(blkdev_t *dev);
int blkdev_mount(blkdev_t *dev);
int blkdev_unmount(blkdev_t *dev);

/**
 * Convenience helper that allocates and registers a block device from raw
 * callback pointers.
 */
void regist_blkdev(char *name, void *ptr, uint64_t block_size, uint64_t size,
                   uint64_t max_op_size,
                   uint64_t (*read)(void *data, uint64_t lba, void *buffer,
                                    uint64_t size),
                   uint64_t (*write)(void *data, uint64_t lba, void *buffer,
                                     uint64_t size),
                   int (*flush)(void *data));
void unregist_blkdev(void *ptr);

enum {
    IOCTL_GETBLKSIZE,
    IOCTL_GETSIZE,
};

/**
 * Block-device ioctl entry point used by higher-level storage code.
 */
uint64_t blkdev_ioctl(uint64_t drive, uint64_t cmd, uint64_t arg);

/**
 * Read a byte range from a registered block device.
 */
uint64_t blkdev_read(uint64_t drive, uint64_t offset, void *buf, uint64_t len);
/**
 * Write a byte range to a registered block device.
 */
uint64_t blkdev_write(uint64_t drive, uint64_t offset, const void *buf,
                      uint64_t len);
/** Persist all completed writes to stable storage. */
int blkdev_flush(uint64_t drive);
