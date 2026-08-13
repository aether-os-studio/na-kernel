#pragma once

#include <drivers/virtio/queue.h>
#include <drivers/virtio/virtio.h>
#include <net/netdev.h>

#define VIRTIO_NET_MAX_QUEUE_PAIRS 4

/* One receive queue (virtqueue index 2*qidx) with its own buffer pool. */
typedef struct virtio_net_rx_queue {
    virtqueue_t *queue;
    void *buffers[64];
    spinlock_t lock;
} virtio_net_rx_queue_t;

/* One transmit queue (virtqueue index 2*qidx + 1). Only the first one is used
 * for TX; the rest exist so the device's queue pairs are complete. */
typedef struct virtio_net_tx_queue {
    virtqueue_t *queue;
    void *buffers[64];
    uint32_t buffer_sizes[64];
    spinlock_t lock;
} virtio_net_tx_queue_t;

typedef struct virtio_net_device {
    virtio_driver_t *driver;
    uint8_t mac[6];
    uint16_t mtu;
    uint16_t net_hdr_size;
    uint64_t features;
    uint16_t num_rx_queues;
    virtio_net_rx_queue_t rx_queues[VIRTIO_NET_MAX_QUEUE_PAIRS];
    virtio_net_tx_queue_t tx_queues[VIRTIO_NET_MAX_QUEUE_PAIRS];
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

#define VIRTIO_NET_F_CSUM (1ULL << 0)
#define VIRTIO_NET_F_GUEST_CSUM (1ULL << 1)
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS (1ULL << 2)
#define VIRTIO_NET_F_MTU (1ULL << 3)
#define VIRTIO_NET_F_MAC (1ULL << 5)
#define VIRTIO_NET_F_GUEST_TSO4 (1ULL << 7)
#define VIRTIO_NET_F_GUEST_TSO6 (1ULL << 8)
#define VIRTIO_NET_F_HOST_TSO4 (1ULL << 11)
#define VIRTIO_NET_F_HOST_TSO6 (1ULL << 12)
#define VIRTIO_NET_F_MRG_RXBUF (1ULL << 15)
#define VIRTIO_NET_F_STATUS (1ULL << 16)
#define VIRTIO_NET_F_MQ (1ULL << 22)
#define VIRTIO_NET_DEFAULT_MTU 1500

#define VIRTIO_NET_HDR_F_NEEDS_CSUM 1
#define VIRTIO_NET_HDR_F_DATA_VALID 2
#define VIRTIO_NET_HDR_F_RSC_INFO 4

#define VIRTIO_NET_HDR_GSO_NONE 0
#define VIRTIO_NET_HDR_GSO_TCPV4 1
#define VIRTIO_NET_HDR_GSO_UDP 3
#define VIRTIO_NET_HDR_GSO_TCPV6 4
#define VIRTIO_NET_HDR_GSO_ECN 0x80

int virtio_net_init(virtio_driver_t *driver);
int virtio_net_send(virtio_net_device_t *net_dev, void *data, uint32_t len);
int virtio_net_sendv(virtio_net_device_t *net_dev, const netdev_iovec_t *iov,
                     uint32_t iovcnt, uint32_t total_len);
int virtio_net_send_gso(virtio_net_device_t *net_dev, const netdev_iovec_t *iov,
                        uint32_t iovcnt, uint32_t total_len,
                        const netdev_gso_info_t *gso);
int virtio_net_receive(virtio_net_device_t *net_dev, void *buffer,
                       uint32_t buffer_size);
int virtio_net_receive_q(virtio_net_device_t *net_dev, uint16_t qidx,
                         void *buffer, uint32_t buffer_size);
bool virtio_net_has_packets(virtio_net_device_t *net_dev);
bool virtio_net_has_packets_q(virtio_net_device_t *net_dev, uint16_t qidx);
virtio_net_device_t *virtio_net_get_device(uint32_t index);
uint32_t virtio_net_get_device_count(void);
