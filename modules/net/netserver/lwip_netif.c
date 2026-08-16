#include "netserver_internal.h"

typedef enum naos_lwip_link_state {
    NAOS_LINK_DETACHED,
    NAOS_LINK_ATTACHING,
    NAOS_LINK_ATTACHED,
    NAOS_LINK_STOPPING,
} naos_lwip_link_state_t;

typedef struct naos_lwip_link {
    netdev_t *netdev;
    naos_lwip_link_state_t state;
    uint16_t rx_threads;
} naos_lwip_link_t;

static naos_lwip_link_t naos_link;
static task_t *naos_lwip_manager_task;
static bool naos_lwip_manager_pending;
struct netif naos_lwip_netif;

typedef struct naos_lwip_netdev_event {
    netdev_t *netdev;
    bool admin_up;
    bool link_up;
} naos_lwip_netdev_event_t;

static netdev_t *naos_lwip_device(const naos_lwip_link_t *link) {
    return link ? __atomic_load_n(&link->netdev, __ATOMIC_ACQUIRE) : NULL;
}

static naos_lwip_link_state_t
naos_lwip_link_state(const naos_lwip_link_t *link) {
    return link ? __atomic_load_n(&link->state, __ATOMIC_ACQUIRE)
                : NAOS_LINK_DETACHED;
}

static void naos_lwip_wake_manager(void) {
    __atomic_store_n(&naos_lwip_manager_pending, true, __ATOMIC_RELEASE);
    task_t *manager =
        __atomic_load_n(&naos_lwip_manager_task, __ATOMIC_ACQUIRE);
    if (manager)
        task_unblock(manager, EOK);
}

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

    naos_lwip_publish_ipv4_state(naos_lwip_device(&naos_link));
}

static void naos_lwip_apply_link_state(void *arg) {
    naos_lwip_netdev_event_t *event = (naos_lwip_netdev_event_t *)arg;
    ip4_addr_t zero_addr;

    if (!event) {
        return;
    }

    ip4_addr_set_zero(&zero_addr);

    if (event->netdev != naos_lwip_device(&naos_link)) {
        free(event);
        return;
    }

    if (!event->admin_up) {
#if LWIP_IPV4 && LWIP_DHCP
        netifapi_dhcp_release_and_stop(&naos_lwip_netif);
        netifapi_netif_set_addr(&naos_lwip_netif, &zero_addr, &zero_addr,
                                &zero_addr);
#endif
        netifapi_netif_set_link_down(&naos_lwip_netif);
        netifapi_netif_set_down(&naos_lwip_netif);
        naos_lwip_publish_ipv4_state(event->netdev);
        free(event);
        return;
    }

    netifapi_netif_set_up(&naos_lwip_netif);

    if (!event->link_up) {
#if LWIP_IPV4 && LWIP_DHCP
        netifapi_dhcp_release_and_stop(&naos_lwip_netif);
        netifapi_netif_set_addr(&naos_lwip_netif, &zero_addr, &zero_addr,
                                &zero_addr);
#endif
        netifapi_netif_set_link_down(&naos_lwip_netif);
        naos_lwip_publish_ipv4_state(event->netdev);
        free(event);
        return;
    }

    netifapi_netif_set_link_up(&naos_lwip_netif);

#if LWIP_IPV4
#if LWIP_DHCP
    netifapi_dhcp_start(&naos_lwip_netif);
#endif
#endif

    naos_lwip_publish_ipv4_state(event->netdev);
    free(event);
}

static void naos_lwip_queue_link_state_update(const naos_lwip_link_t *link) {
    naos_lwip_netdev_event_t *event = NULL;

    netdev_t *netdev = naos_lwip_device(link);
    if (!netdev || naos_lwip_link_state(link) != NAOS_LINK_ATTACHED) {
        return;
    }

    event = malloc(sizeof(*event));
    if (!event) {
        return;
    }

    event->netdev = netdev;
    event->admin_up = netdev_admin_is_up(netdev);
    event->link_up = netdev_link_is_up(netdev);

    if (tcpip_callback(naos_lwip_apply_link_state, event) != ERR_OK) {
        free(event);
    }
}

static void naos_lwip_netdev_event(netdev_t *dev, uint32_t events, void *ctx) {
    naos_lwip_link_t *link = (naos_lwip_link_t *)ctx;

    if (!dev || !link || naos_lwip_device(link) != dev) {
        return;
    }
    if (events & NETDEV_EVENT_UNREGISTERING) {
        __atomic_store_n(&link->state, NAOS_LINK_STOPPING, __ATOMIC_RELEASE);
        netdev_unregister_listener(dev, naos_lwip_netdev_event, link);
        naos_lwip_wake_manager();
    }
    if (!(events & (NETDEV_EVENT_ADMIN_UP | NETDEV_EVENT_ADMIN_DOWN |
                    NETDEV_EVENT_LINK_UP | NETDEV_EVENT_LINK_DOWN |
                    NETDEV_EVENT_UNREGISTERING))) {
        return;
    }

    naos_lwip_queue_link_state_update(link);
}

static uint16_t naos_lwip_gso_type_to_netdev(u8_t gso_type) {
    switch (gso_type) {
    case NETIF_GSO_TCPV4:
        return NETDEV_GSO_TCPV4;
    case NETIF_GSO_TCPV6:
        return NETDEV_GSO_TCPV6;
    case NETIF_GSO_UDP:
        return NETDEV_GSO_UDP;
    default:
        return NETDEV_GSO_NONE;
    }
}

static err_t naos_lwip_linkoutput(struct netif *netif, struct pbuf *p) {
    naos_lwip_link_t *link = netif ? (naos_lwip_link_t *)netif->state : NULL;
    netdev_t *netdev = naos_lwip_device(link);
    netdev_iovec_t stack_iov[8];
    netdev_iovec_t *iov = stack_iov;
    uint32_t iovcnt = 0;
    struct pbuf *q = NULL;
    int ret = 0;
    bool gso;

    if (!p || !netdev || naos_lwip_link_state(link) != NAOS_LINK_ATTACHED) {
        return ERR_IF;
    }
    gso = p->gso_type != NETIF_GSO_NONE;

    if (!gso && !p->next && p->len == p->tot_len) {
        ret = netdev_send(netdev, p->payload, (uint32_t)p->len);
        return ret == -EAGAIN ? ERR_MEM : (ret < 0 ? ERR_IF : ERR_OK);
    }

    for (q = p; q; q = q->next) {
        iovcnt++;
    }
    if (iovcnt > LWIP_ARRAYSIZE(stack_iov)) {
        iov = malloc((size_t)iovcnt * sizeof(*iov));
        if (!iov) {
            return ERR_MEM;
        }
    }

    iovcnt = 0;
    for (q = p; q; q = q->next) {
        iov[iovcnt].data = q->payload;
        iov[iovcnt].len = q->len;
        iovcnt++;
    }

    if (gso) {
        netdev_gso_info_t gso_info;
        gso_info.type = naos_lwip_gso_type_to_netdev(p->gso_type);
        gso_info.mss = p->gso_size;
        gso_info.hdr_len = 0; /* driver derives it from the frame */
        ret = netdev_send_gso(netdev, iov, iovcnt, (uint32_t)p->tot_len,
                              &gso_info);
    } else {
        ret = netdev_sendv(netdev, iov, iovcnt, (uint32_t)p->tot_len);
    }

    if (iov != stack_iov) {
        free(iov);
    }
    return ret == -EAGAIN ? ERR_MEM : (ret < 0 ? ERR_IF : ERR_OK);
}

static err_t naos_lwip_netif_init(struct netif *netif) {
    naos_lwip_link_t *link = netif ? (naos_lwip_link_t *)netif->state : NULL;
    netdev_t *netdev = naos_lwip_device(link);

    if (!netif || !netdev) {
        return ERR_IF;
    }

    if (netdev->type == NETDEV_TYPE_WIFI) {
        netif->name[0] = 'w';
        netif->name[1] = 'l';
    } else {
        netif->name[0] = 'e';
        netif->name[1] = 'n';
    }
    netif->hostname = "naos";
    netif->output = etharp_output;
    netif->linkoutput = naos_lwip_linkoutput;
    netif->mtu = (u16_t)netdev->mtu;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, netdev->mac, 6);
    netif->flags =
        NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;

    {
        uint32_t caps = netdev_get_caps(netdev);
        netif->chksum_flags = NETIF_CHECKSUM_ENABLE_ALL;
        if (caps & NETDEV_CAP_TX_CSUM) {
            netif->chksum_flags &=
                ~(NETIF_CHECKSUM_GEN_TCP | NETIF_CHECKSUM_GEN_UDP);
        }
        if (caps & NETDEV_CAP_RX_CSUM) {
            netif->chksum_flags &=
                ~(NETIF_CHECKSUM_CHECK_IP | NETIF_CHECKSUM_CHECK_TCP |
                  NETIF_CHECKSUM_CHECK_UDP);
        }
        if (caps & (NETDEV_CAP_TSO4 | NETDEV_CAP_TSO6)) {
            netif->flags |= NETIF_FLAG_TSO;
        }
    }
    if (netdev_admin_is_up(netdev)) {
        netif->flags |= NETIF_FLAG_UP;
    }
    if (netdev_link_is_up(netdev)) {
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

typedef struct naos_lwip_rx_ctx {
    naos_lwip_link_t *link;
    netdev_t *netdev;
    uint16_t qidx;
} naos_lwip_rx_ctx_t;

static bool naos_lwip_rx_stopping(const naos_lwip_rx_ctx_t *ctx) {
    return !ctx || naos_lwip_device(ctx->link) != ctx->netdev ||
           naos_lwip_link_state(ctx->link) == NAOS_LINK_STOPPING;
}

static void naos_lwip_rx_thread(uint64_t arg) {
    naos_lwip_rx_ctx_t *ctx = (naos_lwip_rx_ctx_t *)arg;
    naos_lwip_link_t *link = ctx->link;
    netdev_t *netdev = ctx->netdev;
    uint16_t qidx = ctx->qidx;
    uint32_t max_len = 0;
    uint32_t rx_budget = 0;
    bool gro = false;
    struct pbuf *rx_pbuf = NULL;

    if (!link || !netdev) {
        if (netdev)
            netdev_put(netdev);
        if (link) {
            __atomic_fetch_sub(&link->rx_threads, 1, __ATOMIC_ACQ_REL);
            naos_lwip_wake_manager();
        }
        free(ctx);
        return;
    }

    gro = (netdev_get_caps(netdev) & NETDEV_CAP_GRO) != 0;
    if (gro) {
        /* GRO: a coalesced segment can be much larger than the MTU, so use a
         * contiguous buffer large enough for a full GSO segment. */
        max_len = NETDEV_GSO_MAX_SIZE + 256;
    } else {
        max_len = netdev_max_frame_len(netdev->mtu);
    }

    for (;;) {
        int len;

        if (naos_lwip_rx_stopping(ctx)) {
            break;
        }

        if (!rx_pbuf) {
            rx_pbuf = pbuf_alloc(PBUF_RAW, (u16_t)max_len,
                                 gro ? PBUF_RAM : PBUF_POOL);
            if (!rx_pbuf) {
                (void)task_block(current_task, TASK_BLOCKING, 1000000,
                                 "lwip_rx_nomem");
                if (naos_lwip_rx_stopping(ctx))
                    break;
                continue;
            }
        }

        /* The RX thread holds a netdev reference for its whole lifetime, so
         * this fast path can skip the per-packet refcount pair. */
        len = netdev_recv_noref_q(netdev, qidx, rx_pbuf->payload, max_len);
        if (len <= 0) {
            uint64_t rx_seq;

            if (len == -ENODEV || naos_lwip_rx_stopping(ctx)) {
                break;
            }

            /* Sample the RX sequence only when we are about to block.  The
             * busy path above never needs it, avoiding one lock per packet. */
            rx_seq = netdev_rx_seq(netdev);
            int wait_ret = netdev_wait_rx_q(netdev, qidx, rx_seq);
            if (wait_ret == -ENODEV || naos_lwip_rx_stopping(ctx))
                break;
            continue;
        }

        pbuf_realloc(rx_pbuf, (u16_t)len);

        err_t input_err;
        while ((input_err = naos_lwip_netif.input(rx_pbuf, &naos_lwip_netif)) ==
               ERR_MEM) {
            if (naos_lwip_rx_stopping(ctx))
                break;
            (void)task_block(current_task, TASK_BLOCKING, 1000000,
                             "lwip_rx_backpressure");
        }

        if (input_err != ERR_OK) {
            pbuf_free(rx_pbuf);
            rx_pbuf = NULL;
            if (naos_lwip_rx_stopping(ctx))
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
    netdev_put(netdev);
    __atomic_fetch_sub(&link->rx_threads, 1, __ATOMIC_ACQ_REL);
    naos_lwip_wake_manager();
    free(ctx);
}

static void naos_lwip_start_rx_threads(naos_lwip_link_t *link,
                                       netdev_t *netdev) {
    uint16_t queue_count = netdev_rx_queue_count(netdev);

    for (uint16_t queue = 0; queue < queue_count; queue++) {
        naos_lwip_rx_ctx_t *ctx;

        if (naos_lwip_link_state(link) != NAOS_LINK_ATTACHED ||
            !netdev_get(netdev))
            break;
        ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            netdev_put(netdev);
            break;
        }
        ctx->link = link;
        ctx->netdev = netdev;
        ctx->qidx = queue;
        __atomic_fetch_add(&link->rx_threads, 1, __ATOMIC_ACQ_REL);
        if (!task_create("lwip-rx", naos_lwip_rx_thread, (uint64_t)ctx,
                         NORMAL_PRIORITY)) {
            __atomic_fetch_sub(&link->rx_threads, 1, __ATOMIC_ACQ_REL);
            netdev_put(netdev);
            free(ctx);
            break;
        }
    }
}

static int naos_lwip_attach(netdev_t *netdev) {
    ip4_addr_t ipaddr, netmask, gateway;
    naos_lwip_link_state_t expected;
    bool listener_registered = false;
    bool netif_added = false;

    if (!netdev)
        return -EINVAL;

    __atomic_store_n(&naos_link.netdev, netdev, __ATOMIC_RELEASE);
    __atomic_store_n(&naos_link.state, NAOS_LINK_ATTACHING, __ATOMIC_RELEASE);
    __atomic_store_n(&naos_link.rx_threads, 0, __ATOMIC_RELEASE);
    memset(&naos_lwip_netif, 0, sizeof(naos_lwip_netif));

    if (netdev_register_listener(netdev, naos_lwip_netdev_event, &naos_link) !=
        0)
        goto fail;
    listener_registered = true;

    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gateway);
    if (netifapi_netif_add(&naos_lwip_netif, &ipaddr, &netmask, &gateway,
                           &naos_link, naos_lwip_netif_init,
                           tcpip_input) != ERR_OK)
        goto fail;
    netif_added = true;

#if LWIP_NETIF_STATUS_CALLBACK
    netif_set_status_callback(&naos_lwip_netif, naos_lwip_status_callback);
#endif
    netifapi_netif_set_default(&naos_lwip_netif);

    expected = NAOS_LINK_ATTACHING;
    if (!__atomic_compare_exchange_n(&naos_link.state, &expected,
                                     NAOS_LINK_ATTACHED, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        goto fail;

    naos_lwip_start_rx_threads(&naos_link, netdev);
    naos_lwip_queue_link_state_update(&naos_link);
    printk("netserver: attached netdev %s\n", netdev->name);
    return 0;

fail:
    __atomic_store_n(&naos_link.state, NAOS_LINK_STOPPING, __ATOMIC_RELEASE);
    if (listener_registered)
        netdev_unregister_listener(netdev, naos_lwip_netdev_event, &naos_link);
    if (netif_added)
        netifapi_netif_remove(&naos_lwip_netif);
    memset(&naos_lwip_netif, 0, sizeof(naos_lwip_netif));
    __atomic_store_n(&naos_link.netdev, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&naos_link.state, NAOS_LINK_DETACHED, __ATOMIC_RELEASE);
    netdev_put(netdev);
    return -EIO;
}

static void naos_lwip_detach(void) {
    netdev_t *netdev = naos_lwip_device(&naos_link);
    netdev_ipv4_info_t ipv4 = {0};
    ip4_addr_t zero_addr;

    if (!netdev) {
        __atomic_store_n(&naos_link.state, NAOS_LINK_DETACHED,
                         __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&naos_link.rx_threads, __ATOMIC_ACQUIRE))
        return;

    netdev_unregister_listener(netdev, naos_lwip_netdev_event, &naos_link);
    ip4_addr_set_zero(&zero_addr);
#if LWIP_IPV4 && LWIP_DHCP
    netifapi_dhcp_release_and_stop(&naos_lwip_netif);
#endif
#if LWIP_IPV4
    netifapi_netif_set_addr(&naos_lwip_netif, &zero_addr, &zero_addr,
                            &zero_addr);
#endif
    netifapi_netif_set_link_down(&naos_lwip_netif);
    netifapi_netif_set_down(&naos_lwip_netif);
    netdev_set_ipv4_info(netdev, &ipv4);
    netifapi_netif_remove(&naos_lwip_netif);
    memset(&naos_lwip_netif, 0, sizeof(naos_lwip_netif));

    __atomic_store_n(&naos_link.netdev, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&naos_link.state, NAOS_LINK_DETACHED, __ATOMIC_RELEASE);
    printk("netserver: detached netdev %s\n", netdev->name);
    netdev_put(netdev);
}

static void naos_lwip_topology_event(netdev_t *netdev, uint32_t events,
                                     void *ctx) {
    (void)netdev;
    (void)ctx;
    if (events & (NETDEV_EVENT_REGISTERED | NETDEV_EVENT_UNREGISTERING |
                  NETDEV_EVENT_UNREGISTERED))
        naos_lwip_wake_manager();
}

static void naos_lwip_manager_wait(void) {
    if (__atomic_exchange_n(&naos_lwip_manager_pending, false,
                            __ATOMIC_ACQ_REL))
        return;

    task_prepare_block(current_task);
    if (!__atomic_exchange_n(&naos_lwip_manager_pending, false,
                             __ATOMIC_ACQ_REL))
        (void)task_block(current_task, TASK_BLOCKING, 1000000000LL,
                         "lwip-netdev");
    task_cancel_block_prepare(current_task);
}

static void naos_lwip_manager_thread(uint64_t arg) {
    (void)arg;

    for (;;) {
        naos_lwip_link_state_t state = naos_lwip_link_state(&naos_link);

        if (state == NAOS_LINK_DETACHED) {
            netdev_t *netdev = get_default_netdev();
            if (netdev) {
                if (naos_lwip_attach(netdev) == 0)
                    continue;
            }
        } else if (state == NAOS_LINK_STOPPING &&
                   !__atomic_load_n(&naos_link.rx_threads, __ATOMIC_ACQUIRE)) {
            naos_lwip_detach();
            continue;
        }
        naos_lwip_manager_wait();
    }
}

int lwip_module_init() {
    static bool core_initialized;
    static bool initialized;
    sys_sem_t init_sem = NULL;
    task_t *manager;

    if (initialized)
        return 0;

    if (!core_initialized) {
        naos_lwip_thread_sem_registry_init();
        if (sys_sem_new(&init_sem, 0) != ERR_OK)
            return -ENOMEM;

        tcpip_init(naos_lwip_tcpip_init_done, &init_sem);
        sys_arch_sem_wait(&init_sem, 0);
        sys_sem_free(&init_sem);
        core_initialized = true;
    }

    memset(&naos_link, 0, sizeof(naos_link));
    memset(&naos_lwip_netif, 0, sizeof(naos_lwip_netif));
    __atomic_store_n(&naos_link.state, NAOS_LINK_DETACHED, __ATOMIC_RELEASE);
    if (netdev_register_notifier(naos_lwip_topology_event, NULL) != 0)
        return -ENOSPC;

    manager = task_create("lwip-netdev", naos_lwip_manager_thread, 0,
                          NORMAL_PRIORITY);
    if (!manager) {
        netdev_unregister_notifier(naos_lwip_topology_event, NULL);
        return -ENOMEM;
    }
    __atomic_store_n(&naos_lwip_manager_task, manager, __ATOMIC_RELEASE);
    initialized = true;
    naos_lwip_wake_manager();
    printk("netserver: lwIP ready, waiting for netdev\n");
    return 0;
}
