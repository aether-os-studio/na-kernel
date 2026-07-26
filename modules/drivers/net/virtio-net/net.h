#pragma once

#include <drivers/virtio/queue.h>
#include <drivers/virtio/virtio.h>
#include <net/netdev.h>

typedef struct virtio_net_device {
    virtio_driver_t *driver;
    uint8_t mac[6];
    uint16_t mtu;
    uint16_t net_hdr_size;
    virtqueue_t *send_queue;
    virtqueue_t *recv_queue;
    void *rx_buffers[64];
    void *tx_buffers[64];
    uint32_t tx_buffer_sizes[64];
    spinlock_t tx_lock;
    spinlock_t rx_lock;
    netdev_t *netdev;
} virtio_net_device_t;

typedef struct virtio_net_config {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} virtio_net_config_t;

typedef struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} virtio_net_hdr_t;

typedef struct virtio_net_hdr_v1 {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} virtio_net_hdr_v1_t;

#define VIRTIO_NET_F_MTU (1ULL << 3)
#define VIRTIO_NET_F_MAC (1ULL << 5)
#define VIRTIO_NET_F_MRG_RXBUF (1ULL << 15)
#define VIRTIO_NET_F_STATUS (1ULL << 16)
#define VIRTIO_NET_DEFAULT_MTU 1500

int virtio_net_init(virtio_driver_t *driver);
int virtio_net_send(virtio_net_device_t *net_dev, void *data, uint32_t len);
int virtio_net_receive(virtio_net_device_t *net_dev, void *buffer,
                       uint32_t buffer_size);
bool virtio_net_has_packets(virtio_net_device_t *net_dev);
virtio_net_device_t *virtio_net_get_device(uint32_t index);
uint32_t virtio_net_get_device_count(void);
