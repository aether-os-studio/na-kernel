#include "netserver_internal.h"
#include <lwip/err.h>
#include <lwip/raw.h>
#include <lwip/udp.h>
#include <init/callbacks.h>

#define FIONBIO_INTERNAL_DISABLE ((ssize_t) - 1)
#define FIONBIO_INTERNAL_ENABLE ((ssize_t) - 2)
#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891B
#define SIOCGIFMTU 0x8921
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFINDEX 0x8933
#define SIOCGIWNAME 0x8B01
#define SIOCGIWMODE 0x8B07
#define SIOCGIWAP 0x8B15
#define SIOCGIWESSID 0x8B1B
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#define IW_MODE_INFRA 2
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

typedef struct naos_iw_point {
    void *pointer;
    uint16_t length;
    uint16_t flags;
} naos_iw_point_t;

typedef struct naos_iwreq {
    union {
        char ifrn_name[IFNAMSIZ];
    } ifr_ifrn;
    union {
        char name[IFNAMSIZ];
        uint32_t mode;
        struct sockaddr ap_addr;
        naos_iw_point_t essid;
    } u;
} naos_iwreq_t;

typedef struct naos_ifmap {
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char irq;
    unsigned char dma;
    unsigned char port;
} naos_ifmap_t;

typedef struct naos_ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_ifindex;
        int ifru_metric;
        int ifru_mtu;
        naos_ifmap_t ifru_map;
        char ifru_slave[IFNAMSIZ];
        char ifru_newname[IFNAMSIZ];
        void *ifru_data;
    } ifr_ifru;
} naos_ifreq_t;

typedef struct naos_ifconf {
    int ifc_len;
    union {
        void *ifcu_buf;
        naos_ifreq_t *ifcu_req;
    } ifc_ifcu;
} naos_ifconf_t;

_Static_assert(sizeof(naos_ifreq_t) == 40,
               "x86_64 Linux ifreq ABI size must be 40 bytes");
_Static_assert(sizeof(naos_ifconf_t) == 16,
               "x86_64 Linux ifconf ABI size must be 16 bytes");

extern int err_to_errno(err_t err);

static int lwip_socket_poll(fd_t *fd, size_t events);
static int lwip_socket_ioctl(fd_t *fd, ssize_t cmd, ssize_t arg);
static ssize_t lwip_socket_read(fd_t *fd, void *buf, size_t offset,
                                size_t limit);
static ssize_t lwip_socket_write(fd_t *fd, const void *buf, size_t offset,
                                 size_t limit);
static socket_op_t lwip_socket_ops;

static int lwip_errno_from_err(err_t err) {
    if (err == ERR_OK) {
        return 0;
    }
    return -err_to_errno(err);
}

static void lwip_socket_netif_name(const struct netif *netif, char *name) {
    if (!name) {
        return;
    }

    memset(name, 0, IFNAMSIZ);
    if (!netif) {
        return;
    }

    snprintf(name, IFNAMSIZ, "%c%c%u", netif->name[0], netif->name[1],
             (unsigned int)netif->num);
}

static void lwip_socket_fill_ipv4_ifreq(naos_ifreq_t *req,
                                        const struct netif *netif) {
    struct sockaddr_in *addr = NULL;

    if (!req || !netif) {
        return;
    }

    memset(req, 0, sizeof(*req));
    lwip_socket_netif_name(netif, req->ifr_name);

    addr = (struct sockaddr_in *)&req->ifr_ifru.ifru_addr;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = ip4_addr_get_u32(netif_ip4_addr(netif));
}

static int lwip_socket_ioctl_ifconf(ssize_t arg) {
    naos_ifconf_t ifc;
    naos_ifreq_t entries[MAX_NETDEV_NUM];
    int out_len = 0;
    struct netif *netif = NULL;
    size_t capacity = 0;
    size_t needed = 0;
    size_t copied = 0;
    size_t count = 0;
    void *user_buf = NULL;

    if (!arg || copy_from_user(&ifc, (const void *)arg, sizeof(ifc))) {
        return -EFAULT;
    }

    user_buf = ifc.ifc_ifcu.ifcu_buf;
    if (user_buf) {
        if (ifc.ifc_len < 0) {
            return -EINVAL;
        }
        capacity = (size_t)ifc.ifc_len;
    }

    LOCK_TCPIP_CORE();
    NETIF_FOREACH(netif) {
#if LWIP_IPV4
        if (ip4_addr_isany_val(*netif_ip4_addr(netif))) {
            continue;
        }

        needed += sizeof(naos_ifreq_t);
        if (count < MAX_NETDEV_NUM) {
            lwip_socket_fill_ipv4_ifreq(&entries[count], netif);
            count++;
        }
#endif
    }
    UNLOCK_TCPIP_CORE();

    if (!user_buf) {
        out_len = (int)needed;
        return copy_to_user((void *)arg, &out_len, sizeof(out_len)) ? -EFAULT
                                                                    : 0;
    }

    while (copied + sizeof(naos_ifreq_t) <= capacity &&
           copied / sizeof(naos_ifreq_t) < count) {
        naos_ifreq_t *req = &entries[copied / sizeof(naos_ifreq_t)];
        if (copy_to_user((uint8_t *)user_buf + copied, req, sizeof(*req))) {
            return -EFAULT;
        }
        copied += sizeof(*req);
    }

    out_len = (int)copied;
    if (copy_to_user((void *)arg, &out_len, sizeof(out_len))) {
        return -EFAULT;
    }

    return 0;
}

static int lwip_socket_ioctl_ifreq(ssize_t cmd, ssize_t arg) {
    naos_ifreq_t req;
    netdev_t *dev = NULL;
    netdev_ipv4_info_t ipv4;

    if (!arg || copy_from_user(&req, (const void *)arg, sizeof(req))) {
        return -EFAULT;
    }

    req.ifr_name[IFNAMSIZ - 1] = '\0';
    dev = netdev_get_by_name(req.ifr_name);
    if (!dev) {
        return -ENODEV;
    }

    memset(&ipv4, 0, sizeof(ipv4));
    netdev_get_ipv4_info(dev, &ipv4);

    if (cmd == SIOCGIFFLAGS) {
        short flags = IFF_BROADCAST | IFF_MULTICAST;
        if (netdev_admin_is_up(dev)) {
            flags |= IFF_UP;
        }
        if (netdev_link_is_up(dev)) {
            flags |= IFF_RUNNING;
        }
        req.ifr_ifru.ifru_flags = flags;
    } else if (cmd == SIOCGIFNETMASK) {
        struct sockaddr_in *addr =
            (struct sockaddr_in *)&req.ifr_ifru.ifru_addr;
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_addr.s_addr = ipv4.present ? ipv4.netmask : 0;
    } else if (cmd == SIOCGIFBRDADDR) {
        struct sockaddr_in *addr =
            (struct sockaddr_in *)&req.ifr_ifru.ifru_addr;
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        if (ipv4.present) {
            addr->sin_addr.s_addr = ipv4.address | ~ipv4.netmask;
        }
    } else if (cmd == SIOCGIFMTU) {
        req.ifr_ifru.ifru_mtu = (int)dev->mtu;
    } else if (cmd == SIOCGIFHWADDR) {
        memset(&req.ifr_ifru.ifru_hwaddr, 0, sizeof(req.ifr_ifru.ifru_hwaddr));
        req.ifr_ifru.ifru_hwaddr.sa_family = ARPHRD_ETHER;
        memcpy(req.ifr_ifru.ifru_hwaddr.sa_data, dev->mac, sizeof(dev->mac));
    } else {
        netdev_put(dev);
        return -ENOTTY;
    }

    netdev_put(dev);

    if (copy_to_user((void *)arg, &req, sizeof(req))) {
        return -EFAULT;
    }

    return 0;
}

static inline bool lwip_socket_is_tcp(const lwip_socket_state_t *sock) {
    return sock && ((sock->type & 0xF) == SOCK_STREAM);
}

static inline bool lwip_socket_is_dgram(const lwip_socket_state_t *sock) {
    return sock && ((sock->type & 0xF) == SOCK_DGRAM);
}

static inline bool lwip_socket_is_raw(const lwip_socket_state_t *sock) {
    return sock && ((sock->type & 0xF) == SOCK_RAW);
}

static inline bool lwip_socket_is_icmp_dgram(int domain, int type,
                                             int protocol) {
    int sock_type = type & 0xF;

    return sock_type == SOCK_DGRAM &&
           ((domain == AF_INET && protocol == IPPROTO_ICMP) ||
            (domain == AF_INET6 && protocol == IPPROTO_ICMPV6));
}

static inline int lwip_socket_recv_avail(lwip_socket_state_t *sock) {
    int avail = 0;
    int queued = 0;
    int cached = 0;

    if (!sock) {
        return 0;
    }
    if (sock->conn) {
        SYS_ARCH_GET(sock->conn->recv_avail, queued);
        if (queued > 0) {
            avail += queued;
        }
    }
    spin_lock(&sock->event_lock);
    cached = sock->rx_cached_avail;
    spin_unlock(&sock->event_lock);
    if (cached > 0) {
        avail += cached;
    }
    return avail;
}

static void lwip_socket_notify(lwip_socket_state_t *sock, uint32_t events) {
    if (!sock || !events)
        return;
    if (sock->node)
        vfs_poll_notify_inode(sock->node, events);
}

static void lwip_socket_notify_ready_state(lwip_socket_state_t *sock) {
    s16_t rcvevent = 0;
    u16_t sendevent = 0;
    u16_t errevent = 0;
    int pending_error = 0;
    bool peer_closed = false;
    bool shut_rd = false;
    bool shut_wr = false;
    uint32_t events = 0;

    if (!sock) {
        return;
    }

    spin_lock(&sock->event_lock);
    rcvevent = sock->rcvevent;
    sendevent = sock->sendevent;
    errevent = sock->errevent;
    pending_error = sock->pending_error;
    peer_closed = sock->peer_closed;
    shut_rd = sock->shut_rd;
    shut_wr = sock->shut_wr;
    spin_unlock(&sock->event_lock);

    if (rcvevent > 0 || lwip_socket_recv_avail(sock) > 0 || peer_closed ||
        shut_rd) {
        events |= EPOLLIN;
    }
    if (sendevent && !shut_wr) {
        events |= EPOLLOUT;
    }
    if (errevent || pending_error != 0) {
        events |= EPOLLERR;
    }
    if (sock->closed) {
        events |= EPOLLHUP | EPOLLRDHUP;
    } else if (peer_closed) {
        events |= EPOLLRDHUP;
    }

    lwip_socket_notify(sock, events);
}

static int lwip_socket_take_pending_error(lwip_socket_state_t *sock) {
    int error = 0;

    if (!sock) {
        return 0;
    }

    spin_lock(&sock->event_lock);
    error = sock->pending_error;
    sock->pending_error = 0;
    sock->errevent = 0;
    spin_unlock(&sock->event_lock);

    return error;
}

static void lwip_socket_publish_rx_avail(lwip_socket_state_t *sock) {
    int avail = 0;

    if (!sock) {
        return;
    }

    if (sock->rx_pbuf) {
        avail += (int)(sock->rx_pbuf->tot_len - sock->rx_pbuf_offset);
    }
    if (sock->rx_netbuf) {
        avail += (int)(netbuf_len(sock->rx_netbuf) - sock->rx_netbuf_offset);
    }

    spin_lock(&sock->event_lock);
    sock->rx_cached_avail = avail;
    spin_unlock(&sock->event_lock);
}

static err_t lwip_socket_peek_conn_pending_err(struct netconn *conn) {
    err_t err = ERR_OK;
    SYS_ARCH_DECL_PROTECT(lev);

    if (!conn) {
        return ERR_OK;
    }

    SYS_ARCH_PROTECT(lev);
    err = conn->pending_err;
    SYS_ARCH_UNPROTECT(lev);
    return err;
}

static inline int lwip_socket_apply_fd_flags(fd_t *fd, int flags) {
    if (fd && (fd_get_flags(fd) & O_NONBLOCK)) {
        flags |= MSG_DONTWAIT;
    }
    return flags;
}

static bool lwip_socket_io_lock(wait_mutex_t *lock, fd_t *fd, int flags) {
    if ((flags & MSG_DONTWAIT) || (fd && (fd_get_flags(fd) & O_NONBLOCK)))
        return wait_mutex_trylock(lock);

    wait_mutex_lock(lock);
    return true;
}

static int lwip_socket_bind_netif(lwip_socket_state_t *sock,
                                  struct netif *netif) {
    int ret = 0;

    if (!sock || !sock->conn) {
        return -EBADF;
    }

    LOCK_TCPIP_CORE();
    /*
     * A netconn can outlive its PCB after an asynchronous close or reset.
     * The PCB must be checked while holding the core lock: checking it before
     * taking the lock would still race with the tcpip thread clearing it.
     */
    if (!sock->conn->pcb.ip) {
        ret = -EINVAL;
        goto out;
    }

    switch (NETCONNTYPE_GROUP(netconn_type(sock->conn))) {
    case NETCONN_TCP:
        tcp_bind_netif(sock->conn->pcb.tcp, netif);
        break;
    case NETCONN_UDP:
        udp_bind_netif(sock->conn->pcb.udp, netif);
        break;
    case NETCONN_RAW:
        raw_bind_netif(sock->conn->pcb.raw, netif);
        break;
    default:
        ret = -EOPNOTSUPP;
        break;
    }
out:
    UNLOCK_TCPIP_CORE();

    return ret;
}

static inline bool lwip_socket_buffer_is_userspace(const void *buf,
                                                   size_t len) {
    return buf && !check_user_overflow((uint64_t)buf, len);
}

static int lwip_socket_alloc_copy_from_buffer(const void *src, size_t len,
                                              void **out_buf) {
    bool src_is_userspace = lwip_socket_buffer_is_userspace(src, len);
    void *buf = NULL;

    if (!out_buf) {
        return -EINVAL;
    }
    if (!len) {
        *out_buf = NULL;
        return 0;
    }
    if (!src) {
        return -EFAULT;
    }

    buf = malloc(len);
    if (!buf) {
        return -ENOMEM;
    }

    if (src_is_userspace) {
        if (copy_from_user(buf, src, len)) {
            free(buf);
            return -EFAULT;
        }
    } else {
        memcpy(buf, src, len);
    }

    *out_buf = buf;
    return 0;
}

static int lwip_socket_copy_to_buffer(void *dst, const void *src, size_t len) {
    bool dst_is_userspace = lwip_socket_buffer_is_userspace(dst, len);

    if (!len) {
        return 0;
    }
    if (!dst || !src) {
        return -EFAULT;
    }

    if (dst_is_userspace) {
        if (copy_to_user(dst, src, len)) {
            return -EFAULT;
        }
    } else {
        memcpy(dst, src, len);
    }

    return 0;
}

static void lwip_socket_free_bounce_iov(struct iovec *iov, void **buffers,
                                        size_t iovlen) {
    if (buffers) {
        for (size_t i = 0; i < iovlen; i++) {
            free(buffers[i]);
        }
    }

    free(buffers);
    free(iov);
}

static int lwip_socket_build_bounce_iov(const struct msghdr *msg,
                                        struct iovec **out_iov,
                                        void ***out_buffers) {
    struct iovec *bounce_iov = NULL;
    void **buffers = NULL;

    if (!out_iov || !out_buffers) {
        return -EINVAL;
    }

    *out_iov = NULL;
    *out_buffers = NULL;

    if (!msg) {
        return -EINVAL;
    }
    if (!msg->msg_iovlen) {
        return 0;
    }
    if (!msg->msg_iov) {
        return -EFAULT;
    }

    bounce_iov = calloc(msg->msg_iovlen, sizeof(*bounce_iov));
    buffers = calloc(msg->msg_iovlen, sizeof(*buffers));
    if (!bounce_iov || !buffers) {
        lwip_socket_free_bounce_iov(bounce_iov, buffers, msg->msg_iovlen);
        return -ENOMEM;
    }

    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        if (lwip_socket_buffer_is_userspace(msg->msg_iov[i].iov_base,
                                            msg->msg_iov[i].len)) {
            int ret = lwip_socket_alloc_copy_from_buffer(
                msg->msg_iov[i].iov_base, msg->msg_iov[i].len, &buffers[i]);
            if (ret < 0) {
                lwip_socket_free_bounce_iov(bounce_iov, buffers,
                                            msg->msg_iovlen);
                return ret;
            }
            bounce_iov[i].iov_base = buffers[i];
        } else {
            bounce_iov[i].iov_base = msg->msg_iov[i].iov_base;
        }

        bounce_iov[i].len = msg->msg_iov[i].len;
    }

    *out_iov = bounce_iov;
    *out_buffers = buffers;
    return 0;
}

static void lwip_socket_mark_peer_closed(lwip_socket_state_t *sock) {
    if (!sock) {
        return;
    }

    spin_lock(&sock->event_lock);
    sock->peer_closed = true;
    spin_unlock(&sock->event_lock);
}

static void lwip_socket_set_shutdown(lwip_socket_state_t *sock, bool shut_rd,
                                     bool shut_wr) {
    if (!sock) {
        return;
    }

    spin_lock(&sock->event_lock);
    if (shut_rd) {
        sock->shut_rd = true;
    }
    if (shut_wr) {
        sock->shut_wr = true;
    }
    spin_unlock(&sock->event_lock);
}

static void lwip_socket_attach_conn(struct netconn *conn,
                                    lwip_socket_state_t *sock) {
    s16_t queued_rcvevent = 0;
    err_t pending_err = ERR_OK;
    SYS_ARCH_DECL_PROTECT(lev);

    if (!conn || !sock) {
        return;
    }

    SYS_ARCH_PROTECT(lev);
    if (conn->callback_arg_type == NETCONN_CALLBACK_ARG_SOCKET &&
        conn->callback_arg.socket < -1) {
        queued_rcvevent = (s16_t)(-1 - conn->callback_arg.socket);
    }
    pending_err = conn->pending_err;
    netconn_set_callback_arg(conn, sock);
    SYS_ARCH_UNPROTECT(lev);

    if (queued_rcvevent > 0) {
        sock->rcvevent = queued_rcvevent;
    }
    if (pending_err != ERR_OK) {
        sock->errevent = 1;
        sock->pending_error = err_to_errno(pending_err);
    }
}

static void lwip_socket_event_callback(struct netconn *conn,
                                       enum netconn_evt evt, u16_t len) {
    lwip_socket_state_t *sock = NULL;
    int pending_error = 0;
    SYS_ARCH_DECL_PROTECT(lev);

    LWIP_UNUSED_ARG(len);

    if (!conn) {
        return;
    }

    SYS_ARCH_PROTECT(lev);
    if (conn->callback_arg_type != NETCONN_CALLBACK_ARG_PTR) {
        if (evt == NETCONN_EVT_RCVPLUS && conn->callback_arg.socket < 0) {
            conn->callback_arg.socket--;
        }
        SYS_ARCH_UNPROTECT(lev);
        return;
    }
    sock = conn->callback_arg.ptr;
    SYS_ARCH_UNPROTECT(lev);

    if (!sock) {
        return;
    }

    if (evt == NETCONN_EVT_ERROR) {
        err_t err = lwip_socket_peek_conn_pending_err(conn);
        if (err != ERR_OK) {
            pending_error = err_to_errno(err);
        }
    }

    spin_lock(&sock->event_lock);
    switch (evt) {
    case NETCONN_EVT_RCVPLUS:
        sock->rcvevent++;
        break;
    case NETCONN_EVT_RCVMINUS:
        if (sock->rcvevent > 0) {
            sock->rcvevent--;
        }
        break;
    case NETCONN_EVT_SENDPLUS:
        sock->sendevent = 1;
        break;
    case NETCONN_EVT_SENDMINUS:
        sock->sendevent = 0;
        break;
    case NETCONN_EVT_ERROR:
        if (pending_error != 0) {
            sock->errevent = 1;
            sock->pending_error = pending_error;
        }
        break;
    default:
        break;
    }
    spin_unlock(&sock->event_lock);

    if (evt == NETCONN_EVT_RCVPLUS) {
        lwip_socket_notify(sock, EPOLLIN);
    } else if (evt == NETCONN_EVT_SENDPLUS) {
        lwip_socket_notify(sock, EPOLLOUT);
    } else if (evt == NETCONN_EVT_ERROR && pending_error != 0) {
        lwip_socket_notify(sock, EPOLLERR);
    }
}

static void lwip_socket_free_rx_cache(lwip_socket_state_t *sock) {
    if (!sock) {
        return;
    }
    if (sock->rx_pbuf) {
        pbuf_free(sock->rx_pbuf);
        sock->rx_pbuf = NULL;
        sock->rx_pbuf_offset = 0;
        sock->rx_pbuf_announced = 0;
    }
    if (sock->rx_netbuf) {
        netbuf_delete(sock->rx_netbuf);
        sock->rx_netbuf = NULL;
        sock->rx_netbuf_offset = 0;
    }
    lwip_socket_publish_rx_avail(sock);
}

static void lwip_socket_drain_tcp_rx(lwip_socket_state_t *sock) {
    if (!sock || !sock->conn || !lwip_socket_is_tcp(sock)) {
        return;
    }

    lwip_socket_free_rx_cache(sock);

    while (true) {
        struct pbuf *p = NULL;
        err_t err = netconn_recv_tcp_pbuf_flags(
            sock->conn, &p, NETCONN_DONTBLOCK | NETCONN_NOAUTORCVD);

        if (err == ERR_OK) {
            if (!p) {
                lwip_socket_mark_peer_closed(sock);
                break;
            }
            pbuf_free(p);
            continue;
        }

        if (err == ERR_CLSD) {
            lwip_socket_mark_peer_closed(sock);
        }
        break;
    }
}

static void lwip_socket_release_conn(lwip_socket_state_t *sock) {
    err_t err = ERR_OK;

    if (!sock || !sock->conn) {
        return;
    }

    if (lwip_socket_is_tcp(sock)) {
        lwip_socket_drain_tcp_rx(sock);
    } else {
        lwip_socket_free_rx_cache(sock);
    }

    /* Core-side API_EVENT callbacks run under the lwIP core lock.  Clear the
     * adapter pointer under that same lock so an in-flight RX/TX callback has
     * completed before the socket state is released. */
    LOCK_TCPIP_CORE();
    netconn_set_callback_arg(sock->conn, NULL);
    UNLOCK_TCPIP_CORE();
    err = netconn_prepare_delete(sock->conn);
    if (err == ERR_OK) {
        netconn_delete(sock->conn);
    } else {
        netconn_delete(sock->conn);
    }
    sock->conn = NULL;
}

static lwip_socket_state_t *lwip_socket_alloc(struct netconn *conn, int domain,
                                              int type, int protocol) {
    lwip_socket_state_t *sock = calloc(1, sizeof(*sock));
    if (!sock) {
        return NULL;
    }

    sock->conn = conn;
    sock->domain = domain;
    sock->type = type & 0xF;
    sock->protocol = protocol;
    // Fresh TCP sockets are not writable until connect() completes.
    sock->sendevent = lwip_socket_is_tcp(sock) ? 0 : 1;
    sock->sndbuf = lwip_socket_is_tcp(sock) ? TCP_SND_BUF : 0;
    spin_init(&sock->event_lock);
    wait_mutex_init(&sock->rx_lock);
    wait_mutex_init(&sock->tx_lock);

    if (conn) {
        lwip_socket_attach_conn(conn, sock);
    }

    return sock;
}

static void lwip_socket_handle_release(socket_handle_t *handle);
static lwip_socket_state_t *lwip_socket_state_from_node(vfs_node_t *node);
static lwip_socket_state_t *lwip_socket_state_from_file(fd_t *file);

static int lwip_socket_install_fd(lwip_socket_state_t *sock, int open_type,
                                  uint64_t accept_flags) {
    struct vfs_file *file = NULL;
    socket_handle_t *handle = NULL;
    uint64_t file_flags = O_RDWR;
    int ret = 0;

    if (!sock) {
        return -EINVAL;
    }

    handle = calloc(1, sizeof(*handle));
    if (!handle) {
        lwip_socket_release_conn(sock);
        free(sock);
        return -ENOMEM;
    }

    handle->op = &lwip_socket_ops;
    handle->sock = sock;
    handle->read_op = lwip_socket_read;
    handle->write_op = lwip_socket_write;
    handle->ioctl_op = lwip_socket_ioctl;
    handle->poll_op = lwip_socket_poll;
    handle->release = lwip_socket_handle_release;

    if ((open_type & O_NONBLOCK) || (accept_flags & O_NONBLOCK)) {
        file_flags |= O_NONBLOCK;
        if (sock->conn) {
            netconn_set_nonblocking(sock->conn, 1);
        }
    }

    ret = sockfs_create_handle_file(handle, file_flags, &file);
    if (ret < 0) {
        free(handle);
        lwip_socket_release_conn(sock);
        free(sock);
        return ret;
    }

    sock->node = file->f_inode;
    ret = task_install_file(
        current_task, file,
        ((open_type | accept_flags) & O_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    if (ret < 0) {
        lwip_socket_handle_release(handle);
        return ret;
    }

    lwip_socket_notify_ready_state(sock);
    return ret;
}

static int lwip_sockaddr_to_ip(const void *addr, socklen_t addrlen, int domain,
                               ip_addr_t *ipaddr, uint16_t *port) {
    if (!addr || !ipaddr || !port) {
        return -EFAULT;
    }

    if (domain == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        if (addrlen < sizeof(*in) || in->sin_family != AF_INET) {
            return -EINVAL;
        }
        ip_addr_set_zero(ipaddr);
        ip_2_ip4(ipaddr)->addr = in->sin_addr.s_addr;
        IP_SET_TYPE(ipaddr, IPADDR_TYPE_V4);
        *port = lwip_ntohs(in->sin_port);
        return 0;
    }

    if (domain == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        if (addrlen < sizeof(*in6) || in6->sin6_family != AF_INET6) {
            return -EINVAL;
        }
        ip_addr_set_zero(ipaddr);
        memcpy(ip_2_ip6(ipaddr), &in6->sin6_addr, sizeof(struct in6_addr));
        IP_SET_TYPE(ipaddr, IPADDR_TYPE_V6);
        *port = lwip_ntohs(in6->sin6_port);
        return 0;
    }

    return -EAFNOSUPPORT;
}

static int lwip_ip_to_sockaddr(const ip_addr_t *ipaddr, uint16_t port,
                               void *addr, socklen_t *addrlen, int domain) {
    if (!addrlen) {
        return -EFAULT;
    }

    if (domain == AF_INET) {
        struct sockaddr_in out = {0};
        out.sin_family = AF_INET;
        out.sin_port = lwip_htons(port);
        out.sin_addr.s_addr = ip_2_ip4(ipaddr)->addr;
        if (addr && *addrlen) {
            memcpy(addr, &out, MIN((size_t)*addrlen, sizeof(out)));
        }
        *addrlen = sizeof(out);
        return 0;
    }

    if (domain == AF_INET6) {
        struct sockaddr_in6 out6 = {0};
        out6.sin6_family = AF_INET6;
        out6.sin6_port = lwip_htons(port);
        memcpy(&out6.sin6_addr, ip_2_ip6(ipaddr), sizeof(out6.sin6_addr));
        if (addr && *addrlen) {
            memcpy(addr, &out6, MIN((size_t)*addrlen, sizeof(out6)));
        }
        *addrlen = sizeof(out6);
        return 0;
    }

    return -EAFNOSUPPORT;
}

static enum netconn_type lwip_pick_netconn_type(int domain, int type) {
    int sock_type = type & 0xF;

    if (domain == AF_INET) {
        if (sock_type == SOCK_STREAM) {
            return NETCONN_TCP;
        }
        if (sock_type == SOCK_DGRAM) {
            return NETCONN_UDP;
        }
        if (sock_type == SOCK_RAW) {
            return NETCONN_RAW;
        }
    } else if (domain == AF_INET6) {
        if (sock_type == SOCK_STREAM) {
            return NETCONN_TCP_IPV6;
        }
        if (sock_type == SOCK_DGRAM) {
            return NETCONN_UDP_IPV6;
        }
        if (sock_type == SOCK_RAW) {
            return NETCONN_RAW_IPV6;
        }
    }

    return NETCONN_INVALID;
}

static struct netconn *lwip_socket_new_conn(int domain, int type,
                                            int protocol) {
    enum netconn_type conn_type =
        lwip_socket_is_icmp_dgram(domain, type, protocol)
            ? (domain == AF_INET ? NETCONN_RAW : NETCONN_RAW_IPV6)
            : lwip_pick_netconn_type(domain, type);
    int sock_type = type & 0xF;

    if (conn_type == NETCONN_INVALID) {
        return NULL;
    }

    if (sock_type == SOCK_STREAM) {
        if (protocol != 0 && protocol != IPPROTO_TCP) {
            return NULL;
        }
        return netconn_new_with_proto_and_callback(conn_type, 0,
                                                   lwip_socket_event_callback);
    }

    if (sock_type == SOCK_DGRAM) {
        if (lwip_socket_is_icmp_dgram(domain, type, protocol)) {
            return netconn_new_with_proto_and_callback(
                conn_type, (u8_t)protocol, lwip_socket_event_callback);
        }
        if (protocol != 0 && protocol != IPPROTO_UDP) {
            return NULL;
        }
        return netconn_new_with_proto_and_callback(conn_type, 0,
                                                   lwip_socket_event_callback);
    }

    if (sock_type == SOCK_RAW) {
        if (!protocol) {
            protocol = (domain == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_RAW;
        }
        return netconn_new_with_proto_and_callback(conn_type, (u8_t)protocol,
                                                   lwip_socket_event_callback);
    }

    return NULL;
}

static int lwip_socket_setsockopt_impl(lwip_socket_state_t *sock, int level,
                                       int optname, const void *optval,
                                       socklen_t optlen) {
    int value = 0;

    if (!sock || !sock->conn) {
        return -EBADF;
    }

    if (optlen >= sizeof(int) && optval) {
        value = *(const int *)optval;
    }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            if (value) {
                ip_set_option(sock->conn->pcb.ip, SOF_REUSEADDR);
            } else {
                ip_reset_option(sock->conn->pcb.ip, SOF_REUSEADDR);
            }
            UNLOCK_TCPIP_CORE();
            sock->reuseaddr = value ? true : false;
            return 0;
        case SO_REUSEPORT:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->reuseport = value ? true : false;
            return 0;
        case SO_KEEPALIVE:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            if (value) {
                ip_set_option(sock->conn->pcb.ip, SOF_KEEPALIVE);
            } else {
                ip_reset_option(sock->conn->pcb.ip, SOF_KEEPALIVE);
            }
            UNLOCK_TCPIP_CORE();
            sock->keepalive = value ? true : false;
            return 0;
        case SO_BROADCAST:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            if (value) {
                ip_set_option(sock->conn->pcb.ip, SOF_BROADCAST);
            } else {
                ip_reset_option(sock->conn->pcb.ip, SOF_BROADCAST);
            }
            UNLOCK_TCPIP_CORE();
            sock->broadcast = value ? true : false;
            return 0;
        case SO_DONTROUTE:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->dontroute = value ? true : false;
            return 0;
        case SO_OOBINLINE:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->oobinline = value ? true : false;
            return 0;
        case SO_PRIORITY:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->priority = value;
            return 0;
        case SO_MARK:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->mark = (uint32_t)value;
            return 0;
        case SO_BINDTOIFINDEX:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            if (value) {
                netdev_t *dev = netdev_get_by_index((uint32_t)value);
                if (!dev) {
                    return -ENODEV;
                }
                strncpy(sock->bind_to_dev, dev->name, IFNAMSIZ - 1);
                sock->bind_to_dev[IFNAMSIZ - 1] = '\0';
                netdev_put(dev);
            } else {
                sock->bind_to_dev[0] = '\0';
            }
            sock->bind_to_ifindex = value;
            return lwip_socket_bind_netif(sock,
                                          value ? &naos_lwip_netif : NULL);
        case SO_BINDTODEVICE:
            if (optlen > IFNAMSIZ) {
                return -EINVAL;
            }
            if (!optval || optlen == 0 || !*(const char *)optval) {
                sock->bind_to_dev[0] = '\0';
                sock->bind_to_ifindex = 0;
                return lwip_socket_bind_netif(sock, NULL);
            }
            {
                char ifname[IFNAMSIZ];
                netdev_t *dev = NULL;

                memset(ifname, 0, sizeof(ifname));
                memcpy(ifname, optval, optlen);
                ifname[IFNAMSIZ - 1] = '\0';

                dev = netdev_get_by_name(ifname);
                if (!dev) {
                    return -ENODEV;
                }
                strncpy(sock->bind_to_dev, dev->name, IFNAMSIZ - 1);
                sock->bind_to_dev[IFNAMSIZ - 1] = '\0';
                sock->bind_to_ifindex = (int)(dev->id + 1);
                netdev_put(dev);
            }
            return lwip_socket_bind_netif(sock, &naos_lwip_netif);
        case SO_SNDBUF:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->sndbuf = value;
            return 0;
        case SO_RCVBUF:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            netconn_set_recvbufsize(sock->conn, value);
            return 0;
        case SO_RCVTIMEO_OLD:
        case SO_RCVTIMEO_NEW:
            if (optlen < sizeof(struct timeval)) {
                return -EINVAL;
            }
            {
                const struct timeval *tv = (const struct timeval *)optval;
                u32_t ms = (u32_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
                memcpy(&sock->rcvtimeo, tv, sizeof(*tv));
                netconn_set_recvtimeout(sock->conn, ms);
            }
            return 0;
        case SO_SNDTIMEO_OLD:
        case SO_SNDTIMEO_NEW:
            if (optlen < sizeof(struct timeval)) {
                return -EINVAL;
            }
            {
                const struct timeval *tv = (const struct timeval *)optval;
                s32_t ms = (s32_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
                memcpy(&sock->sndtimeo, tv, sizeof(*tv));
                netconn_set_sendtimeout(sock->conn, ms);
            }
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    if (level == IPPROTO_IP) {
        switch (optname) {
        case IP_TTL:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            sock->conn->pcb.ip->ttl = (u8_t)value;
            UNLOCK_TCPIP_CORE();
            return 0;
        case IP_TOS:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            sock->conn->pcb.ip->tos = (u8_t)value;
            UNLOCK_TCPIP_CORE();
            return 0;
        case IP_PKTINFO:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ip_pktinfo = value ? true : false;
            LOCK_TCPIP_CORE();
            if (value) {
                sock->conn->flags |= NETCONN_FLAG_PKTINFO;
            } else {
                sock->conn->flags &= ~NETCONN_FLAG_PKTINFO;
            }
            UNLOCK_TCPIP_CORE();
            return 0;
        case IP_RECVERR:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ip_recverr = value ? true : false;
            return 0;
        case IP_FREEBIND:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ip_freebind = value ? true : false;
            return 0;
        case IP_BIND_ADDRESS_NO_PORT:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ip_bind_address_no_port = value ? true : false;
            return 0;
        case IP_MTU_DISCOVER:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ip_mtu_discover = value;
            return 0;
        case IP_LOCAL_PORT_RANGE:
            if (optlen < sizeof(uint32_t)) {
                return -EINVAL;
            }
            sock->ip_local_port_range = *(const uint32_t *)optval;
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    if (level == IPPROTO_TCP && lwip_socket_is_tcp(sock)) {
        if (optlen < sizeof(int)) {
            return -EINVAL;
        }

        LOCK_TCPIP_CORE();
        if (!sock->conn->pcb.tcp) {
            UNLOCK_TCPIP_CORE();
            return -EINVAL;
        }
        switch (optname) {
        case TCP_NODELAY:
            if (value) {
                tcp_nagle_disable(sock->conn->pcb.tcp);
            } else {
                tcp_nagle_enable(sock->conn->pcb.tcp);
            }
            UNLOCK_TCPIP_CORE();
            return 0;
        case TCP_KEEPIDLE:
            sock->conn->pcb.tcp->keep_idle = 1000U * (u32_t)value;
            UNLOCK_TCPIP_CORE();
            return 0;
        case TCP_KEEPINTVL:
            sock->conn->pcb.tcp->keep_intvl = 1000U * (u32_t)value;
            UNLOCK_TCPIP_CORE();
            return 0;
        case TCP_KEEPCNT:
            sock->conn->pcb.tcp->keep_cnt = (u32_t)value;
            UNLOCK_TCPIP_CORE();
            return 0;
        default:
            UNLOCK_TCPIP_CORE();
            break;
        }
    }

    if (level == IPPROTO_IPV6 && sock->domain == AF_INET6) {
        switch (optname) {
        case IPV6_V6ONLY:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            netconn_set_ipv6only(sock->conn, value ? 1 : 0);
            return 0;
        case IPV6_UNICAST_HOPS:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            sock->conn->pcb.ip->ttl = (u8_t)value;
            UNLOCK_TCPIP_CORE();
            return 0;
        case IPV6_RECVPKTINFO:
        case IPV6_PKTINFO:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ipv6_pktinfo = value ? true : false;
            LOCK_TCPIP_CORE();
            if (value) {
                sock->conn->flags |= NETCONN_FLAG_PKTINFO;
            } else {
                sock->conn->flags &= ~NETCONN_FLAG_PKTINFO;
            }
            UNLOCK_TCPIP_CORE();
            return 0;
        case IPV6_RECVERR:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ipv6_recverr = value ? true : false;
            return 0;
        case IPV6_MTU_DISCOVER:
            if (optlen < sizeof(int)) {
                return -EINVAL;
            }
            sock->ipv6_mtu_discover = value;
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    return -ENOPROTOOPT;
}

static int lwip_socket_getsockopt_impl(lwip_socket_state_t *sock, int level,
                                       int optname, void *optval,
                                       socklen_t *optlen) {
    int value = 0;

    if (!sock || !sock->conn || !optlen) {
        return -EBADF;
    }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_ERROR: {
            err_t err = netconn_err(sock->conn);
            value = lwip_socket_take_pending_error(sock);
            if (value == 0 && err != ERR_OK) {
                value = err_to_errno(err);
            }
            break;
        }
        case SO_TYPE:
            value = sock->type;
            break;
        case SO_DOMAIN:
            value = sock->domain;
            break;
        case SO_PROTOCOL:
            value = sock->protocol;
            break;
        case SO_ACCEPTCONN:
            value = sock->listening ? 1 : 0;
            break;
        case SO_REUSEADDR:
            value = sock->reuseaddr ? 1 : 0;
            break;
        case SO_REUSEPORT:
            value = sock->reuseport ? 1 : 0;
            break;
        case SO_KEEPALIVE:
            value = sock->keepalive ? 1 : 0;
            break;
        case SO_OOBINLINE:
            value = sock->oobinline ? 1 : 0;
            break;
        case SO_BROADCAST:
            value = sock->broadcast ? 1 : 0;
            break;
        case SO_DONTROUTE:
            value = sock->dontroute ? 1 : 0;
            break;
        case SO_PRIORITY:
            value = sock->priority;
            break;
        case SO_MARK:
            value = (int)sock->mark;
            break;
        case SO_BINDTOIFINDEX:
            value = sock->bind_to_ifindex;
            break;
        case SO_BINDTODEVICE: {
            size_t len = strlen(sock->bind_to_dev) + 1;
            if (*optlen < len) {
                return -EINVAL;
            }
            memcpy(optval, sock->bind_to_dev, len);
            *optlen = (socklen_t)len;
            return 0;
        }
        case SO_SNDBUF:
            value = sock->sndbuf;
            break;
        case SO_RCVBUF:
            value = netconn_get_recvbufsize(sock->conn);
            break;
        case SO_RCVTIMEO_OLD:
        case SO_RCVTIMEO_NEW:
            if (*optlen < sizeof(struct timeval)) {
                return -EINVAL;
            }
            memcpy(optval, &sock->rcvtimeo, sizeof(sock->rcvtimeo));
            *optlen = sizeof(sock->rcvtimeo);
            return 0;
        case SO_SNDTIMEO_OLD:
        case SO_SNDTIMEO_NEW:
            if (*optlen < sizeof(struct timeval)) {
                return -EINVAL;
            }
            memcpy(optval, &sock->sndtimeo, sizeof(sock->sndtimeo));
            *optlen = sizeof(sock->sndtimeo);
            return 0;
        default:
            return -ENOPROTOOPT;
        }

        if (*optlen < sizeof(int)) {
            return -EINVAL;
        }
        *(int *)optval = value;
        *optlen = sizeof(int);
        return 0;
    }

    if (level == IPPROTO_IP) {
        switch (optname) {
        case IP_TTL:
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            value = sock->conn->pcb.ip->ttl;
            UNLOCK_TCPIP_CORE();
            break;
        case IP_TOS:
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            value = sock->conn->pcb.ip->tos;
            UNLOCK_TCPIP_CORE();
            break;
        case IP_PKTINFO:
            value = sock->ip_pktinfo ? 1 : 0;
            break;
        case IP_RECVERR:
            value = sock->ip_recverr ? 1 : 0;
            break;
        case IP_FREEBIND:
            value = sock->ip_freebind ? 1 : 0;
            break;
        case IP_BIND_ADDRESS_NO_PORT:
            value = sock->ip_bind_address_no_port ? 1 : 0;
            break;
        case IP_MTU_DISCOVER:
            value = sock->ip_mtu_discover;
            break;
        case IP_LOCAL_PORT_RANGE:
            if (*optlen < sizeof(uint32_t)) {
                return -EINVAL;
            }
            *(uint32_t *)optval = sock->ip_local_port_range;
            *optlen = sizeof(uint32_t);
            return 0;
        default:
            return -ENOPROTOOPT;
        }

        if (*optlen < sizeof(int)) {
            return -EINVAL;
        }
        *(int *)optval = value;
        *optlen = sizeof(int);
        return 0;
    }

    if (level == IPPROTO_TCP && lwip_socket_is_tcp(sock)) {
        if (*optlen < sizeof(int)) {
            return -EINVAL;
        }

        LOCK_TCPIP_CORE();
        if (!sock->conn->pcb.tcp) {
            UNLOCK_TCPIP_CORE();
            return -EINVAL;
        }
        switch (optname) {
        case TCP_NODELAY:
            *(int *)optval = tcp_nagle_disabled(sock->conn->pcb.tcp);
            UNLOCK_TCPIP_CORE();
            *optlen = sizeof(int);
            return 0;
        case TCP_KEEPIDLE:
            *(int *)optval = (int)(sock->conn->pcb.tcp->keep_idle / 1000U);
            UNLOCK_TCPIP_CORE();
            *optlen = sizeof(int);
            return 0;
        case TCP_KEEPINTVL:
            *(int *)optval = (int)(sock->conn->pcb.tcp->keep_intvl / 1000U);
            UNLOCK_TCPIP_CORE();
            *optlen = sizeof(int);
            return 0;
        case TCP_KEEPCNT:
            *(int *)optval = (int)sock->conn->pcb.tcp->keep_cnt;
            UNLOCK_TCPIP_CORE();
            *optlen = sizeof(int);
            return 0;
        default:
            UNLOCK_TCPIP_CORE();
            break;
        }
    }

    if (level == IPPROTO_IPV6 && sock->domain == AF_INET6) {
        switch (optname) {
        case IPV6_V6ONLY:
            value = netconn_get_ipv6only(sock->conn) ? 1 : 0;
            break;
        case IPV6_UNICAST_HOPS:
            LOCK_TCPIP_CORE();
            if (!sock->conn->pcb.ip) {
                UNLOCK_TCPIP_CORE();
                return -EINVAL;
            }
            value = sock->conn->pcb.ip->ttl;
            UNLOCK_TCPIP_CORE();
            break;
        case IPV6_RECVPKTINFO:
        case IPV6_PKTINFO:
            value = sock->ipv6_pktinfo ? 1 : 0;
            break;
        case IPV6_RECVERR:
            value = sock->ipv6_recverr ? 1 : 0;
            break;
        case IPV6_MTU_DISCOVER:
            value = sock->ipv6_mtu_discover;
            break;
        default:
            return -ENOPROTOOPT;
        }

        if (*optlen < sizeof(int)) {
            return -EINVAL;
        }
        *(int *)optval = value;
        *optlen = sizeof(int);
        return 0;
    }

    return -ENOPROTOOPT;
}

static int lwip_socket_fetch_tcp(lwip_socket_state_t *sock, int flags) {
    err_t err = ERR_OK;
    u8_t recv_flags = NETCONN_NOAUTORCVD;
    bool peer_closed = false;
    bool shut_rd = false;

    if (flags & MSG_DONTWAIT) {
        recv_flags |= NETCONN_DONTBLOCK;
    }

    spin_lock(&sock->event_lock);
    peer_closed = sock->peer_closed;
    shut_rd = sock->shut_rd;
    spin_unlock(&sock->event_lock);

    if (shut_rd) {
        return 0;
    }

    if (peer_closed && lwip_socket_recv_avail(sock) <= 0) {
        return 0;
    }

    if (!sock->rx_pbuf) {
        err =
            netconn_recv_tcp_pbuf_flags(sock->conn, &sock->rx_pbuf, recv_flags);
        if (err == ERR_CLSD) {
            lwip_socket_mark_peer_closed(sock);
            return 0;
        }
        if (err != ERR_OK) {
            return lwip_errno_from_err(err);
        }
        sock->rx_pbuf_offset = 0;
        sock->rx_pbuf_announced = 0;
        lwip_socket_publish_rx_avail(sock);
    }

    return 0;
}

static int lwip_socket_fetch_datagram(lwip_socket_state_t *sock, int flags) {
    err_t err = ERR_OK;
    u8_t recv_flags = 0;

    if (flags & MSG_DONTWAIT) {
        recv_flags |= NETCONN_DONTBLOCK;
    }

    if (!sock->rx_netbuf) {
        err = netconn_recv_udp_raw_netbuf_flags(sock->conn, &sock->rx_netbuf,
                                                recv_flags);
        if (err != ERR_OK) {
            return lwip_errno_from_err(err);
        }
        sock->rx_netbuf_offset = 0;
        lwip_socket_publish_rx_avail(sock);
    }

    return 0;
}

static ssize_t lwip_socket_copy_pbuf_to_iov(const struct pbuf *p,
                                            size_t pbuf_offset, size_t len,
                                            struct iovec *iov, size_t iovlen,
                                            size_t *copied_total) {
    const struct pbuf *q = p;
    size_t q_offset = pbuf_offset;
    size_t iov_index = 0;
    size_t iov_offset = 0;
    size_t copied = 0;

    while (q && q_offset >= q->len) {
        q_offset -= q->len;
        q = q->next;
    }

    while (q && copied < len && iov_index < iovlen) {
        size_t q_avail = q->len - q_offset;
        const uint8_t *src = (const uint8_t *)q->payload + q_offset;

        while (q_avail && copied < len && iov_index < iovlen) {
            while (iov_index < iovlen && iov_offset >= iov[iov_index].len) {
                iov_index++;
                iov_offset = 0;
            }
            if (iov_index >= iovlen)
                break;

            size_t part = MIN(q_avail, iov[iov_index].len - iov_offset);
            part = MIN(part, len - copied);
            int ret = lwip_socket_copy_to_buffer(
                (uint8_t *)iov[iov_index].iov_base + iov_offset, src, part);
            if (ret < 0)
                return ret;

            src += part;
            q_avail -= part;
            iov_offset += part;
            copied += part;
        }

        q = q->next;
        q_offset = 0;
    }

    if (copied_total)
        *copied_total = copied;
    return (ssize_t)copied;
}

static int lwip_socket_iov_total(const struct iovec *iov, size_t iovlen,
                                 size_t *out_total) {
    size_t total = 0;

    if (!out_total) {
        return -EINVAL;
    }

    *out_total = 0;
    if (iovlen && !iov) {
        return -EFAULT;
    }

    for (size_t i = 0; i < iovlen; i++) {
        if (iov[i].len > SIZE_MAX - total) {
            return -EMSGSIZE;
        }
        total += iov[i].len;
    }

    *out_total = total;
    return 0;
}

static ssize_t lwip_socket_recvmsg_common(lwip_socket_state_t *sock, fd_t *fd,
                                          struct msghdr *msg, int flags) {
    size_t total = 0;
    size_t want = 0;
    int total_ret = lwip_socket_iov_total(msg->msg_iov, msg->msg_iovlen, &want);
    if (total_ret < 0) {
        return total_ret;
    }

    flags = lwip_socket_apply_fd_flags(fd, flags);
    msg->msg_flags = 0;

    if (flags & MSG_ERRQUEUE) {
        return -EAGAIN;
    }

    if (lwip_socket_is_tcp(sock)) {
        int ret = lwip_socket_fetch_tcp(sock, flags);
        if (ret < 0) {
            return ret;
        }
        if (!sock->rx_pbuf) {
            return 0;
        }

        size_t avail = sock->rx_pbuf->tot_len - sock->rx_pbuf_offset;
        size_t take = MIN(avail, want);
        ssize_t copied = lwip_socket_copy_pbuf_to_iov(
            sock->rx_pbuf, sock->rx_pbuf_offset, take, msg->msg_iov,
            msg->msg_iovlen, &total);
        if (copied < 0)
            return copied;

        if (!(flags & MSG_PEEK)) {
            sock->rx_pbuf_offset += total;
            if (total > 0) {
                netconn_tcp_recvd(sock->conn, total);
            }
            if (sock->rx_pbuf_offset >= sock->rx_pbuf->tot_len) {
                pbuf_free(sock->rx_pbuf);
                sock->rx_pbuf = NULL;
                sock->rx_pbuf_offset = 0;
            }
            sock->rx_pbuf_announced = 0;
            lwip_socket_publish_rx_avail(sock);
        }

        if (msg->msg_name && msg->msg_namelen > 0) {
            socklen_t namelen = (socklen_t)msg->msg_namelen;
            ip_addr_t addr;
            u16_t port;
            if (netconn_peer(sock->conn, &addr, &port) == ERR_OK) {
                lwip_ip_to_sockaddr(&addr, port, msg->msg_name, &namelen,
                                    sock->domain);
                msg->msg_namelen = namelen;
            } else {
                msg->msg_namelen = 0;
            }
        }

        return (ssize_t)total;
    }

    {
        int ret = lwip_socket_fetch_datagram(sock, flags);
        if (ret < 0) {
            return ret;
        }
        if (!sock->rx_netbuf) {
            return 0;
        }

        size_t avail = netbuf_len(sock->rx_netbuf) - sock->rx_netbuf_offset;
        size_t take = MIN(avail, want);
        ssize_t copied = lwip_socket_copy_pbuf_to_iov(
            sock->rx_netbuf->p, sock->rx_netbuf_offset, take, msg->msg_iov,
            msg->msg_iovlen, &total);
        if (copied < 0)
            return copied;
        if (avail > want) {
            msg->msg_flags |= MSG_TRUNC;
        }

        if (msg->msg_name && msg->msg_namelen > 0) {
            socklen_t namelen = (socklen_t)msg->msg_namelen;
            lwip_ip_to_sockaddr(netbuf_fromaddr(sock->rx_netbuf),
                                netbuf_fromport(sock->rx_netbuf), msg->msg_name,
                                &namelen, sock->domain);
            msg->msg_namelen = namelen;
        }

        if (!(flags & MSG_PEEK)) {
            netbuf_delete(sock->rx_netbuf);
            sock->rx_netbuf = NULL;
            sock->rx_netbuf_offset = 0;
            lwip_socket_publish_rx_avail(sock);
        }

        return (flags & MSG_TRUNC) ? (ssize_t)avail : (ssize_t)total;
    }
}

static ssize_t lwip_socket_sendmsg_common(lwip_socket_state_t *sock, fd_t *fd,
                                          const struct msghdr *msg, int flags) {
    err_t err = ERR_OK;
    size_t written = 0;

    flags = lwip_socket_apply_fd_flags(fd, flags);

    if (lwip_socket_is_tcp(sock)) {
        struct iovec *bounce_iov = NULL;
        void **bounce_buffers = NULL;
        u8_t write_flags = NETCONN_COPY;
        int ret = 0;

        ret = lwip_socket_build_bounce_iov(msg, &bounce_iov, &bounce_buffers);
        if (ret < 0) {
            return ret;
        }

        if (flags & MSG_DONTWAIT) {
            write_flags |= NETCONN_DONTBLOCK;
        }
        if (flags & MSG_MORE) {
            write_flags |= NETCONN_MORE;
        }

        err = netconn_write_vectors_partly(
            sock->conn, (struct netvector *)bounce_iov, (u16_t)msg->msg_iovlen,
            write_flags, &written);
        lwip_socket_free_bounce_iov(bounce_iov, bounce_buffers,
                                    msg->msg_iovlen);
        if (err != ERR_OK) {
            return lwip_errno_from_err(err);
        }
        return (ssize_t)written;
    }

    {
        ip_addr_t dst;
        u16_t port = 0;
        bool has_dest = false;
        size_t total = 0;
        struct netbuf *buf = NULL;
        void *payload = NULL;
        int total_ret =
            lwip_socket_iov_total(msg->msg_iov, msg->msg_iovlen, &total);

        if (total_ret < 0) {
            return total_ret;
        }

        buf = netbuf_new();
        if (!buf) {
            return -ENOMEM;
        }
        if (total > 0xFFFF) {
            netbuf_delete(buf);
            return -EMSGSIZE;
        }

        if (msg->msg_name && msg->msg_namelen) {
            int ret =
                lwip_sockaddr_to_ip(msg->msg_name, (socklen_t)msg->msg_namelen,
                                    sock->domain, &dst, &port);
            if (ret < 0) {
                netbuf_delete(buf);
                return ret;
            }
            has_dest = true;
        } else {
            if (!sock->connected) {
                netbuf_delete(buf);
                return -EDESTADDRREQ;
            }
            ip_addr_set_any(NETCONNTYPE_ISIPV6(netconn_type(sock->conn)), &dst);
        }

        payload = netbuf_alloc(buf, (u16_t)total);
        if (!payload && total) {
            netbuf_delete(buf);
            return -ENOMEM;
        }

        written = 0;
        for (size_t i = 0; i < msg->msg_iovlen; i++) {
            if (msg->msg_iov[i].len > total - written) {
                netbuf_delete(buf);
                return -EMSGSIZE;
            }
            if (msg->msg_iov[i].len) {
                memcpy((uint8_t *)payload + written, msg->msg_iov[i].iov_base,
                       msg->msg_iov[i].len);
            }
            written += msg->msg_iov[i].len;
        }

        if (has_dest) {
            err = netconn_sendto(sock->conn, buf, &dst, port);
        } else {
            ip_addr_set(&buf->addr, &dst);
            netbuf_fromport(buf) = 0;
            err = netconn_send(sock->conn, buf);
        }
        netbuf_delete(buf);
        if (err != ERR_OK) {
            return lwip_errno_from_err(err);
        }
        return (ssize_t)written;
    }
}

static lwip_socket_state_t *lwip_socket_state_from_node(vfs_node_t *node) {
    socket_handle_t *handle = sockfs_inode_socket_handle(node);
    return handle ? (lwip_socket_state_t *)handle->sock : NULL;
}

static lwip_socket_state_t *lwip_socket_state_from_file(fd_t *file) {
    socket_handle_t *handle = sockfs_file_handle(file);
    return handle ? (lwip_socket_state_t *)handle->sock : NULL;
}

static fd_t *lwip_socket_file_from_fd(uint64_t fd,
                                      lwip_socket_state_t **sock_out) {
    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file) {
        return NULL;
    }

    lwip_socket_state_t *sock = lwip_socket_state_from_file(file);
    if (!sock) {
        vfs_file_put(file);
        return NULL;
    }

    if (sock_out)
        *sock_out = sock;
    return file;
}

static int lwip_socket_socket(int domain, int type, int protocol) {
    struct netconn *conn = NULL;
    lwip_socket_state_t *sock = NULL;

    conn = lwip_socket_new_conn(domain, type, protocol);
    if (!conn) {
        return -ESOCKTNOSUPPORT;
    }

    sock = lwip_socket_alloc(conn, domain, type, protocol);
    if (!sock) {
        netconn_delete(conn);
        return -ENOMEM;
    }

    return lwip_socket_install_fd(sock, type, 0);
}

static int lwip_socket_socketpair(int family, int type, int protocol, int *sv) {
    LWIP_UNUSED_ARG(family);
    LWIP_UNUSED_ARG(type);
    LWIP_UNUSED_ARG(protocol);
    LWIP_UNUSED_ARG(sv);
    return -EOPNOTSUPP;
}

static int lwip_socket_bind(uint64_t fd, const struct sockaddr_un *addr,
                            socklen_t addrlen) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    ip_addr_t ipaddr;
    u16_t port = 0;
    int ret = 0;

    if (!file) {
        return -EBADF;
    }

    ret = lwip_sockaddr_to_ip(addr, addrlen, sock->domain, &ipaddr, &port);
    if (ret < 0) {
        vfs_file_put(file);
        return ret;
    }

    ret = lwip_errno_from_err(netconn_bind(sock->conn, &ipaddr, port));
    vfs_file_put(file);
    return ret;
}

static int lwip_socket_listen(uint64_t fd, int backlog) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);

    if (!file) {
        return -EBADF;
    }
    if (!lwip_socket_is_tcp(sock)) {
        vfs_file_put(file);
        return -EOPNOTSUPP;
    }

    if (backlog <= 0) {
        backlog = 1;
    }

    if (netconn_listen_with_backlog(sock->conn, (u8_t)MIN(backlog, 255)) !=
        ERR_OK) {
        vfs_file_put(file);
        return -EIO;
    }
    sock->listening = true;
    vfs_file_put(file);
    return 0;
}

static int lwip_socket_accept(uint64_t fd, struct sockaddr_un *addr,
                              socklen_t *addrlen, uint64_t flags) {
    lwip_socket_state_t *listener = NULL;
    fd_t *listener_fd = lwip_socket_file_from_fd(fd, &listener);
    struct netconn *accepted = NULL;
    lwip_socket_state_t *sock = NULL;
    int newfd = 0;

    if (!listener_fd) {
        return -EBADF;
    }

    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        vfs_file_put(listener_fd);
        return -EINVAL;
    }

    err_t accept_err = netconn_accept(listener->conn, &accepted);
    if (accept_err != ERR_OK) {
        vfs_file_put(listener_fd);
        return lwip_errno_from_err(accept_err);
    }

    sock = lwip_socket_alloc(accepted, listener->domain, listener->type,
                             listener->protocol);
    if (!sock) {
        netconn_delete(accepted);
        vfs_file_put(listener_fd);
        return -ENOMEM;
    }

    if (lwip_socket_is_tcp(sock)) {
        spin_lock(&sock->event_lock);
        sock->sendevent = 1;
        spin_unlock(&sock->event_lock);
    }

    newfd = lwip_socket_install_fd(
        sock, listener_fd ? (int)fd_get_flags(listener_fd) : O_RDWR, flags);
    if (newfd < 0) {
        vfs_file_put(listener_fd);
        return newfd;
    }

    if (addr && addrlen) {
        ip_addr_t peer_addr;
        u16_t peer_port = 0;
        socklen_t outlen = *addrlen;
        if (netconn_peer(sock->conn, &peer_addr, &peer_port) == ERR_OK) {
            lwip_ip_to_sockaddr(&peer_addr, peer_port, addr, &outlen,
                                sock->domain);
            *addrlen = outlen;
        } else {
            *addrlen = 0;
        }
    }

    vfs_file_put(listener_fd);
    return newfd;
}

static int lwip_socket_connect(uint64_t fd, const struct sockaddr_un *addr,
                               socklen_t addrlen) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    ip_addr_t ipaddr;
    u16_t port = 0;
    int ret = 0;
    enum tcp_state state = CLOSED;

    if (!file) {
        return -EBADF;
    }

    if (addr && addrlen >= sizeof(uint16_t) &&
        ((const struct sockaddr *)addr)->sa_family == AF_UNSPEC) {
        if (lwip_socket_is_tcp(sock)) {
            vfs_file_put(file);
            return -EINVAL;
        }
        ret = lwip_errno_from_err(netconn_disconnect(sock->conn));
        if (ret == 0) {
            sock->connected = false;
            sock->peer_port = 0;
            ip_addr_set_zero(&sock->peer_addr);
        }
        vfs_file_put(file);
        return ret;
    }

    ret = lwip_sockaddr_to_ip(addr, addrlen, sock->domain, &ipaddr, &port);
    if (ret < 0) {
        vfs_file_put(file);
        return ret;
    }

    if (lwip_socket_is_tcp(sock)) {
        LOCK_TCPIP_CORE();
        if (sock->conn->pcb.tcp) {
            state = sock->conn->pcb.tcp->state;
        }
        UNLOCK_TCPIP_CORE();

        if (state != CLOSED) {
            if (state == SYN_SENT || state == SYN_RCVD) {
                vfs_file_put(file);
                return -EALREADY;
            }
            vfs_file_put(file);
            return -EISCONN;
        }
    }

    ret = lwip_errno_from_err(netconn_connect(sock->conn, &ipaddr, port));
    if (ret == 0) {
        sock->connected = true;
        ip_addr_set(&sock->peer_addr, &ipaddr);
        sock->peer_port = port;
    }
    vfs_file_put(file);
    return ret;
}

static size_t lwip_socket_sendto(uint64_t fd, uint8_t *in, size_t limit,
                                 int flags, struct sockaddr_un *addr,
                                 uint32_t len) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    struct iovec iov = {.iov_base = in, .len = limit};
    struct msghdr msg = {0};

    if (!file) {
        return (size_t)-EBADF;
    }

    msg.msg_name = addr;
    msg.msg_namelen = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (!lwip_socket_io_lock(&sock->tx_lock, file, flags)) {
        vfs_file_put(file);
        return (size_t)-EAGAIN;
    }
    size_t ret = (size_t)lwip_socket_sendmsg_common(sock, file, &msg, flags);
    wait_mutex_unlock(&sock->tx_lock);
    vfs_file_put(file);
    return ret;
}

static size_t lwip_socket_recvfrom(uint64_t fd, uint8_t *out, size_t limit,
                                   int flags, struct sockaddr_un *addr,
                                   uint32_t *len) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    struct iovec iov = {.iov_base = out, .len = limit};
    struct msghdr msg = {0};
    socklen_t namelen = len ? *len : 0;
    ssize_t ret = 0;

    if (!file) {
        return (size_t)-EBADF;
    }

    msg.msg_name = addr;
    msg.msg_namelen = namelen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (!lwip_socket_io_lock(&sock->rx_lock, file, flags)) {
        vfs_file_put(file);
        return (size_t)-EAGAIN;
    }
    ret = lwip_socket_recvmsg_common(sock, file, &msg, flags);
    wait_mutex_unlock(&sock->rx_lock);
    if (ret >= 0 && len) {
        *len = (uint32_t)msg.msg_namelen;
    }
    vfs_file_put(file);
    return (size_t)ret;
}

static size_t lwip_socket_sendmsg(uint64_t fd, const struct msghdr *msg,
                                  int flags) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    if (!file) {
        return (size_t)-EBADF;
    }
    if (!lwip_socket_io_lock(&sock->tx_lock, file, flags)) {
        vfs_file_put(file);
        return (size_t)-EAGAIN;
    }
    size_t ret = (size_t)lwip_socket_sendmsg_common(sock, file, msg, flags);
    wait_mutex_unlock(&sock->tx_lock);
    vfs_file_put(file);
    return ret;
}

static size_t lwip_socket_recvmsg(uint64_t fd, struct msghdr *msg, int flags) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    if (!file) {
        return (size_t)-EBADF;
    }
    if (!lwip_socket_io_lock(&sock->rx_lock, file, flags)) {
        vfs_file_put(file);
        return (size_t)-EAGAIN;
    }
    size_t ret = (size_t)lwip_socket_recvmsg_common(sock, file, msg, flags);
    wait_mutex_unlock(&sock->rx_lock);
    vfs_file_put(file);
    return ret;
}

static int lwip_socket_getsockname(uint64_t fd, struct sockaddr_un *addr,
                                   socklen_t *addrlen) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    ip_addr_t ipaddr;
    u16_t port = 0;

    if (!file) {
        return -EBADF;
    }
    if (!addrlen) {
        vfs_file_put(file);
        return -EFAULT;
    }

    if (netconn_addr(sock->conn, &ipaddr, &port) != ERR_OK) {
        vfs_file_put(file);
        return -ENOTCONN;
    }

    int ret = lwip_ip_to_sockaddr(&ipaddr, port, addr, addrlen, sock->domain);
    vfs_file_put(file);
    return ret;
}

static size_t lwip_socket_getpeername(uint64_t fd, struct sockaddr_un *addr,
                                      socklen_t *addrlen) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    ip_addr_t ipaddr;
    u16_t port = 0;

    if (!file) {
        return (size_t)-EBADF;
    }
    if (!addrlen) {
        vfs_file_put(file);
        return (size_t)-EFAULT;
    }

    if (netconn_peer(sock->conn, &ipaddr, &port) != ERR_OK) {
        if (!sock->connected) {
            vfs_file_put(file);
            return (size_t)-ENOTCONN;
        }
        ip_addr_set(&ipaddr, &sock->peer_addr);
        port = sock->peer_port;
    }

    size_t ret =
        (size_t)lwip_ip_to_sockaddr(&ipaddr, port, addr, addrlen, sock->domain);
    vfs_file_put(file);
    return ret;
}

static uint64_t lwip_socket_shutdown(uint64_t fd, uint64_t how) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    u8_t shut_rx = 0;
    u8_t shut_tx = 0;
    uint32_t notify_events = 0;
    err_t err = ERR_OK;

    if (!file) {
        return -EBADF;
    }
    if (!lwip_socket_is_tcp(sock)) {
        vfs_file_put(file);
        return -EOPNOTSUPP;
    }
    if (how > SHUT_RDWR) {
        vfs_file_put(file);
        return -EINVAL;
    }

    shut_rx = (how == SHUT_RD || how == SHUT_RDWR) ? 1 : 0;
    shut_tx = (how == SHUT_WR || how == SHUT_RDWR) ? 1 : 0;
    err = netconn_shutdown(sock->conn, shut_rx, shut_tx);
    if (err != ERR_OK) {
        vfs_file_put(file);
        return lwip_errno_from_err(err);
    }

    lwip_socket_set_shutdown(sock, shut_rx != 0, shut_tx != 0);
    if (shut_rx) {
        notify_events |= EPOLLIN;
    }
    if (shut_tx) {
        notify_events |= EPOLLOUT;
    }
    lwip_socket_notify(sock, notify_events);
    vfs_file_put(file);
    return 0;
}

static size_t lwip_socket_setsockopt(uint64_t fd, int level, int optname,
                                     const void *optval, socklen_t optlen) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    if (!file) {
        return (size_t)-EBADF;
    }
    size_t ret = (size_t)lwip_socket_setsockopt_impl(sock, level, optname,
                                                     optval, optlen);
    vfs_file_put(file);
    return ret;
}

static size_t lwip_socket_getsockopt(uint64_t fd, int level, int optname,
                                     void *optval, socklen_t *optlen) {
    lwip_socket_state_t *sock = NULL;
    fd_t *file = lwip_socket_file_from_fd(fd, &sock);
    if (!file) {
        return (size_t)-EBADF;
    }
    size_t ret = (size_t)lwip_socket_getsockopt_impl(sock, level, optname,
                                                     optval, optlen);
    vfs_file_put(file);
    return ret;
}

static int lwip_socket_poll(fd_t *fd, size_t events) {
    vfs_node_t *node = fd ? fd->node : NULL;
    lwip_socket_state_t *sock = lwip_socket_state_from_node(node);
    int revents = 0;
    s16_t rcvevent = 0;
    u16_t sendevent = 0;
    u16_t errevent = 0;
    int pending_error = 0;
    bool peer_closed = false;
    bool shut_rd = false;
    bool shut_wr = false;

    if (!sock) {
        return EPOLLNVAL;
    }

    spin_lock(&sock->event_lock);
    rcvevent = sock->rcvevent;
    sendevent = sock->sendevent;
    errevent = sock->errevent;
    pending_error = sock->pending_error;
    peer_closed = sock->peer_closed;
    shut_rd = sock->shut_rd;
    shut_wr = sock->shut_wr;
    spin_unlock(&sock->event_lock);

    if ((events & EPOLLIN) &&
        (rcvevent > 0 || lwip_socket_recv_avail(sock) > 0 || peer_closed ||
         shut_rd)) {
        revents |= EPOLLIN;
    }

    if ((events & EPOLLOUT) && sendevent && !shut_wr) {
        revents |= EPOLLOUT;
    }

    if (errevent || pending_error != 0) {
        revents |= EPOLLERR;
    }

    if (sock->closed) {
        revents |= EPOLLHUP | EPOLLRDHUP;
    } else if (peer_closed) {
        revents |= EPOLLRDHUP;
    }

    return revents;
}

static int lwip_socket_ioctl(fd_t *fd, ssize_t cmd, ssize_t arg) {
    lwip_socket_state_t *sock = lwip_socket_state_from_file(fd);

    if (!sock) {
        return -EBADF;
    }

    cmd &= 0xFFFFFFFF;

    if (cmd == FIONREAD) {
        int value = lwip_socket_recv_avail(sock);
        if (copy_to_user((void *)arg, &value, sizeof(value))) {
            return -EFAULT;
        }
        return 0;
    }

    if (cmd == FIONBIO) {
        int value = 0;

        if (arg == FIONBIO_INTERNAL_DISABLE) {
            value = 0;
        } else if (arg == FIONBIO_INTERNAL_ENABLE) {
            value = 1;
        } else {
            if (!arg || check_user_overflow((uint64_t)arg, sizeof(value)) ||
                check_unmapped((uint64_t)arg, sizeof(value)) ||
                copy_from_user(&value, (const void *)arg, sizeof(value))) {
                return -EFAULT;
            }
        }

        netconn_set_nonblocking(sock->conn, value ? 1 : 0);
        return 0;
    }

    if (cmd == SIOCGIFCONF) {
        return lwip_socket_ioctl_ifconf(arg);
    }

    if (cmd == SIOCGIFFLAGS || cmd == SIOCGIFBRDADDR || cmd == SIOCGIFNETMASK ||
        cmd == SIOCGIFMTU || cmd == SIOCGIFHWADDR) {
        return lwip_socket_ioctl_ifreq(cmd, arg);
    }

    if (cmd == SIOCGIFINDEX) {
        naos_ifreq_t req;
        netdev_t *dev;

        if (!arg || copy_from_user(&req, (const void *)arg, sizeof(req))) {
            return -EFAULT;
        }

        req.ifr_name[IFNAMSIZ - 1] = '\0';
        dev = netdev_get_by_name(req.ifr_name);
        if (!dev) {
            return -ENODEV;
        }

        req.ifr_ifru.ifru_ifindex = (int)(dev->id + 1);
        netdev_put(dev);

        if (copy_to_user((void *)arg, &req, sizeof(req))) {
            return -EFAULT;
        }
        return 0;
    }

    if (cmd == SIOCGIWNAME || cmd == SIOCGIWMODE || cmd == SIOCGIWAP ||
        cmd == SIOCGIWESSID) {
        naos_iwreq_t req;
        netdev_t *dev;
        netdev_wireless_info_t wireless;

        if (!arg || copy_from_user(&req, (const void *)arg, sizeof(req))) {
            return -EFAULT;
        }

        req.ifr_ifrn.ifrn_name[IFNAMSIZ - 1] = '\0';
        dev = netdev_get_by_name(req.ifr_ifrn.ifrn_name);
        if (!dev) {
            return -ENODEV;
        }

        if (!netdev_get_wireless_info(dev, &wireless)) {
            netdev_put(dev);
            return -EOPNOTSUPP;
        }

        if (cmd == SIOCGIWNAME) {
            memset(req.u.name, 0, sizeof(req.u.name));
            strncpy(req.u.name, "IEEE 802.11", IFNAMSIZ - 1);
        } else if (cmd == SIOCGIWMODE) {
            req.u.mode = IW_MODE_INFRA;
        } else if (cmd == SIOCGIWAP) {
            memset(&req.u.ap_addr, 0, sizeof(req.u.ap_addr));
            req.u.ap_addr.sa_family = ARPHRD_ETHER;
            if (wireless.connected) {
                memcpy(req.u.ap_addr.sa_data, wireless.bssid,
                       sizeof(wireless.bssid));
            }
        } else if (cmd == SIOCGIWESSID) {
            req.u.essid.flags = wireless.connected && wireless.ssid_len ? 1 : 0;
            req.u.essid.length = wireless.ssid_len;
            if (req.u.essid.pointer && wireless.ssid_len) {
                if (copy_to_user(req.u.essid.pointer, wireless.ssid,
                                 wireless.ssid_len)) {
                    netdev_put(dev);
                    return -EFAULT;
                }
            }
        }
        netdev_put(dev);

        if (copy_to_user((void *)arg, &req, sizeof(req))) {
            return -EFAULT;
        }
        return 0;
    }

    printk("Unknown lwip socket ioctl: %#010x\n", cmd);

    return -ENOTTY;
}

static void lwip_socket_handle_release(socket_handle_t *handle) {
    lwip_socket_state_t *sock = NULL;

    if (!handle) {
        return;
    }

    sock = (lwip_socket_state_t *)handle->sock;

    if (!sock) {
        free(handle);
        return;
    }

    sock->closed = true;
    lwip_socket_notify(sock, EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
    sock->node = NULL;
    lwip_socket_release_conn(sock);

    free(sock);
    free(handle);
}

static ssize_t lwip_socket_read(fd_t *fd, void *buf, size_t offset,
                                size_t limit) {
    struct iovec iov = {.iov_base = buf, .len = limit};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    lwip_socket_state_t *sock = lwip_socket_state_from_file(fd);
    int flags = 0;

    LWIP_UNUSED_ARG(offset);

    if (!sock) {
        return -EBADF;
    }

    if (fd_get_flags(fd) & O_NONBLOCK) {
        flags |= MSG_DONTWAIT;
    }

    if (!lwip_socket_io_lock(&sock->rx_lock, fd, flags))
        return -EAGAIN;
    ssize_t ret = lwip_socket_recvmsg_common(sock, fd, &msg, flags);
    wait_mutex_unlock(&sock->rx_lock);
    return ret;
}

static ssize_t lwip_socket_write(fd_t *fd, const void *buf, size_t offset,
                                 size_t limit) {
    struct iovec iov = {.iov_base = (void *)buf, .len = limit};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    lwip_socket_state_t *sock = lwip_socket_state_from_file(fd);
    int flags = 0;

    LWIP_UNUSED_ARG(offset);

    if (!sock) {
        return -EBADF;
    }
    if (!limit) {
        return 0;
    }

    if (fd_get_flags(fd) & O_NONBLOCK) {
        flags |= MSG_DONTWAIT;
    }

    if (!lwip_socket_io_lock(&sock->tx_lock, fd, flags))
        return -EAGAIN;
    ssize_t ret = lwip_socket_sendmsg_common(sock, fd, &msg, flags);
    wait_mutex_unlock(&sock->tx_lock);
    return ret;
}

static socket_op_t lwip_socket_ops = {
    .shutdown = lwip_socket_shutdown,
    .getpeername = lwip_socket_getpeername,
    .getsockname = lwip_socket_getsockname,
    .bind = lwip_socket_bind,
    .listen = lwip_socket_listen,
    .accept = lwip_socket_accept,
    .connect = lwip_socket_connect,
    .sendto = lwip_socket_sendto,
    .recvfrom = lwip_socket_recvfrom,
    .recvmsg = lwip_socket_recvmsg,
    .sendmsg = lwip_socket_sendmsg,
    .getsockopt = lwip_socket_getsockopt,
    .setsockopt = lwip_socket_setsockopt,
};

extern int lwip_module_init();

void real_socket_v4_init(void) {
    regist_socket(AF_INET, lwip_module_init, lwip_socket_socket,
                  lwip_socket_socketpair);
}

void real_socket_v6_init(void) {
    regist_socket(AF_INET6, lwip_module_init, lwip_socket_socket,
                  lwip_socket_socketpair);
}
