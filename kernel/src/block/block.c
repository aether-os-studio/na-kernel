#include <block/block.h>
#include <mm/mm.h>
#include <block/partition.h>
#include <dev/device.h>
#include <arch/arch.h>
#include <task/task.h>
#include <libs/klibc.h>

DEFINE_LLIST(blk_dev_list);
uint64_t blk_devnum = 0;
spinlock_t blk_dev_lock = SPIN_INIT;

uint64_t device_regist_blk(int subtype, void *data, char *name, void *ioctl,
                           void *read, void *write) {
    return device_install(DEV_BLOCK, subtype, data, name, 0, NULL, NULL, ioctl,
                          NULL, read, write, NULL);
}

blkdev_t *find_blkdev_by_ptr(void *ptr) {
    blkdev_t *dev, *n, *found = NULL;
    spin_lock(&blk_dev_lock);
    llist_for_each(dev, n, &blk_dev_list, list) {
        if (dev->ptr == ptr) {
            found = dev;
            break;
        }
    }
    spin_unlock(&blk_dev_lock);
    return found;
}

blkdev_t *find_blkdev_by_id(uint64_t id) {
    blkdev_t *dev, *n, *found = NULL;
    spin_lock(&blk_dev_lock);
    llist_for_each(dev, n, &blk_dev_list, list) {
        if (dev->id == id) {
            found = dev;
            break;
        }
    }
    spin_unlock(&blk_dev_lock);
    return found;
}

blkdev_t *find_blkdev_by_name(const char *name) {
    blkdev_t *dev, *n, *found = NULL;
    spin_lock(&blk_dev_lock);
    llist_for_each(dev, n, &blk_dev_list, list) {
        if (dev->name && strcmp(dev->name, name) == 0) {
            found = dev;
            break;
        }
    }
    spin_unlock(&blk_dev_lock);
    return found;
}

void blkdev_register(blkdev_t *dev) {
    spin_lock(&blk_dev_lock);
    llist_append(&blk_dev_list, &dev->list);
    dev->id = blk_devnum++;
    spin_unlock(&blk_dev_lock);
    dev->mounted = false;
}

void blkdev_unregister(blkdev_t *dev) {
    if (dev->mounted)
        blkdev_unmount(dev);
    spin_lock(&blk_dev_lock);
    llist_delete(&dev->list);
    spin_unlock(&blk_dev_lock);
    free(dev->name);
}

int blkdev_mount(blkdev_t *dev) {
    if (!dev || dev->mounted)
        return -1;

    uint64_t lba_size = dev->block_size ? dev->block_size : 512;
    char *iso9660_detect;
    bool is_iso9660;
    struct MBR_DPT *boot_sector;

    struct GPT_DPT *buffer = (struct GPT_DPT *)malloc(sizeof(struct GPT_DPT));
    if (!buffer)
        return -ENOMEM;
    if (blkdev_read(dev->id, lba_size, buffer, sizeof(struct GPT_DPT)) !=
        sizeof(struct GPT_DPT)) {
        free(buffer);
        goto probe_mbr;
    }

    if (memcmp(buffer->signature, GPT_HEADER_SIGNATURE, 8) ||
        buffer->num_partition_entries == 0 ||
        buffer->partition_entry_lba == 0) {
        free(buffer);
        goto probe_mbr;
    }

    struct GPT_DPTE *dptes =
        (struct GPT_DPTE *)malloc(128 * sizeof(struct GPT_DPTE));
    if (!dptes) {
        free(buffer);
        return -ENOMEM;
    }
    if (blkdev_read(dev->id, buffer->partition_entry_lba * lba_size, dptes,
                    128 * sizeof(struct GPT_DPTE)) !=
        128 * sizeof(struct GPT_DPTE)) {
        free(dptes);
        free(buffer);
        return -EIO;
    }

    spin_lock(&partition_lock);
    for (uint32_t j = 0; j < 128; j++) {
        if (dptes[j].starting_lba == 0 || dptes[j].ending_lba == 0)
            continue;
        if (partition_num >= MAX_PARTITIONS_NUM) {
            spin_unlock(&partition_lock);
            free(dptes);
            free(buffer);
            return -ENOSPC;
        }

        partition_t *part = &partitions[partition_num];
        part->blkdev_id = dev->id;
        part->starting_lba = dptes[j].starting_lba;
        part->ending_lba = dptes[j].ending_lba;
        part->type = GPT;

        char pname[32];
        sprintf(pname, "%spart%d", dev->name, j);
        partitions[partition_num].dev =
            device_regist_blk(DEV_PART, &partitions[partition_num], pname,
                              partition_ioctl, partition_read, partition_write);

        partition_num++;
    }
    spin_unlock(&partition_lock);

    free(dptes);
    free(buffer);
    dev->mounted = true;
    return 0;

probe_mbr:
    iso9660_detect = (char *)malloc(5);
    if (!iso9660_detect)
        return -ENOMEM;
    memset(iso9660_detect, 0, 5);
    is_iso9660 = blkdev_read(dev->id, 0x8001, iso9660_detect, 5) == 5 &&
                 !memcmp(iso9660_detect, "CD001", 5);
    if (is_iso9660) {
        spin_lock(&partition_lock);
        if (partition_num >= MAX_PARTITIONS_NUM) {
            spin_unlock(&partition_lock);
            free(iso9660_detect);
            return -ENOSPC;
        }

        partition_t *part = &partitions[partition_num];
        part->blkdev_id = dev->id;
        part->starting_lba = 0;
        part->ending_lba =
            blkdev_ioctl(dev->id, IOCTL_GETSIZE, 0) / lba_size - 1;
        part->type = RAW;

        char pname[32];
        sprintf(pname, "%spart%d", dev->name, dev->id);
        partitions[partition_num].dev =
            device_regist_blk(DEV_PART, &partitions[partition_num], pname,
                              partition_ioctl, partition_read, partition_write);

        partition_num++;
        spin_unlock(&partition_lock);
        free(iso9660_detect);
        dev->mounted = true;
        return 0;
    }
    free(iso9660_detect);

    boot_sector = (struct MBR_DPT *)malloc(sizeof(struct MBR_DPT));
    if (!boot_sector)
        return -ENOMEM;
    if (blkdev_read(dev->id, 0, boot_sector, sizeof(struct MBR_DPT)) !=
        sizeof(struct MBR_DPT)) {
        free(boot_sector);
        return -EIO;
    }

    uint64_t old_partition_num = partition_num;

    spin_lock(&partition_lock);
    for (int j = 0; j < MBR_MAX_PARTITION_NUM; j++) {
        if (boot_sector->dpte[j].start_lba == 0 ||
            boot_sector->dpte[j].sectors_limit == 0)
            continue;
        if (partition_num >= MAX_PARTITIONS_NUM) {
            spin_unlock(&partition_lock);
            free(boot_sector);
            return -ENOSPC;
        }

        partition_t *part = &partitions[partition_num];
        part->blkdev_id = dev->id;
        part->starting_lba = boot_sector->dpte[j].start_lba;
        part->ending_lba = boot_sector->dpte[j].start_lba +
                           boot_sector->dpte[j].sectors_limit - 1;
        part->type = MBR;

        char pname[32];
        sprintf(pname, "%spart%d", dev->name, j);
        partitions[partition_num].dev =
            device_regist_blk(DEV_PART, &partitions[partition_num], pname,
                              partition_ioctl, partition_read, partition_write);

        partition_num++;
    }
    spin_unlock(&partition_lock);

    free(boot_sector);

    if (old_partition_num == partition_num) {
        spin_lock(&partition_lock);
        partition_t *part = &partitions[partition_num];
        part->blkdev_id = dev->id;
        part->starting_lba = 0;
        part->ending_lba =
            blkdev_ioctl(dev->id, IOCTL_GETSIZE, 0) / lba_size - 1;
        part->type = RAW;

        char pname[32];
        sprintf(pname, "%spart%d", dev->name, dev->id);
        partitions[partition_num].dev =
            device_regist_blk(DEV_PART, &partitions[partition_num], pname,
                              partition_ioctl, partition_read, partition_write);

        partition_num++;
        spin_unlock(&partition_lock);
    }

    dev->mounted = true;
    return 0;
}

int blkdev_unmount(blkdev_t *dev) {
    if (!dev || !dev->mounted)
        return -1;

    spin_lock(&partition_lock);
    for (int i = 0; i < partition_num; i++) {
        if (partitions[i].blkdev_id == dev->id) {
            partitions[i].blkdev_id = (uint64_t)-1;
            partitions[i].starting_lba = 0;
            partitions[i].ending_lba = 0;
        }
    }
    spin_unlock(&partition_lock);
    dev->mounted = false;
    return 0;
}

void regist_blkdev(char *name, void *ptr, uint64_t block_size, uint64_t size,
                   uint64_t max_op_size,
                   uint64_t (*read)(void *data, uint64_t lba, void *buffer,
                                    uint64_t size),
                   uint64_t (*write)(void *data, uint64_t lba, void *buffer,
                                     uint64_t size),
                   int (*flush)(void *data)) {
    blkdev_t *dev = (blkdev_t *)malloc(sizeof(blkdev_t));
    dev->name = strdup(name);
    dev->ptr = ptr;
    dev->block_size = block_size ? block_size : 512;
    dev->size = size;
    dev->max_op_size = max_op_size;
    dev->read = read;
    dev->write = write;
    dev->flush = flush;

    blkdev_register(dev);
    blkdev_mount(dev);
}

void unregist_blkdev(void *ptr) {
    blkdev_t *dev = find_blkdev_by_ptr(ptr);
    if (dev)
        blkdev_unregister(dev);
}

uint64_t blkdev_ioctl(uint64_t drive, uint64_t cmd, uint64_t arg) {
    blkdev_t *dev = find_blkdev_by_id(drive);
    if (!dev)
        return 0;

    switch (cmd) {
    case IOCTL_GETSIZE:
        return dev->size;
    case IOCTL_GETBLKSIZE:
        return dev->block_size;

    default:
        break;
    }

    return 0;
}

#define DMA_ALIGN PAGE_SIZE
#define IS_DMA_BUF(p) (((uintptr_t)(p) & (DMA_ALIGN - 1)) == 0)

static bool blk_buffer_is_userspace(const void *buf, uint64_t len) {
    return buf && !check_user_overflow((uint64_t)buf, len);
}

static bool blk_copy_to_buffer(void *dst, const void *src, uint64_t len,
                               bool dst_is_userspace) {
    if (len == 0)
        return true;
    if (!dst || !src)
        return false;

    if (dst_is_userspace)
        return !copy_to_user(dst, src, len);

    memcpy(dst, src, len);
    return true;
}

static bool blk_copy_from_buffer(void *dst, const void *src, uint64_t len,
                                 bool src_is_userspace) {
    if (len == 0)
        return true;
    if (!dst || !src)
        return false;

    if (src_is_userspace)
        return !copy_from_user(dst, src, len);

    memcpy(dst, src, len);
    return true;
}

uint64_t blkdev_read(uint64_t drive, uint64_t offset, void *buf, uint64_t len) {
    if (len == 0)
        return 0;

    blkdev_t *dev = find_blkdev_by_id(drive);
    if (!dev)
        return (uint64_t)-ENODEV;
    if (!dev || !dev->ptr || !dev->read)
        return (uint64_t)-ENOSYS;

    const uint64_t bs = dev->block_size;
    const uint64_t max_sec = dev->max_op_size / bs;
    uint8_t *dst = (uint8_t *)buf;
    uint64_t sector = offset / bs;
    uint64_t blk_off = offset % bs;
    uint64_t rem = len;
    uint64_t total = 0;
    bool dst_is_userspace = blk_buffer_is_userspace(buf, len);

    if (!dst_is_userspace && blk_off == 0 && (len % bs) == 0 &&
        IS_DMA_BUF(dst)) {
        uint64_t secs_left = len / bs;
        while (secs_left > 0) {
            uint64_t n = MIN(secs_left, max_sec);
            if (dev->read(dev->ptr, sector, dst, n) != n)
                return (uint64_t)-1;
            uint64_t bytes = n * bs;
            dst += bytes;
            sector += n;
            secs_left -= n;
            total += bytes;
        }
        return total;
    }

    if (blk_off != 0) {
        uint64_t head = MIN(bs - blk_off, rem);
        uint8_t *bounce = alloc_frames_bytes(bs);

        if (dev->read(dev->ptr, sector, bounce, 1) != 1) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        if (!blk_copy_to_buffer(dst, bounce + blk_off, head,
                                dst_is_userspace)) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        free_frames_bytes(bounce, bs);

        dst += head;
        rem -= head;
        total += head;
        sector++;
    }

    uint64_t mid_secs = rem / bs;

    if (mid_secs > 0 && !dst_is_userspace && IS_DMA_BUF(dst)) {
        while (mid_secs > 0) {
            uint64_t n = MIN(mid_secs, max_sec);
            if (dev->read(dev->ptr, sector, dst, n) != n)
                return (uint64_t)-1;
            uint64_t bytes = n * bs;
            dst += bytes;
            rem -= bytes;
            total += bytes;
            sector += n;
            mid_secs -= n;
        }
    } else if (mid_secs > 0) {
        uint64_t bn = MIN(mid_secs, max_sec);
        uint64_t bsz = bn * bs;
        uint8_t *bounce = alloc_frames_bytes(bsz);

        while (mid_secs > 0) {
            uint64_t n = MIN(mid_secs, bn);
            if (dev->read(dev->ptr, sector, bounce, n) != n) {
                free_frames_bytes(bounce, bsz);
                return (uint64_t)-1;
            }
            uint64_t bytes = n * bs;
            if (!blk_copy_to_buffer(dst, bounce, bytes, dst_is_userspace)) {
                free_frames_bytes(bounce, bsz);
                return (uint64_t)-1;
            }
            dst += bytes;
            rem -= bytes;
            total += bytes;
            sector += n;
            mid_secs -= n;
        }
        free_frames_bytes(bounce, bsz);
    }

    if (rem > 0) {
        uint8_t *bounce = alloc_frames_bytes(bs);

        if (dev->read(dev->ptr, sector, bounce, 1) != 1) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        if (!blk_copy_to_buffer(dst, bounce, rem, dst_is_userspace)) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        free_frames_bytes(bounce, bs);
        total += rem;
    }

    return total;
}

uint64_t blkdev_write(uint64_t drive, uint64_t offset, const void *buf,
                      uint64_t len) {
    if (len == 0)
        return 0;

    blkdev_t *dev = find_blkdev_by_id(drive);
    if (!dev)
        return (uint64_t)-ENODEV;
    if (!dev->ptr || !dev->read || !dev->write)
        return (uint64_t)-ENOSYS;

    const uint64_t bs = dev->block_size;
    const uint64_t max_sec = dev->max_op_size / bs;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t sector = offset / bs;
    uint64_t blk_off = offset % bs;
    uint64_t rem = len;
    uint64_t total = 0;
    bool src_is_userspace = blk_buffer_is_userspace(buf, len);

    if (!src_is_userspace && blk_off == 0 && (len % bs) == 0 &&
        IS_DMA_BUF(src)) {
        uint64_t secs_left = len / bs;
        while (secs_left > 0) {
            uint64_t n = MIN(secs_left, max_sec);
            if (dev->write(dev->ptr, sector, (void *)src, n) != n)
                return (uint64_t)-1;
            uint64_t bytes = n * bs;
            src += bytes;
            sector += n;
            secs_left -= n;
            total += bytes;
        }
        return total;
    }

    if (blk_off != 0) {
        if (!dev->read)
            return (uint64_t)-1;

        uint64_t head = MIN(bs - blk_off, rem);
        uint8_t *bounce = alloc_frames_bytes(bs);

        if (dev->read(dev->ptr, sector, bounce, 1) != 1) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        if (!blk_copy_from_buffer(bounce + blk_off, src, head,
                                  src_is_userspace)) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        if (dev->write(dev->ptr, sector, bounce, 1) != 1) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        free_frames_bytes(bounce, bs);

        src += head;
        rem -= head;
        total += head;
        sector++;
    }

    uint64_t mid_secs = rem / bs;

    if (mid_secs > 0 && !src_is_userspace && IS_DMA_BUF(src)) {
        while (mid_secs > 0) {
            uint64_t n = MIN(mid_secs, max_sec);
            if (dev->write(dev->ptr, sector, (void *)src, n) != n)
                return (uint64_t)-1;
            uint64_t bytes = n * bs;
            src += bytes;
            rem -= bytes;
            total += bytes;
            sector += n;
            mid_secs -= n;
        }
    } else if (mid_secs > 0) {
        uint64_t bn = MIN(mid_secs, max_sec);
        uint64_t bsz = bn * bs;
        uint8_t *bounce = alloc_frames_bytes(bsz);

        while (mid_secs > 0) {
            uint64_t n = MIN(mid_secs, bn);
            uint64_t bytes = n * bs;
            if (!blk_copy_from_buffer(bounce, src, bytes, src_is_userspace)) {
                free_frames_bytes(bounce, bsz);
                return (uint64_t)-1;
            }
            if (dev->write(dev->ptr, sector, bounce, n) != n) {
                free_frames_bytes(bounce, bsz);
                return (uint64_t)-1;
            }
            src += bytes;
            rem -= bytes;
            total += bytes;
            sector += n;
            mid_secs -= n;
        }
        free_frames_bytes(bounce, bsz);
    }

    if (rem > 0) {
        if (!dev->read)
            return (uint64_t)-1;

        uint8_t *bounce = alloc_frames_bytes(bs);
        if (dev->read(dev->ptr, sector, bounce, 1) != 1) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        if (!blk_copy_from_buffer(bounce, src, rem, src_is_userspace)) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        if (dev->write(dev->ptr, sector, bounce, 1) != 1) {
            free_frames_bytes(bounce, bs);
            return (uint64_t)-1;
        }
        free_frames_bytes(bounce, bs);
        total += rem;
    }

    return total;
}

int blkdev_flush(uint64_t drive) {
    blkdev_t *dev = find_blkdev_by_id(drive);
    if (!dev)
        return -ENODEV;
    if (!dev->flush)
        return -EOPNOTSUPP;
    return dev->flush(dev->ptr);
}
