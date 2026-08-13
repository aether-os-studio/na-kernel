// Copyright (C) 2025-2026  lihanrui2913
#include "queue.h"

void queue_part_sizes(uint16_t queue_size, uint64_t *desc_size,
                      uint64_t *avail_size, uint64_t *used_size) {
    *desc_size = queue_size * sizeof(virtio_descriptor_t);
    *avail_size = sizeof(uint16_t) * (3 + queue_size);
    *used_size = sizeof(uint16_t) * 3 + sizeof(virtio_used_elem_t) * queue_size;
}

static uint64_t virt_queue_desc_bytes(const virtqueue_t *queue) {
    return (uint64_t)queue->size * sizeof(virtio_descriptor_t);
}

static uint64_t virt_queue_avail_bytes(const virtqueue_t *queue) {
    return sizeof(uint16_t) * (3 + queue->size);
}

static uint64_t virt_queue_used_bytes(const virtqueue_t *queue) {
    return sizeof(uint16_t) * 3 + sizeof(virtio_used_elem_t) * queue->size;
}

virtqueue_t *virt_queue_new(virtio_driver_t *driver, uint16_t queue_idx,
                            bool indirect, bool event_idx) {
    if (driver->op->queue_used(driver->data, queue_idx))
        return NULL;

    uint32_t max_queue_size =
        driver->op->get_max_queue_size(driver->data, queue_idx);
    uint16_t queue_size =
        max_queue_size < SIZE ? (uint16_t)max_queue_size : SIZE;
    if (queue_size == 0) {
        return NULL;
    }

    virtqueue_t *queue = malloc(sizeof(virtqueue_t));
    memset(queue, 0, sizeof(virtqueue_t));

    virtio_descriptor_t *desc = NULL;
    virtio_avail_ring_t *avail = NULL;
    virtio_used_ring_t *used = NULL;

    if (driver->op->requires_legacy_layout(driver->data)) {
        queue->is_modern = false;

        uint64_t desc_size, avail_size, used_size;
        queue_part_sizes(queue_size, &desc_size, &avail_size, &used_size);

        uint64_t size = PADDING_UP(desc_size + avail_size, PAGE_SIZE) +
                        PADDING_UP(used_size, PAGE_SIZE);

        queue->inner.legacy = malloc(sizeof(virtqueue_legacy_t));
        memset(queue->inner.legacy, 0, sizeof(virtqueue_legacy_t));
        queue->inner.legacy->paddr =
            virt_to_phys(alloc_frames_bytes(PADDING_UP(size, PAGE_SIZE)));
        queue->inner.legacy->avail_offset = desc_size;
        queue->inner.legacy->used_offset =
            PADDING_UP(desc_size + avail_size, PAGE_SIZE);

        driver->op->queue_set(driver->data, queue_idx, queue_size,
                              queue->inner.legacy->paddr, 0, 0);

        desc = (virtio_descriptor_t *)phys_to_virt(queue->inner.legacy->paddr);
        avail = (virtio_avail_ring_t *)phys_to_virt(
            (queue->inner.legacy->paddr + queue->inner.legacy->avail_offset));
        used = (virtio_used_ring_t *)phys_to_virt(
            (queue->inner.legacy->paddr + queue->inner.legacy->used_offset));
    } else {
        queue->is_modern = true;
        uint64_t desc_size, avail_size, used_size;
        queue_part_sizes(queue_size, &desc_size, &avail_size, &used_size);
        queue->inner.modern = malloc(sizeof(virtqueue_modern_t));
        memset(queue->inner.modern, 0, sizeof(virtqueue_modern_t));
        queue->inner.modern->driver_to_device_paddr = virt_to_phys(
            alloc_frames_bytes(PADDING_UP(desc_size + avail_size, PAGE_SIZE)));
        queue->inner.modern->driver_to_device_size =
            PADDING_UP(desc_size + avail_size, PAGE_SIZE);
        queue->inner.modern->device_to_driver_paddr =
            virt_to_phys(alloc_frames_bytes(PADDING_UP(used_size, PAGE_SIZE)));
        queue->inner.modern->device_to_driver_size =
            PADDING_UP(used_size, PAGE_SIZE);
        queue->inner.modern->avail_offset = desc_size;

        driver->op->queue_set(driver->data, queue_idx, queue_size,
                              queue->inner.modern->driver_to_device_paddr,
                              queue->inner.modern->driver_to_device_paddr +
                                  queue->inner.modern->avail_offset,
                              queue->inner.modern->device_to_driver_paddr);

        desc = (virtio_descriptor_t *)phys_to_virt(
            queue->inner.modern->driver_to_device_paddr);
        avail = (virtio_avail_ring_t *)phys_to_virt(
            (queue->inner.modern->driver_to_device_paddr +
             queue->inner.modern->avail_offset));
        used = (virtio_used_ring_t *)phys_to_virt(
            (queue->inner.modern->device_to_driver_paddr));
    }

    queue->desc = desc;
    queue->avail = avail;
    queue->used = used;

    memset(queue->desc, 0, queue_size * sizeof(virtio_descriptor_t));
    memset(queue->avail, 0, sizeof(uint16_t) * (3 + queue_size));
    memset(queue->used, 0,
           sizeof(uint16_t) * 3 + sizeof(virtio_used_elem_t) * queue_size);

    for (int i = 0; i < queue_size; i++) {
        if (i < queue_size - 1) {
            desc[i].next = i + 1;
        } else {
            desc[i].next = 0xFFFF;
        }
    }

    queue->queue_idx = queue_idx;
    queue->size = queue_size;

    queue->num_used = 0;
    queue->free_head = 0;

    queue->event_idx = event_idx;

    queue->avail_idx = 0;
    queue->last_used_idx = 0;

    dma_sync_cpu_to_device(queue->desc, virt_queue_desc_bytes(queue));
    dma_sync_cpu_to_device(queue->avail, virt_queue_avail_bytes(queue));
    dma_sync_cpu_to_device(queue->used, virt_queue_used_bytes(queue));

    return queue;
}

void virt_queue_set_dev_notify(virtqueue_t *queue, bool enable) {
    uint16_t avail_ring_flags = enable ? 0x0000 : 0x0001;
    if (!queue->event_idx) {
        queue->avail->flags = avail_ring_flags;
        dma_sync_cpu_to_device(queue->avail, virt_queue_avail_bytes(queue));
    }
}

bool virt_queue_should_notify(virtqueue_t *queue) {
    if (!queue) {
        return true;
    }

    dma_sync_device_to_cpu(queue->used, virt_queue_used_bytes(queue));
    if (queue->event_idx) {
        return true;
    }
    return (queue->used->flags & 0x0001) == 0;
}

bool virt_queue_can_pop(virtqueue_t *queue) {
    dma_sync_device_to_cpu(queue->used, virt_queue_used_bytes(queue));
    return queue->last_used_idx != queue->used->index;
}

uint16_t virt_queue_count_free_desc(virtqueue_t *queue) {
    if (!queue) {
        return 0;
    }

    uint16_t count = 0;
    uint16_t idx = queue->free_head;
    while (idx != 0xFFFF && count < queue->size) {
        count++;
        idx = queue->desc[idx].next;
    }
    return count;
}

uint16_t virt_queue_get_free_desc(virtqueue_t *queue) {
    if (queue->free_head == 0xFFFF) {
        return 0xFFFF; // No free descriptors
    }

    uint16_t desc_idx = queue->free_head;
    queue->free_head = queue->desc[desc_idx].next;

    return desc_idx;
}

void virt_queue_free_desc(virtqueue_t *queue, uint16_t desc_idx) {
    uint16_t current_idx = desc_idx;
    uint16_t chain[SIZE];
    uint16_t chain_len = 0;

    while (current_idx != 0xFFFF && chain_len < queue->size) {
        uint16_t next_idx = (queue->desc[current_idx].flags & DESC_FLAGS_NEXT)
                                ? queue->desc[current_idx].next
                                : 0xFFFF;
        chain[chain_len++] = current_idx;
        current_idx = next_idx;
    }

    if (chain_len == 0) {
        return;
    }

    for (uint16_t i = 0; i < chain_len; i++) {
        uint16_t idx = chain[i];
        queue->desc[idx].flags = 0;
        queue->desc[idx].len = 0;
        queue->desc[idx].addr = 0;
        if (i + 1 < chain_len) {
            queue->desc[idx].next = chain[i + 1];
        } else {
            queue->desc[idx].next = queue->free_head;
        }
    }

    queue->free_head = chain[0];
    dma_sync_cpu_to_device(queue->desc, virt_queue_desc_bytes(queue));
}

uint16_t virt_queue_add_buf(virtqueue_t *queue, virtio_buffer_t *bufs,
                            uint16_t num_bufs, bool *device_writable) {
    if (num_bufs == 0 || queue->free_head == 0xFFFF ||
        (num_bufs > 1 && virt_queue_count_free_desc(queue) < num_bufs)) {
        return 0xFFFF;
    }

    uint16_t head_idx = queue->free_head;
    uint16_t current_idx = head_idx;
    uint16_t next_idx;
    uint16_t reserved[SIZE];
    uint16_t reserved_count = 0;

    for (uint16_t i = 0; i < num_bufs; i++) {
        if (current_idx == 0xFFFF) {
            for (uint16_t j = 0; j < reserved_count; j++) {
                uint16_t idx = reserved[j];
                queue->desc[idx].flags = 0;
                queue->desc[idx].len = 0;
                queue->desc[idx].addr = 0;
                queue->desc[idx].next =
                    (j + 1 < reserved_count) ? reserved[j + 1] : 0xFFFF;
            }
            queue->free_head = head_idx;
            dma_sync_cpu_to_device(queue->desc, virt_queue_desc_bytes(queue));
            return 0xFFFF;
        }

        next_idx = queue->desc[current_idx].next;
        reserved[reserved_count++] = current_idx;

        virtio_descriptor_set_buf(
            &queue->desc[current_idx], (void *)bufs[i].addr, bufs[i].size,
            device_writable[i] ? VIRTIO_DESC_BUFFER_DIR_DEVICE_TO_DRIVER
                               : VIRTIO_DESC_BUFFER_DIR_DRIVER_TO_DEVICE,
            i < num_bufs - 1 ? DESC_FLAGS_NEXT : 0);

        if (i < num_bufs - 1) {
            queue->desc[current_idx].next = next_idx;
        } else {
            queue->desc[current_idx].next = 0xFFFF;
        }

        current_idx = next_idx;
    }

    queue->free_head = current_idx;
    dma_sync_cpu_to_device(queue->desc, virt_queue_desc_bytes(queue));

    return head_idx;
}

void virt_queue_submit_buf(virtqueue_t *queue, uint16_t desc_idx) {
    queue->avail->ring[queue->avail_idx % queue->size] = desc_idx;
    queue->avail_idx++;
    dma_wmb();
    queue->avail->index = queue->avail_idx;
    dma_sync_cpu_to_device(queue->avail, virt_queue_avail_bytes(queue));
}

uint16_t virt_queue_get_used_buf(virtqueue_t *queue, uint32_t *len) {
    dma_sync_device_to_cpu(queue->used, virt_queue_used_bytes(queue));
    if (queue->last_used_idx == queue->used->index) {
        return 0xFFFF; // No used buffers
    }

    virtio_used_elem_t *used_elem =
        &queue->used->ring[queue->last_used_idx % queue->size];
    uint16_t desc_idx = used_elem->id;
    if (len) {
        *len = used_elem->len;
    }

    queue->last_used_idx++;
    return desc_idx;
}

uint16_t virt_queue_size(const virtqueue_t *queue) {
    return queue ? queue->size : 0;
}

void virt_queue_notify(virtio_driver_t *driver, virtqueue_t *queue) {
    if (!driver || !queue) {
        return;
    }

    driver->op->notify(driver->data, queue->queue_idx);
}
