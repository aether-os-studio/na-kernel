#pragma once

#include <libs/klibc.h>
#include <task/wait.h>

typedef int (*netdev_send_t)(void *dev, void *data, uint32_t len);
typedef int (*netdev_recv_t)(void *dev, void *data, uint32_t len);
typedef bool (*netdev_poll_rx_t)(void *dev);
/* Multiqueue receive: the same as recv/poll_rx but addressed to one RX queue.
 */
typedef int (*netdev_recv_q_t)(void *dev, uint16_t qidx, void *data,
                               uint32_t len);
typedef bool (*netdev_poll_rx_q_t)(void *dev, uint16_t qidx);
typedef struct netdev_iovec {
    const void *data;
    uint32_t len;
} netdev_iovec_t;
typedef int (*netdev_sendv_t)(void *dev, const netdev_iovec_t *iov,
                              uint32_t iovcnt, uint32_t total_len);

/* Generic segmentation offload (TSO) info carried with a TX request.  The
 * device is expected to split the single large frame into `mss`-sized
 * segments on the wire.  `hdr_len` is the number of L2+L3+L4 header bytes at
 * the front of the frame (0 lets the driver derive it by parsing the frame). */
#define NETDEV_GSO_NONE 0
#define NETDEV_GSO_TCPV4 1
#define NETDEV_GSO_TCPV6 2
#define NETDEV_GSO_UDP 3

typedef struct netdev_gso_info {
    uint16_t type;    /* one of NETDEV_GSO_* */
    uint16_t mss;     /* segment size */
    uint16_t hdr_len; /* L2+L3+L4 header bytes; 0 = derive in driver */
} netdev_gso_info_t;

typedef int (*netdev_send_gso_t)(void *dev, const netdev_iovec_t *iov,
                                 uint32_t iovcnt, uint32_t total_len,
                                 const netdev_gso_info_t *gso);

/* Offload capabilities advertised by a netdev. */
#define NETDEV_CAP_TX_CSUM (1u << 0) /* generate L4 checksums in HW */
#define NETDEV_CAP_RX_CSUM (1u << 1) /* validate L4 checksums in HW */
#define NETDEV_CAP_TSO4 (1u << 2)    /* segment TCPv4 in HW */
#define NETDEV_CAP_TSO6 (1u << 3)    /* segment TCPv6 in HW */
#define NETDEV_CAP_GRO (1u << 4)     /* coalesce RX in HW */

/* Max L4 payload in a single TSO/GRO segment. Kept at 60 KiB so a full frame
 * (payload + Ethernet/IP/TCP headers) still fits lwIP's u16_t pbuf length. */
#define NETDEV_GSO_MAX_SIZE 61440

struct netdev;
typedef void (*netdev_event_cb_t)(struct netdev *dev, uint32_t events,
                                  void *ctx);

#define MAX_NETDEV_NUM 8
#define NETDEV_ETH_FRAME_OVERHEAD 18
#define NETDEV_NAME_LEN 16
#define NETDEV_WIPHY_NAME_LEN 16
#define NETDEV_MAX_EVENT_LISTENERS 8
#define NETDEV_MAX_NOTIFIERS 8
#define NETDEV_MAX_SCAN_SSIDS 4
#define NETDEV_MAX_SCAN_RESULTS 64
#define NETDEV_MAX_SCAN_IE_LEN 768

enum netdev_type {
    NETDEV_TYPE_ETHERNET = 0,
    NETDEV_TYPE_WIFI = 1,
};

enum netdev_event {
    NETDEV_EVENT_REGISTERED = 1U << 0,
    NETDEV_EVENT_ADMIN_UP = 1U << 1,
    NETDEV_EVENT_ADMIN_DOWN = 1U << 2,
    NETDEV_EVENT_LINK_UP = 1U << 3,
    NETDEV_EVENT_LINK_DOWN = 1U << 4,
    NETDEV_EVENT_CONFIG_CHANGED = 1U << 5,
    NETDEV_EVENT_UNREGISTERING = 1U << 6,
    NETDEV_EVENT_UNREGISTERED = 1U << 7,
};

typedef struct netdev_listener {
    netdev_event_cb_t cb;
    void *ctx;
} netdev_listener_t;
typedef netdev_listener_t netdev_notifier_t;

typedef struct netdev_wireless_info {
    bool present;
    bool connected;
    uint32_t wiphy_index;
    uint32_t iftype;
    uint32_t interface_modes;
    uint32_t max_scan_ssids;
    uint32_t frequency;
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t bssid[6];
    char wiphy_name[NETDEV_WIPHY_NAME_LEN];
} netdev_wireless_info_t;

typedef struct netdev_ipv4_info {
    bool present;
    bool has_default_route;
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
} netdev_ipv4_info_t;

typedef struct netdev_scan_ssid {
    uint8_t len;
    uint8_t ssid[32];
} netdev_scan_ssid_t;

typedef struct netdev_scan_params {
    uint32_t n_ssids;
    netdev_scan_ssid_t ssids[NETDEV_MAX_SCAN_SSIDS];
} netdev_scan_params_t;

typedef struct netdev_scan_result {
    bool valid;
    uint8_t bssid[6];
    uint32_t frequency;
    int32_t signal_mbm;
    uint64_t tsf;
    uint32_t seen_ms_ago;
    uint16_t beacon_interval;
    uint16_t capability;
    uint16_t ie_len;
    uint8_t ies[NETDEV_MAX_SCAN_IE_LEN];
} netdev_scan_result_t;

typedef struct netdev_scan_state {
    bool running;
    bool last_aborted;
    uint32_t generation;
    uint32_t request_portid;
    uint32_t result_count;
    netdev_scan_result_t results[NETDEV_MAX_SCAN_RESULTS];
} netdev_scan_state_t;

typedef int (*netdev_trigger_scan_t)(void *desc,
                                     const netdev_scan_params_t *params,
                                     uint32_t request_portid);
typedef struct netdev_connect_params {
    bool has_bssid;
    bool privacy;
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint8_t bssid[6];
    uint32_t frequency;
    uint32_t auth_type;
} netdev_connect_params_t;
typedef int (*netdev_trigger_connect_t)(void *desc,
                                        const netdev_connect_params_t *params,
                                        uint32_t request_portid);
typedef int (*netdev_trigger_disconnect_t)(void *desc, uint16_t reason_code,
                                           uint32_t request_portid);

static inline uint32_t netdev_max_frame_len(uint32_t mtu) {
    return mtu + NETDEV_ETH_FRAME_OVERHEAD;
}

typedef struct netdev {
    char name[NETDEV_NAME_LEN];
    uint8_t mac[6];
    uint32_t mtu;
    uint32_t id;
    uint32_t type;
    uint32_t refcount;
    bool admin_up;
    bool link_up;
    bool unregistering;
    void *desc;
    netdev_send_t send;
    netdev_sendv_t sendv;
    netdev_send_gso_t send_gso;
    netdev_recv_t recv;
    netdev_poll_rx_t poll_rx;
    netdev_recv_q_t recv_q;
    netdev_poll_rx_q_t poll_rx_q;
    uint16_t rx_queue_count;
    uint32_t capabilities;
    netdev_trigger_scan_t trigger_scan;
    netdev_trigger_connect_t trigger_connect;
    netdev_trigger_disconnect_t trigger_disconnect;
    spinlock_t lock;
    netdev_wireless_info_t wireless;
    netdev_ipv4_info_t ipv4;
    netdev_scan_state_t scan;
    netdev_listener_t listeners[NETDEV_MAX_EVENT_LISTENERS];
    wait_queue_head_t rx_wait;
    uint64_t rx_seq;
} netdev_t;

netdev_t *netdev_register_full(const char *name, uint32_t type, void *desc,
                               const uint8_t *mac, uint32_t mtu,
                               netdev_send_t send, netdev_recv_t recv,
                               netdev_poll_rx_t poll_rx);
netdev_t *netdev_register(const char *name, uint32_t type, void *desc,
                          const uint8_t *mac, uint32_t mtu, netdev_send_t send,
                          netdev_recv_t recv);
netdev_t *regist_netdev(void *desc, uint8_t *mac, uint32_t mtu,
                        netdev_send_t send, netdev_recv_t recv);

netdev_t *get_default_netdev();
netdev_t *netdev_get_by_name(const char *name);
netdev_t *netdev_get_by_index(uint32_t ifindex);
size_t netdev_snapshot(netdev_t **out, size_t max);

int netdev_set_name(netdev_t *dev, const char *name);
int netdev_set_sendv(netdev_t *dev, netdev_sendv_t sendv);
int netdev_set_send_gso(netdev_t *dev, netdev_send_gso_t send_gso);
int netdev_set_recv_q(netdev_t *dev, netdev_recv_q_t recv_q);
int netdev_set_poll_rx_q(netdev_t *dev, netdev_poll_rx_q_t poll_rx_q);
void netdev_set_rx_queue_count(netdev_t *dev, uint16_t count);
uint16_t netdev_rx_queue_count(netdev_t *dev);
void netdev_set_caps(netdev_t *dev, uint32_t caps);
uint32_t netdev_get_caps(netdev_t *dev);
int netdev_set_trigger_scan(netdev_t *dev, netdev_trigger_scan_t trigger_scan);
int netdev_set_trigger_connect(netdev_t *dev,
                               netdev_trigger_connect_t trigger_connect);
int netdev_set_trigger_disconnect(
    netdev_t *dev, netdev_trigger_disconnect_t trigger_disconnect);
int netdev_set_wireless_info(netdev_t *dev, const netdev_wireless_info_t *info);
bool netdev_get_wireless_info(netdev_t *dev, netdev_wireless_info_t *info);
int netdev_set_ipv4_info(netdev_t *dev, const netdev_ipv4_info_t *info);
bool netdev_get_ipv4_info(netdev_t *dev, netdev_ipv4_info_t *info);
int netdev_trigger_scan(netdev_t *dev, const netdev_scan_params_t *params,
                        uint32_t request_portid);
int netdev_trigger_connect(netdev_t *dev, const netdev_connect_params_t *params,
                           uint32_t request_portid);
int netdev_trigger_disconnect(netdev_t *dev, uint16_t reason_code,
                              uint32_t request_portid);
int netdev_scan_begin(netdev_t *dev, uint32_t request_portid);
int netdev_scan_store_result(netdev_t *dev, const netdev_scan_result_t *result);
int netdev_scan_complete(netdev_t *dev, bool aborted);
bool netdev_get_scan_state(netdev_t *dev, netdev_scan_state_t *state);
int netdev_set_link_state(netdev_t *dev, bool link_up);
int netdev_set_admin_state(netdev_t *dev, bool admin_up);
int netdev_unregister(netdev_t *dev);
bool netdev_link_is_up(const netdev_t *dev);
bool netdev_admin_is_up(const netdev_t *dev);
bool netdev_get(netdev_t *dev);
void netdev_put(netdev_t *dev);

int netdev_register_listener(netdev_t *dev, netdev_event_cb_t cb, void *ctx);
void netdev_unregister_listener(netdev_t *dev, netdev_event_cb_t cb, void *ctx);
int netdev_register_notifier(netdev_event_cb_t cb, void *ctx);
void netdev_unregister_notifier(netdev_event_cb_t cb, void *ctx);
void netdev_notify(netdev_t *dev, uint32_t events);
void netdev_notify_rx(netdev_t *dev);
uint64_t netdev_rx_seq(netdev_t *dev);
int netdev_wait_rx(netdev_t *dev, uint64_t observed_seq);
int netdev_wait_rx_q(netdev_t *dev, uint16_t qidx, uint64_t observed_seq);

int netdev_send(netdev_t *dev, void *data, uint32_t len);
int netdev_sendv(netdev_t *dev, const netdev_iovec_t *iov, uint32_t iovcnt,
                 uint32_t total_len);
int netdev_send_gso(netdev_t *dev, const netdev_iovec_t *iov, uint32_t iovcnt,
                    uint32_t total_len, const netdev_gso_info_t *gso);
int netdev_recv(netdev_t *dev, void *data, uint32_t len);
int netdev_recv_q(netdev_t *dev, uint16_t qidx, void *data, uint32_t len);
bool netdev_poll_rx_q(netdev_t *dev, uint16_t qidx);

static inline int netdev_recv_noref(netdev_t *dev, void *data, uint32_t len) {
    if (!dev || !dev->recv || !data) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    return dev->recv(dev->desc, data, len);
}

/* Queue-addressed receive, skipping the refcount pair (the caller holds a
 * netdev reference for the duration of the RX loop). */
static inline int netdev_recv_noref_q(netdev_t *dev, uint16_t qidx, void *data,
                                      uint32_t len) {
    if (!dev || !data || len == 0) {
        return -EINVAL;
    }
    if (dev->recv_q) {
        return dev->recv_q(dev->desc, qidx, data, len);
    }
    if (qidx == 0 && dev->recv) {
        return dev->recv(dev->desc, data, len);
    }
    return -EINVAL;
}
