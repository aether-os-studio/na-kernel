// Copyright (C) 2025-2026  lihanrui2913
#include "net.h"
#include <mm/mm.h>

virtio_net_device_t *virtio_net_devices[MAX_NETDEV_NUM];
int virtio_net_idx = 0;

/* virtio_net_hdr + Ethernet/IP/TCP headers + a full GSO/TSO segment. Large
 * enough that a single buffer can hold a GRO-coalesced receive segment. */
#define RX_BUFFER_SIZE (NETDEV_GSO_MAX_SIZE + 512)
#define TX_BUFFER_SIZE (NETDEV_GSO_MAX_SIZE + 512)

/* RX buffers per queue: keep the total across all queues roughly constant so
 * multiqueue does not blow up memory usage. */
static uint16_t virtio_net_rx_buffer_count(uint16_t num_pairs) {
    uint16_t n = (uint16_t)(SIZE / num_pairs);
    if (n < 8)
        n = 8;
    return n;
}

static void virtio_net_reap_tx(virtio_net_tx_queue_t *txq) {
    uint32_t used_len = 0;
    uint16_t used_desc_idx = 0;

    while ((used_desc_idx = virt_queue_get_used_buf(txq->queue, &used_len)) !=
           0xFFFF) {
        if (used_desc_idx < SIZE && txq->buffers[used_desc_idx]) {
            txq->buffer_sizes[used_desc_idx] = 0;
        }
        virt_queue_free_desc(txq->queue, used_desc_idx);
    }
}

static void virtio_net_irq_handler(void *opaque, uint8_t isr_status) {
    virtio_net_device_t *net_dev = (virtio_net_device_t *)opaque;
    uint16_t q;

    if (!net_dev || !(isr_status & 0x1) || !net_dev->netdev)
        return;

    for (q = 0; q < net_dev->num_rx_queues; q++) {
        if (virtio_net_has_packets_q(net_dev, q)) {
            netdev_notify_rx(net_dev->netdev);
            return;
        }
    }
}

static int virtio_net_populate_rx(virtio_net_rx_queue_t *rxq, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        void *rx_buffer = alloc_frames_bytes(RX_BUFFER_SIZE);
        if (!rx_buffer)
            return -ENOMEM;

        virtio_buffer_t buf = {.addr = (uint64_t)rx_buffer,
                               .size = RX_BUFFER_SIZE};
        bool writable = true;
        dma_sync_cpu_to_device(rx_buffer, RX_BUFFER_SIZE);
        uint16_t desc_idx = virt_queue_add_buf(rxq->queue, &buf, 1, &writable);
        if (desc_idx != 0xFFFF) {
            rxq->buffers[desc_idx] = rx_buffer;
            virt_queue_submit_buf(rxq->queue, desc_idx);
        } else {
            free_frames_bytes(rx_buffer, RX_BUFFER_SIZE);
            return -ENOMEM;
        }
    }
    return 0;
}

int virtio_net_init(virtio_driver_t *driver) {
    uint64_t features = virtio_begin_init(
        driver, VIRTIO_NET_F_MTU | VIRTIO_NET_F_MAC | VIRTIO_NET_F_MRG_RXBUF |
                    VIRTIO_NET_F_STATUS | VIRTIO_NET_F_CSUM |
                    VIRTIO_NET_F_GUEST_CSUM | VIRTIO_NET_F_HOST_TSO4 |
                    VIRTIO_NET_F_GUEST_TSO4 | VIRTIO_NET_F_MQ |
                    VIRTIO_F_RING_INDIRECT_DESC | VIRTIO_F_RING_EVENT_IDX |
                    VIRTIO_F_VERSION_1);

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

    if (!(features & VIRTIO_NET_F_MQ) || max_virtqueue_pairs < 1)
        max_virtqueue_pairs = 1;
    if (max_virtqueue_pairs > VIRTIO_NET_MAX_QUEUE_PAIRS)
        max_virtqueue_pairs = VIRTIO_NET_MAX_QUEUE_PAIRS;

    printk("virtio_net: Got mac address: %02x:%02x:%02x:%02x:%02x:%02x, %u rx "
           "queues\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], max_virtqueue_pairs);

    virtio_net_device_t *net_device =
        (virtio_net_device_t *)malloc(sizeof(virtio_net_device_t));
    memset(net_device, 0, sizeof(virtio_net_device_t));

    net_device->driver = driver;
    memcpy(net_device->mac, mac, 6);
    net_device->mtu = mtu;
    net_device->features = features;
    net_device->num_rx_queues = 0;
    net_device->net_hdr_size =
        (!driver->op->requires_legacy_layout(driver->data) &&
         (features & VIRTIO_F_VERSION_1)) ||
                (features & VIRTIO_NET_F_MRG_RXBUF)
            ? sizeof(virtio_net_hdr_v1_t)
            : sizeof(virtio_net_hdr_t);

    bool indirect = !!(features & VIRTIO_F_RING_INDIRECT_DESC);
    bool event_idx = !!(features & VIRTIO_F_RING_EVENT_IDX);

    uint16_t nq = 0;
    for (uint16_t i = 0; i < max_virtqueue_pairs; i++) {
        virtio_net_rx_queue_t *rxq = &net_device->rx_queues[i];
        virtio_net_tx_queue_t *txq = &net_device->tx_queues[i];

        rxq->queue =
            virt_queue_new(driver, (uint16_t)(2 * i), indirect, event_idx);
        txq->queue =
            virt_queue_new(driver, (uint16_t)(2 * i + 1), indirect, event_idx);
        if (!rxq->queue || !txq->queue) {
            printk("virtio_net: queue pair %u unavailable, using %u\n", i, i);
            break;
        }
        spin_init(&rxq->lock);
        spin_init(&txq->lock);
        nq = (uint16_t)(i + 1);
    }

    if (nq == 0) {
        printk("virtio_net: cannot create any queue pair\n");
        return -ENOMEM;
    }

    for (int j = 0; j < SIZE; j++) {
        net_device->tx_queues[0].buffers[j] =
            alloc_frames_bytes(TX_BUFFER_SIZE);
        if (!net_device->tx_queues[0].buffers[j]) {
            printk("virtio_net: tx buffer %d alloc failed\n", j);
            return -ENOMEM;
        }
    }

    uint16_t rx_count = virtio_net_rx_buffer_count(nq);
    for (uint16_t i = 0; i < nq; i++) {
        if (virtio_net_populate_rx(&net_device->rx_queues[i], rx_count) != 0) {
            printk("virtio_net: rx queue %u buffer alloc failed, using %u\n", i,
                   i);
            nq = i;
            break;
        }
        virt_queue_notify(driver, net_device->rx_queues[i].queue);
    }
    if (nq == 0) {
        printk("virtio_net: rx buffer allocation failed\n");
        return -ENOMEM;
    }

    net_device->num_rx_queues = nq;
    if (nq != max_virtqueue_pairs) {
        printk("virtio_net: using %u of %u queue pairs\n", nq,
               max_virtqueue_pairs);
    }

    if (driver->op->supports_interrupts &&
        driver->op->supports_interrupts(driver->data) &&
        driver->op->set_interrupt_handler) {
        driver->op->set_interrupt_handler(driver->data, virtio_net_irq_handler,
                                          net_device);
    }

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
    netdev_set_sendv(net_device->netdev, (netdev_sendv_t)virtio_net_sendv);
    netdev_set_recv_q(net_device->netdev,
                      (netdev_recv_q_t)virtio_net_receive_q);
    netdev_set_poll_rx_q(net_device->netdev,
                         (netdev_poll_rx_q_t)virtio_net_has_packets_q);
    netdev_set_rx_queue_count(net_device->netdev, net_device->num_rx_queues);

    {
        uint32_t caps = 0;
        if (features & VIRTIO_NET_F_CSUM)
            caps |= NETDEV_CAP_TX_CSUM;
        if (features & VIRTIO_NET_F_GUEST_CSUM)
            caps |= NETDEV_CAP_RX_CSUM;
        if (features & VIRTIO_NET_F_HOST_TSO4)
            caps |= NETDEV_CAP_TSO4;
        if (features & VIRTIO_NET_F_HOST_TSO6)
            caps |= NETDEV_CAP_TSO6;
        if (features & VIRTIO_NET_F_GUEST_TSO4)
            caps |= NETDEV_CAP_GRO;
        if (caps & (NETDEV_CAP_TSO4 | NETDEV_CAP_TSO6)) {
            netdev_set_send_gso(net_device->netdev,
                                (netdev_send_gso_t)virtio_net_send_gso);
        }
        netdev_set_caps(net_device->netdev, caps);
    }

    virtio_net_devices[virtio_net_idx++] = net_device;

    return 0;
}

static void virtio_net_fill_hdr(virtio_net_device_t *net_dev, void *hdr,
                                const void *frame, uint32_t frame_len,
                                const netdev_gso_info_t *gso) {
    virtio_net_hdr_v1_t *vh = (virtio_net_hdr_v1_t *)hdr;
    const uint8_t *f = (const uint8_t *)frame;

    if (frame_len < 14 + 20) {
        return;
    }

    uint16_t eth_type = ((uint16_t)f[12] << 8) | f[13];
    if (eth_type != 0x0800) {
        return; /* IPv4 only */
    }

    const uint8_t *ip = f + 14;
    uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || frame_len < (uint32_t)(14 + ihl + 20)) {
        return;
    }
    uint8_t proto = ip[9];
    bool is_tcp = (proto == 6);
    bool is_udp = (proto == 17);

    if (gso && gso->type == NETDEV_GSO_TCPV4 && is_tcp) {
        /* Segmentation offload: the device splits the frame into mss-sized
         * segments and computes each segment's checksum itself. */
        uint8_t tcp_hl = (f[14 + ihl + 12] >> 4) * 4;
        vh->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
        vh->gso_size = gso->mss;
        vh->hdr_len = 14 + ihl + tcp_hl;
        return;
    }

    if ((net_dev->features & VIRTIO_NET_F_CSUM) && (is_tcp || is_udp)) {
        /* Checksum offload: the device computes the L4 checksum. */
        vh->flags |= VIRTIO_NET_HDR_F_NEEDS_CSUM;
        vh->csum_start = 14 + ihl;
        vh->csum_offset = is_tcp ? 16 : 6;
    }
}

static int virtio_net_xmit(virtio_net_device_t *net_dev,
                           const netdev_iovec_t *iov, uint32_t iovcnt,
                           uint32_t total_len, const netdev_gso_info_t *gso) {
    uint32_t copied = 0;
    virtio_net_tx_queue_t *txq = &net_dev->tx_queues[0];

    if (!net_dev || !iov || !iovcnt || !total_len) {
        return -1;
    }

    if (!gso && total_len > netdev_max_frame_len(net_dev->mtu)) {
        return -1;
    }

    spin_lock(&txq->lock);
    virtio_net_reap_tx(txq);

    uint16_t next_desc = txq->queue->free_head;
    if (next_desc == 0xFFFF || next_desc >= SIZE || !txq->buffers[next_desc]) {
        spin_unlock(&txq->lock);
        return -EAGAIN;
    }

    uint32_t dma_len = net_dev->net_hdr_size + total_len;
    if (dma_len > TX_BUFFER_SIZE) {
        spin_unlock(&txq->lock);
        return -1;
    }
    void *send_buffer = txq->buffers[next_desc];
    memset(send_buffer, 0, net_dev->net_hdr_size);
    for (uint32_t i = 0; i < iovcnt; i++) {
        if (!iov[i].data || iov[i].len > total_len - copied) {
            spin_unlock(&txq->lock);
            return -1;
        }
        memcpy((uint8_t *)send_buffer + net_dev->net_hdr_size + copied,
               iov[i].data, iov[i].len);
        copied += iov[i].len;
    }
    if (copied != total_len) {
        spin_unlock(&txq->lock);
        return -1;
    }

    virtio_net_fill_hdr(net_dev, send_buffer,
                        (uint8_t *)send_buffer + net_dev->net_hdr_size,
                        total_len, gso);

    dma_sync_cpu_to_device(send_buffer, dma_len);

    virtio_buffer_t buf = {.addr = (uint64_t)send_buffer, .size = dma_len};
    bool writable = false;
    uint16_t desc_idx = virt_queue_add_buf(txq->queue, &buf, 1, &writable);
    if (desc_idx == 0xFFFF) {
        spin_unlock(&txq->lock);
        return -EAGAIN;
    }

    if (desc_idx != next_desc) {
        virt_queue_free_desc(txq->queue, desc_idx);
        spin_unlock(&txq->lock);
        return -EAGAIN;
    }
    txq->buffer_sizes[desc_idx] = dma_len;

    virt_queue_submit_buf(txq->queue, desc_idx);
    virt_queue_notify(net_dev->driver, txq->queue);

    spin_unlock(&txq->lock);

    return total_len;
}

int virtio_net_sendv(virtio_net_device_t *net_dev, const netdev_iovec_t *iov,
                     uint32_t iovcnt, uint32_t total_len) {
    return virtio_net_xmit(net_dev, iov, iovcnt, total_len, NULL);
}

int virtio_net_send_gso(virtio_net_device_t *net_dev, const netdev_iovec_t *iov,
                        uint32_t iovcnt, uint32_t total_len,
                        const netdev_gso_info_t *gso) {
    if (!gso || gso->type == NETDEV_GSO_NONE) {
        return -1;
    }
    return virtio_net_xmit(net_dev, iov, iovcnt, total_len, gso);
}

int virtio_net_send(virtio_net_device_t *net_dev, void *data, uint32_t len) {
    netdev_iovec_t iov = {.data = data, .len = len};

    return virtio_net_sendv(net_dev, &iov, 1, len);
}

static void virtio_net_refill_rx(virtio_net_device_t *net_dev,
                                 virtio_net_rx_queue_t *rxq,
                                 uint16_t used_desc_idx) {
    void *rx_data = rxq->buffers[used_desc_idx];

    if (!rx_data) {
        virtio_descriptor_t *desc = &rxq->queue->desc[used_desc_idx];
        rx_data = phys_to_virt(desc->addr);
    }

    rxq->buffers[used_desc_idx] = NULL;
    virt_queue_free_desc(rxq->queue, used_desc_idx);

    virtio_buffer_t buf = {.addr = (uint64_t)rx_data, .size = RX_BUFFER_SIZE};
    bool writable = true;
    dma_sync_cpu_to_device(rx_data, RX_BUFFER_SIZE);
    uint16_t new_desc_idx = virt_queue_add_buf(rxq->queue, &buf, 1, &writable);
    if (new_desc_idx != 0xFFFF) {
        rxq->buffers[new_desc_idx] = rx_data;
        virt_queue_submit_buf(rxq->queue, new_desc_idx);
        virt_queue_notify(net_dev->driver, rxq->queue);
    }
}

int virtio_net_receive_q(virtio_net_device_t *net_dev, uint16_t qidx,
                         void *buffer, uint32_t buffer_size) {
    if (!net_dev || !buffer || buffer_size == 0 ||
        qidx >= net_dev->num_rx_queues) {
        return -1;
    }

    virtio_net_rx_queue_t *rxq = &net_dev->rx_queues[qidx];

    spin_lock(&rxq->lock);

    uint32_t len;
    uint16_t desc_idx = virt_queue_get_used_buf(rxq->queue, &len);
    if (desc_idx == 0xFFFF) {
        spin_unlock(&rxq->lock);
        return 0; // No packets available
    }

    void *rx_data = rxq->buffers[desc_idx];
    if (!rx_data) {
        virtio_descriptor_t *desc = &rxq->queue->desc[desc_idx];
        rx_data = phys_to_virt(desc->addr);
    }
    dma_sync_device_to_cpu(rx_data, len);

    if (len <= net_dev->net_hdr_size) {
        virtio_net_refill_rx(net_dev, rxq, desc_idx);
        spin_unlock(&rxq->lock);
        return 0;
    }

    uint32_t data_len = len - net_dev->net_hdr_size;

    if (data_len > buffer_size) {
        data_len = buffer_size;
    }

    memcpy(buffer, (uint8_t *)rx_data + net_dev->net_hdr_size, data_len);

    virtio_net_refill_rx(net_dev, rxq, desc_idx);
    spin_unlock(&rxq->lock);
    return (int)data_len;
}

int virtio_net_receive(virtio_net_device_t *net_dev, void *buffer,
                       uint32_t buffer_size) {
    return virtio_net_receive_q(net_dev, 0, buffer, buffer_size);
}

bool virtio_net_has_packets_q(virtio_net_device_t *net_dev, uint16_t qidx) {
    bool ready;

    if (!net_dev || qidx >= net_dev->num_rx_queues) {
        return false;
    }

    virtio_net_rx_queue_t *rxq = &net_dev->rx_queues[qidx];
    spin_lock(&rxq->lock);
    ready = virt_queue_can_pop(rxq->queue);
    spin_unlock(&rxq->lock);
    return ready;
}

bool virtio_net_has_packets(virtio_net_device_t *net_dev) {
    return virtio_net_has_packets_q(net_dev, 0);
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
