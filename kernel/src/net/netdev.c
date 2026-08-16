#include <net/netdev.h>
#include <net/netlink.h>
#include <mm/mm.h>
#include <task/task.h>
#include <task/signal.h>
#include <fs/sys.h>

netdev_t *netdevs[MAX_NETDEV_NUM] = {NULL};
static spinlock_t netdevs_lock = SPIN_INIT;
static netdev_notifier_t netdev_notifiers[NETDEV_MAX_NOTIFIERS];
static spinlock_t netdev_notifiers_lock = SPIN_INIT;

#define NETDEV_RX_POLL_FALLBACK_NS (2ULL * 1000ULL * 1000ULL)

static void netdev_default_name(char *name, uint32_t type, uint32_t id) {
    if (!name) {
        return;
    }

    if (type == NETDEV_TYPE_WIFI) {
        snprintf(name, NETDEV_NAME_LEN, "wlan%u", id);
        return;
    }

    snprintf(name, NETDEV_NAME_LEN, "net%u", id);
}

static void netdev_create_sysfs(netdev_t *dev) {
    char path[256];
    char buf[512];
    vfs_node_t *net_dir;
    vfs_node_t *node;
    int len;

    if (!dev)
        return;

    snprintf(path, sizeof(path), "/sys/class/net/%s", dev->name);
    net_dir = sysfs_ensure_dir(path);
    if (!net_dir)
        return;

    node = sysfs_child_append(net_dir, "ifindex", false);
    if (node) {
        snprintf(buf, sizeof(buf), "%u\n", dev->id + 1);
        sysfs_write_node(node, buf, strlen(buf), 0);
        vfs_iput(node);
    }

    node = sysfs_child_append(net_dir, "uevent", false);
    if (node) {
        if (dev->type == NETDEV_TYPE_WIFI) {
            len = snprintf(buf, sizeof(buf),
                           "DEVTYPE=wlan\nINTERFACE=%s\nIFINDEX=%u\n",
                           dev->name, dev->id + 1);
        } else {
            len = snprintf(buf, sizeof(buf), "INTERFACE=%s\nIFINDEX=%u\n",
                           dev->name, dev->id + 1);
        }
        sysfs_write_node(node, buf, (size_t)len, 0);
        vfs_iput(node);
    }

    if (dev->type == NETDEV_TYPE_WIFI) {
        node = sysfs_child_append(net_dir, "phy80211", true);
        if (node)
            vfs_iput(node);

        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%u", dev->id);
        node = sysfs_ensure_dir(path);
        if (node) {
            vfs_node_t *child;
            char phy_path[256];

            snprintf(phy_path, sizeof(phy_path), "/sys/class/net/%s/phy80211",
                     dev->name);

            child = sysfs_child_append(node, "name", false);
            if (child) {
                snprintf(buf, sizeof(buf), "%s\n",
                         dev->wireless.wiphy_name[0] ? dev->wireless.wiphy_name
                                                     : "phy0");
                sysfs_write_node(child, buf, strlen(buf), 0);
                vfs_iput(child);
            }

            child = sysfs_child_append(node, "type", false);
            if (child) {
                sysfs_write_node(child, "wlan\n", 5, 0);
                vfs_iput(child);
            }

            child = sysfs_child_append(node, "state", false);
            if (child) {
                sysfs_write_node(child, "1\n", 2, 0);
                vfs_iput(child);
            }

            child = sysfs_child_append(node, "soft", false);
            if (child) {
                sysfs_write_node(child, "0\n", 2, 0);
                vfs_iput(child);
            }

            child = sysfs_child_append(node, "hard", false);
            if (child) {
                sysfs_write_node(child, "0\n", 2, 0);
                vfs_iput(child);
            }

            child = sysfs_child_append_symlink(node, "device", phy_path);
            if (child)
                vfs_iput(child);
            vfs_iput(node);
        }
    }

    vfs_iput(net_dir);

    {
        char uevent_nl[512];
        size_t nl_len = 0;
        bool last_was_nul = false;
        int seqnum = alloc_seq_num();
        const char *devpath = path + 4;

        len = snprintf(buf, sizeof(buf),
                       "add@%s\nACTION=add\nDEVPATH=%s\nSUBSYSTEM=net\n"
                       "INTERFACE=%s\nIFINDEX=%u\nSEQNUM=%d\n",
                       devpath, devpath, dev->name, dev->id + 1, seqnum);

        if (dev->type == NETDEV_TYPE_WIFI && (size_t)len < sizeof(buf)) {
            int extra = snprintf(buf + len, sizeof(buf) - (size_t)len,
                                 "DEVTYPE=wlan\n");
            if (extra > 0)
                len += extra;
        }
        if ((size_t)len >= sizeof(buf))
            len = (int)(sizeof(buf) - 1);

        for (size_t i = 0; i < (size_t)len && nl_len < sizeof(uevent_nl) - 1;
             i++) {
            char c = buf[i];
            if (c == '\n')
                c = '\0';
            if (c == '\0') {
                if (last_was_nul)
                    continue;
                last_was_nul = true;
            } else {
                last_was_nul = false;
            }
            uevent_nl[nl_len++] = c;
        }
        if (nl_len == 0 || uevent_nl[nl_len - 1] != '\0')
            uevent_nl[nl_len++] = '\0';

        netlink_kernel_uevent_send(uevent_nl, (int)nl_len);
    }
}

netdev_t *netdev_register_full(const char *name, uint32_t type, void *desc,
                               const uint8_t *mac, uint32_t mtu,
                               netdev_send_t send, netdev_recv_t recv,
                               netdev_poll_rx_t poll_rx) {
    netdev_t *dev = NULL;

    spin_lock(&netdevs_lock);
    for (uint32_t i = 0; i < MAX_NETDEV_NUM; i++) {
        if (netdevs[i] != NULL) {
            continue;
        }

        dev = calloc(1, sizeof(*dev));
        if (!dev) {
            return NULL;
        }

        dev->id = i;
        dev->type = type;
        dev->desc = desc;
        dev->mtu = mtu;
        dev->send = send;
        dev->recv = recv;
        dev->poll_rx = poll_rx;
        dev->lock = SPIN_INIT;
        dev->refcount = 1;
        dev->rx_queue_count = 1;
        wait_queue_init(&dev->rx_wait);

        if (name && name[0] != '\0') {
            strncpy(dev->name, name, NETDEV_NAME_LEN - 1);
        } else {
            netdev_default_name(dev->name, type, i);
        }

        if (mac) {
            memcpy(dev->mac, mac, sizeof(dev->mac));
        }

        if (type == NETDEV_TYPE_WIFI) {
            dev->admin_up = false;
            dev->link_up = false;
        } else {
            dev->admin_up = true;
            dev->link_up = true;
        }

        netdevs[i] = dev;
        spin_unlock(&netdevs_lock);
        netdev_create_sysfs(dev);
        netdev_notify(dev, NETDEV_EVENT_REGISTERED);
        return dev;
    }
    spin_unlock(&netdevs_lock);

    return NULL;
}

netdev_t *netdev_register(const char *name, uint32_t type, void *desc,
                          const uint8_t *mac, uint32_t mtu, netdev_send_t send,
                          netdev_recv_t recv) {
    return netdev_register_full(name, type, desc, mac, mtu, send, recv, NULL);
}

netdev_t *regist_netdev(void *desc, uint8_t *mac, uint32_t mtu,
                        netdev_send_t send, netdev_recv_t recv) {
    return netdev_register_full(NULL, NETDEV_TYPE_ETHERNET, desc, mac, mtu,
                                send, recv, NULL);
}

netdev_t *get_default_netdev() {
    netdev_t *dev = NULL;
    netdev_t *fallback = NULL;

    spin_lock(&netdevs_lock);
    for (uint32_t i = 0; i < MAX_NETDEV_NUM; i++) {
        if (netdevs[i] && !netdevs[i]->unregistering) {
            if (!fallback) {
                fallback = netdevs[i];
            }

            if (netdevs[i]->admin_up && netdevs[i]->link_up) {
                dev = netdevs[i];
                break;
            }
        }
    }
    if (!dev) {
        dev = fallback;
    }
    if (dev) {
        spin_lock(&dev->lock);
        if (dev->unregistering) {
            dev = NULL;
        } else {
            dev->refcount++;
        }
        spin_unlock(&dev->lock);
    }
    spin_unlock(&netdevs_lock);

    return dev;
}

netdev_t *netdev_get_by_name(const char *name) {
    netdev_t *dev = NULL;

    if (!name) {
        return NULL;
    }

    spin_lock(&netdevs_lock);
    for (uint32_t i = 0; i < MAX_NETDEV_NUM; i++) {
        if (netdevs[i] && !netdevs[i]->unregistering &&
            strcmp(netdevs[i]->name, name) == 0) {
            spin_lock(&netdevs[i]->lock);
            if (!netdevs[i]->unregistering) {
                netdevs[i]->refcount++;
                dev = netdevs[i];
            }
            spin_unlock(&netdevs[i]->lock);
            break;
        }
    }
    spin_unlock(&netdevs_lock);

    return dev;
}

netdev_t *netdev_get_by_index(uint32_t ifindex) {
    netdev_t *dev = NULL;

    if (ifindex == 0) {
        return NULL;
    }

    spin_lock(&netdevs_lock);
    for (uint32_t i = 0; i < MAX_NETDEV_NUM; i++) {
        if (!netdevs[i] || netdevs[i]->unregistering) {
            continue;
        }
        if (netdevs[i]->id + 1 != ifindex) {
            continue;
        }

        spin_lock(&netdevs[i]->lock);
        if (!netdevs[i]->unregistering) {
            netdevs[i]->refcount++;
            dev = netdevs[i];
        }
        spin_unlock(&netdevs[i]->lock);
        break;
    }
    spin_unlock(&netdevs_lock);

    return dev;
}

size_t netdev_snapshot(netdev_t **out, size_t max) {
    size_t count = 0;

    if (!out || max == 0) {
        return 0;
    }

    spin_lock(&netdevs_lock);
    for (uint32_t i = 0; i < MAX_NETDEV_NUM && count < max; i++) {
        netdev_t *dev = netdevs[i];

        if (!dev) {
            continue;
        }

        spin_lock(&dev->lock);
        if (!dev->unregistering) {
            dev->refcount++;
            out[count++] = dev;
        }
        spin_unlock(&dev->lock);
    }
    spin_unlock(&netdevs_lock);

    return count;
}

int netdev_set_name(netdev_t *dev, const char *name) {
    if (!dev || !name || name[0] == '\0') {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    strncpy(dev->name, name, NETDEV_NAME_LEN - 1);
    dev->name[NETDEV_NAME_LEN - 1] = '\0';
    spin_unlock(&dev->lock);

    netdev_notify(dev, NETDEV_EVENT_CONFIG_CHANGED);
    return 0;
}

int netdev_set_sendv(netdev_t *dev, netdev_sendv_t sendv) {
    if (!dev)
        return -EINVAL;

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->sendv = sendv;
    spin_unlock(&dev->lock);
    return 0;
}

int netdev_set_send_gso(netdev_t *dev, netdev_send_gso_t send_gso) {
    if (!dev)
        return -EINVAL;

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->send_gso = send_gso;
    spin_unlock(&dev->lock);
    return 0;
}

void netdev_set_caps(netdev_t *dev, uint32_t caps) {
    if (!dev)
        return;

    spin_lock(&dev->lock);
    if (!dev->unregistering) {
        dev->capabilities = caps;
    }
    spin_unlock(&dev->lock);
}

uint32_t netdev_get_caps(netdev_t *dev) {
    uint32_t caps = 0;

    if (!dev)
        return 0;

    spin_lock(&dev->lock);
    if (!dev->unregistering) {
        caps = dev->capabilities;
    }
    spin_unlock(&dev->lock);
    return caps;
}

int netdev_set_recv_q(netdev_t *dev, netdev_recv_q_t recv_q) {
    if (!dev)
        return -EINVAL;

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->recv_q = recv_q;
    spin_unlock(&dev->lock);
    return 0;
}

int netdev_set_poll_rx_q(netdev_t *dev, netdev_poll_rx_q_t poll_rx_q) {
    if (!dev)
        return -EINVAL;

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->poll_rx_q = poll_rx_q;
    spin_unlock(&dev->lock);
    return 0;
}

void netdev_set_rx_queue_count(netdev_t *dev, uint16_t count) {
    if (!dev || count == 0)
        return;

    spin_lock(&dev->lock);
    if (!dev->unregistering) {
        dev->rx_queue_count = count;
    }
    spin_unlock(&dev->lock);
}

uint16_t netdev_rx_queue_count(netdev_t *dev) {
    uint16_t count = 1;

    if (!dev)
        return 1;

    spin_lock(&dev->lock);
    if (!dev->unregistering && dev->rx_queue_count > 0) {
        count = dev->rx_queue_count;
    }
    spin_unlock(&dev->lock);
    return count;
}

int netdev_set_trigger_scan(netdev_t *dev, netdev_trigger_scan_t trigger_scan) {
    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->trigger_scan = trigger_scan;
    spin_unlock(&dev->lock);
    return 0;
}

int netdev_set_trigger_connect(netdev_t *dev,
                               netdev_trigger_connect_t trigger_connect) {
    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->trigger_connect = trigger_connect;
    spin_unlock(&dev->lock);
    return 0;
}

int netdev_set_trigger_disconnect(
    netdev_t *dev, netdev_trigger_disconnect_t trigger_disconnect) {
    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->trigger_disconnect = trigger_disconnect;
    spin_unlock(&dev->lock);
    return 0;
}

int netdev_set_wireless_info(netdev_t *dev,
                             const netdev_wireless_info_t *info) {
    if (!dev || !info) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->wireless = *info;
    dev->wireless.wiphy_name[NETDEV_WIPHY_NAME_LEN - 1] = '\0';
    spin_unlock(&dev->lock);

    netdev_notify(dev, NETDEV_EVENT_CONFIG_CHANGED);
    return 0;
}

bool netdev_get_wireless_info(netdev_t *dev, netdev_wireless_info_t *info) {
    if (!dev || !info) {
        return false;
    }

    spin_lock(&dev->lock);
    *info = dev->wireless;
    spin_unlock(&dev->lock);
    return info->present;
}

int netdev_set_ipv4_info(netdev_t *dev, const netdev_ipv4_info_t *info) {
    if (!dev || !info) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->ipv4 = *info;
    spin_unlock(&dev->lock);

    netdev_notify(dev, NETDEV_EVENT_CONFIG_CHANGED);
    return 0;
}

bool netdev_get_ipv4_info(netdev_t *dev, netdev_ipv4_info_t *info) {
    if (!dev || !info) {
        return false;
    }

    spin_lock(&dev->lock);
    *info = dev->ipv4;
    spin_unlock(&dev->lock);
    return info->present;
}

int netdev_trigger_scan(netdev_t *dev, const netdev_scan_params_t *params,
                        uint32_t request_portid) {
    netdev_trigger_scan_t trigger_scan;
    void *desc;

    if (!dev) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    spin_lock(&dev->lock);
    trigger_scan = dev->trigger_scan;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (!trigger_scan) {
        netdev_put(dev);
        return -EOPNOTSUPP;
    }

    int ret = trigger_scan(desc, params, request_portid);
    netdev_put(dev);
    return ret;
}

int netdev_trigger_connect(netdev_t *dev, const netdev_connect_params_t *params,
                           uint32_t request_portid) {
    netdev_trigger_connect_t trigger_connect;
    void *desc;
    int ret;

    if (!dev || !params) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    spin_lock(&dev->lock);
    trigger_connect = dev->trigger_connect;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (!trigger_connect) {
        netdev_put(dev);
        return -EOPNOTSUPP;
    }

    ret = trigger_connect(desc, params, request_portid);
    netdev_put(dev);
    return ret;
}

int netdev_trigger_disconnect(netdev_t *dev, uint16_t reason_code,
                              uint32_t request_portid) {
    netdev_trigger_disconnect_t trigger_disconnect;
    void *desc;
    int ret;

    if (!dev) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    spin_lock(&dev->lock);
    trigger_disconnect = dev->trigger_disconnect;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (!trigger_disconnect) {
        netdev_put(dev);
        return -EOPNOTSUPP;
    }

    ret = trigger_disconnect(desc, reason_code, request_portid);
    netdev_put(dev);
    return ret;
}

int netdev_scan_begin(netdev_t *dev, uint32_t request_portid) {
    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    if (dev->scan.running) {
        spin_unlock(&dev->lock);
        return -EBUSY;
    }
    /* Keep the BSS cache across scans, like cfg80211 does. */
    dev->scan.running = true;
    dev->scan.last_aborted = false;
    dev->scan.request_portid = request_portid;
    dev->scan.generation++;
    spin_unlock(&dev->lock);

    return 0;
}

int netdev_scan_store_result(netdev_t *dev,
                             const netdev_scan_result_t *result) {
    uint32_t slot = NETDEV_MAX_SCAN_RESULTS;

    if (!dev || !result) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    /* BSS cache updates are not limited to an active scan window. */
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }

    for (uint32_t i = 0; i < NETDEV_MAX_SCAN_RESULTS; i++) {
        if (!dev->scan.results[i].valid) {
            if (slot == NETDEV_MAX_SCAN_RESULTS)
                slot = i;
            continue;
        }
        if (memcmp(dev->scan.results[i].bssid, result->bssid,
                   sizeof(result->bssid)) == 0 &&
            dev->scan.results[i].frequency == result->frequency) {
            slot = i;
            break;
        }
    }

    if (slot == NETDEV_MAX_SCAN_RESULTS) {
        spin_unlock(&dev->lock);
        return -ENOSPC;
    }

    dev->scan.results[slot] = *result;
    dev->scan.results[slot].valid = true;

    dev->scan.result_count = 0;
    for (uint32_t i = 0; i < NETDEV_MAX_SCAN_RESULTS; i++) {
        if (dev->scan.results[i].valid)
            dev->scan.result_count++;
    }

    printk("netdev: scan cache update if=%s slot=%u count=%u running=%d\n",
           dev->name, slot, dev->scan.result_count, dev->scan.running ? 1 : 0);

    spin_unlock(&dev->lock);
    return 0;
}

int netdev_scan_complete(netdev_t *dev, bool aborted) {
    uint32_t request_portid;
    uint32_t result_count;

    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    dev->scan.running = false;
    dev->scan.last_aborted = aborted;
    request_portid = dev->scan.request_portid;
    result_count = dev->scan.result_count;
    spin_unlock(&dev->lock);

    netlink_publish_scan_event(dev, aborted);

    printk("netdev: scan complete if=%s aborted=%d count=%u\n", dev->name,
           aborted ? 1 : 0, result_count);

    spin_lock(&dev->lock);
    if (!dev->unregistering && dev->scan.request_portid == request_portid)
        dev->scan.request_portid = 0;
    spin_unlock(&dev->lock);

    return 0;
}

bool netdev_get_scan_state(netdev_t *dev, netdev_scan_state_t *state) {
    if (!dev || !state) {
        return false;
    }

    spin_lock(&dev->lock);
    *state = dev->scan;
    spin_unlock(&dev->lock);
    return state->running || state->result_count > 0;
}

int netdev_set_link_state(netdev_t *dev, bool link_up) {
    bool changed = false;

    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    changed = dev->link_up != link_up;
    dev->link_up = link_up;
    spin_unlock(&dev->lock);

    if (changed) {
        netdev_notify(dev,
                      link_up ? NETDEV_EVENT_LINK_UP : NETDEV_EVENT_LINK_DOWN);
    }

    return 0;
}

int netdev_set_admin_state(netdev_t *dev, bool admin_up) {
    bool changed = false;

    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    changed = dev->admin_up != admin_up;
    dev->admin_up = admin_up;
    spin_unlock(&dev->lock);

    if (changed) {
        netdev_notify(dev, admin_up ? NETDEV_EVENT_ADMIN_UP
                                    : NETDEV_EVENT_ADMIN_DOWN);
    }

    return 0;
}

bool netdev_link_is_up(const netdev_t *dev) {
    return dev ? dev->link_up : false;
}

bool netdev_admin_is_up(const netdev_t *dev) {
    return dev ? dev->admin_up : false;
}

bool netdev_get(netdev_t *dev) {
    if (!dev) {
        return false;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return false;
    }
    dev->refcount++;
    spin_unlock(&dev->lock);

    return true;
}

void netdev_put(netdev_t *dev) {
    bool release = false;

    if (!dev) {
        return;
    }

    spin_lock(&dev->lock);
    if (dev->refcount > 0) {
        dev->refcount--;
    }
    release = dev->unregistering && dev->refcount == 0;
    spin_unlock(&dev->lock);

    if (release) {
        free(dev);
    }
}

int netdev_unregister(netdev_t *dev) {
    uint32_t slot = UINT32_MAX;

    if (!dev) {
        return -EINVAL;
    }

    spin_lock(&netdevs_lock);
    for (uint32_t i = 0; i < MAX_NETDEV_NUM; i++) {
        if (netdevs[i] == dev) {
            slot = i;
            netdevs[i] = NULL;
            break;
        }
    }
    spin_unlock(&netdevs_lock);

    if (slot == UINT32_MAX) {
        return -ENOENT;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return 0;
    }
    dev->unregistering = true;
    dev->link_up = false;
    dev->admin_up = false;
    spin_unlock(&dev->lock);

    netdev_notify(dev, NETDEV_EVENT_LINK_DOWN | NETDEV_EVENT_ADMIN_DOWN |
                           NETDEV_EVENT_UNREGISTERING);
    netdev_notify_rx(dev);

    for (;;) {
        uint32_t refs = 0;

        spin_lock(&dev->lock);
        refs = dev->refcount;
        spin_unlock(&dev->lock);

        if (refs <= 1) {
            break;
        }

        bool irq_state = arch_interrupt_enabled();
        arch_enable_interrupt();
        arch_wait_for_interrupt();
        if (!irq_state) {
            arch_disable_interrupt();
        }
    }

    netdev_notify(dev, NETDEV_EVENT_UNREGISTERED);
    netdev_put(dev);
    return 0;
}

int netdev_register_listener(netdev_t *dev, netdev_event_cb_t cb, void *ctx) {
    if (!dev || !cb) {
        return -EINVAL;
    }

    spin_lock(&dev->lock);
    if (dev->unregistering) {
        spin_unlock(&dev->lock);
        return -ENODEV;
    }
    for (uint32_t i = 0; i < NETDEV_MAX_EVENT_LISTENERS; i++) {
        if (dev->listeners[i].cb == NULL) {
            dev->listeners[i].cb = cb;
            dev->listeners[i].ctx = ctx;
            spin_unlock(&dev->lock);
            return 0;
        }
    }
    spin_unlock(&dev->lock);

    return -ENOSPC;
}

void netdev_unregister_listener(netdev_t *dev, netdev_event_cb_t cb,
                                void *ctx) {
    if (!dev || !cb) {
        return;
    }

    spin_lock(&dev->lock);
    for (uint32_t i = 0; i < NETDEV_MAX_EVENT_LISTENERS; i++) {
        if (dev->listeners[i].cb == cb && dev->listeners[i].ctx == ctx) {
            dev->listeners[i].cb = NULL;
            dev->listeners[i].ctx = NULL;
            break;
        }
    }
    spin_unlock(&dev->lock);
}

int netdev_register_notifier(netdev_event_cb_t cb, void *ctx) {
    if (!cb)
        return -EINVAL;

    spin_lock(&netdev_notifiers_lock);
    for (uint32_t i = 0; i < NETDEV_MAX_NOTIFIERS; i++) {
        if (!netdev_notifiers[i].cb) {
            netdev_notifiers[i] = (netdev_notifier_t){.cb = cb, .ctx = ctx};
            spin_unlock(&netdev_notifiers_lock);
            return 0;
        }
    }
    spin_unlock(&netdev_notifiers_lock);
    return -ENOSPC;
}

void netdev_unregister_notifier(netdev_event_cb_t cb, void *ctx) {
    if (!cb)
        return;

    spin_lock(&netdev_notifiers_lock);
    for (uint32_t i = 0; i < NETDEV_MAX_NOTIFIERS; i++) {
        if (netdev_notifiers[i].cb == cb && netdev_notifiers[i].ctx == ctx) {
            netdev_notifiers[i] = (netdev_notifier_t){0};
            break;
        }
    }
    spin_unlock(&netdev_notifiers_lock);
}

void netdev_notify(netdev_t *dev, uint32_t events) {
    netdev_listener_t listeners[NETDEV_MAX_EVENT_LISTENERS];
    netdev_notifier_t notifiers[NETDEV_MAX_NOTIFIERS];

    if (!dev || !events) {
        return;
    }

    spin_lock(&dev->lock);
    memcpy(listeners, dev->listeners, sizeof(listeners));
    spin_unlock(&dev->lock);
    spin_lock(&netdev_notifiers_lock);
    memcpy(notifiers, netdev_notifiers, sizeof(notifiers));
    spin_unlock(&netdev_notifiers_lock);

    for (uint32_t i = 0; i < NETDEV_MAX_EVENT_LISTENERS; i++) {
        if (listeners[i].cb) {
            listeners[i].cb(dev, events, listeners[i].ctx);
        }
    }
    for (uint32_t i = 0; i < NETDEV_MAX_NOTIFIERS; i++) {
        if (notifiers[i].cb)
            notifiers[i].cb(dev, events, notifiers[i].ctx);
    }

    netlink_publish_netdev_event(dev, events);
}

void netdev_notify_rx(netdev_t *dev) {
    if (!dev)
        return;

    spin_lock(&dev->lock);
    dev->rx_seq++;
    spin_unlock(&dev->lock);

    wait_queue_wake_all(&dev->rx_wait, 0, EOK);
}

uint64_t netdev_rx_seq(netdev_t *dev) {
    uint64_t seq = 0;

    if (!dev)
        return 0;

    spin_lock(&dev->lock);
    seq = dev->rx_seq;
    spin_unlock(&dev->lock);
    return seq;
}

int netdev_wait_rx_q(netdev_t *dev, uint16_t qidx, uint64_t observed_seq) {
    wait_queue_entry_t wait;
    netdev_poll_rx_t poll_rx = NULL;
    netdev_poll_rx_q_t poll_rx_q = NULL;
    void *desc = NULL;
    bool ready = false;
    bool gone = false;
    int reason;

    if (!dev)
        return -EINVAL;

    task_prepare_block(current_task);
    wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
    wait_queue_add(&dev->rx_wait, &wait);

    spin_lock(&dev->lock);
    ready = dev->rx_seq != observed_seq;
    gone = dev->unregistering;
    poll_rx = dev->poll_rx;
    poll_rx_q = dev->poll_rx_q;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (!ready && !gone) {
        if (poll_rx_q && poll_rx_q(desc, qidx)) {
            ready = true;
        } else if (qidx == 0 && poll_rx && poll_rx(desc)) {
            ready = true;
        }
    }

    if (ready || gone) {
        wait_queue_remove(&dev->rx_wait, &wait);
        task_cancel_block_prepare(current_task);
        return gone ? -ENODEV : EOK;
    }

    if (task_signal_has_deliverable(current_task)) {
        wait_queue_remove(&dev->rx_wait, &wait);
        task_cancel_block_prepare(current_task);
        return -EINTR;
    }

    reason = task_block(current_task, TASK_BLOCKING, NETDEV_RX_POLL_FALLBACK_NS,
                        "netdev_rx");

    wait_queue_remove(&dev->rx_wait, &wait);
    task_cancel_block_prepare(current_task);

    if (reason == ETIMEDOUT)
        return EOK;
    if (reason < 0)
        return reason;
    if (reason != EOK && task_signal_has_deliverable(current_task))
        return -EINTR;
    return reason == EOK ? EOK : -EINTR;
}

int netdev_wait_rx(netdev_t *dev, uint64_t observed_seq) {
    return netdev_wait_rx_q(dev, 0, observed_seq);
}

int netdev_send(netdev_t *dev, void *data, uint32_t len) {
    if (dev == NULL || data == NULL) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    if (len == 0) {
        netdev_put(dev);
        return 0;
    }

    int ret = dev->send(dev->desc, data, len);
    netdev_put(dev);
    return ret;
}

int netdev_sendv(netdev_t *dev, const netdev_iovec_t *iov, uint32_t iovcnt,
                 uint32_t total_len) {
    netdev_sendv_t sendv = NULL;
    void *desc = NULL;
    void *frame = NULL;
    uint32_t copied = 0;
    int ret = 0;

    if (!dev || !iov || !iovcnt || !total_len ||
        total_len > netdev_max_frame_len(dev->mtu)) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    spin_lock(&dev->lock);
    sendv = dev->sendv;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (sendv) {
        ret = sendv(desc, iov, iovcnt, total_len);
        netdev_put(dev);
        return ret;
    }

    frame = alloc_frames_bytes(total_len);
    if (!frame) {
        netdev_put(dev);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < iovcnt; i++) {
        if (!iov[i].data || iov[i].len > total_len - copied) {
            free_frames_bytes(frame, total_len);
            netdev_put(dev);
            return -EINVAL;
        }
        memcpy((uint8_t *)frame + copied, iov[i].data, iov[i].len);
        copied += iov[i].len;
    }
    if (copied != total_len) {
        free_frames_bytes(frame, total_len);
        netdev_put(dev);
        return -EINVAL;
    }

    ret = dev->send(desc, frame, total_len);
    free_frames_bytes(frame, total_len);
    netdev_put(dev);
    return ret;
}

int netdev_send_gso(netdev_t *dev, const netdev_iovec_t *iov, uint32_t iovcnt,
                    uint32_t total_len, const netdev_gso_info_t *gso) {
    netdev_send_gso_t send_gso = NULL;
    void *desc = NULL;
    int ret = 0;

    if (!dev || !iov || !iovcnt || !total_len || !gso ||
        gso->type == NETDEV_GSO_NONE) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    spin_lock(&dev->lock);
    send_gso = dev->send_gso;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (send_gso) {
        ret = send_gso(desc, iov, iovcnt, total_len, gso);
        netdev_put(dev);
        return ret;
    }

    /* Device has no segmentation offload; fall back to a normal send. */
    ret = netdev_sendv(dev, iov, iovcnt, total_len);
    netdev_put(dev);
    return ret;
}

int netdev_recv(netdev_t *dev, void *data, uint32_t len) {
    if (dev == NULL || data == NULL) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    if (len == 0) {
        netdev_put(dev);
        return 0;
    }

    int ret = dev->recv(dev->desc, data, len);

    netdev_put(dev);
    return ret;
}

int netdev_recv_q(netdev_t *dev, uint16_t qidx, void *data, uint32_t len) {
    netdev_recv_q_t recv_q = NULL;
    netdev_recv_t recv = NULL;
    void *desc = NULL;
    int ret = 0;

    if (!dev || !data) {
        return -EINVAL;
    }
    if (!netdev_get(dev)) {
        return -ENODEV;
    }

    if (len == 0) {
        netdev_put(dev);
        return 0;
    }

    spin_lock(&dev->lock);
    recv_q = dev->recv_q;
    recv = dev->recv;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (recv_q) {
        ret = recv_q(desc, qidx, data, len);
    } else if (qidx == 0 && recv) {
        ret = recv(desc, data, len);
    } else {
        ret = -EINVAL;
    }

    netdev_put(dev);
    return ret;
}

bool netdev_poll_rx_q(netdev_t *dev, uint16_t qidx) {
    netdev_poll_rx_q_t poll_rx_q = NULL;
    netdev_poll_rx_t poll_rx = NULL;
    void *desc = NULL;
    bool ready = false;

    if (!dev)
        return false;

    spin_lock(&dev->lock);
    poll_rx_q = dev->poll_rx_q;
    poll_rx = dev->poll_rx;
    desc = dev->desc;
    spin_unlock(&dev->lock);

    if (poll_rx_q) {
        ready = poll_rx_q(desc, qidx);
    } else if (qidx == 0 && poll_rx) {
        ready = poll_rx(desc);
    }
    return ready;
}
