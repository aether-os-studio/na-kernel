#include "fs/vfs/vfs.h"
#include "dev/device.h"
#include "block/partition.h"
#include "block/block.h"
#include "drivers/logger.h"
#include "arch/arch.h"

partition_t partitions[MAX_PARTITIONS_NUM] = {0};
uint64_t partition_num = 0;

static inline uint64_t partition_lba_size(partition_t *part) {
    uint64_t lba_size = blkdev_ioctl(part->blkdev_id, IOCTL_GETBLKSIZE, 0);
    return lba_size ? lba_size : 512;
}

ssize_t partition_read(void *data, void *buf, uint64_t offset, uint64_t len,
                       uint64_t flags) {
    partition_t *part = (partition_t *)data;
    uint64_t lba_size = partition_lba_size(part);
    return blkdev_read(part->blkdev_id, part->starting_lba * lba_size + offset,
                       buf, len);
}

ssize_t partition_write(void *data, const void *buf, uint64_t offset,
                        uint64_t len, uint64_t flags) {
    partition_t *part = (partition_t *)data;
    uint64_t lba_size = partition_lba_size(part);
    return blkdev_write(part->blkdev_id, part->starting_lba * lba_size + offset,
                        buf, len);
}

int partition_ioctl(void *data, int cmd, void *args, fd_t *fd) {
    partition_t *part = (partition_t *)data;
    switch (cmd) {
    case DEV_CMD_SECTOR_START:
        return part->starting_lba;
    case DEV_CMD_SECTOR_COUNT:
        return part->ending_lba - part->starting_lba + 1;
    case DEV_CMD_SECTOR_SIZE:
        return partition_lba_size(part);
    case DEV_CMD_FLUSH:
        return blkdev_flush(part->blkdev_id);
    default:
        return -EINVAL;
    }
}
