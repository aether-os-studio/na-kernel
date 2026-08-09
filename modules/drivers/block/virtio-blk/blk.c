// Copyright (C) 2025-2026  lihanrui2913
#include "blk.h"
#include <mm/mm.h>
#include <libs/klibc.h>
#include <task/task.h>

#define VIRTIO_BLK_DMA_ALIGN PAGE_SIZE
#define VIRTIO_BLK_IS_DMA_BUF(p)                                               \
    (((uintptr_t)(p) & (VIRTIO_BLK_DMA_ALIGN - 1)) == 0)

static bool virtio_blk_buffer_is_userspace(const void *buf, uint64_t len) {
    return buf && !check_user_overflow((uint64_t)buf, len);
}

static void virtio_blk_reap_completed_locked(virtio_blk_device_t *blk_dev) {
    uint32_t len = 0;
    uint16_t used_desc_idx = 0;

    while ((used_desc_idx = virt_queue_get_used_buf(blk_dev->request_queue,
                                                    &len)) != 0xFFFF) {
        virtio_blk_req_slot_t *slot = NULL;
        if (used_desc_idx < SIZE) {
            int16_t slot_idx = blk_dev->pending_slot_by_desc[used_desc_idx];
            if (slot_idx >= 0 && slot_idx < blk_dev->slot_count) {
                slot = &blk_dev->slots[slot_idx];
                blk_dev->pending_slot_by_desc[used_desc_idx] = -1;
            }
        }

        if (!slot) {
            virt_queue_free_desc(blk_dev->request_queue, used_desc_idx);
            continue;
        }

        virt_queue_free_desc(blk_dev->request_queue, used_desc_idx);
        slot->desc_idx = 0xFFFF;
        __atomic_store_n(&slot->completed, true, __ATOMIC_RELEASE);
    }
}

static void virtio_blk_irq_handler(void *opaque, uint8_t isr_status) {
    virtio_blk_device_t *blk_dev = (virtio_blk_device_t *)opaque;

    if (!blk_dev || !(isr_status & 0x1))
        return;

    spin_lock(&blk_dev->request_lock);
    virtio_blk_reap_completed_locked(blk_dev);
    spin_unlock(&blk_dev->request_lock);
    wait_queue_wake_all(&blk_dev->request_wait, 0, EOK);
}

static virtio_blk_req_slot_t *
virtio_blk_alloc_slot_locked(virtio_blk_device_t *blk_dev) {
    for (;;) {
        virtio_blk_reap_completed_locked(blk_dev);

        for (uint16_t i = 0; i < blk_dev->slot_count; i++) {
            virtio_blk_req_slot_t *slot = &blk_dev->slots[i];
            if (!slot->in_use) {
                slot->in_use = true;
                slot->use_bounce = false;
                slot->device_writes_data = false;
                slot->completed = false;
                slot->data_len = 0;
                slot->desc_idx = 0xFFFF;
                return slot;
            }
        }

        spin_unlock(&blk_dev->request_lock);
        schedule(0);
        spin_lock(&blk_dev->request_lock);
    }
}

static void virtio_blk_release_slot(virtio_blk_device_t *blk_dev,
                                    virtio_blk_req_slot_t *slot) {
    if (!slot) {
        return;
    }

    spin_lock(&blk_dev->request_lock);
    if (slot->desc_idx != 0xFFFF) {
        if (slot->desc_idx < SIZE) {
            blk_dev->pending_slot_by_desc[slot->desc_idx] = -1;
        }
        virt_queue_free_desc(blk_dev->request_queue, slot->desc_idx);
        slot->desc_idx = 0xFFFF;
    }
    slot->completed = false;
    slot->in_use = false;
    spin_unlock(&blk_dev->request_lock);
}

static int virtio_blk_wait_slot(virtio_blk_device_t *blk_dev,
                                virtio_blk_req_slot_t *slot) {
    for (;;) {
        if (__atomic_load_n(&slot->completed, __ATOMIC_ACQUIRE)) {
            return 0;
        }

        spin_lock(&blk_dev->request_lock);
        virtio_blk_reap_completed_locked(blk_dev);
        bool done = __atomic_load_n(&slot->completed, __ATOMIC_ACQUIRE);
        spin_unlock(&blk_dev->request_lock);
        if (done) {
            return 0;
        }

        if (blk_dev->driver->op->supports_interrupts &&
            blk_dev->driver->op->supports_interrupts(blk_dev->driver->data) &&
            current_task) {
            wait_queue_entry_t wait;

            task_prepare_block(current_task);
            wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
            wait_queue_add(&blk_dev->request_wait, &wait);

            if (__atomic_load_n(&slot->completed, __ATOMIC_ACQUIRE)) {
                wait_queue_remove(&blk_dev->request_wait, &wait);
                task_cancel_block_prepare(current_task);
                return 0;
            }

            int reason =
                task_block(current_task, TASK_BLOCKING, -1, "virtio_blk_wait");
            wait_queue_remove(&blk_dev->request_wait, &wait);
            task_cancel_block_prepare(current_task);
            if (reason < 0 && reason != EOK)
                return reason;
            continue;
        }

        schedule(0);
    }
}

static int virtio_blk_submit_rw(virtio_blk_device_t *blk_dev, uint32_t type,
                                uint64_t sector, void *buffer,
                                uint32_t total_size) {
    bool buffer_is_userspace =
        virtio_blk_buffer_is_userspace(buffer, total_size);
    bool use_direct =
        total_size > 0 && !buffer_is_userspace && VIRTIO_BLK_IS_DMA_BUF(buffer);

    spin_lock(&blk_dev->request_lock);
    virtio_blk_req_slot_t *slot = virtio_blk_alloc_slot_locked(blk_dev);
    spin_unlock(&blk_dev->request_lock);
    if (!slot) {
        return -1;
    }

    slot->req_header->type = type;
    slot->req_header->reserved = 0;
    slot->req_header->sector = sector;
    *slot->status_byte = 0xFF;
    slot->data_len = total_size;
    slot->device_writes_data = (type == VIRTIO_BLK_T_IN);

    void *data_addr = buffer;
    if (total_size > 0 && !use_direct) {
        if (slot->bounce_capacity < total_size) {
            virtio_blk_release_slot(blk_dev, slot);
            return -1;
        }

        slot->use_bounce = true;
        data_addr = slot->bounce_buffer;
        if (type == VIRTIO_BLK_T_OUT) {
            memcpy(slot->bounce_buffer, buffer, total_size);
        }
        dma_sync_cpu_to_device(slot->bounce_buffer, total_size);
    } else if (total_size > 0) {
        slot->use_bounce = false;
        dma_sync_cpu_to_device(buffer, total_size);
    }

    dma_sync_cpu_to_device(slot->req_header, sizeof(*slot->req_header));
    dma_sync_cpu_to_device(slot->status_byte, sizeof(*slot->status_byte));

    virtio_buffer_t bufs[3];
    bool writable[3];
    uint16_t num_bufs = 0;

    bufs[num_bufs].addr = (uint64_t)slot->req_header;
    bufs[num_bufs].size = sizeof(*slot->req_header);
    writable[num_bufs++] = false;

    if (total_size > 0) {
        bufs[num_bufs].addr = (uint64_t)data_addr;
        bufs[num_bufs].size = total_size;
        writable[num_bufs++] = (type == VIRTIO_BLK_T_IN);
    }

    bufs[num_bufs].addr = (uint64_t)slot->status_byte;
    bufs[num_bufs].size = sizeof(*slot->status_byte);
    writable[num_bufs++] = true;

    spin_lock(&blk_dev->request_lock);
    uint16_t desc_idx =
        virt_queue_add_buf(blk_dev->request_queue, bufs, num_bufs, writable);
    if (desc_idx == 0xFFFF) {
        slot->in_use = false;
        spin_unlock(&blk_dev->request_lock);
        return -1;
    }

    slot->desc_idx = desc_idx;
    blk_dev->pending_slot_by_desc[desc_idx] = (int16_t)(slot - blk_dev->slots);

    virt_queue_submit_buf(blk_dev->request_queue, desc_idx);
    virt_queue_notify(blk_dev->driver, blk_dev->request_queue);
    spin_unlock(&blk_dev->request_lock);

    if (virtio_blk_wait_slot(blk_dev, slot) != 0) {
        virtio_blk_release_slot(blk_dev, slot);
        return -1;
    }

    dma_sync_device_to_cpu(slot->status_byte, sizeof(*slot->status_byte));
    if (*slot->status_byte != VIRTIO_BLK_S_OK) {
        printk("virtio_blk: request type %u failed with status %u\n", type,
               *slot->status_byte);
        virtio_blk_release_slot(blk_dev, slot);
        return -1;
    }

    if (total_size > 0 && type == VIRTIO_BLK_T_IN) {
        void *src = slot->use_bounce ? slot->bounce_buffer : buffer;
        dma_sync_device_to_cpu(src, total_size);
        if (slot->use_bounce) {
            memcpy(buffer, src, total_size);
        }
    }

    virtio_blk_release_slot(blk_dev, slot);
    return (int)total_size;
}

static int virtio_blk_submit_flush(virtio_blk_device_t *blk_dev) {
    spin_lock(&blk_dev->request_lock);
    virtio_blk_req_slot_t *slot = virtio_blk_alloc_slot_locked(blk_dev);
    spin_unlock(&blk_dev->request_lock);
    if (!slot) {
        return -1;
    }

    slot->req_header->type = VIRTIO_BLK_T_FLUSH;
    slot->req_header->reserved = 0;
    slot->req_header->sector = 0;
    *slot->status_byte = 0xFF;
    slot->data_len = 0;
    slot->device_writes_data = false;
    slot->use_bounce = false;

    dma_sync_cpu_to_device(slot->req_header, sizeof(*slot->req_header));
    dma_sync_cpu_to_device(slot->status_byte, sizeof(*slot->status_byte));

    virtio_buffer_t bufs[2];
    bool writable[2] = {false, true};
    bufs[0].addr = (uint64_t)slot->req_header;
    bufs[0].size = sizeof(*slot->req_header);
    bufs[1].addr = (uint64_t)slot->status_byte;
    bufs[1].size = sizeof(*slot->status_byte);

    spin_lock(&blk_dev->request_lock);
    uint16_t desc_idx =
        virt_queue_add_buf(blk_dev->request_queue, bufs, 2, writable);
    if (desc_idx == 0xFFFF) {
        slot->in_use = false;
        spin_unlock(&blk_dev->request_lock);
        return -1;
    }

    slot->desc_idx = desc_idx;
    blk_dev->pending_slot_by_desc[desc_idx] = (int16_t)(slot - blk_dev->slots);
    virt_queue_submit_buf(blk_dev->request_queue, desc_idx);
    virt_queue_notify(blk_dev->driver, blk_dev->request_queue);
    spin_unlock(&blk_dev->request_lock);

    if (virtio_blk_wait_slot(blk_dev, slot) != 0) {
        virtio_blk_release_slot(blk_dev, slot);
        return -1;
    }

    dma_sync_device_to_cpu(slot->status_byte, sizeof(*slot->status_byte));
    if (*slot->status_byte != VIRTIO_BLK_S_OK) {
        printk("virtio_blk: Flush failed with status %u\n", *slot->status_byte);
        virtio_blk_release_slot(blk_dev, slot);
        return -1;
    }

    virtio_blk_release_slot(blk_dev, slot);
    return 0;
}

volatile uint64_t virtioblk_drive_id = 0;

virtio_blk_device_t *virtio_blk_devices[MAX_VIRTIO_BLKDEV_NUM];
int virtio_blk_idx = 0;

uint64_t virtio_read(void *data, uint64_t lba, void *buffer, uint64_t count) {
    virtio_blk_device_t *blk = data;
    uint64_t ret = virtio_blk_read(blk, lba, buffer, count);
    return ret / blk->block_size;
}

uint64_t virtio_write(void *data, uint64_t lba, void *buffer, uint64_t count) {
    virtio_blk_device_t *blk = data;
    uint64_t ret = virtio_blk_write(blk, lba, buffer, count);
    return ret / blk->block_size;
}

static int virtio_flush(void *data) { return virtio_blk_flush(data); }

int virtio_blk_init(virtio_driver_t *driver) {
    uint64_t supported_features = (1ULL << 5) | (1ULL << 9) |
                                  VIRTIO_F_RING_INDIRECT_DESC |
                                  VIRTIO_F_RING_EVENT_IDX | VIRTIO_F_VERSION_1;
    uint64_t features = virtio_begin_init(driver, supported_features);

    // Read block device configuration
    virtio_blk_config_t config;
    memset(&config, 0, sizeof(config));

    // Read configuration space
    for (uint32_t i = 0; i < sizeof(virtio_blk_config_t) / sizeof(uint32_t);
         i++) {
        uint32_t value =
            driver->op->read_config_space(driver->data, i * sizeof(uint32_t));
        memcpy((uint8_t *)&config + i * sizeof(uint32_t), &value,
               sizeof(uint32_t));
    }

    // Create request queue
    virtqueue_t *request_queue =
        virt_queue_new(driver, 0, !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                       !!(features & VIRTIO_F_RING_EVENT_IDX));
    if (!request_queue) {
        printk("virtio_blk: Failed to create request queue\n");
        return -1;
    }

    // Create block device structure
    virtio_blk_device_t *blk_device =
        (virtio_blk_device_t *)malloc(sizeof(virtio_blk_device_t));
    memset(blk_device, 0, sizeof(virtio_blk_device_t));

    blk_device->driver = driver;
    blk_device->capacity = config.capacity;
    blk_device->block_size =
        config.blk_size ? config.blk_size : 512; // Default to 512 bytes
    blk_device->request_queue = request_queue;
    blk_device->sector_size = 512; // Standard sector size
    blk_device->max_transfer_bytes = PAGE_SIZE * 32;
    blk_device->max_transfer_sectors =
        blk_device->max_transfer_bytes / blk_device->sector_size;
    blk_device->slot_count = request_queue->size / 3;
    if (blk_device->slot_count == 0) {
        blk_device->slot_count = 1;
    }
    spin_init(&blk_device->request_lock);
    wait_queue_init(&blk_device->request_wait);

    blk_device->slots =
        calloc(blk_device->slot_count, sizeof(virtio_blk_req_slot_t));
    if (!blk_device->slots) {
        free(blk_device);
        return -1;
    }

    for (uint16_t i = 0; i < SIZE; i++) {
        blk_device->pending_slot_by_desc[i] = -1;
    }

    if (driver->op->supports_interrupts &&
        driver->op->supports_interrupts(driver->data) &&
        driver->op->set_interrupt_handler) {
        driver->op->set_interrupt_handler(driver->data, virtio_blk_irq_handler,
                                          blk_device);
    }

    for (uint16_t i = 0; i < blk_device->slot_count; i++) {
        virtio_blk_req_slot_t *slot = &blk_device->slots[i];
        slot->req_header =
            (virtio_blk_req_t *)alloc_frames_bytes(sizeof(virtio_blk_req_t));
        slot->status_byte = (uint8_t *)alloc_frames_bytes(sizeof(uint8_t));
        slot->bounce_buffer =
            alloc_frames_bytes(blk_device->max_transfer_bytes);
        slot->bounce_capacity = blk_device->max_transfer_bytes;
        slot->desc_idx = 0xFFFF;
        if (!slot->req_header || !slot->status_byte || !slot->bounce_buffer) {
            printk("virtio_blk: Failed to preallocate request slot %u\n", i);
            for (uint16_t j = 0; j <= i; j++) {
                virtio_blk_req_slot_t *free_slot = &blk_device->slots[j];
                if (free_slot->req_header) {
                    free_frames_bytes(free_slot->req_header,
                                      sizeof(virtio_blk_req_t));
                }
                if (free_slot->status_byte) {
                    free_frames_bytes(free_slot->status_byte, sizeof(uint8_t));
                }
                if (free_slot->bounce_buffer) {
                    free_frames_bytes(free_slot->bounce_buffer,
                                      blk_device->max_transfer_bytes);
                }
            }
            free(blk_device->slots);
            free(blk_device);
            return -1;
        }
    }

    printk("virtio_blk: Found block device with capacity %llu sectors (%llu "
           "MB), block size: %u bytes\n",
           config.capacity,
           (config.capacity * blk_device->sector_size) / (1024 * 1024),
           blk_device->block_size);

    virtio_finish_init(driver);

    // Store device in global array
    if (virtio_blk_idx < MAX_BLKDEV_NUM) {
        virtio_blk_devices[virtio_blk_idx++] = blk_device;
    } else {
        printk("virtio_blk: Maximum number of block devices reached\n");
        free(blk_device);
        return -1;
    }

    char name[16];
    snprintf(name, sizeof(name), "virtioblk%d", virtioblk_drive_id++);

    regist_blkdev(name, blk_device, blk_device->block_size,
                  config.capacity * blk_device->sector_size, PAGE_SIZE * 32,
                  virtio_read, virtio_write, virtio_flush);

    return 0;
}

int virtio_blk_read(virtio_blk_device_t *blk_dev, uint64_t sector, void *buffer,
                    uint32_t count) {
    if (!blk_dev || !buffer || count == 0) {
        return -1;
    }

    uint32_t total_size = count * blk_dev->sector_size;
    if (total_size > blk_dev->max_transfer_bytes) {
        return -1;
    }
    return virtio_blk_submit_rw(blk_dev, VIRTIO_BLK_T_IN, sector, buffer,
                                total_size);
}

int virtio_blk_write(virtio_blk_device_t *blk_dev, uint64_t sector,
                     const void *buffer, uint32_t count) {
    if (!blk_dev || !buffer || count == 0) {
        return -1;
    }

    uint32_t total_size = count * blk_dev->sector_size;
    if (total_size > blk_dev->max_transfer_bytes) {
        return -1;
    }
    return virtio_blk_submit_rw(blk_dev, VIRTIO_BLK_T_OUT, sector,
                                (void *)buffer, total_size);
}

int virtio_blk_flush(virtio_blk_device_t *blk_dev) {
    if (!blk_dev) {
        return -1;
    }
    return virtio_blk_submit_flush(blk_dev);
}

virtio_blk_device_t *virtio_blk_get_device(uint32_t index) {
    if (index >= virtio_blk_idx) {
        return NULL;
    }
    return virtio_blk_devices[index];
}

uint32_t virtio_blk_get_device_count(void) { return virtio_blk_idx; }

static virtio_device_driver_t virtio_blk_driver = {
    .name = "virtio-blk",
    .device_type = VIRTIO_DEVICE_TYPE_BLOCK,
    .probe = virtio_blk_init,
    .remove = NULL,
    .shutdown = NULL,
};

int dlmain(void) { return virtio_register_device_driver(&virtio_blk_driver); }
