// Copyright (C) 2025-2026  lihanrui2913
#include "net.h"
#include <mm/mm.h>

virtio_net_device_t *virtio_net_devices[MAX_NETDEV_NUM];
int virtio_net_idx = 0;

#define RX_BUFFER_SIZE 8192
#define RX_BUFFER_COUNT SIZE
#define TX_BUFFER_SIZE 8192

static void virtio_net_reap_tx(virtio_net_device_t *net_dev) {
    uint32_t used_len = 0;
    uint16_t used_desc_idx = 0;

    while ((used_desc_idx = virt_queue_get_used_buf(net_dev->send_queue,
                                                    &used_len)) != 0xFFFF) {
        if (used_desc_idx < SIZE && net_dev->tx_buffers[used_desc_idx]) {
            net_dev->tx_buffer_sizes[used_desc_idx] = 0;
        }
        virt_queue_free_desc(net_dev->send_queue, used_desc_idx);
    }
}

static void virtio_net_irq_handler(void *opaque, uint8_t isr_status) {
    virtio_net_device_t *net_dev = (virtio_net_device_t *)opaque;

    if (!net_dev || !(isr_status & 0x1))
        return;
    if (net_dev->netdev && virtio_net_has_packets(net_dev))
        netdev_notify_rx(net_dev->netdev);
}

int virtio_net_init(virtio_driver_t *driver) {
    uint64_t features = virtio_begin_init(
        driver, VIRTIO_NET_F_MTU | VIRTIO_NET_F_MAC | VIRTIO_NET_F_MRG_RXBUF |
                    VIRTIO_NET_F_STATUS | VIRTIO_F_RING_INDIRECT_DESC |
                    VIRTIO_F_RING_EVENT_IDX | VIRTIO_F_VERSION_1);

    uint32_t mac_low = driver->op->read_config_space(
        driver->data, offsetof(virtio_net_config_t, mac));
    uint32_t mac_high_and_status = driver->op->read_config_space(
        driver->data, offsetof(virtio_net_config_t, mac) + sizeof(uint32_t));
    uint32_t max_virtqueue_pairs_and_mtu = driver->op->read_config_space(
        driver->data, offsetof(virtio_net_config_t, max_virtqueue_pairs));

    uint8_t mac[6];
    mac[0] = mac_low & 0xFF;
    mac[1] = (mac_low >> 8) & 0xFF;
    mac[2] = (mac_low >> 16) & 0xFF;
    mac[3] = (mac_low >> 24) & 0xFF;
    mac[4] = mac_high_and_status & 0xFF;
    mac[5] = (mac_high_and_status >> 8) & 0xFF;

    uint16_t status = mac_high_and_status >> 16;

    uint16_t max_virtqueue_pairs = max_virtqueue_pairs_and_mtu & 0xFFFF;
    uint16_t mtu = VIRTIO_NET_DEFAULT_MTU;

    if (features & VIRTIO_NET_F_MTU) {
        uint16_t negotiated_mtu = (max_virtqueue_pairs_and_mtu >> 16) & 0xFFFF;
        if (negotiated_mtu != 0) {
            mtu = negotiated_mtu;
        }
    }

    printk("virtio_net: Got mac address: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    virtqueue_t *recv_queue =
        virt_queue_new(driver, 0, !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                       !!(features & VIRTIO_F_RING_EVENT_IDX));
    virtqueue_t *send_queue =
        virt_queue_new(driver, 1, !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                       !!(features & VIRTIO_F_RING_EVENT_IDX));

    virtio_net_device_t *net_device =
        (virtio_net_device_t *)malloc(sizeof(virtio_net_device_t));
    memset(net_device, 0, sizeof(virtio_net_device_t));

    net_device->driver = driver;
    memcpy(net_device->mac, mac, 6);
    net_device->mtu = mtu;
    net_device->net_hdr_size =
        (!driver->op->requires_legacy_layout(driver->data) &&
         (features & VIRTIO_F_VERSION_1)) ||
                (features & VIRTIO_NET_F_MRG_RXBUF)
            ? sizeof(virtio_net_hdr_v1_t)
            : sizeof(virtio_net_hdr_t);
    net_device->send_queue = send_queue;
    net_device->recv_queue = recv_queue;
    spin_init(&net_device->tx_lock);
    spin_init(&net_device->rx_lock);
    for (int i = 0; i < SIZE; i++) {
        net_device->tx_buffers[i] = alloc_frames_bytes(TX_BUFFER_SIZE);
        if (!net_device->tx_buffers[i])
            return -ENOMEM;
    }

    if (driver->op->supports_interrupts &&
        driver->op->supports_interrupts(driver->data) &&
        driver->op->set_interrupt_handler) {
        driver->op->set_interrupt_handler(driver->data, virtio_net_irq_handler,
                                          net_device);
    }

    // Pre-allocate and populate receive buffers for polling mode
    for (int i = 0; i < RX_BUFFER_COUNT; i++) {
        void *rx_buffer = alloc_frames_bytes(RX_BUFFER_SIZE);
        if (!rx_buffer) {
            continue;
        }

        // Add receive buffer to receive queue
        virtio_buffer_t buf = {.addr = (uint64_t)rx_buffer,
                               .size = RX_BUFFER_SIZE};
        bool writable = true;
        dma_sync_cpu_to_device(rx_buffer, RX_BUFFER_SIZE);
        uint16_t desc_idx = virt_queue_add_buf(recv_queue, &buf, 1, &writable);
        if (desc_idx != 0xFFFF) {
            net_device->rx_buffers[desc_idx] = rx_buffer;
            virt_queue_submit_buf(recv_queue, desc_idx);
        } else {
            free_frames_bytes(rx_buffer, RX_BUFFER_SIZE);
        }
    }

    // Notify device about the receive buffers
    virt_queue_notify(driver, recv_queue);

    virtio_finish_init(driver);

    net_device->netdev = netdev_register_full(
        NULL, NETDEV_TYPE_ETHERNET, net_device, net_device->mac,
        net_device->mtu, (netdev_send_t)virtio_net_send,
        (netdev_recv_t)virtio_net_receive,
        (netdev_poll_rx_t)virtio_net_has_packets);

    if (!net_device->netdev) {
        printk("virtio_net: Failed to register netdev\n");
        return -ENOMEM;
    }

    virtio_net_devices[virtio_net_idx++] = net_device;

    return 0;
}

int virtio_net_send(virtio_net_device_t *net_dev, void *data, uint32_t len) {
    if (!net_dev || !data || len == 0 ||
        len > netdev_max_frame_len(net_dev->mtu)) {
        return -1;
    }

    spin_lock(&net_dev->tx_lock);
    virtio_net_reap_tx(net_dev);

    uint16_t next_desc = net_dev->send_queue->free_head;
    if (next_desc == 0xFFFF || next_desc >= SIZE ||
        !net_dev->tx_buffers[next_desc]) {
        spin_unlock(&net_dev->tx_lock);
        return -1;
    }

    uint32_t total_len = net_dev->net_hdr_size + len;
    if (total_len > TX_BUFFER_SIZE) {
        spin_unlock(&net_dev->tx_lock);
        return -1;
    }
    void *send_buffer = net_dev->tx_buffers[next_desc];
    memset(send_buffer, 0, net_dev->net_hdr_size);
    memcpy((uint8_t *)send_buffer + net_dev->net_hdr_size, data, len);
    dma_sync_cpu_to_device(send_buffer, total_len);

    virtio_buffer_t buf = {.addr = (uint64_t)send_buffer, .size = total_len};
    bool writable = false;
    uint16_t desc_idx =
        virt_queue_add_buf(net_dev->send_queue, &buf, 1, &writable);
    if (desc_idx == 0xFFFF) {
        spin_unlock(&net_dev->tx_lock);
        return -1;
    }

    if (desc_idx != next_desc) {
        virt_queue_free_desc(net_dev->send_queue, desc_idx);
        spin_unlock(&net_dev->tx_lock);
        return -1;
    }
    net_dev->tx_buffer_sizes[desc_idx] = total_len;

    virt_queue_submit_buf(net_dev->send_queue, desc_idx);
    virt_queue_notify(net_dev->driver, net_dev->send_queue);

    spin_unlock(&net_dev->tx_lock);

    return len;
}

int virtio_net_receive(virtio_net_device_t *net_dev, void *buffer,
                       uint32_t buffer_size) {
    if (!net_dev || !buffer || buffer_size == 0) {
        return -1;
    }

    spin_lock(&net_dev->rx_lock);

    uint32_t len;
    uint16_t desc_idx = virt_queue_get_used_buf(net_dev->recv_queue, &len);
    if (desc_idx == 0xFFFF) {
        spin_unlock(&net_dev->rx_lock);
        return 0; // No packets available
    }

    void *rx_data = net_dev->rx_buffers[desc_idx];
    if (!rx_data) {
        virtio_descriptor_t *desc = &net_dev->recv_queue->desc[desc_idx];
        rx_data = phys_to_virt(desc->addr);
    }
    dma_sync_device_to_cpu(rx_data, len);

    if (len <= net_dev->net_hdr_size) {
        net_dev->rx_buffers[desc_idx] = NULL;
        virt_queue_free_desc(net_dev->recv_queue, desc_idx);

        virtio_buffer_t buf = {.addr = (uint64_t)rx_data,
                               .size = RX_BUFFER_SIZE};
        bool writable = true;
        dma_sync_cpu_to_device(rx_data, RX_BUFFER_SIZE);
        uint16_t new_desc_idx =
            virt_queue_add_buf(net_dev->recv_queue, &buf, 1, &writable);
        if (new_desc_idx != 0xFFFF) {
            net_dev->rx_buffers[new_desc_idx] = rx_data;
            virt_queue_submit_buf(net_dev->recv_queue, new_desc_idx);
            virt_queue_notify(net_dev->driver, net_dev->recv_queue);
        }
        spin_unlock(&net_dev->rx_lock);
        return 0;
    }

    uint32_t data_len = len - net_dev->net_hdr_size;

    if (data_len > buffer_size) {
        data_len = buffer_size;
    }

    memcpy(buffer, (uint8_t *)rx_data + net_dev->net_hdr_size, data_len);

    net_dev->rx_buffers[desc_idx] = NULL;
    virt_queue_free_desc(net_dev->recv_queue, desc_idx);

    virtio_buffer_t buf = {.addr = (uint64_t)rx_data, .size = RX_BUFFER_SIZE};
    bool writable = true;
    dma_sync_cpu_to_device(rx_data, RX_BUFFER_SIZE);
    uint16_t new_desc_idx =
        virt_queue_add_buf(net_dev->recv_queue, &buf, 1, &writable);
    if (new_desc_idx != 0xFFFF) {
        net_dev->rx_buffers[new_desc_idx] = rx_data;
        virt_queue_submit_buf(net_dev->recv_queue, new_desc_idx);
        virt_queue_notify(net_dev->driver, net_dev->recv_queue);
    }

    spin_unlock(&net_dev->rx_lock);
    return data_len;
}

bool virtio_net_has_packets(virtio_net_device_t *net_dev) {
    bool ready;

    if (!net_dev) {
        return false;
    }

    spin_lock(&net_dev->rx_lock);
    ready = virt_queue_can_pop(net_dev->recv_queue);
    spin_unlock(&net_dev->rx_lock);
    return ready;
}

virtio_net_device_t *virtio_net_get_device(uint32_t index) {
    if (index >= virtio_net_idx) {
        return NULL;
    }
    return virtio_net_devices[index];
}

uint32_t virtio_net_get_device_count(void) { return virtio_net_idx; }

static virtio_device_driver_t virtio_net_driver = {
    .name = "virtio-net",
    .device_type = VIRTIO_DEVICE_TYPE_NETWORK,
    .probe = virtio_net_init,
    .remove = NULL,
    .shutdown = NULL,
};

int dlmain(void) { return virtio_register_device_driver(&virtio_net_driver); }
