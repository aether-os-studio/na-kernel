#include <net/netlink.h>
#include <task/task.h>
#include <mm/mm.h>
#include <arch/arch.h>
#include <libs/klibc.h>
#include <fs/fs_syscall.h>
#include <fs/proc.h>
#include <net/netdev.h>
#include <net/real_socket.h>
#include <net/socket.h>
#include <bpf/socket_filter.h>
#include <init/callbacks.h>

// Global netlink socket tracking
#define MAX_NETLINK_SOCKETS 256
static struct netlink_sock *netlink_sockets[MAX_NETLINK_SOCKETS] = {0};
static spinlock_t netlink_sockets_lock = SPIN_INIT;

#define MAX_NETLINK_MSG_POOL_SIZE 1024
#define SIOCGSKNS 0x894C

#define AUDIT_GET 1000
#define AUDIT_SIGNAL_INFO 1010
#define AUDIT_GET_FEATURE 1019
#define AUDIT_FIRST_USER_MSG 1100
#define AUDIT_LAST_USER_MSG 1199
#define AUDIT_FIRST_USER_MSG2 2100
#define AUDIT_LAST_USER_MSG2 2999

struct netlink_msg_pool_entry {
    char message[NETLINK_BUFFER_SIZE];
    size_t length;
    uint64_t timestamp;
    uint32_t seqnum;    // For uevent, 0 for others
    char devpath[256];  // For uevent, empty for others
    uint32_t nl_pid;    // Sender's port ID
    uint32_t nl_groups; // Target groups (bitmask)
    int protocol;       // Netlink protocol (NETLINK_KOBJECT_UEVENT, etc.)
    bool valid;
};

static struct netlink_msg_pool_entry
    netlink_msg_pool[MAX_NETLINK_MSG_POOL_SIZE];
static uint32_t netlink_msg_pool_next = 0;
static spinlock_t netlink_msg_pool_lock = SPIN_INIT;

static void netlink_filter_free(struct sock_fprog *filter) {
    if (!filter)
        return;
    free(filter->filter);
    free(filter);
}

static void netlink_handle_release(socket_handle_t *handle);
static ssize_t netlink_read_op(fd_t *fd, void *buf, size_t offset,
                               size_t count);
static ssize_t netlink_write_op(fd_t *fd, const void *buf, size_t offset,
                                size_t count);
int netlink_ioctl(fd_t *fd, ssize_t cmd, ssize_t arg);
int netlink_poll(fd_t *fd, size_t events);
static int netlink_wait_file(fd_t *file, struct netlink_sock *sock,
                             uint32_t events, const char *reason);
static bool netlink_buffer_has_msg(struct netlink_sock *sock);
static size_t netlink_buffer_peek_msg_len(struct netlink_sock *sock);
static size_t netlink_deliver_to_socket(struct netlink_sock *target,
                                        const char *data, size_t len,
                                        uint32_t sender_pid,
                                        uint32_t sender_groups);
static size_t netlink_buffer_read_packet(struct netlink_sock *sock, char *out,
                                         size_t out_len, uint32_t *nl_pid,
                                         uint32_t *nl_groups, bool peek);

static inline void netlink_fill_sockaddr(struct sockaddr_nl *addr,
                                         uint32_t nl_pid, uint32_t nl_groups) {
    if (!addr)
        return;

    addr->nl_family = AF_NETLINK;
    addr->nl_pad = 0;
    addr->nl_pid = nl_pid;
    addr->nl_groups = nl_groups;
}

static bool netlink_addr_is_kernel(const struct sockaddr_nl *addr) {
    return addr && addr->nl_pid == 0 && addr->nl_groups == 0;
}

static struct netlink_sock *
netlink_lookup_sock_by_portid_locked(int protocol, uint32_t portid) {
    for (int i = 0; i < MAX_NETLINK_SOCKETS; i++) {
        struct netlink_sock *sock = netlink_sockets[i];
        if (!sock)
            continue;
        if (sock->protocol != protocol)
            continue;
        if (sock->portid == portid)
            return sock;
    }

    return NULL;
}

static inline uint32_t netlink_group_bit(uint32_t group) {
    if (group == 0 || group > 32)
        return 0;
    return 1U << (group - 1);
}

static inline bool netlink_is_zero_mac(const uint8_t *addr) {
    static const uint8_t zero_mac[6] = {0};

    return !addr || memcmp(addr, zero_mac, sizeof(zero_mac)) == 0;
}

static int netlink_append_raw_message(char *buf, size_t *offset,
                                      size_t capacity, uint16_t type,
                                      uint16_t flags, uint32_t seq,
                                      const void *payload, size_t payload_len) {
    struct nlmsghdr *nlh;
    size_t total_len;
    size_t start;

    if (!buf || !offset)
        return -EINVAL;

    total_len = NLMSG_SPACE(payload_len);
    if (*offset > capacity || total_len > capacity - *offset)
        return -EMSGSIZE;

    start = *offset;
    nlh = (struct nlmsghdr *)(buf + start);
    nlh->nlmsg_len = NLMSG_LENGTH(payload_len);
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = 0;

    if (payload && payload_len)
        memcpy(NLMSG_DATA(nlh), payload, payload_len);

    if (total_len > nlh->nlmsg_len)
        memset(buf + start + nlh->nlmsg_len, 0, total_len - nlh->nlmsg_len);

    *offset += total_len;
    return 0;
}

static int netlink_append_ack(char *buf, size_t *offset, size_t capacity,
                              const struct nlmsghdr *req, int error) {
    struct nlmsgerr err = {0};

    if (req)
        err.msg = *req;
    err.error = error;

    return netlink_append_raw_message(buf, offset, capacity, NLMSG_ERROR, 0,
                                      req ? req->nlmsg_seq : 0, &err,
                                      sizeof(err));
}

static int netlink_append_done(char *buf, size_t *offset, size_t capacity,
                               const struct nlmsghdr *req) {
    return netlink_append_raw_message(buf, offset, capacity, NLMSG_DONE, 0,
                                      req ? req->nlmsg_seq : 0, NULL, 0);
}

static void netlink_stamp_reply_portid(char *buf, size_t len, uint32_t portid) {
    struct nlmsghdr *nlh;
    size_t remaining;

    if (!buf || !len)
        return;

    remaining = len;
    nlh = (struct nlmsghdr *)buf;
    while (NLMSG_OK(nlh, remaining)) {
        nlh->nlmsg_pid = portid;
        nlh = NLMSG_NEXT(nlh, remaining);
    }
}

static int netlink_flush_reply_to_socket(struct netlink_sock *sock, char *buf,
                                         size_t *offset) {
    if (!sock || !buf || !offset)
        return -EINVAL;
    if (*offset == 0)
        return 0;
    netlink_stamp_reply_portid(buf, *offset, sock->portid);
    if (!netlink_deliver_to_socket(sock, buf, *offset, 0, 0))
        return -ENOBUFS;
    memset(buf, 0, *offset);
    *offset = 0;
    return 0;
}

static int netlink_append_attr(char *buf, size_t *offset, size_t capacity,
                               uint16_t type, const void *data, size_t len) {
    struct rtattr *rta;
    size_t total_len;

    if (!buf || !offset)
        return -EINVAL;

    total_len = RTA_SPACE(len);
    if (*offset > capacity || total_len > capacity - *offset)
        return -EMSGSIZE;

    rta = (struct rtattr *)(buf + *offset);
    rta->rta_len = (uint16_t)RTA_LENGTH(len);
    rta->rta_type = type;

    if (data && len)
        memcpy(RTA_DATA(rta), data, len);

    if (total_len > rta->rta_len)
        memset(buf + *offset + rta->rta_len, 0, total_len - rta->rta_len);

    *offset += total_len;
    return 0;
}

static uint32_t rtnetlink_dev_flags(const netdev_t *dev) {
    uint32_t flags = IFF_BROADCAST | IFF_MULTICAST;

    if (!dev)
        return flags;
    if (dev->admin_up)
        flags |= IFF_UP;
    if (dev->link_up)
        flags |= IFF_RUNNING | IFF_LOWER_UP;

    return flags;
}

static uint8_t rtnetlink_dev_operstate(const netdev_t *dev) {
    if (!dev)
        return IF_OPER_UNKNOWN;
    return dev->link_up ? IF_OPER_UP : IF_OPER_DOWN;
}

static int rtnetlink_append_link_msg(char *buf, size_t *offset, size_t capacity,
                                     const netdev_t *dev, uint16_t nlmsg_type,
                                     uint16_t nlmsg_flags, uint32_t seq) {
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;
    uint32_t mtu;
    uint32_t txqlen = 0;
    uint8_t operstate;
    uint8_t linkmode = 0;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !dev)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(buf, offset, capacity, nlmsg_type,
                                     nlmsg_flags, seq, NULL,
                                     sizeof(struct ifinfomsg));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    memset(ifi, 0, sizeof(*ifi));
    ifi->ifi_type = ARPHRD_ETHER;
    ifi->ifi_index = (int32_t)(dev->id + 1);
    ifi->ifi_flags = rtnetlink_dev_flags(dev);
    ifi->ifi_change = 0xFFFFFFFFU;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    ret = netlink_append_attr(buf, &cur, capacity, IFLA_IFNAME, dev->name,
                              strlen(dev->name) + 1);
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, IFLA_ADDRESS, dev->mac,
                              sizeof(dev->mac));
    if (ret < 0)
        return ret;

    if (dev->type == NETDEV_TYPE_WIFI) {
        ret = netlink_append_attr(buf, &cur, capacity, IFLA_WIRELESS, NULL, 0);
        if (ret < 0)
            return ret;
    }

    mtu = dev->mtu;
    ret = netlink_append_attr(buf, &cur, capacity, IFLA_MTU, &mtu, sizeof(mtu));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, IFLA_TXQLEN, &txqlen,
                              sizeof(txqlen));
    if (ret < 0)
        return ret;

    operstate = rtnetlink_dev_operstate(dev);
    ret = netlink_append_attr(buf, &cur, capacity, IFLA_OPERSTATE, &operstate,
                              sizeof(operstate));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, IFLA_LINKMODE, &linkmode,
                              sizeof(linkmode));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int rtnetlink_handle_getlink(struct netlink_sock *sender_sock,
                                    const struct nlmsghdr *req,
                                    const struct ifinfomsg *ifi_req) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    uint32_t seq = req ? req->nlmsg_seq : 0;
    bool dump = req && (req->nlmsg_flags & NLM_F_DUMP);
    size_t count = 0;
    size_t offset = 0;
    int ret = 0;
    char reply[NETLINK_BUFFER_SIZE];

    memset(reply, 0, sizeof(reply));

    if (!sender_sock)
        return -EINVAL;

    if (!dump && ifi_req && ifi_req->ifi_index > 0) {
        netdev_t *dev = netdev_get_by_index((uint32_t)ifi_req->ifi_index);

        if (!dev)
            return -ENODEV;

        ret = rtnetlink_append_link_msg(reply, &offset, sizeof(reply), dev,
                                        RTM_NEWLINK, 0, seq);
        netdev_put(dev);
        if (ret < 0)
            return ret;
    } else {
        count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));
        for (size_t i = 0; i < count; i++) {
            size_t prev_offset = offset;
            ret = rtnetlink_append_link_msg(reply, &offset, sizeof(reply),
                                            devs[i], RTM_NEWLINK, NLM_F_MULTI,
                                            seq);
            if (ret == -EMSGSIZE && prev_offset > 0) {
                ret = netlink_flush_reply_to_socket(sender_sock, reply,
                                                    &prev_offset);
                if (ret < 0)
                    break;
                offset = 0;
                ret = rtnetlink_append_link_msg(reply, &offset, sizeof(reply),
                                                devs[i], RTM_NEWLINK,
                                                NLM_F_MULTI, seq);
            }
            if (ret < 0)
                break;
        }

        for (size_t i = 0; i < count; i++)
            netdev_put(devs[i]);

        if (ret < 0)
            return ret;

        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
        if (ret == -EMSGSIZE && offset > 0) {
            ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
            if (ret < 0)
                return ret;
            ret = netlink_append_done(reply, &offset, sizeof(reply), req);
        }
        if (ret < 0)
            return ret;
    }

    ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
    if (ret < 0)
        return ret;

    return 0;
}

static int rtnetlink_handle_setlink(struct netlink_sock *sender_sock,
                                    const struct nlmsghdr *req,
                                    const struct ifinfomsg *ifi_req) {
    netdev_t *dev;
    int ret = 0;

    if (!sender_sock || !req || !ifi_req || ifi_req->ifi_index <= 0)
        return -EINVAL;

    dev = netdev_get_by_index((uint32_t)ifi_req->ifi_index);
    if (!dev) {
        printk("netlink: setlink ifindex=%d not found (REQUEST)\n",
               ifi_req->ifi_index);
        return -ENODEV;
    }

    if (ifi_req->ifi_change & IFF_UP)
        ret = netdev_set_admin_state(dev, !!(ifi_req->ifi_flags & IFF_UP));

    netdev_put(dev);
    if (ret < 0)
        return ret;

    return 0;
}

static int rtnetlink_handle_state_update(struct netlink_sock *sender_sock,
                                         const struct nlmsghdr *req) {
    (void)sender_sock;
    (void)req;
    return 0;
}

static uint8_t rtnetlink_prefixlen_from_netmask(uint32_t netmask_be) {
    uint32_t mask = __builtin_bswap32(netmask_be);
    uint8_t prefix = 0;

    while (mask & 0x80000000U) {
        prefix++;
        mask <<= 1;
    }

    return prefix;
}

static int rtnetlink_append_addr_msg(char *buf, size_t *offset, size_t capacity,
                                     const struct nlmsghdr *req,
                                     const netdev_t *dev,
                                     const netdev_ipv4_info_t *ipv4,
                                     uint16_t flags) {
    struct nlmsghdr *nlh;
    struct ifaddrmsg *ifa;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !req || !dev || !ipv4 || !ipv4->present)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(buf, offset, capacity, RTM_NEWADDR, flags,
                                     req->nlmsg_seq, NULL,
                                     sizeof(struct ifaddrmsg));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    memset(ifa, 0, sizeof(*ifa));
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = rtnetlink_prefixlen_from_netmask(ipv4->netmask);
    ifa->ifa_scope = RT_SCOPE_UNIVERSE;
    ifa->ifa_index = dev->id + 1;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);
    ret = netlink_append_attr(buf, &cur, capacity, IFA_ADDRESS, &ipv4->address,
                              sizeof(ipv4->address));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, IFA_LOCAL, &ipv4->address,
                              sizeof(ipv4->address));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, IFA_LABEL, dev->name,
                              strlen(dev->name) + 1);
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int rtnetlink_append_route_msg(char *buf, size_t *offset,
                                      size_t capacity,
                                      const struct nlmsghdr *req,
                                      const netdev_t *dev,
                                      const netdev_ipv4_info_t *ipv4,
                                      bool default_route, uint16_t flags) {
    struct nlmsghdr *nlh;
    struct rtmsg *rtm;
    uint32_t table = RT_TABLE_MAIN;
    uint32_t ifindex = dev->id + 1;
    uint32_t dst = ipv4->address & ipv4->netmask;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !req || !dev || !ipv4 || !ipv4->present)
        return -EINVAL;

    start = *offset;
    ret =
        netlink_append_raw_message(buf, offset, capacity, RTM_NEWROUTE, flags,
                                   req->nlmsg_seq, NULL, sizeof(struct rtmsg));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    rtm = (struct rtmsg *)NLMSG_DATA(nlh);
    memset(rtm, 0, sizeof(*rtm));
    rtm->rtm_family = AF_INET;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = RTPROT_KERNEL;
    rtm->rtm_scope = default_route ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_dst_len =
        default_route ? 0 : rtnetlink_prefixlen_from_netmask(ipv4->netmask);

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    if (!default_route) {
        ret = netlink_append_attr(buf, &cur, capacity, RTA_DST, &dst,
                                  sizeof(dst));
        if (ret < 0)
            return ret;
    } else {
        ret = netlink_append_attr(buf, &cur, capacity, RTA_GATEWAY,
                                  &ipv4->gateway, sizeof(ipv4->gateway));
        if (ret < 0)
            return ret;
    }

    ret = netlink_append_attr(buf, &cur, capacity, RTA_OIF, &ifindex,
                              sizeof(ifindex));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, RTA_PREFSRC, &ipv4->address,
                              sizeof(ipv4->address));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, RTA_TABLE, &table,
                              sizeof(table));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int rtnetlink_append_rule_msg(char *buf, size_t *offset, size_t capacity,
                                     const struct nlmsghdr *req, uint8_t table,
                                     uint32_t priority, uint16_t flags) {
    struct nlmsghdr *nlh;
    struct fib_rule_hdr *frh;
    uint32_t table32 = table;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !req)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(buf, offset, capacity, RTM_NEWRULE, flags,
                                     req->nlmsg_seq, NULL,
                                     sizeof(struct fib_rule_hdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    frh = (struct fib_rule_hdr *)NLMSG_DATA(nlh);
    memset(frh, 0, sizeof(*frh));
    frh->family = AF_INET;
    frh->table = table;
    frh->action = FR_ACT_TO_TBL;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    ret = netlink_append_attr(buf, &cur, capacity, FRA_TABLE, &table32,
                              sizeof(table32));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, FRA_PRIORITY, &priority,
                              sizeof(priority));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int rtnetlink_handle_getaddr(struct netlink_sock *sender_sock,
                                    const struct nlmsghdr *req) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    char reply[NETLINK_BUFFER_SIZE];
    size_t count;
    size_t offset = 0;
    int ret = 0;

    if (!sender_sock || !req)
        return -EINVAL;

    memset(reply, 0, sizeof(reply));
    count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));

    for (size_t i = 0; i < count; i++) {
        netdev_ipv4_info_t ipv4;

        if (!netdev_get_ipv4_info(devs[i], &ipv4))
            continue;

        size_t prev_offset = offset;
        ret = rtnetlink_append_addr_msg(reply, &offset, sizeof(reply), req,
                                        devs[i], &ipv4, NLM_F_MULTI);
        if (ret == -EMSGSIZE && prev_offset > 0) {
            ret =
                netlink_flush_reply_to_socket(sender_sock, reply, &prev_offset);
            if (ret < 0)
                break;
            offset = 0;
            ret = rtnetlink_append_addr_msg(reply, &offset, sizeof(reply), req,
                                            devs[i], &ipv4, NLM_F_MULTI);
        }
        if (ret < 0)
            break;
    }

    for (size_t i = 0; i < count; i++)
        netdev_put(devs[i]);

    if (ret < 0)
        return ret;

    ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    if (ret == -EMSGSIZE && offset > 0) {
        ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
        if (ret < 0)
            return ret;
        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    }
    if (ret < 0)
        return ret;

    ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
    if (ret < 0)
        return ret;

    return 0;
}

static int rtnetlink_handle_getroute(struct netlink_sock *sender_sock,
                                     const struct nlmsghdr *req) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    char reply[NETLINK_BUFFER_SIZE];
    size_t count;
    size_t offset = 0;
    int ret = 0;

    if (!sender_sock || !req)
        return -EINVAL;

    memset(reply, 0, sizeof(reply));
    count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));

    for (size_t i = 0; i < count; i++) {
        netdev_ipv4_info_t ipv4;

        if (!netdev_get_ipv4_info(devs[i], &ipv4))
            continue;

        size_t prev_offset = offset;
        ret = rtnetlink_append_route_msg(reply, &offset, sizeof(reply), req,
                                         devs[i], &ipv4, false, NLM_F_MULTI);
        if (ret == -EMSGSIZE && prev_offset > 0) {
            ret =
                netlink_flush_reply_to_socket(sender_sock, reply, &prev_offset);
            if (ret < 0)
                break;
            offset = 0;
            ret =
                rtnetlink_append_route_msg(reply, &offset, sizeof(reply), req,
                                           devs[i], &ipv4, false, NLM_F_MULTI);
        }
        if (ret < 0)
            break;

        if (ipv4.has_default_route) {
            prev_offset = offset;
            ret = rtnetlink_append_route_msg(reply, &offset, sizeof(reply), req,
                                             devs[i], &ipv4, true, NLM_F_MULTI);
            if (ret == -EMSGSIZE && prev_offset > 0) {
                ret = netlink_flush_reply_to_socket(sender_sock, reply,
                                                    &prev_offset);
                if (ret < 0)
                    break;
                offset = 0;
                ret = rtnetlink_append_route_msg(reply, &offset, sizeof(reply),
                                                 req, devs[i], &ipv4, true,
                                                 NLM_F_MULTI);
            }
            if (ret < 0)
                break;
        }
    }

    for (size_t i = 0; i < count; i++)
        netdev_put(devs[i]);

    if (ret < 0)
        return ret;

    ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    if (ret == -EMSGSIZE && offset > 0) {
        ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
        if (ret < 0)
            return ret;
        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    }
    if (ret < 0)
        return ret;

    ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
    if (ret < 0)
        return ret;

    return 0;
}

static int rtnetlink_handle_getrule(struct netlink_sock *sender_sock,
                                    const struct nlmsghdr *req) {
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    memset(reply, 0, sizeof(reply));

    ret = rtnetlink_append_rule_msg(reply, &offset, sizeof(reply), req,
                                    RT_TABLE_LOCAL, 0, NLM_F_MULTI);
    if (ret < 0)
        return ret;

    ret = rtnetlink_append_rule_msg(reply, &offset, sizeof(reply), req,
                                    RT_TABLE_MAIN, 32766, NLM_F_MULTI);
    if (ret == -EMSGSIZE && offset > 0) {
        ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
        if (ret < 0)
            return ret;
        ret = rtnetlink_append_rule_msg(reply, &offset, sizeof(reply), req,
                                        RT_TABLE_MAIN, 32766, NLM_F_MULTI);
    }
    if (ret < 0)
        return ret;

    ret = rtnetlink_append_rule_msg(reply, &offset, sizeof(reply), req,
                                    RT_TABLE_DEFAULT, 32767, NLM_F_MULTI);
    if (ret == -EMSGSIZE && offset > 0) {
        ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
        if (ret < 0)
            return ret;
        ret = rtnetlink_append_rule_msg(reply, &offset, sizeof(reply), req,
                                        RT_TABLE_DEFAULT, 32767, NLM_F_MULTI);
    }
    if (ret < 0)
        return ret;

    ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    if (ret == -EMSGSIZE && offset > 0) {
        ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
        if (ret < 0)
            return ret;
        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    }
    if (ret < 0)
        return ret;

    ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
    if (ret < 0)
        return ret;

    return 0;
}

static int rtnetlink_handle_request(struct netlink_sock *sender_sock,
                                    const struct nlmsghdr *req) {
    const struct ifinfomsg *ifi_req = NULL;
    size_t payload_len = 0;

    if (!sender_sock || !req)
        return -EINVAL;

    if (req->nlmsg_len < sizeof(*req))
        return -EINVAL;

    payload_len = req->nlmsg_len - NLMSG_HDRLEN;
    if (payload_len >= sizeof(struct ifinfomsg))
        ifi_req = (const struct ifinfomsg *)NLMSG_DATA(req);

    switch (req->nlmsg_type) {
    case RTM_GETLINK:
        return rtnetlink_handle_getlink(sender_sock, req, ifi_req);
    case RTM_GETADDR:
        return rtnetlink_handle_getaddr(sender_sock, req);
    case RTM_GETROUTE:
        return rtnetlink_handle_getroute(sender_sock, req);
    case RTM_GETRULE:
        return rtnetlink_handle_getrule(sender_sock, req);
    case RTM_SETLINK:
        return rtnetlink_handle_setlink(sender_sock, req, ifi_req);
    case RTM_NEWLINK:
        if (!ifi_req || ifi_req->ifi_index <= 0)
            return rtnetlink_handle_getlink(sender_sock, req, ifi_req);
        return rtnetlink_handle_setlink(sender_sock, req, ifi_req);
    case RTM_NEWADDR:
    case RTM_DELADDR:
    case RTM_NEWROUTE:
    case RTM_DELROUTE:
        return rtnetlink_handle_state_update(sender_sock, req);
    default:
        printk("Unsupported req->nlmsg_type = %d\n", req->nlmsg_type);
        return -EOPNOTSUPP;
    }
}

struct naos_genl_family_desc {
    uint16_t id;
    const char *name;
    uint8_t version;
    uint32_t maxattr;
};

static const struct naos_genl_family_desc naos_genl_ctrl_family = {
    .id = GENL_ID_CTRL,
    .name = "nlctrl",
    .version = 2,
    .maxattr = CTRL_ATTR_MCAST_GROUPS,
};

static const struct naos_genl_family_desc naos_genl_nl80211_family = {
    .id = NAOS_GENL_ID_NL80211,
    .name = NL80211_GENL_NAME,
    .version = NL80211_GENL_VERSION,
    .maxattr = NL80211_ATTR_BSSID,
};

static const struct rtattr *netlink_find_attr(const void *data, size_t len,
                                              uint16_t type) {
    const struct rtattr *rta = (const struct rtattr *)data;
    int remaining = (int)len;

    while (RTA_OK(rta, remaining)) {
        if ((rta->rta_type & RTA_TYPE_MASK) == type)
            return rta;
        rta = RTA_NEXT(rta, remaining);
    }

    return NULL;
}

static int netlink_start_nested_attr(char *buf, size_t *offset, size_t capacity,
                                     uint16_t type, size_t *nested_start) {
    struct rtattr *rta;
    size_t start;

    if (!buf || !offset || !nested_start)
        return -EINVAL;
    if (*offset > capacity || RTA_ALIGN(sizeof(*rta)) > capacity - *offset)
        return -EMSGSIZE;

    start = *offset;
    rta = (struct rtattr *)(buf + start);
    rta->rta_len = sizeof(*rta);
    rta->rta_type = type;
    *offset += RTA_ALIGN(sizeof(*rta));
    *nested_start = start;
    return 0;
}

static void netlink_end_nested_attr(char *buf, size_t end_offset,
                                    size_t nested_start) {
    struct rtattr *rta;

    if (!buf || end_offset < nested_start)
        return;

    rta = (struct rtattr *)(buf + nested_start);
    rta->rta_len = (uint16_t)(end_offset - nested_start);
}

static int genl_append_ctrl_mcast_group(char *buf, size_t *offset,
                                        size_t capacity, uint16_t index,
                                        const char *name, uint32_t id) {
    size_t group_start;
    int ret;

    ret = netlink_start_nested_attr(buf, offset, capacity, index, &group_start);
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, offset, capacity, CTRL_ATTR_MCAST_GRP_NAME,
                              name, strlen(name) + 1);
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, offset, capacity, CTRL_ATTR_MCAST_GRP_ID,
                              &id, sizeof(id));
    if (ret < 0)
        return ret;

    netlink_end_nested_attr(buf, *offset, group_start);
    return 0;
}

static int genl_append_ctrl_mcast_groups(char *buf, size_t *offset,
                                         size_t capacity) {
    static const struct {
        const char *name;
        uint32_t id;
    } groups[] = {
        {NL80211_MCGRP_CONFIG_NAME, NL80211_MCGRP_CONFIG_ID},
        {NL80211_MCGRP_SCAN_NAME, NL80211_MCGRP_SCAN_ID},
        {NL80211_MCGRP_MLME_NAME, NL80211_MCGRP_MLME_ID},
    };
    size_t groups_start;
    int ret;

    ret = netlink_start_nested_attr(buf, offset, capacity,
                                    CTRL_ATTR_MCAST_GROUPS, &groups_start);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        ret = genl_append_ctrl_mcast_group(buf, offset, capacity,
                                           (uint16_t)(i + 1), groups[i].name,
                                           groups[i].id);
        if (ret < 0)
            return ret;
    }

    netlink_end_nested_attr(buf, *offset, groups_start);
    return 0;
}

static const struct naos_genl_family_desc *naos_genl_family_by_id(uint16_t id) {
    if (id == naos_genl_ctrl_family.id)
        return &naos_genl_ctrl_family;
    if (id == naos_genl_nl80211_family.id)
        return &naos_genl_nl80211_family;
    return NULL;
}

static const struct naos_genl_family_desc *
naos_genl_family_by_name(const char *name) {
    if (!name)
        return NULL;
    if (strcmp(name, naos_genl_ctrl_family.name) == 0)
        return &naos_genl_ctrl_family;
    if (strcmp(name, naos_genl_nl80211_family.name) == 0)
        return &naos_genl_nl80211_family;
    return NULL;
}

static int
genl_append_ctrl_family_msg(char *buf, size_t *offset, size_t capacity,
                            const struct nlmsghdr *req,
                            const struct naos_genl_family_desc *fam) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint16_t family_id;
    uint32_t version;
    uint32_t hdrsize = 0;
    uint32_t maxattr;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !req || !fam)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(buf, offset, capacity, GENL_ID_CTRL, 0,
                                     req->nlmsg_seq, NULL,
                                     sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = CTRL_CMD_NEWFAMILY;
    genlh->version = naos_genl_ctrl_family.version;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    family_id = fam->id;
    ret = netlink_append_attr(buf, &cur, capacity, CTRL_ATTR_FAMILY_ID,
                              &family_id, sizeof(family_id));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, CTRL_ATTR_FAMILY_NAME,
                              fam->name, strlen(fam->name) + 1);
    if (ret < 0)
        return ret;

    version = fam->version;
    ret = netlink_append_attr(buf, &cur, capacity, CTRL_ATTR_VERSION, &version,
                              sizeof(version));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, CTRL_ATTR_HDRSIZE, &hdrsize,
                              sizeof(hdrsize));
    if (ret < 0)
        return ret;

    maxattr = fam->maxattr;
    ret = netlink_append_attr(buf, &cur, capacity, CTRL_ATTR_MAXATTR, &maxattr,
                              sizeof(maxattr));
    if (ret < 0)
        return ret;

    if (fam->id == NAOS_GENL_ID_NL80211) {
        ret = genl_append_ctrl_mcast_groups(buf, &cur, capacity);
        if (ret < 0)
            return ret;
    }

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int genl_append_nl80211_supported_iftypes(char *buf, size_t *offset,
                                                 size_t capacity,
                                                 uint32_t interface_modes) {
    size_t nested_start;
    int ret;

    ret = netlink_start_nested_attr(
        buf, offset, capacity, NL80211_ATTR_SUPPORTED_IFTYPES, &nested_start);
    if (ret < 0)
        return ret;

    for (uint32_t iftype = 0; iftype < 32; iftype++) {
        if (!(interface_modes & (1UL << iftype)))
            continue;
        ret = netlink_append_attr(buf, offset, capacity, (uint16_t)iftype, NULL,
                                  0);
        if (ret < 0)
            return ret;
    }

    netlink_end_nested_attr(buf, *offset, nested_start);
    return 0;
}

static int genl_append_nl80211_wiphy_msg(char *buf, size_t *offset,
                                         size_t capacity,
                                         const struct nlmsghdr *req,
                                         const netdev_t *dev,
                                         const netdev_wireless_info_t *wireless,
                                         uint16_t flags) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t wiphy_index;
    uint8_t max_scan_ssids;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !req || !dev || !wireless || !wireless->present)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(
        buf, offset, capacity, NAOS_GENL_ID_NL80211, flags, req->nlmsg_seq,
        NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_NEW_WIPHY;
    genlh->version = NL80211_GENL_VERSION;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    wiphy_index = wireless->wiphy_index;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY,
                              &wiphy_index, sizeof(wiphy_index));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY_NAME,
                              wireless->wiphy_name,
                              strlen(wireless->wiphy_name) + 1);
    if (ret < 0)
        return ret;

    ret = genl_append_nl80211_supported_iftypes(buf, &cur, capacity,
                                                wireless->interface_modes);
    if (ret < 0)
        return ret;

    max_scan_ssids = wireless->max_scan_ssids > UINT8_MAX
                         ? UINT8_MAX
                         : (uint8_t)wireless->max_scan_ssids;
    ret = netlink_append_attr(buf, &cur, capacity,
                              NL80211_ATTR_MAX_NUM_SCAN_SSIDS, &max_scan_ssids,
                              sizeof(max_scan_ssids));
    if (ret < 0)
        return ret;

    /* NL80211_ATTR_SUPPORTED_COMMANDS — tell NM what ops the driver supports.
     * Each entry: nla_type = cmd number, nla_data = cmd number (u32).
     * This matches Linux kernel's nl80211 encoding exactly.                 */
    {
        static const uint32_t cmds[] = {
            NL80211_CMD_GET_WIPHY,      NL80211_CMD_GET_INTERFACE,
            NL80211_CMD_SET_INTERFACE,  NL80211_CMD_TRIGGER_SCAN,
            NL80211_CMD_GET_SCAN,       NL80211_CMD_CONNECT,
            NL80211_CMD_DISCONNECT,     NL80211_CMD_GET_STATION,
            NL80211_CMD_SET_POWER_SAVE, NL80211_CMD_GET_POWER_SAVE,
        };
        size_t sc_start;
        ret = netlink_start_nested_attr(
            buf, &cur, capacity, NL80211_ATTR_SUPPORTED_COMMANDS, &sc_start);
        if (ret < 0)
            return ret;
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
            ret = netlink_append_attr(buf, &cur, capacity, (uint16_t)cmds[i],
                                      &cmds[i], sizeof(cmds[i]));
            if (ret < 0)
                return ret;
        }
        netlink_end_nested_attr(buf, cur, sc_start);
    }

    /* NL80211_ATTR_CIPHER_SUITES */
    {
        static const uint32_t ciphers[] = {
            WLAN_CIPHER_SUITE_WEP40,
            WLAN_CIPHER_SUITE_TKIP,
            WLAN_CIPHER_SUITE_CCMP,
            WLAN_CIPHER_SUITE_WEP104,
        };
        ret =
            netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_CIPHER_SUITES,
                                ciphers, sizeof(ciphers));
        if (ret < 0)
            return ret;
    }

    /* NL80211_ATTR_WIPHY_BANDS — 2.4 GHz band with common channels */
    {
        static const uint32_t freq_24ghz[] = {
            2412, 2417, 2422, 2427, 2432, 2437,
            2442, 2447, 2452, 2457, 2462, 2472,
        };
        size_t bands_start, band_start, freqs_start, freq_start;

        ret = netlink_start_nested_attr(buf, &cur, capacity,
                                        NL80211_ATTR_WIPHY_BANDS, &bands_start);
        if (ret < 0)
            return ret;

        ret = netlink_start_nested_attr(buf, &cur, capacity, NL80211_BAND_2GHZ,
                                        &band_start);
        if (ret < 0)
            return ret;

        ret = netlink_start_nested_attr(buf, &cur, capacity,
                                        NL80211_BAND_ATTR_FREQS, &freqs_start);
        if (ret < 0)
            return ret;

        for (size_t i = 0; i < sizeof(freq_24ghz) / sizeof(freq_24ghz[0]);
             i++) {
            ret = netlink_start_nested_attr(buf, &cur, capacity,
                                            (uint16_t)(i + 1), &freq_start);
            if (ret < 0)
                return ret;
            ret = netlink_append_attr(buf, &cur, capacity,
                                      NL80211_FREQUENCY_ATTR_FREQ,
                                      &freq_24ghz[i], sizeof(freq_24ghz[i]));
            if (ret < 0)
                return ret;
            netlink_end_nested_attr(buf, cur, freq_start);
        }

        netlink_end_nested_attr(buf, cur, freqs_start);
        netlink_end_nested_attr(buf, cur, band_start);
        netlink_end_nested_attr(buf, cur, bands_start);
    }

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int genl_append_nl80211_interface_msg(
    char *buf, size_t *offset, size_t capacity, const struct nlmsghdr *req,
    const netdev_t *dev, const netdev_wireless_info_t *wireless,
    uint16_t flags) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t wiphy_index;
    uint32_t ifindex;
    uint32_t iftype;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !req || !dev || !wireless || !wireless->present)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(
        buf, offset, capacity, NAOS_GENL_ID_NL80211, flags, req->nlmsg_seq,
        NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_NEW_INTERFACE;
    genlh->version = NL80211_GENL_VERSION;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    wiphy_index = wireless->wiphy_index;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY,
                              &wiphy_index, sizeof(wiphy_index));
    if (ret < 0)
        return ret;

    ifindex = dev->id + 1;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFINDEX,
                              &ifindex, sizeof(ifindex));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFNAME,
                              dev->name, strlen(dev->name) + 1);
    if (ret < 0)
        return ret;

    iftype = wireless->iftype;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFTYPE, &iftype,
                              sizeof(iftype));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_MAC, dev->mac,
                              sizeof(dev->mac));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int genl_append_nl80211_scan_result_msg(
    char *buf, size_t *offset, size_t capacity, const struct nlmsghdr *req,
    const netdev_t *dev, const netdev_wireless_info_t *wireless,
    const netdev_scan_state_t *scan, const netdev_scan_result_t *result,
    uint16_t flags) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t wiphy_index;
    uint32_t ifindex;
    uint32_t generation;
    size_t start;
    size_t cur;
    size_t bss_start;
    uint32_t bss_status;
    int ret;

    if (!buf || !offset || !req || !dev || !wireless || !scan || !result ||
        !result->valid)
        return -EINVAL;

    start = *offset;
    ret = netlink_append_raw_message(
        buf, offset, capacity, NAOS_GENL_ID_NL80211, flags, req->nlmsg_seq,
        NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_NEW_SCAN_RESULTS;
    genlh->version = NL80211_GENL_VERSION;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    wiphy_index = wireless->wiphy_index;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY,
                              &wiphy_index, sizeof(wiphy_index));
    if (ret < 0)
        return ret;

    ifindex = dev->id + 1;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFINDEX,
                              &ifindex, sizeof(ifindex));
    if (ret < 0)
        return ret;

    generation = scan->generation;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_GENERATION,
                              &generation, sizeof(generation));
    if (ret < 0)
        return ret;

    ret = netlink_start_nested_attr(buf, &cur, capacity, NL80211_ATTR_BSS,
                                    &bss_start);
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_BSSID,
                              result->bssid, sizeof(result->bssid));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_FREQUENCY,
                              &result->frequency, sizeof(result->frequency));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_TSF,
                              &result->tsf, sizeof(result->tsf));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_BEACON_INTERVAL,
                              &result->beacon_interval,
                              sizeof(result->beacon_interval));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_CAPABILITY,
                              &result->capability, sizeof(result->capability));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity,
                              NL80211_BSS_INFORMATION_ELEMENTS, result->ies,
                              result->ie_len);
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_BEACON_IES,
                              result->ies, result->ie_len);
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_SIGNAL_MBM,
                              &result->signal_mbm, sizeof(result->signal_mbm));
    if (ret < 0)
        return ret;

    ret =
        netlink_append_attr(buf, &cur, capacity, NL80211_BSS_SEEN_MS_AGO,
                            &result->seen_ms_ago, sizeof(result->seen_ms_ago));
    if (ret < 0)
        return ret;

    if (wireless->connected &&
        memcmp(wireless->bssid, result->bssid, sizeof(result->bssid)) == 0) {
        bss_status = NL80211_BSS_STATUS_ASSOCIATED;
        ret = netlink_append_attr(buf, &cur, capacity, NL80211_BSS_STATUS,
                                  &bss_status, sizeof(bss_status));
        if (ret < 0)
            return ret;
    }

    netlink_end_nested_attr(buf, cur, bss_start);
    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int genl_append_nl80211_scan_event_msg(
    char *buf, size_t *offset, size_t capacity, const netdev_t *dev,
    const netdev_wireless_info_t *wireless, const netdev_scan_state_t *scan,
    bool aborted) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t wiphy_index;
    uint32_t ifindex;
    uint32_t generation;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !dev || !wireless || !scan)
        return -EINVAL;

    start = *offset;
    ret =
        netlink_append_raw_message(buf, offset, capacity, NAOS_GENL_ID_NL80211,
                                   0, 0, NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd =
        aborted ? NL80211_CMD_SCAN_ABORTED : NL80211_CMD_NEW_SCAN_RESULTS;
    genlh->version = NL80211_GENL_VERSION;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    wiphy_index = wireless->wiphy_index;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY,
                              &wiphy_index, sizeof(wiphy_index));
    if (ret < 0)
        return ret;

    ifindex = dev->id + 1;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFINDEX,
                              &ifindex, sizeof(ifindex));
    if (ret < 0)
        return ret;

    generation = scan->generation;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_GENERATION,
                              &generation, sizeof(generation));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int
genl_append_nl80211_connect_event_msg(char *buf, size_t *offset,
                                      size_t capacity, const netdev_t *dev,
                                      const netdev_wireless_info_t *wireless,
                                      uint32_t seq, uint16_t status_code) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t wiphy_index;
    uint32_t ifindex;
    size_t start;
    size_t cur;
    int ret;

    if (!buf || !offset || !dev || !wireless || !wireless->present)
        return -EINVAL;

    start = *offset;
    ret =
        netlink_append_raw_message(buf, offset, capacity, NAOS_GENL_ID_NL80211,
                                   0, seq, NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_CONNECT;
    genlh->version = NL80211_GENL_VERSION;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    wiphy_index = wireless->wiphy_index;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY,
                              &wiphy_index, sizeof(wiphy_index));
    if (ret < 0)
        return ret;

    ifindex = dev->id + 1;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFINDEX,
                              &ifindex, sizeof(ifindex));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_STATUS_CODE,
                              &status_code, sizeof(status_code));
    if (ret < 0)
        return ret;

    if (wireless->ssid_len) {
        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_SSID,
                                  wireless->ssid, wireless->ssid_len);
        if (ret < 0)
            return ret;
    }

    if (!netlink_is_zero_mac(wireless->bssid)) {
        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_MAC,
                                  wireless->bssid, sizeof(wireless->bssid));
        if (ret < 0)
            return ret;

        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_BSSID,
                                  wireless->bssid, sizeof(wireless->bssid));
        if (ret < 0)
            return ret;
    }

    if (wireless->frequency) {
        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY_FREQ,
                                  &wireless->frequency,
                                  sizeof(wireless->frequency));
        if (ret < 0)
            return ret;
    }

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static int genl_append_nl80211_disconnect_event_msg(
    char *buf, size_t *offset, size_t capacity, const netdev_t *dev,
    const netdev_wireless_info_t *wireless, uint32_t seq, uint16_t reason_code,
    bool by_ap) {
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t wiphy_index;
    uint32_t ifindex;
    size_t start;
    size_t cur;
    uint8_t disconnected_by_ap = by_ap ? 1 : 0;
    int ret;

    if (!buf || !offset || !dev || !wireless || !wireless->present)
        return -EINVAL;

    start = *offset;
    ret =
        netlink_append_raw_message(buf, offset, capacity, NAOS_GENL_ID_NL80211,
                                   0, seq, NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)(buf + start);
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_DISCONNECT;
    genlh->version = NL80211_GENL_VERSION;

    cur = start + NLMSG_ALIGN(nlh->nlmsg_len);

    wiphy_index = wireless->wiphy_index;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_WIPHY,
                              &wiphy_index, sizeof(wiphy_index));
    if (ret < 0)
        return ret;

    ifindex = dev->id + 1;
    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_IFINDEX,
                              &ifindex, sizeof(ifindex));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_REASON_CODE,
                              &reason_code, sizeof(reason_code));
    if (ret < 0)
        return ret;

    ret = netlink_append_attr(buf, &cur, capacity,
                              NL80211_ATTR_DISCONNECTED_BY_AP,
                              &disconnected_by_ap, sizeof(disconnected_by_ap));
    if (ret < 0)
        return ret;

    if (wireless->ssid_len) {
        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_SSID,
                                  wireless->ssid, wireless->ssid_len);
        if (ret < 0)
            return ret;
    }

    if (!netlink_is_zero_mac(wireless->bssid)) {
        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_MAC,
                                  wireless->bssid, sizeof(wireless->bssid));
        if (ret < 0)
            return ret;

        ret = netlink_append_attr(buf, &cur, capacity, NL80211_ATTR_BSSID,
                                  wireless->bssid, sizeof(wireless->bssid));
        if (ret < 0)
            return ret;
    }

    nlh->nlmsg_len = (uint32_t)(cur - start);
    *offset = cur;
    return 0;
}

static void genl_parse_scan_params(const struct rtattr *scan_ssids_attr,
                                   netdev_scan_params_t *params) {
    const struct rtattr *rta;
    int remaining;

    if (!params)
        return;

    memset(params, 0, sizeof(*params));
    params->n_ssids = 1;

    if (!scan_ssids_attr || scan_ssids_attr->rta_len <= RTA_LENGTH(0))
        return;

    rta = (const struct rtattr *)RTA_DATA(scan_ssids_attr);
    remaining = (int)(scan_ssids_attr->rta_len - RTA_LENGTH(0));
    params->n_ssids = 0;

    while (RTA_OK(rta, remaining) && params->n_ssids < NETDEV_MAX_SCAN_SSIDS) {
        size_t ssid_len =
            rta->rta_len > RTA_LENGTH(0) ? rta->rta_len - RTA_LENGTH(0) : 0;

        if (ssid_len > sizeof(params->ssids[0].ssid))
            ssid_len = sizeof(params->ssids[0].ssid);

        params->ssids[params->n_ssids].len = (uint8_t)ssid_len;
        memcpy(params->ssids[params->n_ssids].ssid, RTA_DATA(rta), ssid_len);
        params->n_ssids++;
        rta = RTA_NEXT(rta, remaining);
    }

    if (params->n_ssids == 0)
        params->n_ssids = 1;
}

static void genl_parse_connect_params(const void *attrs, size_t attr_len,
                                      netdev_connect_params_t *params) {
    const struct rtattr *ssid_attr;
    const struct rtattr *auth_type_attr;
    const struct rtattr *freq_attr;
    const struct rtattr *privacy_attr;
    const struct rtattr *bssid_attr;
    const struct rtattr *mac_attr;
    size_t ssid_len;

    if (!params)
        return;

    memset(params, 0, sizeof(*params));
    params->auth_type = NL80211_AUTHTYPE_AUTOMATIC;

    ssid_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_SSID);
    if (ssid_attr && ssid_attr->rta_len > RTA_LENGTH(0)) {
        ssid_len = ssid_attr->rta_len - RTA_LENGTH(0);
        if (ssid_len > sizeof(params->ssid))
            ssid_len = sizeof(params->ssid);
        params->ssid_len = (uint8_t)ssid_len;
        memcpy(params->ssid, RTA_DATA(ssid_attr), ssid_len);
    }

    auth_type_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_AUTH_TYPE);
    if (auth_type_attr &&
        auth_type_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t))) {
        params->auth_type = *(const uint32_t *)RTA_DATA(auth_type_attr);
    }

    freq_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_WIPHY_FREQ);
    if (freq_attr && freq_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t)))
        params->frequency = *(const uint32_t *)RTA_DATA(freq_attr);

    params->privacy =
        netlink_find_attr(attrs, attr_len, NL80211_ATTR_PRIVACY) != NULL;

    bssid_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_BSSID);
    mac_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_MAC);
    if (bssid_attr && bssid_attr->rta_len >= RTA_LENGTH(6)) {
        memcpy(params->bssid, RTA_DATA(bssid_attr), sizeof(params->bssid));
        params->has_bssid = true;
    } else if (mac_attr && mac_attr->rta_len >= RTA_LENGTH(6)) {
        memcpy(params->bssid, RTA_DATA(mac_attr), sizeof(params->bssid));
        params->has_bssid = true;
    }
}

static int genl_handle_ctrl_getfamily(struct netlink_sock *sender_sock,
                                      const struct nlmsghdr *req,
                                      const struct genlmsghdr *genlh,
                                      const void *attrs, size_t attr_len) {
    const struct naos_genl_family_desc *fam = NULL;
    const struct rtattr *name_attr;
    const struct rtattr *id_attr;
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    int ret;

    (void)genlh;

    if (!sender_sock || !req)
        return -EINVAL;

    name_attr = netlink_find_attr(attrs, attr_len, CTRL_ATTR_FAMILY_NAME);
    id_attr = netlink_find_attr(attrs, attr_len, CTRL_ATTR_FAMILY_ID);

    if (name_attr && name_attr->rta_len > sizeof(*name_attr)) {
        fam = naos_genl_family_by_name((const char *)RTA_DATA(name_attr));
    } else if (id_attr && id_attr->rta_len >= RTA_LENGTH(sizeof(uint16_t))) {
        fam = naos_genl_family_by_id(*(const uint16_t *)RTA_DATA(id_attr));
    } else {
        fam = &naos_genl_nl80211_family;
    }

    if (!fam)
        return -ENOENT;

    memset(reply, 0, sizeof(reply));
    ret = genl_append_ctrl_family_msg(reply, &offset, sizeof(reply), req, fam);
    if (ret < 0)
        return ret;

    if (!netlink_deliver_to_socket(sender_sock, reply, offset, 0, 0))
        return -ENOBUFS;

    return 0;
}

static int genl_handle_nl80211_get_wiphy(struct netlink_sock *sender_sock,
                                         const struct nlmsghdr *req,
                                         const void *attrs, size_t attr_len) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    const struct rtattr *wiphy_attr;
    uint32_t want_wiphy = 0;
    char reply[NETLINK_BUFFER_SIZE];
    size_t count;
    size_t offset = 0;
    int ret = -ENOENT;

    if (!sender_sock || !req)
        return -EINVAL;

    wiphy_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_WIPHY);
    if (wiphy_attr && wiphy_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t)))
        want_wiphy = *(const uint32_t *)RTA_DATA(wiphy_attr);

    memset(reply, 0, sizeof(reply));
    count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));

    for (size_t i = 0; i < count; i++) {
        netdev_wireless_info_t wireless;

        if (!netdev_get_wireless_info(devs[i], &wireless))
            continue;
        if (want_wiphy && wireless.wiphy_index != want_wiphy)
            continue;

        ret = genl_append_nl80211_wiphy_msg(
            reply, &offset, sizeof(reply), req, devs[i], &wireless,
            (req->nlmsg_flags & NLM_F_DUMP) ? NLM_F_MULTI : 0);
        if (ret < 0)
            break;
        if (!(req->nlmsg_flags & NLM_F_DUMP))
            break;
        ret = 0;
    }

    for (size_t i = 0; i < count; i++)
        netdev_put(devs[i]);

    if (ret < 0)
        return ret;

    if ((req->nlmsg_flags & NLM_F_DUMP)) {
        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
        if (ret < 0)
            return ret;
    }

    if (!offset || !netlink_deliver_to_socket(sender_sock, reply, offset, 0, 0))
        return -ENOBUFS;

    return 0;
}

static int genl_handle_nl80211_get_interface(struct netlink_sock *sender_sock,
                                             const struct nlmsghdr *req,
                                             const void *attrs,
                                             size_t attr_len) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    const struct rtattr *ifindex_attr;
    const struct rtattr *wiphy_attr;
    uint32_t want_ifindex = 0;
    uint32_t want_wiphy = 0;
    char reply[NETLINK_BUFFER_SIZE];
    size_t count;
    size_t offset = 0;
    int ret = -ENOENT;

    if (!sender_sock || !req)
        return -EINVAL;

    ifindex_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFINDEX);
    if (ifindex_attr && ifindex_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t)))
        want_ifindex = *(const uint32_t *)RTA_DATA(ifindex_attr);

    wiphy_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_WIPHY);
    if (wiphy_attr && wiphy_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t)))
        want_wiphy = *(const uint32_t *)RTA_DATA(wiphy_attr);

    memset(reply, 0, sizeof(reply));
    count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));

    for (size_t i = 0; i < count; i++) {
        netdev_wireless_info_t wireless;

        if (!netdev_get_wireless_info(devs[i], &wireless))
            continue;
        if (want_ifindex && devs[i]->id + 1 != want_ifindex)
            continue;
        if (want_wiphy && wireless.wiphy_index != want_wiphy)
            continue;

        ret = genl_append_nl80211_interface_msg(
            reply, &offset, sizeof(reply), req, devs[i], &wireless,
            (req->nlmsg_flags & NLM_F_DUMP) ? NLM_F_MULTI : 0);
        if (ret < 0)
            break;
        if (!(req->nlmsg_flags & NLM_F_DUMP))
            break;
        ret = 0;
    }

    for (size_t i = 0; i < count; i++)
        netdev_put(devs[i]);

    if (ret < 0)
        return ret;

    if ((req->nlmsg_flags & NLM_F_DUMP)) {
        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
        if (ret < 0)
            return ret;
    }

    if (!offset || !netlink_deliver_to_socket(sender_sock, reply, offset, 0, 0))
        return -ENOBUFS;

    return 0;
}

static int genl_handle_nl80211_set_interface(struct netlink_sock *sender_sock,
                                             const struct nlmsghdr *req,
                                             const void *attrs,
                                             size_t attr_len) {
    const struct rtattr *ifindex_attr;
    const struct rtattr *iftype_attr;
    netdev_wireless_info_t wireless;
    uint32_t ifindex;
    uint32_t iftype;
    netdev_t *dev;
    int ret = 0;

    (void)sender_sock;
    (void)req;

    ifindex_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFINDEX);
    if (!ifindex_attr || ifindex_attr->rta_len < RTA_LENGTH(sizeof(uint32_t)))
        return -EINVAL;

    ifindex = *(const uint32_t *)RTA_DATA(ifindex_attr);
    dev = netdev_get_by_index(ifindex);
    if (!dev)
        return -ENODEV;

    iftype_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFTYPE);
    if (iftype_attr && iftype_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t))) {
        iftype = *(const uint32_t *)RTA_DATA(iftype_attr);
        if (!netdev_get_wireless_info(dev, &wireless)) {
            ret = -EOPNOTSUPP;
        } else if (iftype < 32 && (wireless.interface_modes & (1U << iftype))) {
            wireless.iftype = iftype;
            ret = netdev_set_wireless_info(dev, &wireless);
        } else {
            ret = -EOPNOTSUPP;
        }
    }

    netdev_put(dev);
    return ret;
}

static int genl_handle_nl80211_trigger_scan(struct netlink_sock *sender_sock,
                                            const struct nlmsghdr *req,
                                            const void *attrs,
                                            size_t attr_len) {
    const struct rtattr *ifindex_attr;
    const struct rtattr *scan_ssids_attr;
    netdev_scan_params_t params;
    netdev_t *dev;
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    uint32_t ifindex;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    ifindex_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFINDEX);
    if (!ifindex_attr || ifindex_attr->rta_len < RTA_LENGTH(sizeof(uint32_t)))
        return -EINVAL;

    ifindex = *(const uint32_t *)RTA_DATA(ifindex_attr);
    dev = netdev_get_by_index(ifindex);
    if (!dev)
        return -ENODEV;

    scan_ssids_attr =
        netlink_find_attr(attrs, attr_len, NL80211_ATTR_SCAN_SSIDS);
    genl_parse_scan_params(scan_ssids_attr, &params);

    ret = netdev_trigger_scan(dev, &params, sender_sock->portid);
    netdev_put(dev);
    if (ret < 0)
        return ret;

    return 0;
}

static int genl_handle_nl80211_get_scan(struct netlink_sock *sender_sock,
                                        const struct nlmsghdr *req,
                                        const void *attrs, size_t attr_len) {
    netdev_t *devs[MAX_NETDEV_NUM] = {0};
    const struct rtattr *ifindex_attr;
    uint32_t want_ifindex = 0;
    char reply[NETLINK_BUFFER_SIZE];
    size_t count;
    size_t offset = 0;
    int ret = 0;

    if (!sender_sock || !req)
        return -EINVAL;

    ifindex_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFINDEX);
    if (ifindex_attr && ifindex_attr->rta_len >= RTA_LENGTH(sizeof(uint32_t)))
        want_ifindex = *(const uint32_t *)RTA_DATA(ifindex_attr);

    memset(reply, 0, sizeof(reply));
    count = netdev_snapshot(devs, sizeof(devs) / sizeof(devs[0]));

    for (size_t i = 0; i < count; i++) {
        netdev_wireless_info_t wireless;
        netdev_scan_state_t scan;

        if (!netdev_get_wireless_info(devs[i], &wireless))
            continue;
        if (want_ifindex && devs[i]->id + 1 != want_ifindex)
            continue;
        if (!netdev_get_scan_state(devs[i], &scan))
            continue;

        for (uint32_t j = 0; j < NETDEV_MAX_SCAN_RESULTS; j++) {
            if (!scan.results[j].valid)
                continue;

            size_t prev_offset = offset;
            ret = genl_append_nl80211_scan_result_msg(
                reply, &offset, sizeof(reply), req, devs[i], &wireless, &scan,
                &scan.results[j], NLM_F_MULTI);
            if (ret == -EMSGSIZE && prev_offset > 0) {
                ret = netlink_flush_reply_to_socket(sender_sock, reply,
                                                    &prev_offset);
                if (ret < 0)
                    break;
                offset = 0;
                ret = genl_append_nl80211_scan_result_msg(
                    reply, &offset, sizeof(reply), req, devs[i], &wireless,
                    &scan, &scan.results[j], NLM_F_MULTI);
            }
            if (ret < 0)
                break;
        }
        if (ret < 0)
            break;
        ret = 0;
    }

    for (size_t i = 0; i < count; i++)
        netdev_put(devs[i]);

    if (ret < 0)
        return ret;

    ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    if (ret == -EMSGSIZE && offset > 0) {
        ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
        if (ret < 0)
            return ret;
        ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    }
    if (ret < 0)
        return ret;

    ret = netlink_flush_reply_to_socket(sender_sock, reply, &offset);
    if (ret < 0)
        return ret;

    return 0;
}

static int genl_handle_nl80211_get_station(struct netlink_sock *sender_sock,
                                           const struct nlmsghdr *req) {
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    memset(reply, 0, sizeof(reply));
    ret = netlink_append_done(reply, &offset, sizeof(reply), req);
    if (ret < 0)
        return ret;

    if (!netlink_deliver_to_socket(sender_sock, reply, offset, 0, 0))
        return -ENOBUFS;

    return 0;
}

static int genl_handle_nl80211_get_power_save(struct netlink_sock *sender_sock,
                                              const struct nlmsghdr *req) {
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t ps_state = NL80211_PS_DISABLED;
    size_t cur;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    memset(reply, 0, sizeof(reply));
    ret = netlink_append_raw_message(reply, &offset, sizeof(reply),
                                     NAOS_GENL_ID_NL80211, 0, req->nlmsg_seq,
                                     NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)reply;
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_GET_POWER_SAVE;
    genlh->version = NL80211_GENL_VERSION;

    cur = NLMSG_ALIGN(nlh->nlmsg_len);
    ret = netlink_append_attr(reply, &cur, sizeof(reply), NL80211_ATTR_PS_STATE,
                              &ps_state, sizeof(ps_state));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)cur;

    if (!netlink_deliver_to_socket(sender_sock, reply, cur, 0, 0))
        return -ENOBUFS;

    return 0;
}

static int
genl_handle_nl80211_get_protocol_features(struct netlink_sock *sender_sock,
                                          const struct nlmsghdr *req) {
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    struct nlmsghdr *nlh;
    struct genlmsghdr *genlh;
    uint32_t features = 0;
    size_t cur;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    memset(reply, 0, sizeof(reply));
    ret = netlink_append_raw_message(reply, &offset, sizeof(reply),
                                     NAOS_GENL_ID_NL80211, 0, req->nlmsg_seq,
                                     NULL, sizeof(struct genlmsghdr));
    if (ret < 0)
        return ret;

    nlh = (struct nlmsghdr *)reply;
    genlh = (struct genlmsghdr *)NLMSG_DATA(nlh);
    memset(genlh, 0, sizeof(*genlh));
    genlh->cmd = NL80211_CMD_GET_PROTOCOL_FEATURES;
    genlh->version = NL80211_GENL_VERSION;

    cur = NLMSG_ALIGN(nlh->nlmsg_len);
    ret = netlink_append_attr(reply, &cur, sizeof(reply),
                              NL80211_ATTR_PROTOCOL_FEATURES, &features,
                              sizeof(features));
    if (ret < 0)
        return ret;

    nlh->nlmsg_len = (uint32_t)cur;

    if (!netlink_deliver_to_socket(sender_sock, reply, cur, 0, 0))
        return -ENOBUFS;

    return 0;
}

static int genl_handle_nl80211_connect(struct netlink_sock *sender_sock,
                                       const struct nlmsghdr *req,
                                       const void *attrs, size_t attr_len) {
    const struct rtattr *ifindex_attr;
    netdev_connect_params_t params;
    netdev_t *dev;
    uint32_t ifindex;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    ifindex_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFINDEX);
    if (!ifindex_attr || ifindex_attr->rta_len < RTA_LENGTH(sizeof(uint32_t)))
        return -EINVAL;

    genl_parse_connect_params(attrs, attr_len, &params);
    if (!params.ssid_len && !params.has_bssid)
        return -EINVAL;

    ifindex = *(const uint32_t *)RTA_DATA(ifindex_attr);
    dev = netdev_get_by_index(ifindex);
    if (!dev)
        return -ENODEV;

    ret = netdev_trigger_connect(dev, &params, sender_sock->portid);
    netdev_put(dev);
    if (ret < 0)
        return ret;

    return 0;
}

static int genl_handle_nl80211_disconnect(struct netlink_sock *sender_sock,
                                          const struct nlmsghdr *req,
                                          const void *attrs, size_t attr_len) {
    const struct rtattr *ifindex_attr;
    const struct rtattr *reason_attr;
    netdev_t *dev;
    uint32_t ifindex;
    uint16_t reason_code = 3;
    int ret;

    if (!sender_sock || !req)
        return -EINVAL;

    ifindex_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_IFINDEX);
    if (!ifindex_attr || ifindex_attr->rta_len < RTA_LENGTH(sizeof(uint32_t)))
        return -EINVAL;

    reason_attr = netlink_find_attr(attrs, attr_len, NL80211_ATTR_REASON_CODE);
    if (reason_attr && reason_attr->rta_len >= RTA_LENGTH(sizeof(uint16_t)))
        reason_code = *(const uint16_t *)RTA_DATA(reason_attr);

    ifindex = *(const uint32_t *)RTA_DATA(ifindex_attr);
    dev = netdev_get_by_index(ifindex);
    if (!dev)
        return -ENODEV;

    ret = netdev_trigger_disconnect(dev, reason_code, sender_sock->portid);
    netdev_put(dev);
    if (ret < 0)
        return ret;

    return 0;
}

static int genl_handle_request(struct netlink_sock *sender_sock,
                               const struct nlmsghdr *req) {
    const struct genlmsghdr *genlh;
    const void *attrs;
    size_t payload_len;
    size_t attr_len;

    if (!sender_sock || !req)
        return -EINVAL;
    if (req->nlmsg_len < NLMSG_LENGTH(sizeof(struct genlmsghdr)))
        return -EINVAL;

    genlh = (const struct genlmsghdr *)NLMSG_DATA(req);
    payload_len = req->nlmsg_len - NLMSG_HDRLEN;
    if (payload_len < sizeof(*genlh))
        return -EINVAL;

    attrs = (const char *)genlh + GENL_HDRLEN;
    attr_len = payload_len - GENL_HDRLEN;

    if (req->nlmsg_type == GENL_ID_CTRL) {
        if (genlh->cmd == CTRL_CMD_GETFAMILY)
            return genl_handle_ctrl_getfamily(sender_sock, req, genlh, attrs,
                                              attr_len);
        printk("Unsupported GENL_ID_CTRL genlh->cmd = %d\n", genlh->cmd);
        return -EOPNOTSUPP;
    }

    if (req->nlmsg_type == NAOS_GENL_ID_NL80211) {
        switch (genlh->cmd) {
        case NL80211_CMD_GET_WIPHY:
            return genl_handle_nl80211_get_wiphy(sender_sock, req, attrs,
                                                 attr_len);
        case NL80211_CMD_SET_WIPHY:
            return 0;
        case NL80211_CMD_GET_INTERFACE:
            return genl_handle_nl80211_get_interface(sender_sock, req, attrs,
                                                     attr_len);
        case NL80211_CMD_SET_INTERFACE:
            return genl_handle_nl80211_set_interface(sender_sock, req, attrs,
                                                     attr_len);
        case NL80211_CMD_SET_KEY:
        case NL80211_CMD_NEW_KEY:
        case NL80211_CMD_DEL_KEY:
        case NL80211_CMD_SET_BSS:
        case NL80211_CMD_REGISTER_FRAME:
            return 0;
        case NL80211_CMD_GET_STATION:
            return genl_handle_nl80211_get_station(sender_sock, req);
        case NL80211_CMD_TRIGGER_SCAN:
            return genl_handle_nl80211_trigger_scan(sender_sock, req, attrs,
                                                    attr_len);
        case NL80211_CMD_GET_SCAN:
            return genl_handle_nl80211_get_scan(sender_sock, req, attrs,
                                                attr_len);
        case NL80211_CMD_CONNECT:
            return genl_handle_nl80211_connect(sender_sock, req, attrs,
                                               attr_len);
        case NL80211_CMD_DISCONNECT:
            return genl_handle_nl80211_disconnect(sender_sock, req, attrs,
                                                  attr_len);
        case NL80211_CMD_GET_PROTOCOL_FEATURES:
            return genl_handle_nl80211_get_protocol_features(sender_sock, req);
        case NL80211_CMD_SET_POWER_SAVE:
        case NL80211_CMD_SET_WOWLAN:
            return 0;
        case NL80211_CMD_GET_POWER_SAVE:
            return genl_handle_nl80211_get_power_save(sender_sock, req);
        case NL80211_CMD_GET_WOWLAN:
            return -EOPNOTSUPP;
        default:
            printk("Unsupported NAOS_GENL_ID_NL80211 genlh->cmd = %d\n",
                   genlh->cmd);
            return -EOPNOTSUPP;
        }
    }

    return -ENOENT;
}

static bool audit_msg_is_user_generated(uint16_t type) {
    return (type >= AUDIT_FIRST_USER_MSG && type <= AUDIT_LAST_USER_MSG) ||
           (type >= AUDIT_FIRST_USER_MSG2 && type <= AUDIT_LAST_USER_MSG2);
}

static int audit_handle_request(struct netlink_sock *sender_sock,
                                const struct nlmsghdr *req) {
    (void)sender_sock;

    if (!req || req->nlmsg_len < sizeof(*req))
        return -EINVAL;

    switch (req->nlmsg_type) {
    case AUDIT_GET:
    case AUDIT_SIGNAL_INFO:
    case AUDIT_GET_FEATURE:
        /*
         * systemd/libaudit only needs these requests to be accepted so the
         * caller can detect that audit logging is available.
         */
        return 0;
    default:
        if (audit_msg_is_user_generated(req->nlmsg_type))
            return 0;

        printk("Unsupported audit req->nlmsg_type = %u\n", req->nlmsg_type);
        return -EOPNOTSUPP;
    }
}

static int netlink_send_to_kernel(struct netlink_sock *sender_sock,
                                  const char *data, size_t len,
                                  uint32_t sender_pid) {
    (void)sender_pid;
    const struct nlmsghdr *req;
    size_t remaining;
    char reply[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    int ret = -EOPNOTSUPP;

    if (!sender_sock || !data || len < sizeof(struct nlmsghdr))
        return -EINVAL;

    if (sender_sock->protocol == NETLINK_ROUTE ||
        sender_sock->protocol == NETLINK_GENERIC ||
        sender_sock->protocol == NETLINK_AUDIT) {
        remaining = len;
        req = (const struct nlmsghdr *)data;

        while (NLMSG_OK(req, remaining)) {
            ret = sender_sock->protocol == NETLINK_ROUTE
                      ? rtnetlink_handle_request(sender_sock, req)
                  : sender_sock->protocol == NETLINK_GENERIC
                      ? genl_handle_request(sender_sock, req)
                      : audit_handle_request(sender_sock, req);
            if (ret < 0) {
                if (sender_sock->protocol == NETLINK_GENERIC &&
                    req->nlmsg_len >= NLMSG_LENGTH(sizeof(struct genlmsghdr))) {
                    const struct genlmsghdr *genlh =
                        (const struct genlmsghdr *)NLMSG_DATA(req);
                    printk("netlink: error protocol=%d nlmsg_type=%u "
                           "genl_cmd=%u seq=%u ret=%d flags=0x%x\n",
                           sender_sock->protocol, req->nlmsg_type, genlh->cmd,
                           req->nlmsg_seq, ret, req->nlmsg_flags);
                } else if (sender_sock->protocol == NETLINK_AUDIT) {
                    printk("netlink: error protocol=%d nlmsg_type=%u "
                           "audit_type=%u seq=%u ret=%d flags=0x%x\n",
                           sender_sock->protocol, req->nlmsg_type,
                           req->nlmsg_type, req->nlmsg_seq, ret,
                           req->nlmsg_flags);
                } else {
                    printk("netlink: error protocol=%d nlmsg_type=%u seq=%u "
                           "ret=%d flags=0x%x\n",
                           sender_sock->protocol, req->nlmsg_type,
                           req->nlmsg_seq, ret, req->nlmsg_flags);
                }
                memset(reply, 0, sizeof(reply));
                offset = 0;
                if (netlink_append_ack(reply, &offset, sizeof(reply), req,
                                       ret) < 0)
                    return ret;
                netlink_stamp_reply_portid(reply, offset, sender_sock->portid);
                if (!netlink_deliver_to_socket(sender_sock, reply, offset, 0,
                                               0))
                    return -ENOBUFS;
                return ret;
            }

            if ((req->nlmsg_flags & NLM_F_ACK) &&
                !(req->nlmsg_flags & NLM_F_DUMP)) {
                memset(reply, 0, sizeof(reply));
                offset = 0;
                if (netlink_append_ack(reply, &offset, sizeof(reply), req, 0) <
                    0)
                    return -EMSGSIZE;
                netlink_stamp_reply_portid(reply, offset, sender_sock->portid);
                if (!netlink_deliver_to_socket(sender_sock, reply, offset, 0,
                                               0))
                    return -ENOBUFS;
            }

            req = NLMSG_NEXT(req, remaining);
        }

        return 0;
    }
    printk("Unsupported sender_sock->protocol = %d\n", sender_sock->protocol);

    return ret;
}

static int netlink_wait_for_message(struct netlink_sock *sock, fd_t *file,
                                    int flags, const char *reason) {
    bool nonblock;
    bool has_msg;

    if (!sock || !file)
        return -EINVAL;

    nonblock = !!(flags & MSG_DONTWAIT) || !!(fd_get_flags(file) & O_NONBLOCK);
    has_msg = netlink_buffer_has_msg(sock);
    if (!has_msg && nonblock)
        return -EAGAIN;

    while (!has_msg) {
        int ret = netlink_wait_file(file, sock, EPOLLIN, reason);
        if (ret != EOK)
            return ret < 0 ? ret : -EINTR;
        has_msg = netlink_buffer_has_msg(sock);
    }

    return 0;
}

static size_t netlink_iov_total_len(const struct iovec *iov, size_t iovlen) {
    size_t total = 0;

    if (!iov)
        return 0;

    for (size_t i = 0; i < iovlen; i++) {
        if (!iov[i].iov_base && iov[i].len)
            return SIZE_MAX;
        if (iov[i].len > SIZE_MAX - total)
            return SIZE_MAX;
        total += iov[i].len;
    }

    return total;
}

static size_t netlink_copy_to_iov(struct iovec *iov, size_t iovlen,
                                  const char *src, size_t len) {
    size_t copied = 0;

    if (!iov || !src)
        return 0;

    for (size_t i = 0; i < iovlen && copied < len; i++) {
        size_t to_copy = MIN(iov[i].len, len - copied);
        if (!to_copy)
            continue;
        memcpy(iov[i].iov_base, src + copied, to_copy);
        copied += to_copy;
    }

    return copied;
}

// Function to add message to persistent pool
static void netlink_msg_pool_add(const char *message, size_t length,
                                 uint32_t nl_pid, uint32_t nl_groups,
                                 int protocol, uint32_t seqnum,
                                 const char *devpath) {
    if (message == NULL || length == 0 || nl_groups == 0) {
        return;
    }

    spin_lock(&netlink_msg_pool_lock);

    struct netlink_msg_pool_entry *entry =
        &netlink_msg_pool[netlink_msg_pool_next];
    entry->valid = true;
    entry->length =
        (length < NETLINK_BUFFER_SIZE) ? length : NETLINK_BUFFER_SIZE - 1;
    memcpy(entry->message, message, entry->length);
    entry->message[entry->length] = '\0';
    entry->timestamp = 0; // TODO: Get current time
    entry->nl_pid = nl_pid;
    entry->nl_groups = nl_groups;
    entry->protocol = protocol;
    entry->seqnum = seqnum;

    if (devpath) {
        strncpy(entry->devpath, devpath, sizeof(entry->devpath) - 1);
        entry->devpath[sizeof(entry->devpath) - 1] = '\0';
    } else {
        entry->devpath[0] = '\0';
    }

    netlink_msg_pool_next =
        (netlink_msg_pool_next + 1) % MAX_NETLINK_MSG_POOL_SIZE;

    spin_unlock(&netlink_msg_pool_lock);
}

// Function to retrieve message from pool by seqnum (for uevent)
static bool netlink_msg_pool_get_by_seqnum(uint32_t seqnum, char *buffer,
                                           size_t *length, uint32_t *nl_pid,
                                           uint32_t *nl_groups) {
    spin_lock(&netlink_msg_pool_lock);
    bool found = false;

    for (int i = 0; i < MAX_NETLINK_MSG_POOL_SIZE; i++) {
        struct netlink_msg_pool_entry *entry = &netlink_msg_pool[i];
        if (entry->valid && entry->seqnum == seqnum) {
            size_t copy_len =
                (entry->length < *length) ? entry->length : *length;
            memcpy(buffer, entry->message, copy_len);
            *length = copy_len;
            if (nl_pid)
                *nl_pid = entry->nl_pid;
            if (nl_groups)
                *nl_groups = entry->nl_groups;
            found = true;
            break;
        }
    }

    spin_unlock(&netlink_msg_pool_lock);
    return found;
}

// Function to retrieve message from pool by devpath (for uevent)
static bool netlink_msg_pool_get_by_devpath(const char *devpath, char *buffer,
                                            size_t *length, uint32_t *nl_pid,
                                            uint32_t *nl_groups) {
    spin_lock(&netlink_msg_pool_lock);
    bool found = false;

    for (int i = 0; i < MAX_NETLINK_MSG_POOL_SIZE; i++) {
        struct netlink_msg_pool_entry *entry = &netlink_msg_pool[i];
        if (entry->valid && strcmp(entry->devpath, devpath) == 0) {
            size_t copy_len =
                (entry->length < *length) ? entry->length : *length;
            memcpy(buffer, entry->message, copy_len);
            *length = copy_len;
            if (nl_pid)
                *nl_pid = entry->nl_pid;
            if (nl_groups)
                *nl_groups = entry->nl_groups;
            found = true;
            break;
        }
    }

    spin_unlock(&netlink_msg_pool_lock);
    return found;
}

static void netlink_packet_meta_destroy(void *priv) { free(priv); }

static skb_buff_t *netlink_packet_build_skb(const char *data, size_t len,
                                            uint32_t nl_pid,
                                            uint32_t nl_groups) {
    skb_buff_t *skb = NULL;
    struct netlink_packet_hdr *hdr = NULL;

    skb = skb_alloc(len);
    if (!skb)
        return NULL;

    hdr = malloc(sizeof(*hdr));
    if (!hdr) {
        skb_free(skb, NULL);
        return NULL;
    }

    if (len > 0)
        memcpy(skb->data, data, len);

    hdr->nl_pid = nl_pid;
    hdr->nl_groups = nl_groups;
    hdr->length = (uint32_t)len;
    skb->priv = hdr;
    return skb;
}

static void netlink_buffer_init(struct netlink_buffer *buf) {
    memset(buf, 0, sizeof(struct netlink_buffer));
    buf->size = NETLINK_BUFFER_INITIAL_SIZE;
    buf->max_size = NETLINK_BUFFER_MAX_SIZE;
    skb_queue_init(&buf->queue, buf->size, netlink_packet_meta_destroy);
    buf->lock = SPIN_INIT;
}

static inline size_t
netlink_buffer_trim_target(const struct netlink_buffer *buf,
                           size_t incoming_len) {
    size_t reserve;

    if (!buf)
        return 0;

    reserve = MAX(incoming_len, buf->size / 4);
    if (reserve >= buf->size)
        return 0;
    return buf->size - reserve;
}

static void netlink_buffer_resize_locked(struct netlink_buffer *buf,
                                         size_t new_size) {
    if (!buf)
        return;
    if (new_size < NETLINK_BUFFER_INITIAL_SIZE)
        new_size = NETLINK_BUFFER_INITIAL_SIZE;
    if (new_size > buf->max_size)
        new_size = buf->max_size;
    if (new_size <= buf->size)
        return;

    buf->size = new_size;
    skb_queue_set_limit(&buf->queue, new_size);
}

static bool netlink_buffer_make_room_locked(struct netlink_buffer *buf,
                                            size_t len) {
    size_t target_size;
    size_t trim_target;
    skb_buff_t *skb;

    if (!buf || len == 0)
        return false;
    if (len > buf->max_size)
        return false;

    while (skb_queue_space(&buf->queue) < len && buf->size < buf->max_size) {
        target_size = buf->size ? buf->size : NETLINK_BUFFER_INITIAL_SIZE;
        while (target_size < len && target_size < buf->max_size)
            target_size <<= 1;
        if (target_size < buf->size * 2 && buf->size < buf->max_size)
            target_size = MIN(buf->size * 2, buf->max_size);
        netlink_buffer_resize_locked(buf, target_size);
    }

    trim_target = netlink_buffer_trim_target(buf, len);
    while ((skb_queue_space(&buf->queue) < len ||
            skb_queue_bytes(&buf->queue) > trim_target) &&
           skb_queue_packets(&buf->queue) > 0) {
        skb = skb_queue_pop(&buf->queue);
        if (skb)
            skb_free(skb, buf->queue.priv_destructor);
    }

    buf->used_bytes = skb_queue_bytes(&buf->queue);
    return skb_queue_space(&buf->queue) >= len;
}

static inline void netlink_notify_sock(struct netlink_sock *sock,
                                       uint32_t events) {
    if (!sock || !sock->node || !events)
        return;

    vfs_poll_notify_inode(sock->node, events);
}

static bool netlink_group_mask_matches(uint32_t subscribed_groups,
                                       uint32_t target_groups) {
    return target_groups != 0 && (subscribed_groups & target_groups) != 0;
}

static bool netlink_portid_in_use_locked(const struct netlink_sock *self,
                                         uint32_t portid) {
    if (!portid)
        return true;

    for (int i = 0; i < MAX_NETLINK_SOCKETS; i++) {
        struct netlink_sock *sock = netlink_sockets[i];
        if (!sock || sock == self)
            continue;
        if (sock->portid == portid)
            return true;
    }

    return false;
}

static uint32_t netlink_resolve_portid_locked(struct netlink_sock *self,
                                              uint32_t requested_portid) {
    if (requested_portid) {
        if (netlink_portid_in_use_locked(self, requested_portid))
            return 0;
        return requested_portid;
    }

    uint32_t candidate = (uint32_t)current_task->pid;
    if (!candidate)
        candidate = 1;

    while (netlink_portid_in_use_locked(self, candidate)) {
        candidate++;
        if (!candidate)
            candidate = 1;
    }

    return candidate;
}

static int netlink_wait_file(fd_t *file, struct netlink_sock *sock,
                             uint32_t events, const char *reason) {
    if (!file || !sock || !current_task)
        return -EINVAL;

    const uint32_t want = events | EPOLLERR | EPOLLHUP | EPOLLNVAL | EPOLLRDHUP;

    while (true) {
        int polled = vfs_poll(file, want);
        if (polled < 0)
            return polled;

        if (polled & want)
            return EOK;

        int reason = vfs_poll_wait_interruptible(file, want);
        if (reason < 0)
            return reason;
    }
}

// skb queue operations for netlink packets with sender info
size_t netlink_buffer_write_packet(struct netlink_sock *sock, const char *data,
                                   size_t len, uint32_t nl_pid,
                                   uint32_t nl_groups) {
    struct netlink_buffer *buf = sock->buffer;
    skb_buff_t *skb = NULL;

    if (buf == NULL || data == NULL || len == 0) {
        return 0;
    }

    spin_lock(&sock->lock);
    if (sock->filter) {
        uint32_t accept_bytes = bpf_run(sock->filter->filter, sock->filter->len,
                                        (const uint8_t *)data, (uint32_t)len);
        if (!accept_bytes) {
            spin_unlock(&sock->lock);
            return 0;
        }
        len = MIN(len, accept_bytes);
    }
    spin_unlock(&sock->lock);

    if (len > buf->max_size)
        return 0;

    skb = netlink_packet_build_skb(data, len, nl_pid, nl_groups);
    if (!skb)
        return 0;

    spin_lock(&buf->lock);
    if (!netlink_buffer_make_room_locked(buf, len) ||
        !skb_queue_push(&buf->queue, skb)) {
        spin_unlock(&buf->lock);
        skb_free(skb, netlink_packet_meta_destroy);
        return 0;
    }

    buf->used_bytes = skb_queue_bytes(&buf->queue);
    spin_unlock(&buf->lock);
    netlink_notify_sock(sock, EPOLLIN);
    return len;
}

static size_t netlink_defer_to_socket(struct netlink_sock *target,
                                      const char *data, size_t len,
                                      uint32_t sender_pid,
                                      uint32_t sender_groups) {
    skb_buff_t *skb;

    if (!target || !data || len == 0 || len > NETLINK_BUFFER_MAX_SIZE)
        return 0;

    spin_lock(&target->lock);
    if (target->filter) {
        uint32_t accept_bytes =
            bpf_run(target->filter->filter, target->filter->len,
                    (const uint8_t *)data, (uint32_t)len);
        if (!accept_bytes) {
            spin_unlock(&target->lock);
            return 0;
        }
        len = MIN(len, accept_bytes);
    }
    spin_unlock(&target->lock);

    skb = netlink_packet_build_skb(data, len, sender_pid, sender_groups);
    if (!skb)
        return 0;

    spin_lock(&target->lock);
    while (skb_queue_space(&target->deferred_queue) < len &&
           skb_queue_packets(&target->deferred_queue) > 0)
        skb_queue_drop_head(&target->deferred_queue);
    if (!skb_queue_push(&target->deferred_queue, skb)) {
        spin_unlock(&target->lock);
        skb_free(skb, netlink_packet_meta_destroy);
        return 0;
    }
    spin_unlock(&target->lock);

    netlink_notify_sock(target, EPOLLIN);
    return len;
}

static bool netlink_has_deferred_msg(struct netlink_sock *sock) {
    bool has_msg;

    if (!sock)
        return false;

    spin_lock(&sock->lock);
    has_msg = skb_queue_packets(&sock->deferred_queue) > 0;
    spin_unlock(&sock->lock);
    return has_msg;
}

static void netlink_promote_deferred_if_idle(struct netlink_sock *sock) {
    struct netlink_buffer *buf;
    bool has_queued;

    if (!sock || !sock->buffer)
        return;

    buf = sock->buffer;
    spin_lock(&buf->lock);
    has_queued = skb_queue_packets(&buf->queue) > 0;
    spin_unlock(&buf->lock);
    if (has_queued)
        return;

    for (;;) {
        skb_buff_t *skb;
        size_t len;

        spin_lock(&sock->lock);
        skb = skb_queue_pop(&sock->deferred_queue);
        spin_unlock(&sock->lock);
        if (!skb)
            return;

        len = skb_unread_len(skb);
        spin_lock(&buf->lock);
        if (!netlink_buffer_make_room_locked(buf, len) ||
            !skb_queue_push(&buf->queue, skb)) {
            spin_unlock(&buf->lock);
            skb_free(skb, netlink_packet_meta_destroy);
            return;
        }
        buf->used_bytes = skb_queue_bytes(&buf->queue);
        spin_unlock(&buf->lock);
    }
}

// Read data from buffer with sender info
static size_t netlink_buffer_read_packet(struct netlink_sock *sock, char *out,
                                         size_t out_len, uint32_t *nl_pid,
                                         uint32_t *nl_groups, bool peek) {
    struct netlink_buffer *buf = sock->buffer;
    skb_buff_t *skb = NULL;
    struct netlink_packet_hdr *hdr = NULL;
    size_t packet_len = 0;
    size_t copy_len = 0;

    if (buf == NULL) {
        return 0;
    }

    spin_lock(&buf->lock);
    skb = skb_queue_peek(&buf->queue);
    if (!skb) {
        spin_unlock(&buf->lock);
        return 0;
    }

    hdr = (struct netlink_packet_hdr *)skb->priv;
    packet_len = skb_unread_len(skb);

    if (nl_pid)
        *nl_pid = hdr ? hdr->nl_pid : 0;
    if (nl_groups)
        *nl_groups = hdr ? hdr->nl_groups : 0;

    if (out != NULL && out_len > 0) {
        copy_len = MIN(packet_len, out_len);
        skb_copy_data(skb, 0, out, copy_len);
    } else {
        copy_len = packet_len;
    }

    if (!peek) {
        skb_buff_t *done = skb_queue_pop(&buf->queue);
        if (done) {
            skb_free(done, netlink_packet_meta_destroy);
            buf->used_bytes = skb_queue_bytes(&buf->queue);
        }
    }

    spin_unlock(&buf->lock);
    return copy_len;
}

// Check if a complete message is available
static bool netlink_buffer_has_msg(struct netlink_sock *sock) {
    struct netlink_buffer *buf = sock->buffer;
    bool has_complete_msg = false;

    if (buf == NULL) {
        return false;
    }

    spin_lock(&buf->lock);
    has_complete_msg = skb_queue_packets(&buf->queue) > 0;
    spin_unlock(&buf->lock);
    return has_complete_msg || netlink_has_deferred_msg(sock);
}

// Get the length of next message without consuming it
static size_t netlink_buffer_peek_msg_len(struct netlink_sock *sock) {
    struct netlink_buffer *buf = sock->buffer;
    skb_buff_t *skb = NULL;
    size_t msg_len = 0;

    if (buf == NULL) {
        return 0;
    }

    spin_lock(&buf->lock);
    skb = skb_queue_peek(&buf->queue);
    if (skb)
        msg_len = skb_unread_len(skb);
    spin_unlock(&buf->lock);
    return msg_len;
}

static size_t netlink_buffer_available(struct netlink_buffer *buf) {
    if (buf == NULL) {
        return 0;
    }

    spin_lock(&buf->lock);
    size_t avail = skb_queue_bytes(&buf->queue);
    spin_unlock(&buf->lock);
    return avail;
}

// 内部发送函数，用于向指定 socket 发送消息
static size_t netlink_deliver_to_socket(struct netlink_sock *target,
                                        const char *data, size_t len,
                                        uint32_t sender_pid,
                                        uint32_t sender_groups) {
    if (target == NULL || target->buffer == NULL) {
        return 0;
    }

    return netlink_buffer_write_packet(target, data, len, sender_pid,
                                       sender_groups);
}

// Broadcast message to all listening netlink sockets and save to pool
void netlink_broadcast_to_group(const char *buf, size_t len,
                                uint32_t sender_pid, uint32_t target_groups,
                                int protocol, uint32_t seqnum,
                                const char *devpath) {
    if (buf == NULL || len == 0) {
        return;
    }

    // Add to persistent pool
    netlink_msg_pool_add(buf, len, sender_pid, target_groups, protocol, seqnum,
                         devpath);

    // Broadcast to all sockets subscribed to matching groups
    spin_lock(&netlink_sockets_lock);

    for (int i = 0; i < MAX_NETLINK_SOCKETS; i++) {
        if (netlink_sockets[i] == NULL)
            continue;

        struct netlink_sock *sock = netlink_sockets[i];

        // 检查 protocol 是否匹配
        if (sock->protocol != protocol)
            continue;

        spin_lock(&sock->lock);
        bool subscribed =
            netlink_group_mask_matches(sock->groups, target_groups);
        spin_unlock(&sock->lock);

        if (subscribed) {
            netlink_buffer_write_packet(sock, buf, len, sender_pid,
                                        target_groups);
        }
    }

    spin_unlock(&netlink_sockets_lock);
}

static fd_t *netlink_file_from_fd(uint64_t fd, struct netlink_sock **sock_out) {
    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file || !file->node) {
        vfs_file_put(file);
        return NULL;
    }

    socket_handle_t *handle = sockfs_file_handle(file);
    if (handle == NULL || handle->sock == NULL) {
        vfs_file_put(file);
        return NULL;
    }

    if (sock_out)
        *sock_out = handle->sock;
    return file;
}

int netlink_bind(uint64_t fd, const struct sockaddr_un *addr,
                 socklen_t addrlen) {
    if (addr == NULL || addrlen < sizeof(struct sockaddr_nl)) {
        return -EINVAL;
    }

    struct netlink_sock *sock = NULL;
    fd_t *file = netlink_file_from_fd(fd, &sock);
    if (!file) {
        return -EBADF;
    }

    struct sockaddr_nl *nl_addr = (struct sockaddr_nl *)addr;

    if (nl_addr->nl_family != AF_NETLINK) {
        vfs_file_put(file);
        return -EAFNOSUPPORT;
    }

    spin_lock(&netlink_sockets_lock);
    uint32_t portid = netlink_resolve_portid_locked(sock, nl_addr->nl_pid);
    if (!portid) {
        spin_unlock(&netlink_sockets_lock);
        vfs_file_put(file);
        return -EADDRINUSE;
    }

    spin_lock(&sock->lock);
    sock->portid = portid;
    sock->groups = nl_addr->nl_groups;
    if (nl_addr->nl_groups)
        sock->groups = nl_addr->nl_groups;

    if (sock->bind_addr == NULL) {
        sock->bind_addr = malloc(sizeof(struct sockaddr_nl));
        if (sock->bind_addr == NULL) {
            spin_unlock(&sock->lock);
            spin_unlock(&netlink_sockets_lock);
            vfs_file_put(file);
            return -ENOMEM;
        }
    }
    netlink_fill_sockaddr(sock->bind_addr, sock->portid, sock->groups);
    spin_unlock(&sock->lock);
    spin_unlock(&netlink_sockets_lock);

    vfs_file_put(file);
    return 0;
}

size_t netlink_getsockopt(uint64_t fd, int level, int optname, void *optval,
                          socklen_t *optlen) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }
#define NETLINK_GETSOCKOPT_RETURN(value)                                       \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_TYPE:
            *(int *)optval = nl_sk->type;
            *optlen = sizeof(int);
            break;
        case SO_PROTOCOL:
            *(int *)optval = nl_sk->protocol;
            *optlen = sizeof(int);
            break;
        case SO_REUSEADDR:
            break;
        case SO_PASSCRED:
            if (!optval || !optlen || *optlen < sizeof(int))
                NETLINK_GETSOCKOPT_RETURN(-EINVAL);
            *(int *)optval = nl_sk->passcred ? 1 : 0;
            *optlen = sizeof(int);
            break;
        case SO_SNDBUF:
        case SO_SNDBUFFORCE:
        case SO_RCVBUF:
        case SO_RCVBUFFORCE:
            if (!optval || !optlen || *optlen < sizeof(int))
                NETLINK_GETSOCKOPT_RETURN(-EINVAL);
            *(int *)optval = (int)nl_sk->buffer->size;
            *optlen = sizeof(int);
            break;
        default:
            // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
            //        __LINE__, level, optname);
            NETLINK_GETSOCKOPT_RETURN(-ENOPROTOOPT);
        }
    } else if (level == SOL_NETLINK) {
        NETLINK_GETSOCKOPT_RETURN(0);
    } else {
        // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
        //        __LINE__, level, optname);
        NETLINK_GETSOCKOPT_RETURN(-ENOPROTOOPT);
    }

    vfs_file_put(file);
#undef NETLINK_GETSOCKOPT_RETURN
    return 0;
}

size_t netlink_setsockopt(uint64_t fd, int level, int optname,
                          const void *optval, socklen_t optlen) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }
#define NETLINK_SETSOCKOPT_RETURN(value)                                       \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_ATTACH_FILTER: {
            struct sock_fprog fprog;
            struct sock_fprog *new_filter = NULL;
            struct sock_fprog *old_filter = NULL;
            size_t filter_bytes;

            if (!optval || optlen < sizeof(fprog))
                NETLINK_SETSOCKOPT_RETURN((size_t)-EINVAL);
            memcpy(&fprog, optval, sizeof(fprog));
            if (!fprog.len || !fprog.filter)
                NETLINK_SETSOCKOPT_RETURN((size_t)-EINVAL);
            if (check_user_array_overflow((uint64_t)fprog.filter, fprog.len,
                                          sizeof(struct sock_filter),
                                          &filter_bytes)) {
                NETLINK_SETSOCKOPT_RETURN((size_t)-EFAULT);
            }

            new_filter = calloc(1, sizeof(*new_filter));
            if (!new_filter)
                NETLINK_SETSOCKOPT_RETURN((size_t)-ENOMEM);
            new_filter->filter = malloc(filter_bytes);
            if (!new_filter->filter) {
                netlink_filter_free(new_filter);
                NETLINK_SETSOCKOPT_RETURN((size_t)-ENOMEM);
            }
            new_filter->len = fprog.len;

            if (copy_from_user(new_filter->filter, fprog.filter,
                               filter_bytes)) {
                netlink_filter_free(new_filter);
                NETLINK_SETSOCKOPT_RETURN((size_t)-EFAULT);
            }
            if (bpf_validate(new_filter->filter, new_filter->len) < 0) {
                netlink_filter_free(new_filter);
                NETLINK_SETSOCKOPT_RETURN((size_t)-EINVAL);
            }

            spin_lock(&nl_sk->lock);
            old_filter = nl_sk->filter;
            nl_sk->filter = new_filter;
            spin_unlock(&nl_sk->lock);
            netlink_filter_free(old_filter);
            break;
        }
        case SO_DETACH_FILTER: {
            struct sock_fprog *old_filter = NULL;
            spin_lock(&nl_sk->lock);
            if (nl_sk->filter) {
                old_filter = nl_sk->filter;
                nl_sk->filter = NULL;
            }
            spin_unlock(&nl_sk->lock);
            netlink_filter_free(old_filter);
            break;
        }
        case SO_REUSEADDR:
            break;
        case SO_PASSCRED:
            if (!optval || optlen < sizeof(int))
                NETLINK_SETSOCKOPT_RETURN(-EINVAL);
            spin_lock(&nl_sk->lock);
            nl_sk->passcred = (*(const int *)optval) != 0;
            spin_unlock(&nl_sk->lock);
            break;
        case SO_SNDBUF:
        case SO_SNDBUFFORCE:
        case SO_RCVBUF:
        case SO_RCVBUFFORCE:
            if (!optval || optlen < sizeof(int))
                NETLINK_SETSOCKOPT_RETURN(-EINVAL);
            if (nl_sk->buffer) {
                int req_size = *(const int *)optval;
                size_t new_size = req_size > 0 ? (size_t)req_size
                                               : NETLINK_BUFFER_INITIAL_SIZE;

                spin_lock(&nl_sk->buffer->lock);
                if (new_size > nl_sk->buffer->max_size)
                    nl_sk->buffer->max_size = new_size;
                if (new_size < NETLINK_BUFFER_INITIAL_SIZE)
                    new_size = NETLINK_BUFFER_INITIAL_SIZE;
                if (new_size > nl_sk->buffer->max_size)
                    new_size = nl_sk->buffer->max_size;
                skb_queue_set_limit(&nl_sk->buffer->queue, new_size);
                nl_sk->buffer->size = new_size;
                if (skb_queue_bytes(&nl_sk->buffer->queue) > new_size) {
                    while (skb_queue_bytes(&nl_sk->buffer->queue) > new_size &&
                           skb_queue_packets(&nl_sk->buffer->queue) > 0)
                        skb_queue_drop_head(&nl_sk->buffer->queue);
                }
                nl_sk->buffer->used_bytes =
                    skb_queue_bytes(&nl_sk->buffer->queue);
                spin_unlock(&nl_sk->buffer->lock);
            }
            break;
        default:
            // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
            //        __LINE__, level, optname);
            NETLINK_SETSOCKOPT_RETURN(-ENOPROTOOPT);
        }
    } else if (level == SOL_NETLINK) {
        switch (optname) {
        case NETLINK_ADD_MEMBERSHIP:
        case NETLINK_DROP_MEMBERSHIP: {
            if (!optval || optlen < sizeof(uint32_t))
                NETLINK_SETSOCKOPT_RETURN(-EINVAL);

            uint32_t group = *(const uint32_t *)optval;
            if (group == 0 || group > 32)
                NETLINK_SETSOCKOPT_RETURN(-EINVAL);

            uint32_t bit = 1U << (group - 1);
            spin_lock(&nl_sk->lock);
            if (optname == NETLINK_ADD_MEMBERSHIP) {
                nl_sk->groups |= bit;
            } else {
                nl_sk->groups &= ~bit;
            }
            if (nl_sk->bind_addr)
                nl_sk->bind_addr->nl_groups = nl_sk->groups;
            spin_unlock(&nl_sk->lock);

            NETLINK_SETSOCKOPT_RETURN(0);
        }
        case 3:
        case 11:
        case 12:
            // TODO
            NETLINK_SETSOCKOPT_RETURN(-ENOPROTOOPT);
        default:
            // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
            //        __LINE__, level, optname);
            NETLINK_SETSOCKOPT_RETURN(-ENOPROTOOPT);
        }
    } else {
        // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
        //        __LINE__, level, optname);
        NETLINK_SETSOCKOPT_RETURN(-ENOPROTOOPT);
    }

    vfs_file_put(file);
#undef NETLINK_SETSOCKOPT_RETURN
    return 0;
}

int netlink_getsockname(uint64_t fd, struct sockaddr_un *addr,
                        socklen_t *addrlen) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }

    spin_lock(&nl_sk->lock);
    netlink_fill_sockaddr((struct sockaddr_nl *)addr, nl_sk->portid,
                          nl_sk->groups);
    *addrlen = sizeof(struct sockaddr_nl);

    spin_unlock(&nl_sk->lock);
    vfs_file_put(file);
    return 0;
}

size_t netlink_recvmsg(uint64_t fd, struct msghdr *msg, int flags) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }
#define NETLINK_RECVMSG_RETURN(value)                                          \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    if (nl_sk->buffer == NULL) {
        NETLINK_RECVMSG_RETURN(-EINVAL);
    }

    bool peek = !!(flags & MSG_PEEK);
    char *temp_buf = NULL;
    uint32_t sender_pid = 0;
    uint32_t sender_groups = 0;
    size_t packet_len;
    size_t bytes_read;
    size_t bytes_copied;
    size_t name_len = 0;

    msg->msg_flags = 0;

    int wait_ret =
        netlink_wait_for_message(nl_sk, file, flags, "netlink_recvmsg");
    if (wait_ret < 0)
        NETLINK_RECVMSG_RETURN((size_t)wait_ret);

    netlink_promote_deferred_if_idle(nl_sk);
    packet_len = netlink_buffer_peek_msg_len(nl_sk);
    if (packet_len == 0)
        NETLINK_RECVMSG_RETURN(-EAGAIN);

    temp_buf = malloc(packet_len);
    if (!temp_buf)
        NETLINK_RECVMSG_RETURN(-ENOMEM);

    bytes_read = netlink_buffer_read_packet(nl_sk, temp_buf, packet_len,
                                            &sender_pid, &sender_groups, peek);
    if (bytes_read == 0) {
        free(temp_buf);
        NETLINK_RECVMSG_RETURN(-EAGAIN);
    }

    bytes_copied = netlink_copy_to_iov(msg->msg_iov, msg->msg_iovlen, temp_buf,
                                       bytes_read);

    if (msg->msg_name && msg->msg_namelen > 0) {
        name_len = MIN((size_t)msg->msg_namelen, sizeof(struct sockaddr_nl));
        if (name_len > 0) {
            struct sockaddr_nl nl_addr;
            netlink_fill_sockaddr(&nl_addr, sender_pid, sender_groups);
            memcpy(msg->msg_name, &nl_addr, name_len);
        }
    }
    msg->msg_namelen = msg->msg_name ? sizeof(struct sockaddr_nl) : 0;

    if (msg->msg_control && msg->msg_controllen > 0 && nl_sk->passcred) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
        if (cmsg && msg->msg_controllen >= CMSG_LEN(sizeof(struct ucred))) {
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_CREDENTIALS;

            struct ucred *cred = (struct ucred *)CMSG_DATA(cmsg);
            cred->pid = sender_pid;
            cred->gid = 0;
            cred->uid = 0;

            msg->msg_controllen = cmsg->cmsg_len;
        } else {
            msg->msg_flags |= MSG_CTRUNC;
            msg->msg_controllen = 0;
        }
    } else {
        msg->msg_controllen = 0;
    }

    if (bytes_copied < bytes_read)
        msg->msg_flags |= MSG_TRUNC;

    free(temp_buf);
    size_t ret = (flags & MSG_TRUNC) ? packet_len : bytes_copied;
    vfs_file_put(file);
#undef NETLINK_RECVMSG_RETURN
    return ret;
}

size_t netlink_sendmsg(uint64_t fd, const struct msghdr *msg, int flags) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }
    struct sockaddr_nl *addr = (struct sockaddr_nl *)msg->msg_name;
#define NETLINK_SENDMSG_RETURN(value)                                          \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    if (!msg || !addr)
        NETLINK_SENDMSG_RETURN(-EDESTADDRREQ);
    if (msg->msg_namelen < sizeof(struct sockaddr_nl))
        NETLINK_SENDMSG_RETURN(-EINVAL);
    if (addr->nl_family != AF_NETLINK)
        NETLINK_SENDMSG_RETURN(-EAFNOSUPPORT);

    size_t total_len = netlink_iov_total_len(msg->msg_iov, msg->msg_iovlen);
    if (total_len == 0) {
        NETLINK_SENDMSG_RETURN(0);
    }

    if (total_len == SIZE_MAX ||
        total_len > NETLINK_BUFFER_SIZE - sizeof(struct netlink_packet_hdr)) {
        NETLINK_SENDMSG_RETURN(-EMSGSIZE);
    }

    char buffer[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        if (!msg->msg_iov[i].iov_base || !msg->msg_iov[i].len)
            continue;
        memcpy(buffer + offset, msg->msg_iov[i].iov_base, msg->msg_iov[i].len);
        offset += msg->msg_iov[i].len;
    }

    uint32_t sender_pid = nl_sk->portid;
    uint32_t sender_groups = nl_sk->groups;
    if (sender_pid == 0)
        sender_pid = (uint32_t)current_task->pid;

    if (addr->nl_pid != 0) {
        size_t delivered = 0;
        spin_lock(&netlink_sockets_lock);
        struct netlink_sock *sock =
            netlink_lookup_sock_by_portid_locked(nl_sk->protocol, addr->nl_pid);
        if (sock) {
            delivered = netlink_deliver_to_socket(sock, buffer, total_len,
                                                  sender_pid, 0);
        }
        spin_unlock(&netlink_sockets_lock);
        NETLINK_SENDMSG_RETURN(delivered ? (size_t)total_len
                                         : (size_t)-ECONNREFUSED);
    } else if (addr->nl_groups != 0) {
        netlink_broadcast_to_group(buffer, total_len, sender_pid,
                                   addr->nl_groups, nl_sk->protocol, 0, NULL);
        NETLINK_SENDMSG_RETURN(total_len);
    } else {
        int ret = netlink_send_to_kernel(nl_sk, buffer, total_len, sender_pid);
        NETLINK_SENDMSG_RETURN(ret < 0 ? (size_t)ret : total_len);
    }
#undef NETLINK_SENDMSG_RETURN
}

size_t netlink_sendto(uint64_t fd, uint8_t *in, size_t limit, int flags,
                      struct sockaddr_un *addr, uint32_t len) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }
#define NETLINK_SENDTO_RETURN(value)                                           \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    if (limit == 0) {
        NETLINK_SENDTO_RETURN(0);
    }

    if (limit > NETLINK_BUFFER_SIZE - sizeof(struct netlink_packet_hdr)) {
        NETLINK_SENDTO_RETURN(-EMSGSIZE);
    }

    uint32_t sender_pid = nl_sk->portid;
    uint32_t sender_groups = nl_sk->groups;

    struct sockaddr_nl *nl_addr = (struct sockaddr_nl *)addr;

    if (nl_addr == NULL) {
        NETLINK_SENDTO_RETURN(-EDESTADDRREQ);
    }

    if (len < sizeof(struct sockaddr_nl))
        NETLINK_SENDTO_RETURN(-EINVAL);
    if (nl_addr->nl_family != AF_NETLINK) {
        NETLINK_SENDTO_RETURN(-EAFNOSUPPORT);
    }
    if (sender_pid == 0)
        sender_pid = (uint32_t)current_task->pid;

    if (nl_addr->nl_pid != 0) {
        size_t delivered = 0;
        spin_lock(&netlink_sockets_lock);
        struct netlink_sock *sock = netlink_lookup_sock_by_portid_locked(
            nl_sk->protocol, nl_addr->nl_pid);
        if (sock) {
            delivered = netlink_deliver_to_socket(sock, (char *)in, limit,
                                                  sender_pid, 0);
        }
        spin_unlock(&netlink_sockets_lock);
        NETLINK_SENDTO_RETURN(delivered ? limit : (size_t)-ECONNREFUSED);
    } else if (nl_addr->nl_groups != 0) {
        netlink_broadcast_to_group((char *)in, limit, sender_pid,
                                   nl_addr->nl_groups, nl_sk->protocol, 0,
                                   NULL);
        NETLINK_SENDTO_RETURN(limit);
    }

    int ret = netlink_send_to_kernel(nl_sk, (char *)in, limit, sender_pid);
    NETLINK_SENDTO_RETURN(ret < 0 ? (size_t)ret : limit);
#undef NETLINK_SENDTO_RETURN
}

size_t netlink_recvfrom(uint64_t fd, uint8_t *out, size_t limit, int flags,
                        struct sockaddr_un *addr, uint32_t *len) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }
#define NETLINK_RECVFROM_RETURN(value)                                         \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    if (nl_sk->buffer == NULL) {
        NETLINK_RECVFROM_RETURN(-EINVAL);
    }

    bool peek = !!(flags & MSG_PEEK);
    char *temp_buf = NULL;
    uint32_t sender_pid = 0;
    uint32_t sender_groups = 0;
    int wait_ret =
        netlink_wait_for_message(nl_sk, file, flags, "netlink_recvfrom");
    size_t msg_len;
    size_t bytes_read;

    if (wait_ret < 0)
        NETLINK_RECVFROM_RETURN((size_t)wait_ret);

    netlink_promote_deferred_if_idle(nl_sk);
    msg_len = netlink_buffer_peek_msg_len(nl_sk);
    if (msg_len == 0)
        NETLINK_RECVFROM_RETURN(-EAGAIN);

    temp_buf = malloc(msg_len);
    if (!temp_buf)
        NETLINK_RECVFROM_RETURN(-ENOMEM);

    bytes_read = netlink_buffer_read_packet(nl_sk, temp_buf, msg_len,
                                            &sender_pid, &sender_groups, peek);
    if (bytes_read == 0) {
        free(temp_buf);
        NETLINK_RECVFROM_RETURN(-EAGAIN);
    }

    if (out && limit > 0)
        memcpy(out, temp_buf, MIN(limit, bytes_read));
    free(temp_buf);

    if (addr) {
        struct sockaddr_nl nl_addr_out;
        netlink_fill_sockaddr(&nl_addr_out, sender_pid, sender_groups);
        memcpy(addr, &nl_addr_out,
               MIN((size_t)(len ? *len : sizeof(struct sockaddr_nl)),
                   sizeof(struct sockaddr_nl)));
    }

    if (len) {
        uint32_t addr_len = sizeof(struct sockaddr_nl);
        memcpy(len, &addr_len, sizeof(uint32_t));
    }

    size_t ret = (flags & MSG_TRUNC) ? msg_len : MIN(limit, bytes_read);
    vfs_file_put(file);
#undef NETLINK_RECVFROM_RETURN
    return ret;
}

socket_op_t netlink_ops = {
    .bind = netlink_bind,
    .getsockopt = netlink_getsockopt,
    .setsockopt = netlink_setsockopt,
    .getsockname = netlink_getsockname,
    .sendto = netlink_sendto,
    .recvfrom = netlink_recvfrom,
    .recvmsg = netlink_recvmsg,
    .sendmsg = netlink_sendmsg,
};

int netlink_socket(int domain, int type, int protocol) {
    if (domain != AF_NETLINK) {
        return -EAFNOSUPPORT;
    }
    int sock_type = type & SOCK_TYPE_MASK;
    if (sock_type != SOCK_RAW && sock_type != SOCK_DGRAM) {
        return -ESOCKTNOSUPPORT;
    }

    struct netlink_sock *nl_sk = malloc(sizeof(struct netlink_sock));
    if (nl_sk == NULL) {
        return -ENOMEM;
    }
    memset(nl_sk, 0, sizeof(struct netlink_sock));

    nl_sk->domain = domain;
    nl_sk->type = sock_type;
    nl_sk->protocol = protocol;
    nl_sk->portid = (uint32_t)current_task->pid;
    nl_sk->groups = 0;
    nl_sk->passcred = false;
    nl_sk->net_ns = current_task && current_task->nsproxy
                        ? current_task->nsproxy->net_ns
                        : NULL;
    task_simple_namespace_get(nl_sk->net_ns);
    nl_sk->node = NULL;
    nl_sk->bind_addr = NULL;
    nl_sk->lock = SPIN_INIT;
    skb_queue_init(&nl_sk->deferred_queue, NETLINK_BUFFER_MAX_SIZE,
                   netlink_packet_meta_destroy);

    // Initialize buffer structure
    nl_sk->buffer = malloc(sizeof(struct netlink_buffer));
    if (nl_sk->buffer == NULL) {
        task_simple_namespace_put(nl_sk->net_ns);
        free(nl_sk);
        return -ENOMEM;
    }
    netlink_buffer_init(nl_sk->buffer);

    socket_handle_t *handle = malloc(sizeof(socket_handle_t));
    if (handle == NULL) {
        free(nl_sk->buffer);
        task_simple_namespace_put(nl_sk->net_ns);
        free(nl_sk);
        return -ENOMEM;
    }
    memset(handle, 0, sizeof(socket_handle_t));

    handle->op = &netlink_ops;
    handle->sock = nl_sk;
    handle->read_op = netlink_read_op;
    handle->write_op = netlink_write_op;
    handle->ioctl_op = netlink_ioctl;
    handle->poll_op = netlink_poll;
    handle->release = netlink_handle_release;

    // Add to global socket array
    spin_lock(&netlink_sockets_lock);
    int slot = -1;
    for (int i = 0; i < MAX_NETLINK_SOCKETS; i++) {
        if (netlink_sockets[i] == NULL) {
            netlink_sockets[i] = nl_sk;
            slot = i;
            break;
        }
    }
    spin_unlock(&netlink_sockets_lock);

    if (slot == -1) {
        // No available slot
        free(nl_sk->buffer);
        task_simple_namespace_put(nl_sk->net_ns);
        free(nl_sk);
        free(handle);
        return -ENOMEM;
    }

    uint64_t flags = 0;
    if (type & SOCK_NONBLOCK_FLAG) {
        flags |= O_NONBLOCK;
    }

    struct vfs_file *file = NULL;
    int ret = sockfs_create_handle_file(handle, flags, &file);
    if (ret < 0) {
        spin_lock(&netlink_sockets_lock);
        netlink_sockets[slot] = NULL;
        spin_unlock(&netlink_sockets_lock);
        free(handle);
        free(nl_sk->buffer);
        task_simple_namespace_put(nl_sk->net_ns);
        free(nl_sk);
        return ret;
    }

    nl_sk->node = vfs_igrab(file->f_inode);
    ret = task_install_file(current_task, file,
                            (type & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);

    if (ret < 0) {
        spin_lock(&netlink_sockets_lock);
        netlink_sockets[slot] = NULL;
        spin_unlock(&netlink_sockets_lock);
        return ret;
    }

    return ret;
}

int netlink_socket_pair(int type, int protocol, int *sv) {
    // Netlink doesn't support socketpair
    return -EOPNOTSUPP;
}

int netlink_poll(fd_t *fd, size_t events) {
    socket_handle_t *handle = sockfs_file_handle(fd);
    if (handle == NULL || handle->sock == NULL) {
        return EPOLLERR;
    }

    struct netlink_sock *nl_sk = handle->sock;

    int revents = 0;

    if (events & (EPOLLIN | EPOLLRDNORM)) {
        bool has_msg = netlink_buffer_has_msg(nl_sk);
        if (has_msg) {
            revents |= EPOLLIN | EPOLLRDNORM;
        }
    }

    if (events & (EPOLLOUT | EPOLLWRNORM)) {
        revents |= EPOLLOUT | EPOLLWRNORM;
    }

    return revents;
}

ssize_t netlink_read(uint64_t fd, char *buf, size_t count) {
    return netlink_recvfrom(fd, (uint8_t *)buf, count, 0, NULL, NULL);
}

ssize_t netlink_write(uint64_t fd, const char *buf, size_t count) {
    struct netlink_sock *nl_sk = NULL;
    fd_t *file = netlink_file_from_fd(fd, &nl_sk);
    if (!file) {
        return -EBADF;
    }

    if (nl_sk->protocol == NETLINK_KOBJECT_UEVENT) {
        vfs_file_put(file);
        return count;
    }

    vfs_file_put(file);
    return -EDESTADDRREQ;
}

static ssize_t netlink_read_op(fd_t *fd, void *buf, size_t offset,
                               size_t count) {
    if (!fd || !fd->node || !sockfs_file_handle(fd))
        return -EBADF;

    socket_handle_t *handle = sockfs_file_handle(fd);
    struct netlink_sock *nl_sk = handle->sock;
    if (!nl_sk || !nl_sk->buffer)
        return -EINVAL;

    int wait_ret = netlink_wait_for_message(nl_sk, fd, 0, "netlink_read");
    if (wait_ret < 0)
        return wait_ret;

    netlink_promote_deferred_if_idle(nl_sk);
    uint32_t sender_pid = 0;
    uint32_t sender_groups = 0;
    size_t bytes = netlink_buffer_read_packet(
        nl_sk, (char *)buf, count, &sender_pid, &sender_groups, false);
    if (!bytes)
        return -EAGAIN;
    return (ssize_t)bytes;
}

static ssize_t netlink_write_op(fd_t *fd, const void *buf, size_t offset,
                                size_t count) {
    if (!fd || !fd->node || !sockfs_file_handle(fd))
        return -EBADF;

    socket_handle_t *handle = sockfs_file_handle(fd);
    struct netlink_sock *nl_sk = handle->sock;
    if (!nl_sk)
        return -EBADF;

    if (nl_sk->protocol == NETLINK_KOBJECT_UEVENT)
        return count;

    return -EDESTADDRREQ;
}

int netlink_ioctl(fd_t *fd, ssize_t cmd, ssize_t arg) {
    socket_handle_t *handle = sockfs_file_handle(fd);
    struct netlink_sock *nl_sk = handle->sock;

    switch (cmd) {
    case FIONBIO:
        return 0;
    case SIOCGSKNS:
        if (!nl_sk || !nl_sk->net_ns)
            return -EINVAL;
        return procfs_create_nsfd_for_netns(nl_sk->net_ns);

    default:
        printk("Unsupported netlink ioctl cmd = %#010x\n", cmd);
        return -ENOTTY;
    }
}

static void netlink_handle_release(socket_handle_t *handle) {
    if (handle == NULL) {
        return;
    }

    struct netlink_sock *nl_sk = handle->sock;
    if (nl_sk != NULL) {
        spin_lock(&netlink_sockets_lock);
        for (int i = 0; i < MAX_NETLINK_SOCKETS; i++) {
            if (netlink_sockets[i] == nl_sk) {
                netlink_sockets[i] = NULL;
                break;
            }
        }
        spin_unlock(&netlink_sockets_lock);

        if (nl_sk->node) {
            vfs_node_t *node = nl_sk->node;
            nl_sk->node = NULL;
            vfs_iput(node);
        }
        if (nl_sk->buffer != NULL) {
            skb_queue_purge(&nl_sk->buffer->queue);
            free(nl_sk->buffer);
        }
        if (nl_sk->bind_addr != NULL) {
            free(nl_sk->bind_addr);
        }
        task_simple_namespace_put(nl_sk->net_ns);
        netlink_filter_free(nl_sk->filter);
        skb_queue_purge(&nl_sk->deferred_queue);
        free(nl_sk);
    }

    free(handle);
};

void netlink_init() {
    // Initialize message pool
    spin_lock(&netlink_msg_pool_lock);
    for (int i = 0; i < MAX_NETLINK_MSG_POOL_SIZE; i++) {
        netlink_msg_pool[i].valid = false;
    }
    netlink_msg_pool_next = 0;
    spin_unlock(&netlink_msg_pool_lock);

    regist_socket(16, NULL, netlink_socket, NULL);
}

static int atoi(const char *s) {
    int ans = 0;
    while (is_digit(*s)) {
        ans = ans * 10 + (*s) - '0';
        ++s;
    }
    return ans;
}

void netlink_uevent_resend_by_devpath(const char *devpath) {
    if (devpath == NULL || devpath[0] == '\0') {
        return;
    }

    char buffer[NETLINK_BUFFER_SIZE];
    size_t length = NETLINK_BUFFER_SIZE;
    uint32_t nl_pid = 0;
    uint32_t nl_groups = 0;

    if (netlink_msg_pool_get_by_devpath(devpath, buffer, &length, &nl_pid,
                                        &nl_groups)) {
        // Parse seqnum from the retrieved message
        uint32_t seqnum = 0;
        const char *ptr = buffer;
        while (*ptr) {
            if (strncmp(ptr, "SEQNUM=", 7) == 0) {
                seqnum = atoi(ptr + 7);
                break;
            }
            ptr += strlen(ptr) + 1;
        }

        // Re-broadcast to group
        netlink_broadcast_to_group(buffer, length, nl_pid, nl_groups,
                                   NETLINK_KOBJECT_UEVENT, seqnum, devpath);
    }
}

void netlink_kernel_uevent_send(const char *buf, int len) {
    if (buf == NULL || len <= 0 || len > NETLINK_BUFFER_SIZE) {
        return;
    }

    // Extract seqnum and devpath from uevent message
    uint32_t seqnum = 0;
    char devpath[256] = {0};

    // Parse the uevent message to extract SEQNUM and DEVPATH
    const char *ptr = buf;
    const char *end = buf + len;
    while (ptr < end && *ptr) {
        if (strncmp(ptr, "SEQNUM=", 7) == 0) {
            seqnum = atoi(ptr + 7);
        } else if (strncmp(ptr, "DEVPATH=", 8) == 0) {
            const char *val_end = strchr(ptr + 8, '\0');
            if (val_end) {
                size_t path_len = val_end - (ptr + 8);
                if (path_len < sizeof(devpath)) {
                    memcpy(devpath, ptr + 8, path_len);
                    devpath[path_len] = '\0';
                }
            }
        }
        ptr += strlen(ptr) + 1;
    }

    // Kernel sends with pid=0 and groups=1 (uevent group)
    // This will save to pool and broadcast to existing sockets
    netlink_broadcast_to_group(buf, len, 0, 1, NETLINK_KOBJECT_UEVENT, seqnum,
                               devpath);
}

void netlink_publish_scan_event(netdev_t *dev, bool aborted) {
    netdev_wireless_info_t wireless;
    netdev_scan_state_t scan;
    char buffer[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    struct netlink_sock *sock = NULL;
    uint32_t groups = netlink_group_bit(NL80211_MCGRP_SCAN_ID);

    if (!dev)
        return;
    if (!netdev_get_wireless_info(dev, &wireless))
        return;
    if (!netdev_get_scan_state(dev, &scan))
        return;

    memset(buffer, 0, sizeof(buffer));
    if (genl_append_nl80211_scan_event_msg(buffer, &offset, sizeof(buffer), dev,
                                           &wireless, &scan, aborted) < 0)
        return;

    netlink_broadcast_to_group(buffer, offset, 0, groups, NETLINK_GENERIC, 0,
                               NULL);

    if (!scan.request_portid)
        return;

    spin_lock(&netlink_sockets_lock);
    sock = netlink_lookup_sock_by_portid_locked(NETLINK_GENERIC,
                                                scan.request_portid);
    if (sock) {
        bool subscribed;

        spin_lock(&sock->lock);
        subscribed = netlink_group_mask_matches(sock->groups, groups);
        spin_unlock(&sock->lock);
        if (!subscribed)
            netlink_defer_to_socket(sock, buffer, offset, 0, groups);
    }
    spin_unlock(&netlink_sockets_lock);
}

void netlink_publish_connect_result(netdev_t *dev, uint32_t request_portid,
                                    uint16_t status_code) {
    netdev_wireless_info_t wireless;
    char buffer[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    struct netlink_sock *sock = NULL;
    uint32_t groups = netlink_group_bit(NL80211_MCGRP_MLME_ID);

    if (!dev)
        return;
    if (!netdev_get_wireless_info(dev, &wireless))
        return;

    memset(buffer, 0, sizeof(buffer));
    if (genl_append_nl80211_connect_event_msg(buffer, &offset, sizeof(buffer),
                                              dev, &wireless, 0,
                                              status_code) < 0)
        return;

    netlink_broadcast_to_group(buffer, offset, 0, groups, NETLINK_GENERIC, 0,
                               NULL);

    if (!request_portid)
        return;

    spin_lock(&netlink_sockets_lock);
    sock =
        netlink_lookup_sock_by_portid_locked(NETLINK_GENERIC, request_portid);
    if (sock) {
        bool subscribed;

        spin_lock(&sock->lock);
        subscribed = netlink_group_mask_matches(sock->groups, groups);
        spin_unlock(&sock->lock);
        if (!subscribed)
            netlink_defer_to_socket(sock, buffer, offset, 0, groups);
    }
    spin_unlock(&netlink_sockets_lock);
}

void netlink_publish_disconnect_event(struct netdev *dev,
                                      uint32_t request_portid,
                                      uint16_t reason_code, bool by_ap) {
    netdev_wireless_info_t wireless;
    char buffer[NETLINK_BUFFER_SIZE];
    size_t offset = 0;
    struct netlink_sock *sock = NULL;
    uint32_t groups = netlink_group_bit(NL80211_MCGRP_MLME_ID);

    if (!dev)
        return;
    if (!netdev_get_wireless_info(dev, &wireless))
        return;

    memset(buffer, 0, sizeof(buffer));
    if (genl_append_nl80211_disconnect_event_msg(buffer, &offset,
                                                 sizeof(buffer), dev, &wireless,
                                                 0, reason_code, by_ap) < 0)
        return;

    netlink_broadcast_to_group(buffer, offset, 0, groups, NETLINK_GENERIC, 0,
                               NULL);

    if (!request_portid)
        return;

    spin_lock(&netlink_sockets_lock);
    sock =
        netlink_lookup_sock_by_portid_locked(NETLINK_GENERIC, request_portid);
    if (sock) {
        bool subscribed;

        spin_lock(&sock->lock);
        subscribed = netlink_group_mask_matches(sock->groups, groups);
        spin_unlock(&sock->lock);
        if (!subscribed)
            netlink_defer_to_socket(sock, buffer, offset, 0, groups);
    }
    spin_unlock(&netlink_sockets_lock);
}

void netlink_publish_netdev_event(netdev_t *dev, uint32_t events) {
    char buffer[NETLINK_BUFFER_SIZE];
    struct nlmsghdr req = {0};
    netdev_ipv4_info_t ipv4;
    size_t offset = 0;
    uint16_t msg_type;
    uint32_t groups = netlink_group_bit(RTNLGRP_LINK);

    if (!dev || !events)
        return;

    if (events & NETDEV_EVENT_UNREGISTERED)
        msg_type = RTM_DELLINK;
    else if (events & (NETDEV_EVENT_REGISTERED | NETDEV_EVENT_ADMIN_UP |
                       NETDEV_EVENT_ADMIN_DOWN | NETDEV_EVENT_LINK_UP |
                       NETDEV_EVENT_LINK_DOWN | NETDEV_EVENT_CONFIG_CHANGED))
        msg_type = RTM_NEWLINK;
    else
        return;

    memset(buffer, 0, sizeof(buffer));
    if (rtnetlink_append_link_msg(buffer, &offset, sizeof(buffer), dev,
                                  msg_type, 0, 0) < 0)
        return;

    netlink_broadcast_to_group(buffer, offset, 0, groups, NETLINK_ROUTE, 0,
                               NULL);

    if (!(events & NETDEV_EVENT_CONFIG_CHANGED) ||
        !netdev_get_ipv4_info(dev, &ipv4) || !ipv4.present)
        return;

    memset(&req, 0, sizeof(req));

    offset = 0;
    memset(buffer, 0, sizeof(buffer));
    if (rtnetlink_append_addr_msg(buffer, &offset, sizeof(buffer), &req, dev,
                                  &ipv4, 0) == 0) {
        netlink_broadcast_to_group(buffer, offset, 0,
                                   netlink_group_bit(RTNLGRP_IPV4_IFADDR),
                                   NETLINK_ROUTE, 0, NULL);
    }

    offset = 0;
    memset(buffer, 0, sizeof(buffer));
    if (rtnetlink_append_route_msg(buffer, &offset, sizeof(buffer), &req, dev,
                                   &ipv4, false, 0) == 0) {
        netlink_broadcast_to_group(buffer, offset, 0,
                                   netlink_group_bit(RTNLGRP_IPV4_ROUTE),
                                   NETLINK_ROUTE, 0, NULL);
    }

    if (ipv4.has_default_route) {
        offset = 0;
        memset(buffer, 0, sizeof(buffer));
        if (rtnetlink_append_route_msg(buffer, &offset, sizeof(buffer), &req,
                                       dev, &ipv4, true, 0) == 0) {
            netlink_broadcast_to_group(buffer, offset, 0,
                                       netlink_group_bit(RTNLGRP_IPV4_ROUTE),
                                       NETLINK_ROUTE, 0, NULL);
        }
    }

    offset = 0;
    memset(buffer, 0, sizeof(buffer));
    if (rtnetlink_append_rule_msg(buffer, &offset, sizeof(buffer), &req,
                                  RT_TABLE_MAIN, 32766, 0) == 0) {
        netlink_broadcast_to_group(buffer, offset, 0,
                                   netlink_group_bit(RTNLGRP_IPV4_RULE),
                                   NETLINK_ROUTE, 0, NULL);
    }
}
