#pragma once

#include <fs/proc/proc.h>
#include <fs/vfs/vfs.h>
#include <lwip/api.h>
#include <lwip/dhcp.h>
#include <lwip/err.h>
#include <lwip/etharp.h>
#include <lwip/ip.h>
#include <lwip/netbuf.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <net/netdev.h>
#include <net/real_socket.h>
#include <net/socket.h>

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW 255

#define TCP_NODELAY 1
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define IP_TTL 2
#define IP_TOS 1
#define IP_MTU_DISCOVER 10
#define IP_RECVERR 11
#define IP_FREEBIND 15
#define IP_BIND_ADDRESS_NO_PORT 24
#define IP_LOCAL_PORT_RANGE 51
#define IP_PKTINFO 8
#define IPV6_V6ONLY 26
#define IPV6_UNICAST_HOPS 16
#define IPV6_MTU_DISCOVER 23
#define IPV6_RECVERR 25
#define IPV6_RECVPKTINFO 49
#define IPV6_PKTINFO 50

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct in_addr {
    uint32_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

typedef struct lwip_socket_state {
    vfs_node_t *node;
    struct netconn *conn;
    int domain;
    int type;
    int protocol;
    bool listening;
    bool connected;
    bool closed;
    bool reuseaddr;
    bool reuseport;
    bool keepalive;
    bool broadcast;
    bool dontroute;
    bool ip_pktinfo;
    bool ipv6_pktinfo;
    bool ip_recverr;
    bool ipv6_recverr;
    bool oobinline;
    bool ip_freebind;
    bool ip_bind_address_no_port;
    int ip_mtu_discover;
    int ipv6_mtu_discover;
    uint32_t ip_local_port_range;
    uint32_t mark;
    int priority;
    int bind_to_ifindex;
    char bind_to_dev[IFNAMSIZ];
    ip_addr_t peer_addr;
    uint16_t peer_port;
    int sndbuf;
    struct timeval sndtimeo;
    struct timeval rcvtimeo;
    volatile s16_t rcvevent;
    volatile u16_t sendevent;
    volatile u16_t errevent;
    int pending_error;
    bool peer_closed;
    bool shut_rd;
    bool shut_wr;
    spinlock_t event_lock;
    wait_mutex_t rx_lock;
    wait_mutex_t tx_lock;
    struct pbuf *rx_pbuf;
    size_t rx_pbuf_offset;
    size_t rx_pbuf_announced;
    struct netbuf *rx_netbuf;
    size_t rx_netbuf_offset;
    int rx_cached_avail;
    bool rx_peeked;
} lwip_socket_state_t;

extern int lwip_socket_fsid;
extern struct netif naos_lwip_netif;

int lwip_module_init(void);
void real_socket_v4_init(void);
void real_socket_v6_init(void);
