#include "netserver_internal.h"

typedef struct naos_lwip_link {
    netdev_t *netdev;
    bool netdev_ref_held;
    bool stopping;
    bool use_static_ipv4;
    ip4_addr_t static_ipaddr;
    ip4_addr_t static_netmask;
    ip4_addr_t static_gw;
} naos_lwip_link_t;

static naos_lwip_link_t naos_link;
struct netif naos_lwip_netif;

typedef struct naos_lwip_netdev_event {
    bool admin_up;
    bool link_up;
    bool use_static_ipv4;
    ip4_addr_t static_ipaddr;
    ip4_addr_t static_netmask;
    ip4_addr_t static_gw;
} naos_lwip_netdev_event_t;

static void naos_lwip_publish_ipv4_state(netdev_t *netdev) {
    netdev_ipv4_info_t info;

    if (!netdev) {
        return;
    }

    memset(&info, 0, sizeof(info));

    if (netdev_admin_is_up(netdev) && netdev_link_is_up(netdev) &&
        !ip4_addr_isany_val(*netif_ip4_addr(&naos_lwip_netif))) {
        info.present = true;
        info.address = ip4_addr_get_u32(netif_ip4_addr(&naos_lwip_netif));
        info.netmask = ip4_addr_get_u32(netif_ip4_netmask(&naos_lwip_netif));
        info.gateway = ip4_addr_get_u32(netif_ip4_gw(&naos_lwip_netif));
        info.has_default_route = info.gateway != 0;
    }

    netdev_set_ipv4_info(netdev, &info);
}

static void naos_lwip_status_callback(struct netif *netif) {
    char ipaddr_buf[IP4ADDR_STRLEN_MAX];
    char netmask_buf[IP4ADDR_STRLEN_MAX];
    char gw_buf[IP4ADDR_STRLEN_MAX];

    if (!netif) {
        return;
    }

#if LWIP_IPV4 && LWIP_DHCP
    if (dhcp_supplied_address(netif)) {
        printk("netserver: ipv4=%s netmask=%s gw=%s\n",
               ip4addr_ntoa_r(netif_ip4_addr(netif), ipaddr_buf,
                              sizeof(ipaddr_buf)),
               ip4addr_ntoa_r(netif_ip4_netmask(netif), netmask_buf,
                              sizeof(netmask_buf)),
               ip4addr_ntoa_r(netif_ip4_gw(netif), gw_buf, sizeof(gw_buf)));
    }
#endif

    naos_lwip_publish_ipv4_state(naos_link.netdev);
}

static void naos_lwip_apply_link_state(void *arg) {
    naos_lwip_netdev_event_t *event = (naos_lwip_netdev_event_t *)arg;
    ip4_addr_t zero_addr;

    if (!event) {
        return;
    }

    ip4_addr_set_zero(&zero_addr);

    if (!event->admin_up) {
#if LWIP_IPV4 && LWIP_DHCP
        if (!event->use_static_ipv4) {
            netifapi_dhcp_release_and_stop(&naos_lwip_netif);
            netifapi_netif_set_addr(&naos_lwip_netif, &zero_addr, &zero_addr,
                                    &zero_addr);
        }
#endif
        netifapi_netif_set_link_down(&naos_lwip_netif);
        netifapi_netif_set_down(&naos_lwip_netif);
        naos_lwip_publish_ipv4_state(naos_link.netdev);
        free(event);
        return;
    }

    netifapi_netif_set_up(&naos_lwip_netif);

    if (!event->link_up) {
#if LWIP_IPV4 && LWIP_DHCP
        if (!event->use_static_ipv4) {
            netifapi_dhcp_release_and_stop(&naos_lwip_netif);
            netifapi_netif_set_addr(&naos_lwip_netif, &zero_addr, &zero_addr,
                                    &zero_addr);
        }
#endif
        netifapi_netif_set_link_down(&naos_lwip_netif);
        naos_lwip_publish_ipv4_state(naos_link.netdev);
        free(event);
        return;
    }

    netifapi_netif_set_link_up(&naos_lwip_netif);

#if LWIP_IPV4
    if (event->use_static_ipv4) {
        netifapi_netif_set_addr(&naos_lwip_netif, &event->static_ipaddr,
                                &event->static_netmask, &event->static_gw);
    }
#if LWIP_DHCP
    else {
        netifapi_dhcp_start(&naos_lwip_netif);
    }
#endif
#endif

    naos_lwip_publish_ipv4_state(naos_link.netdev);
    free(event);
}

static void naos_lwip_queue_link_state_update(const naos_lwip_link_t *link) {
    naos_lwip_netdev_event_t *event = NULL;

    if (!link || !link->netdev) {
        return;
    }

    event = malloc(sizeof(*event));
    if (!event) {
        return;
    }

    event->admin_up = netdev_admin_is_up(link->netdev);
    event->link_up = netdev_link_is_up(link->netdev);
    event->use_static_ipv4 = link->use_static_ipv4;
    event->static_ipaddr = link->static_ipaddr;
    event->static_netmask = link->static_netmask;
    event->static_gw = link->static_gw;

    if (tcpip_callback(naos_lwip_apply_link_state, event) != ERR_OK) {
        free(event);
    }
}

static void naos_lwip_netdev_event(netdev_t *dev, uint32_t events, void *ctx) {
    naos_lwip_link_t *link = (naos_lwip_link_t *)ctx;

    if (!dev || !link || link->netdev != dev) {
        return;
    }
    if (events & NETDEV_EVENT_UNREGISTERING) {
        link->stopping = true;
        netdev_unregister_listener(dev, naos_lwip_netdev_event, link);
    }
    if (!(events & (NETDEV_EVENT_ADMIN_UP | NETDEV_EVENT_ADMIN_DOWN |
                    NETDEV_EVENT_LINK_UP | NETDEV_EVENT_LINK_DOWN |
                    NETDEV_EVENT_UNREGISTERING))) {
        return;
    }

    naos_lwip_queue_link_state_update(link);
}

static err_t naos_lwip_linkoutput(struct netif *netif, struct pbuf *p) {
    naos_lwip_link_t *link = netif ? (naos_lwip_link_t *)netif->state : NULL;
    uint8_t *frame = NULL;
    size_t copied = 0;
    struct pbuf *q = NULL;

    if (!link || !link->netdev || link->stopping || !p) {
        return ERR_IF;
    }

    if (!p->next && p->len == p->tot_len) {
        return netdev_send(link->netdev, p->payload, (uint32_t)p->len) < 0
                   ? ERR_IF
                   : ERR_OK;
    }

    frame = alloc_frames_bytes(p->tot_len);
    if (!frame) {
        return ERR_MEM;
    }

    for (q = p; q; q = q->next) {
        memcpy(frame + copied, q->payload, q->len);
        copied += q->len;
    }

    if (netdev_send(link->netdev, frame, (uint32_t)p->tot_len) < 0) {
        free_frames_bytes(frame, p->tot_len);
        return ERR_IF;
    }

    free_frames_bytes(frame, p->tot_len);
    return ERR_OK;
}

static err_t naos_lwip_netif_init(struct netif *netif) {
    naos_lwip_link_t *link = netif ? (naos_lwip_link_t *)netif->state : NULL;

    if (!netif || !link || !link->netdev) {
        return ERR_IF;
    }

    if (link->netdev->type == NETDEV_TYPE_WIFI) {
        netif->name[0] = 'w';
        netif->name[1] = 'l';
    } else {
        netif->name[0] = 'e';
        netif->name[1] = 'n';
    }
    netif->hostname = "naos";
    netif->output = etharp_output;
    netif->linkoutput = naos_lwip_linkoutput;
    netif->mtu = (u16_t)link->netdev->mtu;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, link->netdev->mac, 6);
    netif->flags =
        NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    if (netdev_admin_is_up(link->netdev)) {
        netif->flags |= NETIF_FLAG_UP;
    }
    if (netdev_link_is_up(link->netdev)) {
        netif->flags |= NETIF_FLAG_LINK_UP;
    }

#if LWIP_IPV6
    netif_create_ip6_linklocal_address(netif, 1);
    netif->ip6_autoconfig_enabled = 1;
#endif

    return ERR_OK;
}

static void naos_lwip_tcpip_init_done(void *arg) {
    sys_sem_t *sem = (sys_sem_t *)arg;
    sys_sem_signal(sem);
}

static uint32_t naos_prefixlen_to_mask_u32(uint8_t prefixlen) {
    if (prefixlen == 0) {
        return 0;
    }
    if (prefixlen >= 32) {
        return 0xFFFFFFFFU;
    }
    return __builtin_bswap32(~((1U << (32 - prefixlen)) - 1));
}

static void naos_lwip_rx_thread(uint64_t arg) {
    naos_lwip_link_t *link = (naos_lwip_link_t *)arg;
    uint32_t max_len = 0;
    uint32_t rx_budget = 0;
    struct pbuf *rx_pbuf = NULL;

    if (!link || !link->netdev) {
        return;
    }

    max_len = netdev_max_frame_len(link->netdev->mtu);

    for (;;) {
        uint64_t rx_seq;

        if (link->stopping || !link->netdev) {
            break;
        }

        rx_seq = netdev_rx_seq(link->netdev);
        if (!rx_pbuf) {
            rx_pbuf = pbuf_alloc(PBUF_RAW, (u16_t)max_len, PBUF_POOL);
            if (!rx_pbuf) {
                (void)task_block(current_task, TASK_BLOCKING, 1000000,
                                 "lwip_rx_nomem");
                if (link->stopping)
                    break;
                continue;
            }
        }

        int len = netdev_recv(link->netdev, rx_pbuf->payload, max_len);
        if (len <= 0) {
            if (len == -ENODEV || link->stopping) {
                break;
            }
            int wait_ret = netdev_wait_rx(link->netdev, rx_seq);
            if (wait_ret == -ENODEV || link->stopping)
                break;
            continue;
        }

        pbuf_realloc(rx_pbuf, (u16_t)len);

        err_t input_err;
        while ((input_err = naos_lwip_netif.input(rx_pbuf, &naos_lwip_netif)) ==
               ERR_MEM) {
            if (link->stopping)
                break;
            (void)task_block(current_task, TASK_BLOCKING, 1000000,
                             "lwip_rx_backpressure");
        }

        if (input_err != ERR_OK) {
            pbuf_free(rx_pbuf);
            rx_pbuf = NULL;
            if (link->stopping)
                break;
            continue;
        }
        rx_pbuf = NULL;

        if (++rx_budget >= 64) {
            rx_budget = 0;
            schedule(SCHED_FLAG_YIELD);
        }
    }

    if (rx_pbuf) {
        pbuf_free(rx_pbuf);
    }
    if (link->netdev_ref_held && link->netdev) {
        netdev_put(link->netdev);
        link->netdev_ref_held = false;
    }
    link->netdev = NULL;
}

int lwip_module_init() {
    static bool initialized = false;
    sys_sem_t init_sem = NULL;
    ip4_addr_t ipaddr, netmask, gw;
    netdev_t *netdev = NULL;
    int32_t ifindex = 0;
    uint32_t ipv4_addr = 0;
    uint8_t prefixlen = 0;
    uint32_t gateway = 0;
    bool use_static_ipv4 = false;

    if (initialized) {
        return 0;
    }

    naos_lwip_thread_sem_registry_init();

    netdev = get_default_netdev();
    if (!netdev) {
        printk("netserver: no netdev registered, lwIP stack stays offline\n");
        return -ENODEV;
    }

    if (sys_sem_new(&init_sem, 0) != ERR_OK) {
        return -ENOMEM;
    }

    tcpip_init(naos_lwip_tcpip_init_done, &init_sem);
    sys_arch_sem_wait(&init_sem, 0);
    sys_sem_free(&init_sem);

    memset(&naos_link, 0, sizeof(naos_link));
    memset(&naos_lwip_netif, 0, sizeof(naos_lwip_netif));
    naos_link.netdev = netdev;
    if (!netdev_get(netdev)) {
        return -ENODEV;
    }
    naos_link.netdev_ref_held = true;

    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);

    if (netifapi_netif_add(&naos_lwip_netif, &ipaddr, &netmask, &gw, &naos_link,
                           naos_lwip_netif_init, tcpip_input) != ERR_OK) {
        netdev_put(netdev);
        naos_link.netdev_ref_held = false;
        naos_link.netdev = NULL;
        return -EIO;
    }

#if LWIP_NETIF_STATUS_CALLBACK
    netif_set_status_callback(&naos_lwip_netif, naos_lwip_status_callback);
#endif

    netifapi_netif_set_default(&naos_lwip_netif);
    if (netdev_register_listener(netdev, naos_lwip_netdev_event, &naos_link) !=
        0) {
        netdev_put(netdev);
        naos_link.netdev_ref_held = false;
        naos_link.netdev = NULL;
        return -EIO;
    }
    naos_lwip_queue_link_state_update(&naos_link);

    task_create("lwip-rx", naos_lwip_rx_thread, (uint64_t)&naos_link,
                NORMAL_PRIORITY);

    initialized = true;
    return 0;
}
